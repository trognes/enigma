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

/* Force unrolling of the scorer decode loops.  Neither compiler unrolls
   ngram_ic_decode and g++ unrolls none of them (measured: see
   eval/results-scoreloop-insns.txt).  The aim is LATENCY, not instruction
   count -- instruction count is measured NOT to be the constraint here, while
   the four-deep dependent load chain ct -> steck -> rows -> steck is a
   candidate, and unrolling lets the scheduler overlap that chain across
   iterations.

   APPLIED ONLY TO THE PURE-GATHER LOOPS, and the exclusion is measured.  The
   scorers that also do freq[d]++ -- ic, monoic and ngram_ic (-f) -- REGRESS
   when unrolled: g++ fused long read +11.7% against a 1.4% base-vs-base floor,
   while the gather-only loops read -7 to -12% on search and hillclimb against
   the same floor.  The mechanism is the read-modify-write: unrolling schedules
   four freq[] increments close together, and four random letters out of 26
   collide about 30% of the time, so the store-to-load forwarding the rolled
   loop spaces out becomes a stall.  Unrolling improves the load dependency and
   worsens the store one; only loops without the store are net ahead. */
#if defined(__clang__)
#define SCORE_UNROLL _Pragma("clang loop unroll_count(4)")
#elif defined(__GNUC__)
#define SCORE_UNROLL _Pragma("GCC unroll 4")
#else
#define SCORE_UNROLL
#endif

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
  SCORE_UNROLL
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
  SCORE_UNROLL
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
  SCORE_UNROLL
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
  SCORE_UNROLL
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
  SCORE_UNROLL
  for (int i = 0; i < textlength; i++)
    isum += mono8[decode_at(steck, rows, ct, i)];
  return static_cast<double>(isum) / ngram_scale[SCORE_MONO] + textlength * ngram_bias[SCORE_MONO];
}

/* -S k's IC weight, PER LETTER: the term added is lambda * L * IC.  See the
   decoder below for why it scales with length where -f's does not, and for
   why the exponent is 1 even though the quantity it tracks goes as sqrt(L).
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
   (eval/results-mono-ic-blend.txt). ENIGMA_MONOIC_BLEND overrides the 0.1.

   THE EXPONENT IS NOMINALLY WRONG, AND FIXING IT WAS MEASURED NULL -- do not
   "correct" it again. The quantity lambda has to track is sd(mono)/sd(IC),
   measured 7.4 / 8.8 / 11.0 / 14.2 at L = 40 / 60 / 100 / 167, i.e. about
   1.1 * sqrt(L). A LINEAR rule crosses that curve exactly once, at L = 121,
   so 0.1*L undershoots below and overshoots above: in units of each
   statistic's own spread it gives r = 0.54 / 0.68 / 0.91 / 1.18 rather than a
   constant. lambda = 1.1*sqrt(L), which holds r = 0.94 / 0.97 / 1.00 / 1.00,
   was therefore built and A/B'd against this rule -- paired, both arms in one
   binary via ENIGMA_MONOIC_BLEND, n = 2000 at each of three lengths
   (eval/results-monoic-lambda.txt):

       L = 167   +0.84pp  [-0.47, +2.15]
       L = 100   -0.75pp  [-2.18, +0.68]
       L =  60   -0.93pp  [-1.90, +0.05]
       pooled    -0.40pp  [-1.09, +0.28]

   Every interval spans zero and the sign is mildly AGAINST the tidier rule,
   including at L = 60 where r moved furthest (0.68 -> 0.97). The blend surface
   is a broad plateau -- every r in 0.25..2.0 beats both pure statistics at
   L >= 60 -- so the exponent does not matter empirically. 0.1*L is kept
   because it is the configuration the five-seed end-to-end evidence describes,
   and because it happens to track the probe's measured PEAK r, which itself
   rises with length (0.5 / 1.0 / 1.0 / 1.0-2.0) -- at L = 40 the peak is 0.5
   and 0.1*L gives 0.54, where the sqrt rule would give 0.94. */
