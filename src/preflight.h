/* "Is this ciphertext even Enigma?" -- a free sanity check run once, before the
   sweep, on a wildcarded key.

   Enigma is a permutation cipher, so its output is near-flat. A ciphertext that
   still carries language structure was not produced by one and has no key to
   find. Two statistics settle it -- the index of coincidence, and how many of
   A-Z never occur -- both compared against a LENGTH-DEPENDENT null with closed
   forms, so no tables are needed. See CLAUDE.md for the measured thresholds and
   why they come from the observed tail rather than a nominal p-value. */

#ifndef ENIGMA_PREFLIGHT_H
#define ENIGMA_PREFLIGHT_H

/* Prints the verdict to stderr. Self-gating: silent unless the run is a search
   (a wildcarded key), --no-preflight is off, and there is enough text. */
void report_preflight();

#endif
