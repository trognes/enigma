/* Everything the command line sets: one declaration per option, read by the
   search, the scorers and the diagnostics.

   This header is the INDEX; options.cc is the reference. The definitions there
   carry the rationale for each option and what was measured about it, and they
   carry the defaults, so the default configuration of the whole program is
   readable in one place. The grouping below is navigational only -- do not read
   it as the documentation. */

#ifndef ENIGMA_OPTIONS_H
#define ENIGMA_OPTIONS_H

#include "common.h"

#include <stdint.h>

/* machine and key */
extern const char * opt_ukw;
extern const char * opt_walzen;
extern const char * opt_ringstellung;
extern const char * opt_grundstellung;
extern int opt_maxwheel;
extern int opt_norenigma;
extern int opt_m4;
extern char opt_greek_walzen;
extern char opt_greek_ringstellung;
extern char opt_greek_grundstellung;

/* plugboard knowledge supplied by the caller */
extern const char * opt_steckerbrett;
extern const char * opt_no_plug;
extern const char * opt_soft_plug;

/* scoring model, language and data */
extern int opt_scoring;
extern int opt_model_selector;
extern const char * opt_language;
extern const char * opt_datadir;

/* the staged climb schedule (--score/-S) */
static const int max_stages = 16;
struct climbstage
{
  int model;   /* SCORE_* */
  int cap;     /* max plug pairs this stage may set (1..13; 13 = uncapped) */
};
extern struct climbstage opt_stages[max_stages];
extern int opt_nstages;
extern const char * opt_staged;

/* plugboard search */
extern int opt_hillclimb;
extern int opt_firstimprove;
extern int opt_dynorder;
extern int opt_ic_order;
extern int opt_capmerge;
extern int opt_no_repair;
extern int opt_restarts;
extern int opt_perturb;
extern double opt_biased_random;
extern bool opt_random_set;
extern int opt_exhaust;
extern int opt_anneal;

/* finishers */
extern int opt_cascade;
extern double opt_cascade_gate;
extern int opt_cascade3;
extern int opt_polish;

/* cribs and self-cribs */
extern const char * opt_crib_text;
extern int opt_crib_at;
extern bool opt_crib_dump;
extern int opt_crib_seeds;
extern const char * opt_crib_list;
extern bool opt_crib_reorder;
extern int opt_self_crib_seeds;
extern int opt_self_crib_length;
extern bool opt_self_crib_signature;
extern bool opt_self_crib_tandem;
extern const char * opt_crib_rerank;
extern double opt_crib_weight;
extern int opt_crib;

/* key-space reductions and the pre-filter */
extern int opt_ring_stride;
extern int opt_seed_dedup;
extern int opt_seed_dedup_bits;
extern uint64_t opt_seed_dedup_max;
extern int opt_tune_phase;
extern int opt_prefilter;
extern double opt_prefilter_frac;

/* diagnostics and reporting */
extern int opt_confidence;
extern int opt_doubling_report;
extern double opt_doubling_z;
extern int opt_doubling_z_set;
extern int opt_doubling_mismatches;
extern int opt_doubling_mismatches_set;
extern bool opt_dump_all;
extern bool opt_full_text;
extern bool opt_no_preflight;
extern const char * opt_true_key;
extern char * opt_plaintext;

/* run control */
extern int opt_threads;
extern uint64_t opt_seed;
extern bool opt_seed_set;

/* Bounds the option parser validates against. Compile-time constants in the
   header because both the parser and the code that honours them need them. */

/* Upper bound on -R, purely a sanity guard against a typo: each restart just
   re-runs the hill-climb from a fresh perturbed board, so there is no extra
   memory and the only real limit is patience. One billion is effectively
   unlimited for any run yet stays well within int. */
static const int max_restarts = 1000000000;

static const int pairs_uncapped = asize / 2;   /* 13: a board holds at most this */

static const int default_perturb = 10;  /* --random default kick: near the typical plug count */

static const int max_threads = 256;

/* --doubling-report defaults: one tolerated substitution (Enigma has no
   diffusion, so one garbled ciphertext letter damages exactly one plaintext
   letter, in one copy of the doubling and not the other), and a z gate of 3
   over the calibrated null. */
static const int double_mismatches_default = 1;
static const double double_z_default = 3.0;

#endif
