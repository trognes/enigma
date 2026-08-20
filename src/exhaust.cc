#include "exhaust.h"

#include "common.h"
#include "crib.h"
#include "keyspace.h"
#include "machine.h"
#include "options.h"
#include "plugboard.h"
#include "progress.h"
#include "result.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/resource.h>

/* Number of distinct sets of `pairs` disjoint plug pairs drawable from `free` letters:
   free! / (2^p p! (free-2p)!). Returned as a double (the count explodes fast). Used for the
   exhaustion combo count and for the restart pigeonhole warning (a kick of K pairs among the
   free letters has this many distinct outcomes). */
double disjoint_pair_combinations(int free_letters, int pairs)
{
  double combos = 1.0;
  for (int i = 0; i < pairs; i++)
    combos *= static_cast<double>((free_letters - 2*i) * (free_letters - 2*i - 1))
              / (2.0 * (i + 1));
  return combos;
}
/* Flat list of the free first-pair choices (x,y), two bytes each; built once before the
   search by build_exhaust_firsts(), read-only during it. */
static std::vector<unsigned char> g_exhaust_firsts;
struct exhaust_ctx
{
  int a[pairs_uncapped];   /* the currently-chosen forced pairs (depth of them) */
  int b[pairs_uncapped];
  int target;              /* E forced pairs */
  size_t key_index;        /* for the per-restart RNG seed */
  bool used[asize];        /* letters consumed by -s + forced-so-far (enumeration only) */
  double best;
  bool have;
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
};
/* At a full combo (the E forced pairs in c.a/c.b): run every restart's climb from the seed
   board -- identity + -s + the forced pairs -- plus this restart's --random kick over the
   still-free letters, and keep the best in c. The forced pairs are pinned in plug_fixed so
   the climb (and the kick, which draws only from self-steckered letters) leave them intact. */
