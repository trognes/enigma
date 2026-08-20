/* The parallel search: the worker that sweeps the key space, the climb drivers
   it calls per key, and bruteforce(), which owns the whole run.

   THE WORK SPACE IS keys x restarts, NOT keys, and RESTART IS THE OUTER
   DIMENSION -- every key at restart 0, then every key at restart 1. That
   ordering costs under 1% (the per-key setup is re-done rather than shared)
   and buys something the cheaper ordering cannot: there is no early exit, so
   it does not shorten a run, but it FRONT-LOADS the probability of finding the
   answer. On the measured climb curve, a watcher who kills a sweep at the
   quarter mark has 40% of the finds rather than 22%. It is also what lets a
   fully-specified rotor key use every thread, which the key-only scheme could
   not.

   Determinism is a contract, not an accident: each restart draws from its own
   (key, restart) RNG stream rather than one stream advanced sequentially, and
   the best-merge tie-breaks on the lower work index. The answer is identical
   at any -T. */

#ifndef ENIGMA_SEARCH_H
#define ENIGMA_SEARCH_H

#include "common.h"
#include "machine.h"

#include <stddef.h>
#include <stdint.h>

/* One sampled key and where it came from. The -F pre-filter and the
   --confidence sampler both keep a top-N min-heap of these; keep_worse
   orders it, with a deterministic tie-break so the shortlist does not
   depend on which thread scored which key. */
struct scored_key { double score; size_t idx; };

/* A min-heap that keeps the top-N keys: top() is the eviction candidate -- the
   lowest score, ties broken by the largest idx (so equal scores keep the lower idx,
   which makes the kept set deterministic and -T-independent). */
struct keep_worse
{
  bool operator()(const scored_key & a, const scored_key & b) const
  {
    if (a.score != b.score)
      return a.score > b.score;   /* top() = smallest score */
    return a.idx < b.idx;         /* tie: top() = largest idx */
  }
};

/* Recover the plugboard for one key: -R restarts of the staged climb, or one
   deterministic climb from the seed at -R 0. */
double hillclimb_restarts(machine & m, size_t key_index);

/* One work item: the climb, or whatever the crib / self-crib / exhaust options
   replaced it with. Shared with the --confidence calibration so the null is
   drawn from the same distribution the search reports from. */
double climb_unit(machine & m, size_t key_index, int restart);

/* Sweep the resolved key space and write the winning plaintext to `result`.
   Returns its score. */
double bruteforce(char * result, bool allow_empty);

/* --crib-list: one complete rotor sweep per crib, keeping the best board
   across all of them. Crib-outer, because the shared setup a rotor-outer loop
   would save is 0.6% of a run while the early exit is worth up to 50x. */
void run_crib_list(char * result);

/* The parsed --true-key, a diagnostic that ranks one specific key. Set by
   the option parser, read by the -F tier-1 filter. */
extern int g_tk_u, g_tk_w[3], g_tk_r[3], g_tk_g[3];

/* Diagnostics the final report reads. */
extern size_t g_table_count;
extern size_t g_table_bytes;
extern size_t g_keys_analysed;
extern uint64_t g_plugboards_scored;

#endif
