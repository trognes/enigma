/* Loading the n-gram statistics tables, and the Unicode folding that lets the
   26-letter machine use the accented records in the non-English tables.

   This module owns the READING and QUANTISATION only -- it never names mono8 /
   bi8 / tri8 / quad8 / all8. The caller passes the destination array, so the
   tables the hot scorers read stay defined next to the scorers, and nothing
   here is on any hot path: every function runs once per process, during
   start-up. */

#ifndef ENIGMA_NGRAMS_H
#define ENIGMA_NGRAMS_H

#include <stdint.h>

/* Fold one Unicode code point to an A-Z letter index (0..25), or -1 if it is
   not a foldable Latin letter. Shared with the ciphertext/plaintext readers,
   which fold their input the same way the tables are folded. */
int fold_codepoint(unsigned cp);

/* Read "<datadir>/<language>_<suffix>.txt" into `itable` -- the flat backing
   store of the caller's uint8 array -- and report the per-table quantisation
   bias and scale. `force_ll` (with `force_sym`) selects the log-linear
   all-order mixture that -a / -f use; the default reads a single order. */
void ngrams_read(int n, uint8_t * itable, double * bias_out, double * scale_out,
                 const char * datadir, const char * language,
                 const char * suffix,
                 const double * force_ll = nullptr, bool force_sym = false);

#endif
