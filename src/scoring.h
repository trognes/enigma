/* How language-like a candidate plaintext is: the seven scoring models, the
   quantised n-gram tables they read, and score_iter(), which is the single
   entry point every search path goes through.

   THIS IS THE HOT PATH. Under -c the score loop is ~99% of runtime, and on a
   plain scan it is ~52%. The scorers fuse decoding into their own loops -- each
   character is decoded once into a sliding window that indexes the n-gram
   table, so the plaintext is never written to and read back from a scratch
   array. That fused form is measured 7% faster than storing under g++ and up
   to 45% faster under clang; do not "simplify" it back.

   Most of the module is private on purpose: mono8/bi8/tri8, the individual
   n-gram scorers, ngram_ic_decode and the IC blend weight are read nowhere
   else. What must be visible is score_iter (everything calls it), quad8/all8
   (the gain cascade scans them per symbol), the quantisation affine
   (diagnostics reconstruct a true cross-entropy from it), ic_score_decode
   (simulated annealing and the -F tier-1 filter score in IC directly) and the
   three model-selection helpers. */

#ifndef ENIGMA_SCORING_H
#define ENIGMA_SCORING_H

#include "common.h"
#include "machine.h"

#include <stdint.h>

/* Score the current board's decrypt under m.scoring. Bumps m.plugboards_scored
   once per whole-message score -- the counter the final diagnostic reports. */
double score_iter(machine & m);

/* Index of coincidence alone, no n-gram table and no language. */
double ic_score_decode(machine & m);

/* The quad-shaped tables. quad8 is the plain quadgram model; all8 holds the
   log-linear all-order mixture -a/-f read, folded at load time so the hot path
   treats it exactly like quad8. Exposed only because the gain cascade scans
   them directly. */
extern uint8_t quad8[asize][asize][asize][asize];
extern uint8_t all8[asize][asize][asize][asize];

/* Per-model quantisation affine: a stored byte q means log10 p = q/scale + bias.
   Indexed by SCORE_*. */
extern double ngram_scale[SCORE_MONOIC + 1];
extern double ngram_bias[SCORE_MONOIC + 1];

/* --- histogram-form scoring for the low-order climb stages --------------
   IC, mono and k (-S i / m / k) are functions of one 26-bin histogram of the
   decrypt taken BEFORE the exit plugboard, and that histogram is a sum of
   columns of a per-key co-occurrence table -- so a plugboard toggle costs
   O(26) instead of O(L), and the result is BYTE-IDENTICAL to decoding. See
   the derivation above hist_probe() in scoring.cc.

   Usage from a climb: cooc_build() once (it depends on the key, not the
   board), hist_resync() to take the histogram of the board being sat on, then
   hist_probe() per candidate move, and hist_resync() again after any move that
   is KEPT. cooc_col() is exposed for --biased-random, which scores all 325
   single plugs off the same table. */
/* Read the ENIGMA_HIST override. Called once from main(), before the search.
   The switch exists so the two paths can be COMPARED in the test suite; the
   default is on and the paths are byte-identical. */
void hist_init();

/* Lagged-coincidence profile term (EXPERIMENTAL, off by default; see the
   block above kappa_init in scoring.cc). Read from $ENIGMA_KAPPA at
   startup; mu = 0 leaves the scorer byte-identical. */
void kappa_init();
double kappa_mu();
bool hist_model(int scoring);
void cooc_build(machine & m);
const uint16_t * cooc_col(int c, int d);
void hist_resync(machine & m);
/* Commit an accepted toggle to the histogram. MUST be called BEFORE the board
   moves -- the delta reads the old partners. ENIGMA_HIST=2 makes hist_verify
   check that afterwards; it is a no-op otherwise. */
void hist_apply(machine & m, const int * pos, const int * val, int cnt);
void hist_verify(machine & m);
double hist_probe(machine & m, const int * pos, const int * val, int cnt);
/* All 325 single-plug scores from the same table, for --biased-random's kick
   weights. model is SCORE_IC or SCORE_MONOIC; out is filled in a<b order. */
void cooc_plug_scores(machine & m, int model, double * out);
/* Which model --biased-random ranks by: SCORE_IC (default) or SCORE_MONOIC,
   selected by $ENIGMA_KICK_RANK=i|k. Ranking by k needs the monogram table,
   so it needs a language; args.cc loads it and refuses without one. */
void kick_rank_init();
int kick_rank_model();

/* Read the ENIGMA_IC_BLEND override for -f's IC weight. Called once from
   main(), before the search. */
void ic_blend_init();

/* Load the table backing one model (IC needs none). */
void load_table(int model);

/* Map a scoring-model letter (i/m/b/t/q/a/f/k) to its SCORE_* value. */
int model_of(char c);

/* Record a bare -i/-m/-b/-t/-q/-a/-f selector; fatal on two that disagree. */
void select_model(int model);

#endif
