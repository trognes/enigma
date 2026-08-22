#include "scoring.h"

#include "common.h"
#include "machine.h"
#include "ngrams.h"
#include "options.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>

/* The n-gram scorers read uint8 fixed-point log10-probability tables. This is a
   cache-residency optimisation aimed at the quad table -- by far the largest (26^4
   entries): uint8 shrinks it to 0.45 MB (vs 1.8 MB float / 0.9 MB int16), so it stays
   in a faster cache level during the brute-force scan, where every key decodes a fresh
   message and hits cold table cells. mono/bi/tri are tiny and already cache-resident;
   they use the same representation for consistency (and the int sum is exact and
   order-independent, a small determinism nicety).

   Each entry is q = round((v - bias) * scale) with v = log10(count/total) and a per-table
   bias = the table's minimum v (its floor, log10(1/total), since an unseen gram is scored
   as a hapax -- see ngrams_read). Both the bias AND the scale are now **per-table adaptive**:
   scale = 255 / (vmax - vmin) maps the rarest gram to byte 0 and the most-common to 255, so
   *every* table spends the full 0..255 range regardless of its span (previously a fixed
   scale=32 left the narrow tables short -- danish quad reached only byte 172). The scorers
   sum uint8 into a long, then recover the true log-prob sum as isum/scale + n*bias (n = terms),
   using the same per-table scale. The affine (bias, scale) is invisible to ranking and to SA's
   acceptance calibration; the reconstruction only keeps the reported score a faithful
   cross-entropy -- and the finer per-table step (up to ~1.8x more resolution on the narrow
   tables) trims quantisation error on borderline rankings. The map MUST stay linear (an affine
   image of log10 p) so the additive sum reconstructs; adaptive *scale* is the only free lever,
   not a nonlinear curve. Raw counts live in a transient scratch buffer inside ngrams_read(). */
double ngram_scale[SCORE_MONOIC + 1];   /* per-model: 255/(vmax-vmin), full 0..255 range */
double ngram_bias[SCORE_MONOIC + 1];    /* per-model vmin; indexed by SCORE_* */
static uint8_t mono8[asize];
static uint8_t bi8[asize][asize];
static uint8_t tri8[asize][asize][asize];
uint8_t quad8[asize][asize][asize][asize];
/* SCORE_ALL ("weighted", -a): a quad-shaped table holding the log-linear symmetric
   mixture of all four orders (see load_table); the scorer/gainfix treat it like quad8. */
uint8_t all8[asize][asize][asize][asize];

/* --- plaintext scoring models ------------------------------------------- */

/* The four n-gram scorers fuse decoding into the score loop: each character is
   decoded once, on the fly, into a small sliding window of the last n decoded
   letters that indexes the n-gram table -- so the decoded message is never
   written to and re-read from a scratch array. The quadgram scorer is ~99% of
   hill-climb runtime. The short-text guards (textlength < n) keep the n-1
   pre-roll decodes in bounds and reproduce the old `i < textlength-(n-1)` loops
   (which simply ran zero times for shorter input). */

static double quadgram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  if (textlength < 4)
    return score;

  int a = decode_at(steck, rows, ct, 0);
  int b = decode_at(steck, rows, ct, 1);
  int c = decode_at(steck, rows, ct, 2);
  long isum = 0;   /* sum uint8 fixed-point (exact, order-independent) */
  for (int i = 3; i < textlength; i++)
    {
      int d = decode_at(steck, rows, ct, i);
      isum += quad8[a][b][c][d];
      a = b;
      b = c;
      c = d;
    }
  /* recover the log-prob sum: q = (v - bias)*scale, so v = q/scale + bias per term */
  score = static_cast<double>(isum) / ngram_scale[SCORE_QUAD] + (textlength - 3) * ngram_bias[SCORE_QUAD];
  return score;
}

/* The weighted "all-order" scorer: identical shape to the quad scorer, but reads all8 (the
   log-linear mixture table) and its own bias/scale. A separate function (not a parameterised
   quad scorer) so each stays a distinct global with no aliasing -- the hot-path rule. */
static double allgram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  if (textlength < 4)
    return score;

  int a = decode_at(steck, rows, ct, 0);
  int b = decode_at(steck, rows, ct, 1);
  int c = decode_at(steck, rows, ct, 2);
  long isum = 0;
  for (int i = 3; i < textlength; i++)
    {
      int d = decode_at(steck, rows, ct, i);
      isum += all8[a][b][c][d];
      a = b;
      b = c;
      c = d;
    }
  score = static_cast<double>(isum) / ngram_scale[SCORE_ALL] + (textlength - 3) * ngram_bias[SCORE_ALL];
  return score;
}

