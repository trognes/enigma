/* Known-plaintext attacks: --crib, --crib-list, and the self-crib that a
   doubled word gives for free.

   WHAT THE DEDUCTION IS. Decryption is p = steck[core_i[steck[c]]] and the
   rotor core is an involution, so it rearranges to steck[p] = core_i[steck[c]]
   -- one lookup on the table setup_mapping() already built. Guess a single
   plug, chain it along every crib position, and add reciprocity (Welchman's
   diagonal board, free because the plugboard is an involution). A
   contradiction kills the guess; all 26 dead means the rotor setting cannot
   have produced the crib, so the search skips it without scoring anything.

   A self-crib says only that two positions carry the SAME plaintext letter,
   which cancels the unknown letter out of both sides. Worthless as a filter
   (0 of 160 wrong keys rejected), decisive as a seeder.

   THE MODULE HOLDS BOTH THE DEDUCTION AND ITS CONSUMERS, deliberately. The
   menu tables (crib_p, crib_order, crib_align, crib_anchor_at) are read inside
   crib_try's loop, and the units that climb from a deduced board are the only
   things that need them; cutting between the two would have exported the hot
   state to no purpose. What crosses the boundary is a handful of entry
   points. */

#ifndef ENIGMA_CRIB_H
#define ENIGMA_CRIB_H

#include "common.h"
#include "machine.h"

#include <atomic>
#include <stddef.h>
#include <string>
#include <vector>
#include <stdint.h>
#include <utility>

/* Build the menu and the alignment list once, from the ciphertext and the
   crib. init_self_crib() does the same for the doubled-word hypotheses. */
void init_crib();
void init_self_crib();

/* One work unit: deduce, then climb the surviving hypotheses. Returns
   unit_no_score when the crib proves this key impossible -- a sentinel, not a
   score, so a rejected key never wins the merge and never enters the
   --confidence null. */
double crib_unit(machine & m, size_t key_index, int restart);
double self_crib_unit(machine & m, size_t key_index, int restart);

/* The first alignment at which some hypothesis survives, or -1 if the key is
   impossible. The pure per-key filter, with no climbing. */
int crib_first_stop(const machine & m);

/* Count surviving hypotheses and pinned letters for this key, for the
   crib-cost table. Keeps the menu internals private to this module. */
void crib_count_hypotheses(const machine & m, size_t & hyps, size_t & pins);

/* --crib-rerank: the known-word bonus for a converged board. */
double crib_score(const machine & m);
void load_cribs(const char * fname);

/* --crib-list: the library, in the order it will be tried. */
void load_crib_list(const char * fname);
extern std::vector<std::string> g_crib_list;

/* Keys the crib proved impossible, counted at each key's first work item so
   the total stays independent of the thread count. */
extern std::atomic<size_t> g_crib_rejected;

/* How many alignments the crib can sit at; 0 when no crib is active. */
int crib_alignment_count();

/* Sampling budgets for the crib-cost table, which runs with the search.
   crib_sample_keys keys are sampled for the hypothesis count; the measured
   gain runs both sides of the choice on crib_gain_keys keys, each bounded by
   crib_gain_budget work items so a hopeless crib is reported as a bound
   rather than measured further. */
extern const size_t crib_sample_keys;
extern const size_t crib_gain_keys;
extern const uint64_t crib_gain_budget;

/* Longest doubled word the self-crib hypothesises; nothing in the corpus of
   authentic decrypts reaches 14. */
extern const int selfcrib_maxlen;

/* --crib-rerank's known-word list, read-only once loaded. */
const std::vector<std::pair<std::string, double>> & crib_words();
void crib_words_clear();

/* --crib-dump diagnostic: every surviving hypothesis at this key. */
void crib_dump(machine & m, int r1, int r2, int r3, int g1, int g2, int g3);

/* Hypotheses the self-crib built, for the settings echo. */
extern int g_selfcrib_nhyps;

#endif
