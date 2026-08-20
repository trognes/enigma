/* The --ring-stride refinement: recover the ring2 values the coarse pass never
   tested.

   --ring-stride K samples ring2 in {0, K, 2K, ...}, which is an APPROXIMATION.
   Unlike the three exact collapses in keyspace.h, the right wheel's notch does
   gate further stepping, so a skipped ring2 is a key that was never scored.
   This module is the other half of the bargain: once the coarse pass has a
   winner, every skipped ring2 is re-checked under that winner's wheel order,
   reflector and ring0/start0, and the best board is kept only if it wins on
   score.

   WHAT MAKES IT CHEAP IS THAT THE REST OF THE KEY IS DERIVED, NOT SEARCHED.
   The substitution consumes offsets (start_w - ring_w) and the wheels'
   cumulative step counts; the right wheel's has no schedule term, so start2
   follows from the coarse winner's offset2, and the middle and left wheels'
   offsets follow from the step-count DIFFERENCE between the two schedules --
   computable from the two keys alone, with no knowledge of the truth. That took
   the candidate set from 25 x 130 x 26 = 84 500 to 25 x the start1 range, and
   turned a keyspace where the flag used to LOSE (-r A.. -g A.. at K=2: 162 032
   keys against 110 864 unstrided) into a 1.46x win. archived/refinement.md has
   the algebra; archived/PERFORMANCE.md 7.11 has the measurements.

   THE CAVEATS ARE THE CODE. Four bugs shipped here before the current shape,
   each invisible to the obvious test:

     - Pinning ring1/start1 to the coarse winner. They were optimal only for
       the winner's own corrupted ring2 row.
     - Clamping the ring2 window at the 0/25 boundary instead of wrapping.
       ring2 is circular; a winner at A with the truth at Z was never checked.
     - Re-reading m.ringstellung[0] between sub-searches. The plain-scan path
       leaves those fields stepped (a documented lazy restore), so the second
       search pinned whatever the first one's scan last touched.
     - Rebuilding a wheel_task from the machine. wheel_task carries RAW rotor
       numbers and init_walzen() translates on the way in, so Norway mode got
       translated twice and searched the wrong rotors -- invisible in standard
       and M4 mode, where raw == translated.

   Rejected with -F and --exhaust, which encode best.idx differently. */

#ifndef ENIGMA_REFINE_H
#define ENIGMA_REFINE_H

#include "common.h"
#include "keyspace.h"
#include "machine.h"
#include "result.h"

#include <stddef.h>
#include <vector>

/* Re-check every ring2 the coarse pass skipped and merge any improvement into
   `best`. Returns the number of keys it scored, for the run's key-count
   diagnostic.

   `machines` are the worker machines, with machines[0] ALREADY reconstructed to
   the coarse winner's full state -- rotor key, board and all -- by the caller;
   this reads that state rather than re-deriving it, which is what keeps the one
   best.idx reconstruction in the caller where --polish can share it. `cur_wo`
   is the winning task's index, so tasks[cur_wo] can be reused verbatim (see the
   Norway note above). `range`, `rc` and `gc` describe the ORIGINAL search,
   whose ring1/start1 bounds the refinement re-opens rather than pins. */
size_t refine_ring_stride(std::vector<machine *> & machines,
                          const std::vector<wheel_task> & tasks,
                          size_t cur_wo,
                          const search_range & range,
                          const int * rc, const int * gc,
                          size_t restarts_par,
                          best_result & best);

#endif
