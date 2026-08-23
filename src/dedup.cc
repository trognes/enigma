#include "dedup.h"

#include "common.h"
#include "options.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

/* One 64-bit word per block, so a lookup is a single aligned load. */
static const int block_bits = 64;

static uint64_t * g_filter = nullptr;
static size_t g_blocks_per_key = 0;
static int g_k = 0;                  /* bits set per item */
static double g_bits_per_item = 0.0; /* provisioned, see below */
static double g_fp = 0.0;            /* expected false-positive rate at that */
static size_t g_bytes = 0;
/* Relaxed adds on the per-item path. An earlier version made these
   thread-local with a per-pass flush, on the theory that an atomic RMW before
   each filter access would build a happens-before chain and mask the race the
   TSan case exists to catch. That was TESTED AND IS FALSE -- TSan reports the
   race with the barrier removed either way -- so the simpler form stays. (The
   silence that suggested the theory was a stale object file: the Makefile does
   not track EXTRA_CXXFLAGS, so a sanitizer build over -O2 objects leaves parts
   of the program uninstrumented. make clean first.) At climb rates these are
   ~1e4 adds/s, which is nothing. */
static std::atomic<uint64_t> g_skipped{0};
static std::atomic<uint64_t> g_seeds{0};
/* Not atomic and not thread_local on purpose: it is written only by the thread
   that drives a nested search, outside that search's fan-out and join. */
static bool g_suspended = false;

/* False-positive rate of a BLOCKED filter: a block receiving j items behaves
   like an ordinary filter of block_bits bits holding j, and j is Poisson with
   mean block_bits/bits_per_item. Averaging over j is the whole difference from
   the textbook formula, and at 64-bit blocks it is not a rounding error -- 8
   bits per item reads 3.19% blocked against 2.16% unblocked. It is also why k
   is chosen numerically here rather than from 0.693*bits_per_item: the scatter
   punishes extra probes, so the optimum sits lower (5 rather than 7 at 10 bits
   per item). */
static double fp_blocked(double bits_per_item, int k)
{
  const double lam = block_bits / bits_per_item;
  double p = exp(-lam);
  double tot = 0.0;
  for (int j = 0; j < 4096; j++)
    {
      if (j > 0)
        p *= lam / j;
      const double load = 1.0 - exp(-(k * static_cast<double>(j)) / block_bits);
      tot += p * pow(load, k);
      if ((p < 1e-17) && (static_cast<double>(j) > lam))
        break;
    }
  return tot;
}

/* 26 bytes of involution -> 64 bits. The board is already canonical (steck[i]
   is i's partner, i itself when unplugged), so nothing needs normalising. */
