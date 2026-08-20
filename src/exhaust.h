/* --exhaust E: force E extra plug pairs among the free letters, try every such
   set, and climb from each.

   MEASURED AND DOMINATED -- at matched score_iter a high-restart greedy climb
   beats it by 10-40pp of exact recovery. It survives as an exploration tool,
   and because the machinery is what the crib hybrid's pin set reuses.

   Parallel over the FIRST forced pair, which bounds the unit count at
   C(free,2) <= 325 and lets each worker own its own pin set. */

#ifndef ENIGMA_EXHAUST_H
#define ENIGMA_EXHAUST_H

#include "common.h"
#include "machine.h"

#include <stddef.h>

/* Distinct sets of `pairs` disjoint plug pairs among `free_letters`. Used by
   the pigeonhole warning that fires when -R asks for more restarts than there
   are distinct kicks. */
double disjoint_pair_combinations(int free_letters, int pairs);

/* Enumerate the first forced pairs; each becomes one unit of work. */
void build_exhaust_firsts();
size_t exhaust_unit_count();

/* One unit: sub-exhaust the remaining pairs under this first pair, climbing
   each combination, and return the best score. */
double exhaust_unit(machine & m, size_t key_index, size_t fi);

/* Every combination for one key, used when the sweep is not the unit. */
double exhaust_all_combos(machine & m, size_t key_index);

#endif
