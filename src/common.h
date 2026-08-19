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

void fatal(const char * message);

inline int char2num(char x)
{
  return x - 'A';
}

inline char num2char(int x)
{
  return static_cast<char>('A' + x);
}

#endif