static double trigram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  if (textlength < 3)
    return score;

  int a = decode_at(steck, rows, ct, 0);
  int b = decode_at(steck, rows, ct, 1);
  long isum = 0;
  for (int i = 2; i < textlength; i++)
    {
      int c = decode_at(steck, rows, ct, i);
      isum += tri8[a][b][c];
      a = b;
      b = c;
    }
  score = static_cast<double>(isum) / ngram_scale[SCORE_TRI] + (textlength - 2) * ngram_bias[SCORE_TRI];
  return score;
}

static double bigram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  double score = 0.0;
  if (textlength < 2)
    return score;

  int a = decode_at(steck, rows, ct, 0);
  long isum = 0;
  for (int i = 1; i < textlength; i++)
    {
      int b = decode_at(steck, rows, ct, i);
      isum += bi8[a][b];
      a = b;
    }
  score = static_cast<double>(isum) / ngram_scale[SCORE_BI] + (textlength - 1) * ngram_bias[SCORE_BI];
  return score;
}

static double monogram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  long isum = 0;
  for (int i = 0; i < textlength; i++)
    isum += mono8[decode_at(steck, rows, ct, i)];
  return static_cast<double>(isum) / ngram_scale[SCORE_MONO] + textlength * ngram_bias[SCORE_MONO];
}

/* -S k's IC weight, PER LETTER: the term added is lambda * L * IC.  See the
   decoder below for why it scales with length where -f's does not.
   ENIGMA_MONOIC_BLEND overrides it, as ENIGMA_IC_BLEND does for -f. */
static const double monoic_lambda_default = 0.1;
static double g_monoic_lambda = monoic_lambda_default;

/* -S k (SCORE_MONOIC): the monogram score fused with the index of coincidence.
   BOTH halves are functions of the same 26-bin histogram -- the monogram score
   is sum n_x * log p(x) and IC is sum n_x(n_x-1) / N(N-1) -- so one decode pass
   yields both and the fusion costs 26 multiply-adds, not a second gather. That
   is what makes this cheaper than -f's fusion, which had to accumulate IC
   alongside a gather-bound quad loop.

   lambda is PROPORTIONAL TO LENGTH, unlike -f's baked constant. IC's spread
   falls as ~1/L (a rate over C(L,2) pairs) while the per-symbol monogram
   score's falls as ~1/sqrt(L) (a mean of L terms), so the weight that balances
   them grows with L; measured, the optimum tracks 0.1*L across L = 40..167
   while a fixed lambda drifts away from it, worst at operational length
   (eval/results-mono-ic-blend.txt). ENIGMA_MONOIC_BLEND overrides the 0.1. */
static double monoic_score_decode(machine & m)
{
  int freq[asize];
  for (int j = 0; j < asize; j++)
    freq[j] = 0;

  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  for (int i = 0; i < textlength; i++)
    freq[decode_at(steck, rows, ct, i)]++;

  long isum = 0;
  int coin = 0;
  for (int j = 0; j < asize; j++)
    {
      isum += static_cast<long>(freq[j]) * mono8[j];
      coin += freq[j] * (freq[j] - 1);
    }
  const double mono = static_cast<double>(isum) / ngram_scale[SCORE_MONO]
                      + textlength * ngram_bias[SCORE_MONO];
  const double ic = (textlength > 1)
    ? static_cast<double>(coin)
        / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;
  /* mono per symbol, matching how -f normalises before adding its IC term. */
  return mono / (textlength > 0 ? textlength : 1)
         + g_monoic_lambda * textlength * ic;
}

double ic_score_decode(machine & m)
{
  int freq[asize];
  for(int j=0; j<asize; j++)
    freq[j] = 0;

  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  for (int i = 0; i < textlength; i++)
    freq[decode_at(steck, rows, ct, i)]++;

  /* Accumulate the coincidence count in an INT, not a double. Every freq[]
     entry is at most textlength <= maxlen, so each product fits an int and the
     whole sum fits one too (asize * maxlen^2, asserted below) -- so the old
     double form was computing exact small integers in floating point, paying
     26 int->double conversions per scoring for it. Byte-identical: the sum is
     exact either way, so only the final division sees a double.
       This runs ONCE PER SCORING, i.e. millions of times in a climb. It
     profiled at 10.5% of a `-f -c` run (7.63% on the accumulate, 2.90% on the
     loop); the integer form is worth -8.1% of ngram_ic_decode's instructions
     and -4..6% wall on `-f -c`, -5% on an `-i` scan, and nothing at all on
     -a/-q, which compute no IC. */
  static_assert(static_cast<long>(asize) * maxlen * maxlen < 2147483647L,
                "IC coincidence sum must fit an int");
  int coin = 0;
  for (int j = 0; j < asize; j++)
    coin += freq[j] * (freq[j] - 1);
  return (textlength > 1)
    ? static_cast<double>(coin)
        / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;
}


