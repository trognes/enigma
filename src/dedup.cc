#include "dedup.h"

#include "common.h"
#include "keyspace.h"
#include "options.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>

/* One 64-bit word per block, so a lookup is a single aligned load. */
static const int block_bits = 64;

static uint64_t * g_filter = nullptr;
static size_t g_blocks_per_key = 0;
static int g_k = 0;                  /* bits set per item */
static double g_bits_per_item = 0.0; /* provisioned, see below */
static double g_fp = 0.0;            /* expected false-positive rate at that */
static size_t g_bytes = 0;
static size_t g_slots = 0;           /* regions = keys the sweep visits */

/* THE SLOT MAP: flat key index -> region number, dense over the keys the
   sweep visits. The index decodes as (task, ring combo, start combo) with the
   starts innermost -- the same mixed radix search_worker() uses -- and the
   two collapses skip keys on two independent axes:

     ring:  the two-notch right wheel drops ring2 >= 13, so a halved task has
            13 ring2 values per (ring0, ring1) instead of rc[2]
     start: the §7.12 mask drops (start1, start2) pairs, so a task has `reps`
            visited pairs per start0 instead of gc[1]*gc[2]

   A task's slots are laid out ring-major exactly as its keys are, so within a
   task the region of a key is its rank among the visited ones; across tasks a
   prefix sum gives the base. The start-pair rank is a small per-rotor-pair
   table (gc[1]*gc[2] <= 676 entries), shared by every task on that pair since
   the mask depends on the middle/right rotors alone.

   Cost per query: a handful of divisions and loads, once per work item, next
   to a climb of ~1 ms. */
static size_t g_rg = 0;      /* keys per task */
static size_t g_gsize = 0;   /* start combos per ring combo */
static size_t g_rc2 = 0;     /* ring2 values in the index */
static size_t g_gc0 = 0;     /* start0 values */
static size_t g_gc12 = 0;    /* (start1, start2) pairs in the index */
static std::vector<size_t> g_task_base;           /* first slot of the task */
static std::vector<size_t> g_task_reps;           /* visited pairs per start0 */
static std::vector<unsigned char> g_task_halved;  /* ring2 >= 13 dropped */
static std::vector<const uint16_t *> g_task_rank; /* pair rank, or null */
static std::vector<std::vector<uint16_t>> g_rank_store;  /* per rotor pair */

static inline size_t slot_of(size_t keyidx)
{
  const size_t wo = keyidx / g_rg;
  const size_t rem = keyidx % g_rg;
  const size_t rflat = rem / g_gsize;
  const size_t gflat = rem % g_gsize;
  /* ring rank: identity, or with the dropped ring2 half squeezed out */
  size_t rvis = rflat;
  if (g_task_halved[wo])
    rvis = (rflat / g_rc2) * (asize / 2) + rflat % g_rc2;
  /* start rank: start0 major, then the pair's rank among the visited ones */
  const size_t g1 = gflat / g_gc12;
  const size_t gg = gflat % g_gc12;
  const uint16_t * rank = g_task_rank[wo];
  const size_t reps = g_task_reps[wo];
  const size_t grank = g1 * reps + ((rank != nullptr) ? rank[gg] : gg);
  return g_task_base[wo] + rvis * (g_gc0 * reps) + grank;
}

/* Build the slot map. Returns the slot count, which must equal
   ks.scored_keys -- the two are computed from the same per-task facts but by
   different code, and the equality is the check that the map agrees with the
   sweep (a map that disagreed would alias regions between keys or run off the
   end of the filter, and neither shows in any answer). */
