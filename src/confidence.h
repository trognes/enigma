/* --confidence N: is the winner better than chance?

   A raw score answers nothing on its own. Each model has a distribution on
   text with no signal, and a search reports the MAXIMUM over the keys it
   analysed, which drifts upward as the keyspace grows. This samples N keys
   from the resolved key space before the sweep, scores each exactly as the
   search will, and turns every reported score into a MARGIN: (s - mu)/sd
   minus sqrt(2 ln K), the distance above the null less what the best of K keys
   reaches by chance. Zero is the meaningful line.

   THREE THINGS THE SAMPLING MUST GET RIGHT, each learned from a bug:

     Samples are CLIMBED when -c is on. A climbed key is drawn from a much
     higher distribution than a scanned one, so calibrating a climbed search
     against a scanned null makes every run look significant.

     They are climbed by the SAME UNIT the search runs -- crib and self-crib
     units score somewhere else entirely -- so both go through one helper.

     A key the unit REJECTS is dropped, not sampled. unit_no_score is a
     sentinel, not a score; feeding it to the null overflowed the variance to
     +inf and made every margin identical. */

#ifndef ENIGMA_CONFIDENCE_H
#define ENIGMA_CONFIDENCE_H

#include "common.h"
#include "keyspace.h"
#include "machine.h"

#include <stddef.h>
#include <vector>

/* Build the null before the sweep. K is the TOTAL key count, which keeps the
   margin a constant offset from the score -- monotone, so the merge order and
   the display high-water mark are untouched, and the printed number does not
   depend on thread timing. */
void calibrate_null(machine & m, size_t keys,
                    const std::vector<wheel_task> & tasks,
                    const search_range & range,
                    const int * rc, const int * gc, subst_table all,
                    size_t rg, size_t gsize, size_t rc12, size_t gc12,
                    size_t total_keys);

/* The summary line after the search: mu, sd, the raw sigma count and a
   p-value, with the caveat that the tail is optimistic near zero for every
   model. No line of it may begin with a signed number -- the documented way to
   read a margin off stderr is a grep for exactly that. */
void report_confidence(double best_score);

#endif