/* ENIGMA_IC_BLEND probe (archived/PERFORMANCE.md 6.4): fuse the index of coincidence into the
   target score as `per-symbol ngram + lambda*IC` instead of STAGING IC then quad. The
   premise is that the quad/weighted surface is nearly flat with only a plug or two set,
   while IC still has gradient there -- and IC is permutation-INVARIANT, so unlike a
   monogram/chi-squared term the plugboard cannot game it (see the tier-1 chi-squared
   rejection, archived section 9 item 2). Off by default; 0 disables. */
/* -f (SCORE_FUSED) weight on the index of coincidence, added to the weighted
   all-order score. Baked like -a's order weights rather than exposed as a knob:
   the optimum is a broad plateau (lambda 20/30/40 measured +3.6/+4.4/+3.8pp and
   statistically indistinguishable from each other), so there is nothing for a user
   to tune. ENIGMA_IC_BLEND overrides it for experiments. (It does NOT mirror
   ENIGMA_LOGLIN, which an earlier version of this comment claimed: load_table()
   passes -a's weights as force_ll and that branch ignores the environment, so
   ENIGMA_LOGLIN reshapes the plain QUAD table instead.) archived/PERFORMANCE.md
   6.4. */
static const double fused_lambda_default = 30.0;
static double g_fused_lambda = fused_lambda_default;

void ic_blend_init()
{
  /* Empty means unset, as for the other value-carrying overrides. */
  const char * s = getenv("ENIGMA_IC_BLEND");
  if ((s != nullptr) && (*s != 0))
    g_fused_lambda = parse_opt_double(s, "$ENIGMA_IC_BLEND");
  const char * k = getenv("ENIGMA_MONOIC_BLEND");
  if ((k != nullptr) && (*k != 0))
    g_monoic_lambda = parse_opt_double(k, "$ENIGMA_MONOIC_BLEND");
}


/* Quad/weighted score AND the letter histogram in ONE pass, so the probe costs the
   same number of decodes as the shipped scorer. A two-pass version would inflate wall
   time per score_iter and quietly unfair any matched-score_iter A/B. Returns the
   log-prob SUM (caller normalises); writes the IC through *ic_out. */
static double ngram_ic_decode(machine & m, const uint8_t (* table)[asize][asize][asize],
                              int model, double * ic_out)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  int freq[asize];
  for (int j = 0; j < asize; j++)
    freq[j] = 0;

  *ic_out = 0.0;
  if (textlength < 4)
    return 0.0;

  int a = decode_at(steck, rows, ct, 0);
  int b = decode_at(steck, rows, ct, 1);
  int c = decode_at(steck, rows, ct, 2);
  freq[a]++; freq[b]++; freq[c]++;
  long isum = 0;
  for (int i = 3; i < textlength; i++)
    {
      int d = decode_at(steck, rows, ct, i);
      freq[d]++;
      isum += table[a][b][c][d];
      a = b;
      b = c;
      c = d;
    }

  int coin = 0;                      /* see ic_score_decode: int, not double */
  for (int j = 0; j < asize; j++)
    coin += freq[j] * (freq[j] - 1);
  *ic_out = (textlength > 1)
    ? static_cast<double>(coin)
        / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;

  return static_cast<double>(isum) / ngram_scale[model]
         + (textlength - 3) * ngram_bias[model];
}