/* NOT INLINED, deliberately.  score_iter() is the scan's hottest function and
   clang inlines this decoder straight into it, growing it 623 -> 1042
   instructions -- so an experimental model that no default path calls would
   change the code layout every OTHER model runs through, which is exactly the
   clang/ARM sensitivity documented under "Struct layout matters for the hot
   loop".  Out of line, score_iter is instruction-identical to before. */
__attribute__((noinline))
static double monoic_score_decode(machine & m)
{
  int freq[asize];
  for (int j = 0; j < asize; j++)
    freq[j] = 0;

  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  /* NOT unrolled: see SCORE_UNROLL. */
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

/* IC_UNROLL_HIST -- unroll this decoder 4x with a private histogram per copy.
   ON FOR arm64 ONLY, and the split is measured rather than assumed.  Applying
   the ngram_ic_decode treatment here reads, on the icscan bench tier against a
   calibrated -0.7..+0.5% floor, with all four other tiers flat as controls:

     g++ arm64    -5.9% quick  -8.0% long      clang arm64   -4.8%  -7.0%
     g++ x86_64  +16.0% quick  +9.6% long      clang x86_64 +22.3% +11.9%

   Both compilers agree WITHIN each architecture, so it is an architecture
   split, not a compiler one.  The likely reason is that the 26-bin merge is a
   FIXED cost and this decoder's per-character body is tiny -- five loads and
   no table lookup -- where ngram_ic_decode amortises the same 26 iterations
   against four table gathers per character.  Same fixed cost, four times less
   to hide it behind.  That the sign flips by architecture fits unrolling
   standing in for out-of-order window size: the arm64 cores gain from four
   independent copies of the dependent load chain, the wider x86 cores were
   already extracting that and are left paying only the merge.  The split is
   measured; that explanation is inference.

   `__aarch64__` is an EMPIRICAL PROXY for "narrow enough reorder buffer that
   static unrolling pays", which no macro can express -- it is where this was
   measured, not a claim about every ARM or every x86 part.

   Defined as a value rather than tested with `#if defined(__aarch64__)` so
   BOTH paths compile everywhere: -DIC_UNROLL_HIST=1 on an x86 box builds the
   arm64 form, which is what lets the two be checked against each other
   without an arm64 machine.  Without that, the unrolled path would only ever
   be compiled by the arm64 CI cells. */
#ifndef IC_UNROLL_HIST
#if defined(__aarch64__)
#define IC_UNROLL_HIST 1
#else
#define IC_UNROLL_HIST 0
#endif
#endif

double ic_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  int freq[asize];
  for (int j = 0; j < asize; j++)
    freq[j] = 0;

#if IC_UNROLL_HIST
  /* FOUR HISTOGRAMS, ONE PER UNROLLED COPY.  SCORE_UNROLL alone makes a
     freq[]++ loop SLOWER: four increments scheduled together collide across 26
     bins about 30% of the time, so the store-to-load forwarding the rolled
     loop spaces out becomes a stall.  Private arrays cannot collide, so the
     loop unrolls like a gather loop instead.  Summed back into freq[] rather
     than into the coincidence count directly, so everything below this point
     is shared with the rolled form and cannot drift from it. */
  int f1[asize], f2[asize], f3[asize];
  for (int j = 0; j < asize; j++)
    { f1[j] = 0; f2[j] = 0; f3[j] = 0; }
  int i = 0;
  for (; i + 3 < textlength; i += 4)
    {
      freq[decode_at(steck, rows, ct, i)]++;
      f1[decode_at(steck, rows, ct, i + 1)]++;
      f2[decode_at(steck, rows, ct, i + 2)]++;
      f3[decode_at(steck, rows, ct, i + 3)]++;
    }
  for (; i < textlength; i++)
    freq[decode_at(steck, rows, ct, i)]++;
  for (int j = 0; j < asize; j++)
    freq[j] += f1[j] + f2[j] + f3[j];
#else
  /* NOT unrolled: see IC_UNROLL_HIST above -- it measured +10..22% on x86_64,
     where it measured -5..8% on arm64. */
  for (int i = 0; i < textlength; i++)
    freq[decode_at(steck, rows, ct, i)]++;
#endif

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

/* --- histogram-form scoring: the low-order models without decoding ------

   IC, mono and k are all functions of ONE 26-bin histogram, and that histogram
   is a sum of columns of a per-key table -- so a plugboard toggle costs O(26)
   here where decoding costs O(L). Measured 2.5x at L = 60 and 10x at L = 400
   for k, 3.1x and 14.7x for IC; the T form is FLAT in message length where
   decoding is linear, so the ratio grows without bound
   (eval/results-tclimb-proto.txt). Only the low-order CLIMB STAGES can use it
   -- b/t/q/a/f are additive over positions and have no such form.

   WHY IT IS AN IDENTITY AND NOT AN APPROXIMATION. decode_at is
   steck[rows[i][steck[ct[i]]]]. Write q_i = rows[i][steck[ct[i]]] for the
   decrypt BEFORE the exit board and n for its histogram. The decoders above
   build freq[] over the decrypt AFTER it -- but steck is an involution, hence
   a bijection, so freq[steck[y]] == n_y: freq is n PERMUTED. Therefore

     IC    sum_z freq[z](freq[z]-1)  ==  sum_y n_y(n_y-1)       (permutation)
     mono  sum_z freq[z]*mono8[z]    ==  sum_y n_y*mono8[S[y]]  (relabelling)

   so IC IS BLIND TO THE EXIT BOARD ENTIRELY and mono only relabels its
   coefficients. Both decoders accumulate in INTEGERS (long isum, int coin)
   before the single float division, and integer addition is associative, so
   summing over 26 bins instead of over L positions yields the SAME integers
   and the identical double. Not "close to" -- the same bits. That is the whole
   precondition: a climb that scored differently would make different
   decisions, and every tuning result in CLAUDE.md would have to be re-measured.

   Verified over the moves the climb actually makes -- the real four-case
   toggle operator (add / remove / move / merge) walked so later boards are
   reachable states, and try_repair's 2-plug re-pair -- 0 mismatches for all
   three models at L = 60/100/200/400 (eval/proto_tclimb.cc). */

/* T[c][d][y] = #{i : ct_i == c and rows_i[d] == y}. 26^3 uint16 = 34 KB,
   thread_local rather than a machine member for the reason machine.h gives:
   inlining 34 KB would push the hot per-character tables to large struct
   offsets, worth 20-60% on some targets. Plain array and no constructor --
   a std::vector member gives the struct a throwing constructor, which
   clang-tidy rejects at thread_local storage duration
   (bugprone-throwing-static-initialization). */
struct histscratch
{
  uint16_t t[asize * asize * asize];
  int n[asize];   /* histogram of the board the climb is sitting on */
};
static thread_local histscratch hist_scratch;

/* ENIGMA_HIST selects the path: 0 sends these stages back through the
   decoders, 1 (the default) is the fast path, 2 is the fast path with the
   drift self-check above. A MEASUREMENT AND TEST switch, not an
   option: the two paths
   are byte-identical by construction, so this exists so that identity can be
   CHECKED -- tests/run_tests.sh runs the same climb both ways and compares.
   Without it the claim would only ever have been verifiable against a
   hand-built reference binary, i.e. never again after the day it landed.
   Default on; empty means unset, as for every other ENIGMA_* override. */
static bool g_hist_enabled = true;
/* ENIGMA_HIST=2 turns on a self-check: after every committed move, recompute
   n from scratch and compare. Off by default and one predictable branch when
   off, called once per accepted move rather than per probe. */
static bool g_hist_check = false;

void hist_init()
{
  const char * v = getenv("ENIGMA_HIST");
  if ((v != nullptr) && (*v != 0))
    {
      const int k = parse_opt_int(v, "$ENIGMA_HIST");
      g_hist_enabled = (k != 0);
      g_hist_check = (k == 2);   /* 2 = fast path plus the drift self-check */
    }
}

/* Does this model have a histogram form at all? */
bool hist_model(int scoring)
{
  return g_hist_enabled
         && ((scoring == SCORE_IC) || (scoring == SCORE_MONO)
             || (scoring == SCORE_MONOIC));
}

/* Build T for the key the machine is currently set to. It depends on
   (rows, ciphertext) and NOT on the board, so one build serves every toggle of
   every pass -- 1.3 us at L = 60 rising to 7.6 us at L = 400, which is 0.4-1.5%
   of one restart's cap stage. Rebuilt per hillclimb() call rather than per key,
   deliberately: --tune-phase re-runs setup_mapping between climbs, and a stale
   T would change results silently, which is a far worse failure than 1% of a
   stage. */
void cooc_build(machine & m)
{
  uint16_t * const t = hist_scratch.t;
  memset(t, 0, sizeof hist_scratch.t);
  for (int i = 0; i < textlength; i++)
    {
      const int c = num_ciphertext[i];
      const unsigned char * __restrict row = m.rows[i];
      uint16_t * const base = t + static_cast<size_t>(c) * asize * asize;
      for (int d = 0; d < asize; d++)
        base[d * asize + row[d]]++;
    }
}

const uint16_t * cooc_col(int c, int d)
{
  return hist_scratch.t + (static_cast<size_t>(c) * asize + d) * asize;
}

/* n = sum_c T[c][S[c]] for the board now on the machine. O(26^2) = 26 columns.
   Called once per climb, and after any board move the climb did not commit
   through hist_apply() -- a re-pair or a gain cascade, both of which mutate
   the board on their own terms and are rare enough that a full rebuild there
   is free. Accepted TOGGLES go through hist_apply() instead, which took
   histogram maintenance from 10.5% of the cap stage's column traffic to
   2.7% (measured: 89 076 columns over 3 426 resyncs, against 9 500 over 2 964
   applies plus 12 012 over the 462 rebuilds that remain). */
void hist_resync(machine & m)
{
  int * const n = hist_scratch.n;
  for (int y = 0; y < asize; y++)
    n[y] = 0;
  for (int c = 0; c < asize; c++)
    {
      const uint16_t * const col = cooc_col(c, m.steckerbrett[c]);
      for (int y = 0; y < asize; y++)
        n[y] += col[y];
    }
}

/* Commit an accepted toggle to n: the same 2-4 column delta the probe already
   computed, applied for real. 3.5 columns on average against hist_resync's
   26, and accepted moves are 1 in 63 probes under -J, so this is 6.0% of
   probe work down to 0.8%.

   IT MUST BE CALLED BEFORE THE BOARD MOVES. The delta reads S[pos[k]] as the
   OLD partner, so committing the board first silently subtracts the wrong
   column -- and the resulting drift does not misscore politely, it HANGS the
   steepest-ascent loop, which repeats while best_score > last_best. That is
   why the two live behind one helper in plugboard.cc that does them in order,
   and why ENIGMA_HIST=2 exists to catch it if they ever come apart. */
void hist_apply(machine & m, const int * pos, const int * val, int cnt)
{
  int * const n = hist_scratch.n;
  const unsigned char * __restrict steck = m.steckerbrett;
  for (int k = 0; k < cnt; k++)
    {
      const uint16_t * __restrict rm = cooc_col(pos[k], steck[pos[k]]);
      const uint16_t * __restrict ad = cooc_col(pos[k], val[k]);
      for (int y = 0; y < asize; y++)
        n[y] += ad[y] - rm[y];
    }
}

/* Under ENIGMA_HIST=2, assert that n still describes the board. Called after
   the board has moved, so it recomputes and compares. */
void hist_verify(machine & m)
{
  if (! g_hist_check)
    return;
  int want[asize];
  for (int y = 0; y < asize; y++)
    want[y] = 0;
  for (int c = 0; c < asize; c++)
    {
      const uint16_t * const col = cooc_col(c, m.steckerbrett[c]);
      for (int y = 0; y < asize; y++)
        want[y] += col[y];
    }
  for (int y = 0; y < asize; y++)
    if (want[y] != hist_scratch.n[y])
      fatal("ENIGMA_HIST=2: the incremental histogram has drifted from the "
            "board");
}

/* Which model --biased-random ranks its 325 single plugs by. IC is the
   shipped default and needs no language; ENIGMA_KICK_RANK=k selects the
   mono+IC blend instead -- the model the recommended telegraphic pre-pass
   uses, and the one measured 5.01pp better in THAT role, which is the whole
   reason this switch exists to be A/B'd.

   RANKING BY k COSTS SOMETHING IC DOES NOT: a language. IC is
   language-independent, so --biased-random works today under `-i` or a
   mismatched `-l`; k reads the monogram table and needs one. args.cc loads it
   when this is set, and refuses if there is no -l to load it from -- silently
   ranking on an all-zero mono8 would degenerate to IC and look like a null
   result rather than a mistake. */
static int g_kick_rank = SCORE_IC;

int kick_rank_model()
{
  return g_kick_rank;
}

void kick_rank_init()
{
  const char * v = getenv("ENIGMA_KICK_RANK");
  if ((v == nullptr) || (*v == 0))
    return;
  if ((v[0] == 'k') && (v[1] == 0))
    g_kick_rank = SCORE_MONOIC;
  else if ((v[0] == 'i') && (v[1] == 0))
    g_kick_rank = SCORE_IC;
  else
    fatal("$ENIGMA_KICK_RANK must be i or k");
}

/* Score all 325 SINGLE-PLUG boards -- plug (a,b) on an otherwise empty board
   -- straight from the co-occurrence table, in the a<b order the caller walks.
   325 O(26) deltas instead of 325 decodes, and flat in message length.

   `model` is SCORE_IC or SCORE_MONOIC. The caller z-scores the result, so any
   positive affine transform of it is irrelevant -- which is why IC's L*(L-1)
   denominator is never formed. It is NOT irrelevant for k, whose two halves
   have to be combined in their real units before the z, so that one is formed.

   Deliberately does NOT bump m.plugboards_scored: this prepares a kick, and
   counting it would move a reported diagnostic for every --biased-random run.

   Requires mono8 to be loaded for SCORE_MONOIC -- see the load in args.cc.
   With no monogram table these come back as pure IC scaled, silently, which
   is exactly the bug the loader guard exists to prevent. */
void cooc_plug_scores(machine & m, int model, double * out)
{
  (void) m;
  int n0[asize];                 /* empty board: S is the identity, so the
                                    diagonal of T */
  for (int y = 0; y < asize; y++)
    n0[y] = 0;
  for (int c = 0; c < asize; c++)
    {
      const uint16_t * const col = cooc_col(c, c);
      for (int y = 0; y < asize; y++)
        n0[y] += col[y];
    }

  const bool want_mono = (model == SCORE_MONOIC);
  const double icdiv = (textlength > 1)
    ? static_cast<double>(textlength) * (textlength - 1) : 1.0;

  int i = 0;
  for (int a = 0; a < asize; a++)
    for (int b = a + 1; b < asize; b++)
      {
        /* plugging (a,b) on an empty board swaps two columns of T */
        const uint16_t * const ma = cooc_col(a, a);
        const uint16_t * const mb = cooc_col(b, b);
        const uint16_t * const qa = cooc_col(a, b);
        const uint16_t * const qb = cooc_col(b, a);
        long coin = 0;
        long isum = 0;
        int na = 0, nb = 0;
        for (int y = 0; y < asize; y++)
          {
            const int nn = n0[y] - ma[y] - mb[y] + qa[y] + qb[y];
            coin += static_cast<long>(nn) * (nn - 1);
            if (want_mono)
              {
                isum += static_cast<long>(nn) * mono8[y];
                if (y == a)
                  na = nn;
                if (y == b)
                  nb = nn;
              }
          }
        if (! want_mono)
          {
            out[i++] = static_cast<double>(coin);
            continue;
          }
        /* the board relabels mono's coefficients at a and b only */
        isum += static_cast<long>(na) * (mono8[b] - mono8[a]);
        isum += static_cast<long>(nb) * (mono8[a] - mono8[b]);
        const double mono = static_cast<double>(isum) / ngram_scale[SCORE_MONO]
                            + textlength * ngram_bias[SCORE_MONO];
        const double ic = (textlength > 1)
          ? static_cast<double>(coin) / icdiv : 0.0;
        out[i++] = mono / (textlength > 0 ? textlength : 1)
                   + g_monoic_lambda * textlength * ic;
      }
}

/* Score the board that WOULD result from setting S[pos[k]] = val[k] for k <
   cnt, WITHOUT touching the board: the caller's probe needs no mutate/restore
   pair at all. `pos` holds distinct letters (the toggle operator's four cases
   and try_repair's re-pair all name each changed letter once), cnt <= 4.

   Drop-in for score_iter() on the same hypothetical board, m.plugboards_scored
   included -- so the diagnostic stays comparable across this change and the
   harnesses that read it keep working. */
double hist_probe(machine & m, const int * pos, const int * val, int cnt)
{
  m.plugboards_scored++;

  const unsigned char * __restrict steck = m.steckerbrett;
  const int * __restrict n0 = hist_scratch.n;

  int n[asize];
  for (int y = 0; y < asize; y++)
    n[y] = n0[y];
  for (int k = 0; k < cnt; k++)
    {
      const uint16_t * __restrict rm = cooc_col(pos[k], steck[pos[k]]);
      const uint16_t * __restrict ad = cooc_col(pos[k], val[k]);
      for (int y = 0; y < asize; y++)
        n[y] += ad[y] - rm[y];
    }

  if (m.scoring == SCORE_IC)
    {
      int coin = 0;
      for (int y = 0; y < asize; y++)
        coin += n[y] * (n[y] - 1);
      return (textlength > 1)
        ? static_cast<double>(coin)
            / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;
    }

  /* mono relabels its coefficients by the board, so the cnt changed positions
     need their terms corrected -- everything else reads S unchanged. */
  long isum = 0;
  int coin = 0;
  for (int y = 0; y < asize; y++)
    {
      isum += static_cast<long>(n[y]) * mono8[steck[y]];
      coin += n[y] * (n[y] - 1);
    }
  for (int k = 0; k < cnt; k++)
    isum += static_cast<long>(n[pos[k]])
            * (mono8[val[k]] - mono8[steck[pos[k]]]);

  const double mono = static_cast<double>(isum) / ngram_scale[SCORE_MONO]
                      + textlength * ngram_bias[SCORE_MONO];
  if (m.scoring == SCORE_MONO)
    {
      /* score_iter normalises SCORE_MONO by nterms = textlength (SCORE_MONOIC
         is already per-symbol and is left alone), so the /L belongs here. */
      return (textlength > 0) ? mono / textlength : mono;
    }

  const double ic = (textlength > 1)
    ? static_cast<double>(coin)
        / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;
  return mono / (textlength > 0 ? textlength : 1)
         + g_monoic_lambda * textlength * ic;
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
/* --- NEON decode of 16 positions at a time ---------------------------------

   decode_at is steck[ rows[i][ steck[ct[i]] ] ] -- three dependent lookups,
   and oracle ablation puts the two steck[] ones plus their dependency chain at
   ~37% of this loop while the rows[] gather costs 0.3% (mapping is 2.6 KB at
   L=100, so it is L1-resident and its latency is hidden).  So the part worth
   vectorising is exactly the part that CAN be: steck is 26 bytes, and
   vqtbl2q_u8 looks 16 indices up in a 32-byte table in ONE instruction.

   The rows[] gather stays scalar -- NEON has no gather, and it costs nothing
   anyway.  That is the whole design: two vector shuffles replace 32 scalar
   table lookups, with a 16-byte buffer round trip between them for the gather.

   BYTE-IDENTICAL by construction: same table, same indices, same order of
   integer accumulation.  $ENIGMA_NEON=0 forces the scalar path so the two can
   be compared in one binary, the way $ENIGMA_HIST=0 does for the histogram
   form.  Verified under qemu-aarch64 and by the arm64 CI job.

   The 32-byte padded copy is not decoration: steck is 26 bytes, so loading
   the table's upper half would read 6 bytes past it.  Indices are 0..25, so
   the pad is never selected -- it exists only to make the load legal. */
#if defined(__aarch64__)
#include <arm_neon.h>
#define ENIGMA_HAVE_NEON 1
#else
#define ENIGMA_HAVE_NEON 0
#endif

#if ENIGMA_HAVE_NEON
static bool neon_enabled()
{
  static const bool v = []()
  {
    const char * e = getenv("ENIGMA_NEON");
    if ((e == nullptr) || (*e == 0))
      return true;
    return parse_opt_int(e, "$ENIGMA_NEON") != 0;
  }();
  return v;
}
#endif

static double ngram_ic_decode(machine & m, const uint8_t (* table)[asize][asize][asize],
                              int model, double * ic_out)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  /* FOUR HISTOGRAMS, ONE PER UNROLLED COPY -- the whole point of this
     function's shape.

     The body is unrolled BY HAND rather than by SCORE_UNROLL, because the
     pragma alone makes this loop SLOWER.  It does freq[d]++, a
     read-modify-write, and unrolling schedules four of those together where
     four random letters out of 26 collide about 30% of the time, turning
     store-to-load forwarding into a stall.  Measured, the pragma costs +11.7%
     on the g++ fused long tier while the pure-gather loops gain 7-12%
     (eval/results-scoreloop-insns.txt).

     Giving each unrolled copy its OWN counter array removes the collision by
     construction -- four separate arrays cannot alias -- so the loop unrolls
     like a gather loop and the histogram is recovered by summing them.  Worth
     -18.6..-18.9% on the fused long tier on arm64 (reproduced to 0.3 points),
     -5.8% on one local x86 CPU, and NOTHING on the GitHub g++ x86_64 runner
     -- the size is CPU-dependent.  The reduction is 26*3 adds per
     call, about 0.5 per character at operational length, against a
     4x-unrolled body.  It stays SINGLE-PASS and decodes each character once,
     so this is not the two-pass form rejected above.

     Byte-identical: the counts are the same integers whichever array they land
     in, and the n-gram terms are added in the same order. */
  int f0[asize], f1[asize], f2[asize], f3[asize];
  for (int j = 0; j < asize; j++)
    { f0[j] = 0; f1[j] = 0; f2[j] = 0; f3[j] = 0; }

  *ic_out = 0.0;
  if (textlength < 4)
    return 0.0;

  int a = decode_at(steck, rows, ct, 0);
  int b = decode_at(steck, rows, ct, 1);
  int c = decode_at(steck, rows, ct, 2);
  f0[a]++; f1[b]++; f2[c]++;
  long isum = 0;
  int i = 3;
#if ENIGMA_HAVE_NEON
  if (neon_enabled())
    {
      unsigned char pad[32];
      memcpy(pad, steck, asize);
      memset(pad + asize, 0, sizeof pad - asize);
      uint8x16x2_t stbl;
      stbl.val[0] = vld1q_u8(pad);
      stbl.val[1] = vld1q_u8(pad + 16);
      /* SIXTY-FOUR AT A TIME FIRST, then sixteen.  The 16-wide form of this
         measured +30.5% (g++) and +39.1% (clang) on arm64, and the suspected
         cause is the two vector/scalar transitions per group: a 16-byte
         vst1q_u8 immediately followed by single-byte loads, and single-byte
         stores immediately followed by a 16-byte vld1q_u8, neither of which
         forwards.  If that is the cost then it is PER TRANSITION, and widening
         the group separates each store from its dependent load by ~48 further
         stores -- time for the store buffer to drain -- while quadrupling the
         work each pair of transitions serves.  If instead the cost is
         per-element vector overhead, widening changes nothing.  The two
         predictions differ by a factor of four, which one bench run
         distinguishes.

         The 16-wide loop is kept after it so that both forms cover the same
         characters and the ONLY difference between this and the measured-down
         revision is the group width. */
      unsigned char xb[64], yb[64], db[64];
      for (; i + 63 < textlength; i += 64)
        {
          for (int v = 0; v < 64; v += 16)
            vst1q_u8(xb + v, vqtbl2q_u8(stbl, vld1q_u8(ct + i + v)));
          for (int k = 0; k < 64; k++)
            yb[k] = rows[i + k][xb[k]];
          for (int v = 0; v < 64; v += 16)
            vst1q_u8(db + v, vqtbl2q_u8(stbl, vld1q_u8(yb + v)));
          for (int k = 0; k < 64; k += 4)
            {
              const int d0 = db[k];
              const int d1 = db[k + 1];
              const int d2 = db[k + 2];
              const int d3 = db[k + 3];
              f0[d0]++;
              f1[d1]++;
              f2[d2]++;
              f3[d3]++;
              isum += table[a][b][c][d0];
              isum += table[b][c][d0][d1];
              isum += table[c][d0][d1][d2];
              isum += table[d0][d1][d2][d3];
              a = d1;
              b = d2;
              c = d3;
            }
        }
      for (; i + 15 < textlength; i += 16)
        {
          /* steck[ct[i..i+15]] in one instruction */
          vst1q_u8(xb, vqtbl2q_u8(stbl, vld1q_u8(ct + i)));
          /* the gather: 16 rows, 16 indices -- no NEON form, and none needed */
          for (int k = 0; k < 16; k++)
            yb[k] = rows[i + k][xb[k]];
          /* and the exit board, again in one instruction */
          vst1q_u8(db, vqtbl2q_u8(stbl, vld1q_u8(yb)));
          /* the accumulation is the scalar body below, verbatim, reading the
             decoded letters from db instead of calling decode_at -- so the
             four private histograms and the term order are unchanged. */
          for (int k = 0; k < 16; k += 4)
            {
              const int d0 = db[k];
              const int d1 = db[k + 1];
              const int d2 = db[k + 2];
              const int d3 = db[k + 3];
              f0[d0]++;
              f1[d1]++;
              f2[d2]++;
              f3[d3]++;
              isum += table[a][b][c][d0];
              isum += table[b][c][d0][d1];
              isum += table[c][d0][d1][d2];
              isum += table[d0][d1][d2][d3];
              a = d1;
              b = d2;
              c = d3;
            }
        }
    }
#endif
  for (; i + 3 < textlength; i += 4)
    {
      const int d0 = decode_at(steck, rows, ct, i);
      const int d1 = decode_at(steck, rows, ct, i + 1);
      const int d2 = decode_at(steck, rows, ct, i + 2);
      const int d3 = decode_at(steck, rows, ct, i + 3);
      f0[d0]++;
      f1[d1]++;
      f2[d2]++;
      f3[d3]++;
      isum += table[a][b][c][d0];
      isum += table[b][c][d0][d1];
      isum += table[c][d0][d1][d2];
      isum += table[d0][d1][d2][d3];
      a = d1;
      b = d2;
      c = d3;
    }
  for (; i < textlength; i++)
    {
      const int d = decode_at(steck, rows, ct, i);
      f0[d]++;
      isum += table[a][b][c][d];
      a = b;
      b = c;
      c = d;
    }

  int coin = 0;                      /* see ic_score_decode: int, not double */
  for (int j = 0; j < asize; j++)
    {
      const int n = f0[j] + f1[j] + f2[j] + f3[j];
      coin += n * (n - 1);
    }
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