static void exhaust_leaf(machine & m, exhaust_ctx & c)
{
  const bool kicked = (opt_restarts >= 1);
  const int climbs = kicked ? opt_restarts : 1;
  for (int r = 0; r < climbs; r++)
    {
      init_steckerbrett(m, opt_steckerbrett);   /* board = identity + -s */
      plug_fixed_ex_reset(m);   /* per-worker pins = -s seed ... */
      for (int i = 0; i < c.target; i++)
        {
          m.steckerbrett[c.a[i]] = static_cast<unsigned char>(c.b[i]);
          m.steckerbrett[c.b[i]] = static_cast<unsigned char>(c.a[i]);
          plug_fixed_ex_pin(m, c.a[i]);   /* ... plus the forced pair */
          plug_fixed_ex_pin(m, c.b[i]);
        }
      if (kicked)
        {
          uint64_t rng = restart_seed(c.key_index, r);
          perturb_steckerbrett(m, & rng, opt_perturb);
        }
      double s = run_stages<true>(m);
      if (! c.have || (s > c.best))
        {
          c.best = s;
          c.have = true;
          memcpy(c.best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
          memcpy(c.best_steck, m.steckerbrett, asize);
        }
    }
}
/* Choose the remaining forced pairs (from depth up to c.target), each pair's low letter in
   increasing order (`first`) so every combo is enumerated exactly once. c.used excludes the
   -s letters and the pairs chosen so far. At full depth, climb (exhaust_leaf). */
static void exhaust_recurse(machine & m, exhaust_ctx & c, int depth, int first)
{
  if (depth == c.target)
    {
      exhaust_leaf(m, c);
      return;
    }
  for (int x = first; x < asize; x++)
    {
      if (c.used[x])
        continue;
      for (int y = x + 1; y < asize; y++)
        {
          if (c.used[y])
            continue;
          c.a[depth] = x;
          c.b[depth] = y;
          c.used[x] = c.used[y] = true;
          exhaust_recurse(m, c, depth + 1, x + 1);
          c.used[x] = c.used[y] = false;
        }
    }
}
/* Initialise c for one rotor key: E forced pairs, -s letters marked used. */
static void exhaust_ctx_init(exhaust_ctx & c, size_t key_index)
{
  c.target = opt_exhaust;
  c.key_index = key_index;
  c.best = 0.0;
  c.have = false;
  for (int j = 0; j < asize; j++)
    c.used[j] = false;
  int fixed = static_cast<int>(strlen(opt_steckerbrett) / 2);
  for (int i = 0; i < fixed; i++)
    {
      c.used[char2num(opt_steckerbrett[2*i+0])] = true;
      c.used[char2num(opt_steckerbrett[2*i+1])] = true;
    }
  for (const char * p = opt_no_plug; *p != 0; p++)   /* --no-plug: not available to force */
    c.used[char2num(*p)] = true;
}
double exhaust_unit(machine & m, size_t key_index, size_t fi)
{
  exhaust_ctx c;
  exhaust_ctx_init(c, key_index);
  int x = g_exhaust_firsts[2*fi];
  int y = g_exhaust_firsts[2*fi + 1];
  c.a[0] = x;
  c.b[0] = y;
  c.used[x] = c.used[y] = true;
  exhaust_recurse(m, c, 1, x + 1);   /* remaining E-1 pairs use letters above x */
  if (c.have)
    {
      memcpy(m.plaintext, c.best_pt, static_cast<size_t>(textlength) + 1);
      memcpy(m.steckerbrett, c.best_steck, asize);
      return c.best;
    }
  return unit_no_score;   /* no valid combo under this first pair: never wins the merge */
}
/* Whole-key exhaustion (used by the -F tier-2 climb, which parallelises over keys): every
   first-pair unit, keeping the best board/plaintext. Validation guarantees >= 1 combo. */
double exhaust_all_combos(machine & m, size_t key_index)
{
  double best = 0.0;
  bool have = false;
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
  size_t nfirsts = g_exhaust_firsts.size() / 2;
  for (size_t fi = 0; fi < nfirsts; fi++)
    {
      double s = exhaust_unit(m, key_index, fi);
      if ((! have || (s > best)) && (s > unit_no_score))
        {
          best = s;
          have = true;
          memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
          memcpy(best_steck, m.steckerbrett, asize);
        }
    }
  if (have)
    {
      memcpy(m.plaintext, best_pt, static_cast<size_t>(textlength) + 1);
      memcpy(m.steckerbrett, best_steck, asize);
    }
  return best;
}
/* Enumerate the free first-pair choices (x,y) into g_exhaust_firsts: every pair of letters
   not pinned by -s, low letter first. Built once before the search (read-only after). Bounded
   by C(free,2) <= 325 regardless of E, so it never explodes in memory (unlike a full combo
   list); each unit does its own bounded sub-exhaustion. */
void build_exhaust_firsts()
{
  bool sfixed[asize];
  for (int j = 0; j < asize; j++)
    sfixed[j] = false;
  int fixed = static_cast<int>(strlen(opt_steckerbrett) / 2);
  for (int i = 0; i < fixed; i++)
    {
      sfixed[char2num(opt_steckerbrett[2*i+0])] = true;
      sfixed[char2num(opt_steckerbrett[2*i+1])] = true;
    }
  for (const char * p = opt_no_plug; *p != 0; p++)   /* --no-plug: never force a pair here */
    sfixed[char2num(*p)] = true;
  g_exhaust_firsts.clear();
  for (int x = 0; x < asize; x++)
    {
      if (sfixed[x])
        continue;
      for (int y = x + 1; y < asize; y++)
        {
          if (sfixed[y])
            continue;
          g_exhaust_firsts.push_back(static_cast<unsigned char>(x));
          g_exhaust_firsts.push_back(static_cast<unsigned char>(y));
        }
    }
}
/* Units of work --exhaust resolves to: one per first forced pair, at most
   C(free,2) <= 325. */
size_t exhaust_unit_count()
{
  return g_exhaust_firsts.size() / 2;
}