static inline uint64_t mix64(uint64_t x)
{
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static inline uint64_t hash_board(const unsigned char * board, uint64_t seed)
{
  uint64_t h = seed + 0x9e3779b97f4a7c15ULL;
  uint64_t v;
  memcpy(& v, board + 0, 8);   h = mix64(h ^ v);
  memcpy(& v, board + 8, 8);   h = mix64(h ^ v);
  memcpy(& v, board + 16, 8);  h = mix64(h ^ v);
  h = mix64(h ^ (static_cast<uint64_t>(board[24])
                 | (static_cast<uint64_t>(board[25]) << 8)));
  return h;
}

/* The k bits this item sets, as a mask over the block. Each position needs 6
   bits of hash, so k <= 5 fits in the low half with the high half left for the
   block index; beyond that a second mix supplies more. */
static inline uint64_t pattern_of(uint64_t h, int k)
{
  uint64_t mask = 0;
  uint64_t bits = h;
  for (int j = 0; j < k; j++)
    {
      if (j == 5)
        bits = mix64(h);
      mask |= 1ULL << (bits & 63);
      bits >>= 6;
    }
  return mask;
}

bool seed_dedup_init(size_t nkeys, size_t restarts)
{
  if (! opt_seed_dedup)
    return true;
  if ((nkeys == 0) || (restarts == 0))
    return true;

  /* One item per restart. The realised load is lower -- a duplicate is skipped
     rather than inserted, so only DISTINCT seeds go in -- but that is a
     property of the run, not of the sizing, so provision for the worst case
     and let the final report say what was actually paid. */
  const double want_bits = static_cast<double>(restarts) * opt_seed_dedup_bits;
  size_t bytes_per_key = static_cast<size_t>((want_bits + 63.0) / 64.0) * 8;
  if (bytes_per_key < 8)
    bytes_per_key = 8;

  if (opt_seed_dedup_max > 0)
    {
      /* Refuse rather than thin the filter silently: an under-sized filter
         does not degrade gracefully, it starts skipping distinct seeds and
         costs more coverage than it saves. Name what would fit instead. */
      const double need = static_cast<double>(nkeys)
                          * static_cast<double>(bytes_per_key);
      if (need > static_cast<double>(opt_seed_dedup_max))
        {
          const size_t fit_bytes =
            (opt_seed_dedup_max / nkeys) / 8 * 8;
          const double fit_bits = (fit_bytes >= 8)
            ? (static_cast<double>(fit_bytes) * 8.0
               / static_cast<double>(restarts))
            : 0.0;
          fprintf(stderr,
                  "Error: --seed-dedup needs %.2f GiB at %d bits/item "
                  "(%zu keys x %zu restarts), over the --seed-dedup-max "
                  "of %.2f GiB.\n",
                  need / 1073741824.0, opt_seed_dedup_bits, nkeys, restarts,
                  static_cast<double>(opt_seed_dedup_max) / 1073741824.0);
          if (fit_bits >= 4.0)
            fprintf(stderr,
                    "       --seed-dedup-bits %d fits (%.2f GiB).\n",
                    static_cast<int>(fit_bits),
                    static_cast<double>(nkeys)
                    * static_cast<double>(fit_bytes) / 1073741824.0);
          else
            fprintf(stderr,
                    "       Nothing above 4 bits/item fits; raise the budget "
                    "or lower -R.\n");
          return false;
        }
    }

  g_blocks_per_key = bytes_per_key / 8;
  g_bytes = nkeys * bytes_per_key;
  g_bits_per_item = static_cast<double>(bytes_per_key) * 8.0
                    / static_cast<double>(restarts);

  g_k = 1;
  double bestfp = fp_blocked(g_bits_per_item, 1);
  for (int k = 2; k <= 16; k++)
    {
      const double f = fp_blocked(g_bits_per_item, k);
      if (f < bestfp)
        {
          bestfp = f;
          g_k = k;
        }
    }
  g_fp = bestfp;

  /* calloc, not malloc: every block must start empty, and a read of an
     uninitialised block would skip climbs at random -- a corruption invisible
     to every check except valgrind. calloc also gets the zero pages lazily
     from the kernel, so a multi-gigabyte filter costs nothing up front. */
  g_filter = static_cast<uint64_t *>(calloc(nkeys, bytes_per_key));
  if (g_filter == nullptr)
    {
      fprintf(stderr,
              "Error: --seed-dedup could not allocate %.2f GiB.\n",
              static_cast<double>(g_bytes) / 1073741824.0);
      g_blocks_per_key = 0;
      return false;
    }
  return true;
}

bool seed_dedup_on()
{
  return g_filter != nullptr;
}

void seed_dedup_suspend(bool off)
{
  g_suspended = off;
}

bool seed_dedup_seen(size_t key, const unsigned char * board)
{
  if ((g_filter == nullptr) || g_suspended)
    return false;

  g_seeds.fetch_add(1, std::memory_order_relaxed);

  const uint64_t h = hash_board(board, static_cast<uint64_t>(opt_seed));
  uint64_t * const base = g_filter + key * g_blocks_per_key;
  uint64_t * const block =
    base + static_cast<size_t>(h >> 32) % g_blocks_per_key;
  const uint64_t pattern = pattern_of(h, g_k);

  const uint64_t w = *block;          /* the one load */
  if ((w & pattern) == pattern)
    {
      g_skipped.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  *block = w | pattern;
  return false;
}

uint64_t seed_dedup_skipped()
{
  return g_skipped.load(std::memory_order_relaxed);
}

uint64_t seed_dedup_seeds()
{
  return g_seeds.load(std::memory_order_relaxed);
}

void seed_dedup_describe(char * buf, size_t buflen)
{
  /* PROVISIONED, not effective. The effective figure divides by the DISTINCT
     seed count, which no run knows before it has run, so printing it here
     would be a claim about the outcome. The realised rate follows from the
     final skip line.

     "AT FULL LOAD" is the other half of the same honesty, and it is worth
     several times over: the quoted rate is the filter once every item is in,
     but most queries hit it part-filled, and a k-bit test grows as load^k, so
     the run-average is roughly 1/(k+1) of it. Measured on 43 264 real seeds at
     8 bits/item (k = 4): 303 false positives observed against the 1 301 the
     full-load figure implies, i.e. 4.3x fewer, against the 5x the 1/(k+1) rule
     predicts. Quoting the full-load rate is the conservative direction -- it
     overstates the coverage the user is giving up, never understates it. */
  const char * unit = "bytes";
  double amount = static_cast<double>(g_bytes);
  if (g_bytes >= (1ULL << 30))
    {
      unit = "GiB";
      amount /= 1073741824.0;
    }
  else if (g_bytes >= (1ULL << 20))
    {
      unit = "MiB";
      amount /= 1048576.0;
    }
  else if (g_bytes >= (1ULL << 10))
    {
      unit = "KiB";
      amount /= 1024.0;
    }
  snprintf(buf, buflen,
           "%zu block%s/key (%zu bytes), %.2f %s total, %.2f bits/item "
           "provisioned,\n            k = %d, false positives %.2f%% at "
           "full load",
           g_blocks_per_key, (g_blocks_per_key == 1) ? "" : "s",
           g_blocks_per_key * 8, amount, unit,
           g_bits_per_item, g_k, 100.0 * g_fp);
}

void seed_dedup_free()
{
  free(g_filter);
  g_filter = nullptr;
  g_blocks_per_key = 0;
}
