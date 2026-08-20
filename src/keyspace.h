/* WHICH KEYS THE SEARCH VISITS, and how a flat work index decodes back into a
   machine.

   Most of this module is about NOT enumerating keys that provably decode
   identically. Three collapses are always on when their positions are
   wildcarded, and each is exact rather than approximate:

     wheel 0 (left)    ring x start collapses TOTALLY -- nothing downstream
                       reads that wheel's absolute position, only the offset.
                       26x, unconditional.
     wheel 1 (middle)  PARTIALLY: shifting ring1 and start1 together changes
                       only the notch timing, and a short message never
                       reaches most notch positions. 3-5x at short lengths,
                       derived by SIMULATING the stepping rather than from a
                       formula -- which is why it picks up two-notch wheels
                       for free.
     wheel 2 (right)   a shift of 13 is exact for VI/VII/VIII, whose notches
                       sit at M and Z. 2x per affected wheel.

   THE FAILURE MODE OF GETTING A COLLAPSE WRONG IS SILENT KEY LOSS -- the
   search simply never visits the true key and reports a confident wrong
   answer. tests/run_tests.sh therefore asserts the equivalences by ENCRYPTION
   (shifting must give an identical ciphertext for a two-notch wheel and a
   different one otherwise) before it ever checks recovery.

   The reported ring/start may be a class representative rather than the true
   key: class members are indistinguishable from ciphertext alone. That is the
   documented contract for wheel 0 (always reported as ring A), the middle
   wheel, and the M4 Greek wheel. */

#ifndef ENIGMA_KEYSPACE_H
#define ENIGMA_KEYSPACE_H

#include "common.h"
#include "machine.h"

#include <stddef.h>
#include <stdint.h>
#include <vector>

/* The full key space, before any of the collapses above:
   uwwwrrrggg = 3*8*7*6*26*26*26*26*26*26 = 311 387 102 208
   (reflector x wheel order x ring x start, wheels I-VIII). */

/* The reflector x wheel-order combinations are the unit of parallelism: each is
   independent (its own precompute + ring/start sweep). The ring/start ranges are
   identical for every task. */
struct wheel_task
{
  int u;
  int w[wheels];
  int greek;        /* M4 Greek rotor index, else -1 */
  int greek_off;    /* M4 (Greek start - ring) mod 26, else 0 */
};
struct search_range
{
  int r_min[wheels], r_max[wheels];
  int g_min[wheels], g_max[wheels];
  /* Ring position 2 (the rightmost wheel) is the one dimension that can be a
     NON-CONTIGUOUS set: --ring-stride's coarse pass samples {0, K, 2K, ...} and its
     refinement tests a wrapped window around the coarse winner with that winner
     removed. Both are expressed as an explicit ascending value list, so the decode is
     a plain lookup (r2_vals[i]) instead of arithmetic that has to know about strides.
     r_min[2]/r_max[2] still describe the caller's requested BOUNDS (build_key_space
     derives the list from them); everything that decodes a key reads the list, never
     the bounds. r2_n always equals rc[2]. Filled via set_ring2() below.

     unsigned char, not int, and this is load-bearing: search_worker() reads this
     struct in its per-key decode, so growing it pushes that decode across more cache
     lines. An int[26] list measured a REAL ~5% search regression under g++ -- against
     a base-vs-base noise floor of only 0.5% on that benchmark, so well outside the
     noise (the hillclimb tier's own floor is ~4.5%, which is why its numbers looked
     scattered and meant nothing). A byte holds 0..25 fine and keeps the struct near
     its original footprint. See archived/PERFORMANCE.md §7.11. */
  unsigned char r2_vals[asize];
  /* --tune-phase spaces ring1's starting phases this far apart; 1 otherwise.
     Deliberately here: r2_vals/r2_n leave alignment padding, so this costs the
     struct nothing -- which matters because search_worker() reads it in the
     per-key decode (see the r2_vals note above). */
  unsigned char r_phase_step;
  int r2_n;
};
/* Resolve the search ranges from the options, enumerate the reflector x
   wheel-order tasks, precompute their rotor tables in parallel, then sweep the
   flat (wheel-order x ring x start) key space in parallel. The best decryption
   is written to 'result'. */
/* All the derived search dimensions for one run, built from the opt_* CLI state: the
   task list (reflector x Greek wheel x Greek offset x wheel order) and the ring/start
   ranges and counts. Bundled so bruteforce() reads as phases rather than one long
   setup block. */
struct key_space
{
  std::vector<wheel_task> tasks;
  search_range range;
  int rc[wheels], gc[wheels];   /* per-position ring / start counts */
  size_t rsize, gsize;          /* ring-combo and start-combo counts */
  size_t total_keys;            /* tasks.size() * rsize * gsize -- the INDEX space */
  size_t scored_keys;           /* keys scored (< total_keys if collapsed) */
  bool r2_halved = false;       /* right-wheel collapse fired on some task */
  bool mid_collapsed = false;   /* §7.12 dropped a start1 on some task */
};

/* Build the key space from the resolved options: the reflector x wheel-order
   task list, the ring/start ranges, and the collapses above. */
key_space build_key_space();

/* Which ring2 values the sweep tests (--ring-stride samples a sparse set, and
   its refinement a wrapped window), as an explicit value list rather than a
   range -- it is the one ring position that can be non-contiguous. */
void set_ring2(search_range & r, unsigned int mask);

/* The rotor key behind a work index. The work space is keys x restarts with
   RESTART as the outer dimension, so the key is idx % keys -- both sites that
   reconstruct a winning key go through work_key() so they cannot diverge. */
size_t work_key(size_t idx, size_t keys);
bool key_to_machine(machine & m, size_t idx,
                    const std::vector<wheel_task> & tasks,
                    const search_range & range,
                    const int * rc, const int * gc,
                    subst_table all, size_t rg, size_t gsize,
                    size_t rc12, size_t gc12, size_t & cur_wo, int rg6[6]);

/* True when the right wheel's ring2 >= 13 half is being skipped: the two-notch
   collapse. search_worker() consults it per key, so it is a plain global
   rather than a call. */
extern bool g_r2_halve;

/* The middle-wheel representative mask, or null when the collapse is inert.
   Bit s of row (w1, w2, start2) says "start1 = s is its class's canonical
   member"; search_worker() tests it per key. */
extern const uint32_t * g_mid_rep_mask;

/* One 457 KB rotor-stack table per wheel order, allocated together. */
subst_table allocate_subst_tables(size_t nwo);

#endif