static size_t build_slot_map(const key_space & ks)
{
  g_rg = ks.rsize * ks.gsize;
  g_gsize = ks.gsize;
  g_rc2 = static_cast<size_t>(ks.rc[2]);
  g_gc0 = static_cast<size_t>(ks.gc[0]);
  g_gc12 = static_cast<size_t>(ks.gc[1]) * ks.gc[2];

  const size_t ntasks = ks.tasks.size();
  g_task_base.assign(ntasks, 0);
  g_task_reps.assign(ntasks, 0);
  g_task_halved.assign(ntasks, 0);
  g_task_rank.assign(ntasks, nullptr);
  g_rank_store.assign(static_cast<size_t>(rotor_count) * rotor_count,
                      std::vector<uint16_t>());

  size_t slots = 0;
  for (size_t wo = 0; wo < ntasks; wo++)
    {
      const wheel_task & t = ks.tasks[wo];
      g_task_base[wo] = slots;

      size_t r2_surv = g_rc2;
      if (task_r2_halved(t))
        {
          g_task_halved[wo] = 1;
          r2_surv = asize / 2;
        }
      const size_t rsurv = static_cast<size_t>(ks.rc[0]) * ks.rc[1] * r2_surv;

      size_t reps = g_gc12;
      const uint32_t * row = task_mid_row(t);
      if (row != nullptr)
        {
          const size_t pair =
            static_cast<size_t>(t.w[1]) * rotor_count + t.w[2];
          std::vector<uint16_t> & rank = g_rank_store[pair];
          if (rank.empty())
            {
              /* exclusive prefix count of visited pairs, in index order */
              rank.resize(g_gc12);
              size_t seen = 0;
              const size_t gc2 = static_cast<size_t>(ks.gc[2]);
              for (size_t gg = 0; gg < g_gc12; gg++)
                {
                  const int g2 = ks.range.g_min[1]
                                 + static_cast<int>(gg / gc2);
                  const int g3 = ks.range.g_min[2]
                                 + static_cast<int>(gg % gc2);
                  rank[gg] = static_cast<uint16_t>(seen);
                  if ((row[g3] >> g2) & 1u)
                    seen++;
                }
              /* one past the last entry is the count, kept in the store's
                 size so the table stays exactly gc12 long */
              rank.push_back(static_cast<uint16_t>(seen));
            }
          g_task_rank[wo] = rank.data();
          reps = rank[g_gc12];
        }
      g_task_reps[wo] = reps;
      slots += rsurv * g_gc0 * reps;
    }
  return slots;
}

static void free_slot_map()
{
  g_task_base.clear();
  g_task_reps.clear();
  g_task_halved.clear();
  g_task_rank.clear();
  g_rank_store.clear();
  g_slots = 0;
}
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

bool seed_dedup_init(const key_space & ks, size_t restarts)
{
  if (! opt_seed_dedup)
    return true;
  if ((ks.total_keys == 0) || (restarts == 0))
    return true;

  /* Regions for the keys the sweep VISITS. The map and the key space count
     them independently from the same facts; a mismatch means the map would
     not follow the sweep, which is a bug here and not a budget problem. */
  const size_t nkeys = build_slot_map(ks);
  if (nkeys != ks.scored_keys)
    {
      fprintf(stderr,
              "Error: --seed-dedup slot map holds %zu keys but the sweep "
              "scores %zu.\n", nkeys, ks.scored_keys);
      free_slot_map();
      return false;
    }
  if (nkeys == 0)
    {
      /* every key collapsed away: nothing to climb, nothing to filter */
      free_slot_map();
      return true;
    }
  g_slots = nkeys;

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
          free_slot_map();
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
      free_slot_map();
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

  const size_t slot = slot_of(key);
  /* A slot past the end would be a map that disagrees with the sweep after
     all -- corrupting memory silently, or skipping climbs against another
     key's seeds. One compare per item beside a millisecond climb. */
  if (slot >= g_slots)
    {
      fprintf(stderr, "Error: --seed-dedup slot %zu of %zu for key %zu.\n",
              slot, g_slots, key);
      abort();
    }

  const uint64_t h = hash_board(board, static_cast<uint64_t>(opt_seed));
  uint64_t * const base = g_filter + slot * g_blocks_per_key;
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
  /* The key count is the SCORED one -- the regions the map hands out -- so a
     reader can multiply it out against the "Analysed N rotor combinations"
     line and see that the collapsed keys were not paid for. */
  snprintf(buf, buflen,
           "%zu block%s/key (%zu bytes) for %zu keys, %.2f %s total,\n"
           "            %.2f bits/item provisioned, k = %d, false positives "
           "%.2f%% at full load",
           g_blocks_per_key, (g_blocks_per_key == 1) ? "" : "s",
           g_blocks_per_key * 8, g_slots, amount, unit,
           g_bits_per_item, g_k, 100.0 * g_fp);
}

void seed_dedup_free()
{
  free(g_filter);
  g_filter = nullptr;
  g_blocks_per_key = 0;
  free_slot_map();
}
