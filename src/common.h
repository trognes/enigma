/* Constants and helpers shared by every module: the alphabet/machine sizes, the
   scoring-model enumeration, and the two-line A-Z conversions the whole program
   is written in terms of.

   The sizes are `static const` in a header, so each translation unit gets its
   own copy. That is deliberate rather than an oversight: asize, wheels, maxlen
   and rotor_count are used as ARRAY BOUNDS (struct machine, the wiring tables,
   the per-key scratch), which needs a compile-time constant in every unit that
   sees them -- an `extern const int` would not do. */

#ifndef ENIGMA_COMMON_H
#define ENIGMA_COMMON_H

static const int maxlen = 1024;   /* maximum ciphertext length (letters) */
static const int asize = 26;
static const int wheels = 3;

/* A score lower than any achievable plaintext score. IC scores in [0, ~0.08];
   the n-gram models now score a per-symbol log10-probability (cross-entropy),
   which is <= 0 but bounded well above -1e30 by the unseen-gram floor. */
static const double score_min = -1e30;

/* Plaintext scoring models; values match the scoring_name[] order and the
   *_score_decode dispatch in score_iter(). */
enum scoring { SCORE_IC, SCORE_MONO, SCORE_BI, SCORE_TRI, SCORE_QUAD, SCORE_ALL,
               SCORE_FUSED };

/* The score a work unit reports when it produced NO candidate at all -- every crib
   hypothesis contradicted, or no rotor setting could have produced the crib. It is a
   sentinel, not a score: it exists only so such a unit never wins the merge, and it must
   be EXCLUDED wherever scores are treated as a distribution. calibrate_null() learned
   that the hard way; see the filter there. */
const double unit_no_score = -1e300;

/* [[noreturn]] is load-bearing, not decoration. fatal() ends in exit(1), and
   while it lived in the same file as its callers both the compiler and the
   static analysers could see that for themselves. Across a translation-unit
   boundary they cannot, so `if (f == nullptr) fatal(...);` followed by
   fgets(..., f) reads as a null-pointer dereference -- which is exactly what
   cppcheck (nullPointerRedundantCheck) and clang-tidy
   (clang-analyzer-core.NonNullParamChecker) reported the moment this module
   was split out. Declaring the contract restores what the analysers had been
   deducing, at every one of the ~90 fatal() call sites at once. */
[[noreturn]] void fatal(const char * message);

inline int char2num(char x)
{
  return x - 'A';
}

inline char num2char(int x)
{
  return static_cast<char>('A' + x);
}


#include <stdint.h>

/* splitmix64: a tiny, well-distributed deterministic PRNG. Seeded per key (not
   from the clock or thread id) so a random-restart search stays reproducible and
   independent of the thread count. */
inline uint64_t splitmix64(uint64_t * s)
{
  uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

#endif