double score_iter(machine & m)
{
  m.plugboards_scored++;   /* diagnostic count (once per whole-message score) */

  double score = 0;
  int nterms = 0;   /* number of n-gram terms; 0 = no per-symbol normalisation (IC) */

  /* -f: the weighted all-order score fused with the index of coincidence. The IC term
     is added AFTER per-symbol normalisation, so lambda weighs it against a per-symbol
     cross-entropy rather than a length-scaled sum. IC cannot be folded into the n-gram
     table like -a's four orders are: those are additive over positions, whereas IC is
     quadratic in the whole-message letter histogram. */
  if (m.scoring == SCORE_FUSED)
    {
      double ic = 0.0;
      score = ngram_ic_decode(m, all8, SCORE_ALL, &ic);
      nterms = textlength - 3;
      if (nterms > 0)
        score /= nterms;
      return score + g_fused_lambda * ic;
    }

  switch(m.scoring)
    {
    case SCORE_IC:
      score = ic_score_decode(m);   /* already a normalised ratio; left as-is */
      break;

    case SCORE_MONOIC:
      score = monoic_score_decode(m);   /* already per-symbol; left as-is */
      break;

    case SCORE_MONO:
      score = monogram_score_decode(m);
      nterms = textlength;
      break;

    case SCORE_BI:
      score = bigram_score_decode(m);
      nterms = textlength - 1;
      break;

    case SCORE_TRI:
      score = trigram_score_decode(m);
      nterms = textlength - 2;
      break;

    case SCORE_QUAD:
      score = quadgram_score_decode(m);
      nterms = textlength - 3;
      break;

    case SCORE_ALL:
      score = allgram_score_decode(m);
      nterms = textlength - 3;
      break;

    default:
      fatal("Illegal scoring type");
    }

  /* Per-symbol average turns the summed log-probability into a cross-entropy
     (dits/char): length-independent and comparable across models. It is a constant
     factor within a run (nterms is fixed), so it does not change which key/plugboard
     ranks highest -- only the scale of the reported score. */
  if (nterms > 0)
    score /= nterms;

  return score;
}

/* Load the n-gram table backing a scoring model (IC needs none). */
void load_table(int model)
{
  switch (model)
    {
    case SCORE_MONO:
    case SCORE_MONOIC:   /* -S k reuses mono8; only the IC term differs at score time */
      ngrams_read(1, mono8, & ngram_bias[SCORE_MONO], & ngram_scale[SCORE_MONO],
                  opt_datadir, opt_language, "monograms");
      break;
    case SCORE_BI:
      ngrams_read(2, & bi8[0][0], & ngram_bias[SCORE_BI], & ngram_scale[SCORE_BI],
                  opt_datadir, opt_language, "bigrams");
      break;
    case SCORE_TRI:
      ngrams_read(3, & tri8[0][0][0], & ngram_bias[SCORE_TRI],
                  & ngram_scale[SCORE_TRI],
                  opt_datadir, opt_language, "trigrams");
      break;
    case SCORE_QUAD:
      ngrams_read(4, & quad8[0][0][0][0], & ngram_bias[SCORE_QUAD],
                  & ngram_scale[SCORE_QUAD],
                  opt_datadir, opt_language, "quadgrams");
      break;
    case SCORE_ALL:
    case SCORE_FUSED:   /* -f reuses all8; only the IC term differs at score time */
      {
        /* the weighted all-order model: log-linear symmetric mixture of quad/tri/bi/mono,
           weights tuned across four languages (PR #106): quad 1, tri .6, bi .3, mono .15. */
        static const double AW[4] = { 1.0, 0.6, 0.3, 0.15 };
        ngrams_read(4, & all8[0][0][0][0], & ngram_bias[SCORE_ALL],
                    & ngram_scale[SCORE_ALL],
                    opt_datadir, opt_language, "quadgrams", AW, true);
        break;
      }
    default: break;   /* IC: no table */
    }
}

/* Map a scoring-model letter (i/m/b/t/q/a/f/k) to its SCORE_* value. */
int model_of(char c)
{
  switch (c)
    {
    case 'i': return SCORE_IC;
    case 'm': return SCORE_MONO;
    case 'b': return SCORE_BI;
    case 't': return SCORE_TRI;
    case 'q': return SCORE_QUAD;
    case 'a': return SCORE_ALL;
    case 'f': return SCORE_FUSED;
    case 'k': return SCORE_MONOIC;
    default:  return SCORE_IC;
    }
}

/* Record a bare model selector (-i/-m/-b/-t/-q/-a/-f) as a single uncapped --score <model>
   stage (REDESIGN Part C). Two selectors that disagree (e.g. -m -q) make the intended
   model ambiguous, so reject them; repeats that agree (-q -q) are fine. Sets opt_scoring
   so a run with no --score ranks by the selected model. */
void select_model(int model)
{
  if ((opt_model_selector != -1) && (opt_model_selector != model))
    fatal("Conflicting scoring models: the -i/-m/-b/-t/-q/-a/-f selectors disagree; "
          "pick one");
  opt_model_selector = model;
  opt_scoring = model;
}
