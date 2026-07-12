#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/resource.h>

#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/* uwwwrrrggg = 3*8*7*6*26*26*26*26*26*26 = 311 387 102 208 */

/* --- Enigma wiring tables (reflectors, rotors, notches) ----------------- */

static const char * reflector_string[] =
  {
    "EJMZALYXVBWFCRQUONTSPIKHGD",    // A
    "YRUHQSLDPXNGOKMIEBFZCWVJAT",    // B
    "FVPJIAOYEDRZXWGCTKUQSBNMHL",    // C
    "MOWJYPUXNDSRAIBFVLKZGQCHET",    // Norway
    "ENKQAUYWJICOPBLMDXZVFTHRGS",    // UKW-b M4 thin
    "RDOBJNTKVEHMLFCWZAXGYIPSUQ"     // UKW-c M4 thin
  };

static const char * rotor_string[] =
  {
    "EKMFLGDQVZNTOWYHXUSPAIBRCJ",  // i
    "AJDKSIRUXBLHWTMCQGZNPYFVOE",  // ii
    "BDFHJLCPRTXVZNYEIWGAKMUSQO",  // iii
    "ESOVPZJAYQUIRHXLNFTGKDCMWB",  // iv
    "VZBRGITYUPSDNHLXAWMJQOFECK",  // v
    "JPGVOUMFYQBENHZRDKASXLICTW",  // vi
    "NZJHGRCXMYSWBOUFAIVLPEKQDT",  // vii
    "FKQHTLXOCBJSPDZRAMEWNIUYGV",  // viii
    "WTOKASUYVRBXJHQCPZEFMDINLG",  // Norway i
    "GJLPUBSWEMCTQVHXAOFZDRKYNI",  // Norway ii
    "JWFMHNBPUSDYTIXVZGRQLAOEKC",  // Norway iii
    "FGZJMVXEPBWSHQTLIUDYKCNRAO",  // Norway iv
    "HEJXQOTZBVFDASCILWPGYNMURK",  // Norway v
    "LEYJVCNIXWPBQMDRTAKZGFUHOS",  // Beta
    "FSOKANUERHMBTIYCWLQPZXVGJD"   // Gamma
  };

static const char * notch_string[] =
  {
    "Q",
    "E",
    "V",
    "J",
    "Z",
    "MZ",
    "MZ",
    "MZ",
    "Q",
    "E",
    "V",
    "J",
    "Z",
    "",
    ""
  };

/* --- machine constants, command-line options, and global state ---------- */

static const int maxlen = 1024;   /* maximum ciphertext length (letters) */
static const int asize = 26;
static const int wheels = 3;
static const int reflector_count = sizeof(reflector_string) / sizeof(char *);
static const int rotor_count = sizeof(rotor_string) / sizeof(char *);

/* Layout of the reflector[] / rotor[] wiring tables for Norway Enigma mode:
   reflector index 3 is UKW-N, rotor indices 8-12 are Norway wheels 1-5. */
static const int norway_reflector_index = 3;
static const int norway_rotor_base = 8;

/* A score lower than any achievable plaintext score. IC scores in [0, ~0.08]; the
   n-gram models now score a per-symbol log10-probability (cross-entropy), which is
   <= 0 but bounded well above -1e30 by the unseen-gram floor. */
static const double score_min = -1e30;

/* Plaintext scoring models; values match the scoring_name[] order and the
   *_score_decode dispatch in score_iter(). */
enum scoring { SCORE_IC, SCORE_MONO, SCORE_BI, SCORE_TRI, SCORE_QUAD, SCORE_ALL };

static const char * opt_ukw;
static const char * opt_walzen;
static const char * opt_ringstellung;
static const char * opt_grundstellung;
static const char * opt_steckerbrett;
/* Letters that are part of a fixed (-s) plug pair are never rewired by the hill-climb,
   re-pair or SA toggle, so -s pairs are *known* plugs that survive the climb rather than
   a mere seed (see plug_fixed / PLUG_FIXED_EX below for the storage and the parallel
   --exhaust arrangement, REDESIGN Part D). */
static char * opt_plaintext; /* plaintext to compare to */
static const char * opt_language; /* english, german, danish, french ...; no default */
static const char * opt_datadir;  /* directory holding the n-gram files (default "ngrams") */
static int opt_norenigma; /* use the 5 Norenigma (Norway Enigma) wheels */
static int opt_m4;        /* use M4 (4-rotor naval) mode */
/* M4 mode: 4th "Greek" wheel (Beta/Gamma, rotor indices 13-14) is static (never
   steps) and folds into a thin reflector (UKW-b/c, reflector indices 4-5) to form
   an effective reflector -- so the machine stays a 3-stepping-rotor engine. The
   Greek wheel and ring/start are taken from the first character of -w/-r/-g. */
static const int m4_thin_base = 4;   /* reflector index of UKW-b; UKW-c is +1 */
static const int greek_base = 13;    /* rotor index of Beta; Gamma is +1 */
static char opt_greek_walzen = '.';      /* Greek wheel: B (Beta), G (Gamma) or . */
static char opt_greek_ringstellung = '.';   /* Greek ring letter or . */
static char opt_greek_grundstellung = '.';  /* Greek start letter or . */
static int opt_maxwheel;
static int opt_scoring;   /* the resolved ranking/target model (SCORE_*) */
/* The model chosen by a bare selector -i/-m/-b/-t/-q, or -1 if none was given. The
   selectors are thin aliases for a single uncapped --score <model> stage (REDESIGN
   Part C); this records the choice so conflicting models -- two disagreeing selectors,
   or a selector vs a different --score target -- can be rejected as a fatal error. */
static int opt_model_selector;
static int opt_hillclimb;
/* -I: circular first-improvement climb instead of steepest ascent. Applies the FIRST
   improving move (cursor sweeps a fixed move list and continues from where it accepted),
   so it does far fewer score_iter calls per climb -- ~2.8x cheaper. It recovers WORSE per
   restart (a different, noisier trajectory), so it is a *throughput multiplier*: pair it
   with more restarts (-R) and it wins at equal compute. Off by default; needs -c. */
static int opt_firstimprove;
/* -J: first-improvement (-I) with DYNAMIC best-first move ordering. Each climb first
   scores every move once against the starting (perturbed) board, sorts, and then runs the
   circular first-improvement in that order. The order is rebuilt per restart, so it
   front-loads good moves without collapsing restart diversity (unlike the rejected static
   order). Measured win on the realistic ~10-plug regime (+2-6pp mean at matched compute);
   a loss when few plugs are truly set. Implies -I; off by default; needs -c. */
static int opt_dynorder;
/* --infl-order (experimental, PERFORMANCE.md 4.6): first-improvement (-I) with the move order
   set by the board-state INFLUENCE instead of -J's measured score-delta. Each move (a,b) is
   ranked by w = ct_count[a]+ct_count[b]+pt_count[a]+pt_count[b] over the current ciphertext and
   the current decrypt (the 4.5/4.6 influence weight, sum form) -- an upper-bound proxy for how
   many positions the toggle can change, computed from two 26-bin histograms, so it is ~free
   (no per-move scoring, unlike -J's +24% pre-scan). Per-restart (order derived from the
   perturbed starting board), deterministic (fixed board + index tie-break) -> -T-independent.
   Implies -I; mutually exclusive with -J; off by default; needs -c. */
static int opt_inflorder;
/* -M: make the plug cap a strict descent TARGET, not just a growth ceiling. Default (0):
   at/over the cap only a brand-new add (both ends free) is blocked, so an over-cap board
   (a big --random kick handed to a small stage cap) can converge still over the cap, merely
   reshuffled. With -M: at/over the cap allow only count-REDUCING moves (merges -- both ends
   already plugged to different partners -- plus removals), blocking adds AND count-preserving
   endpoint-moves, so the climb must shed plugs down to the cap while keeping the strongest
   descent move (the merge). Measured a matched-compute win that grows as the true plug count
   falls below the cap: neutral-to-+2.6pp on realistic 10-plug boards (best at the true-count
   kick), and +3..+20pp on known-few-plug boards (-S ...q6), largest at the short/hard end;
   it is also cheaper per climb (quad converges from a tidy basin). Off by default; needs -c.
   Most useful with a tight -S target cap; near-inert (harmless) with no cap. */
static int opt_capmerge;
/* --repair3: a last-resort 3-plug barrier cross, tried only once the cheap climb AND
   try_repair have both converged. Rematches three existing plugs (six letters) into a
   different pairing; a deeper generalisation of try_repair (which does two). Off by
   default (baseline byte-identical); needs -c. See try_repair_3(). */
static int opt_repair3;
/* --no-repair: disable the default 2-plug re-pair barrier cross (try_repair), for
   ablation/measurement. Off by default (baseline byte-identical); needs -c. */
static int opt_no_repair;
/* --gainfix: quadgram-gain directed-repair barrier cross, tried at quad convergence.
   A 2-ply "cascade" that uses per-position gain to propose plug corrections (both
   plugboard contacts, self-encryption pruned), ranks them by the full re-decode
   score, applies the best pair even when the first plug is downhill (which un-masks
   the second), and keeps it only if the pair nets an improvement. Off by default
   (baseline byte-identical); needs -c; quad-only. See gain_cascade(); PERFORMANCE.md 4.10. */
static int opt_gainfix;
/* --gainfix near-solution gate: the cascade only fires on a converged board whose
   per-symbol quad score clears this threshold, so it skips the ~76% junk boards and
   spends its compute only on promising ones. Default -4.9 (English-quad calibrated:
   junk ~-5.3, near-solution 60%+ ~-4.8..-4.2); tune per language via --gainfix=VALUE. */
static double opt_gainfix_gate;
/* --gainfix-best: run the gain cascade ONCE, unconditionally (no score gate), on the
   single best board after all restarts, instead of at every gated convergence. Only
   one board is finished, so no gate is needed. Off by default; needs -c. */
static int opt_gainfix_best;
/* --gainfix3: enable the 3-ply gain cascade (a deeper escalation, tried only when the 2-ply
   cascade found nothing). --gainfix-best3 turns it on for the single best-board finisher. */
static int opt_gainfix3;
static int opt_gainfix_best3;
/* --crib-file / --crib-weight: known-word ("crib") finisher -- Ostwald & Weierud's
   "assessment stage" (NOT RECOMMENDED; measured-down, see below). After each restart climb
   converges, its board is ranked not by the n-gram score alone but by
   score + opt_crib_weight * crib_score(decrypt), where crib_score sums the weights of known
   HG Nord words (BERTA, EINS, FRAGE, ...) present in the decrypt. The intent: lift the TRUE
   board above a spurious one that scores higher on n-grams but contains no real words -- the
   short-message scoring-failure floor. The climb itself still optimises the n-gram model
   (hot path untouched); only the cross-restart winner selection sees cribs.
     MEASURED-DOWN (eval/eval_crib.py, the 69-message held-out set, on top of the telegraphic
   tables): net -0.1pp at weight 0.5, -1.7pp at 1.0. It scores the odd genuine scoring-failure
   win (No. 203: 79->100%) but that is offset by false-positive re-ranking breaking already-good
   boards. The reason: after the telegraphic tables surface the true board, the residual is
   dominated by WRONG-BASIN failures (the true board is not among the converged restarts), so a
   re-ranker has nothing true to promote. Kept as an off-by-default diagnostic (the negative
   answer is the artifact) -- like --score-tt/--repair3. Off by default (no crib file ->
   opt_crib 0 -> byte-identical); needs -c; -T-deterministic (the combined score is a
   deterministic function of the board). See crib_score()/load_cribs(). */
static const char * opt_crib_file = nullptr;
static double opt_crib_weight = 0.5;   /* the least-harmful weight measured (still net ~0) */
static int opt_crib = 0;               /* set once a non-empty crib file is loaded */
static std::vector<std::pair<std::string, double>> g_cribs;   /* read-only after load */
static int opt_restarts;  /* --restarts/-R: number of randomised restart attempts.
                             0 (the default) = one deterministic climb from the seed,
                             no kick; N>=1 = exactly N kicked climbs, keep the best
                             (the un-kicked seed climb is not additionally run). */
static const char * opt_staged;  /* raw --score/-S schedule string (e.g. "i4q10"), or 0;
                                    parse_schedule() expands it into opt_stages[] */
/* Upper bound on -R, purely a sanity guard against a typo (each restart just
   re-runs the hill-climb from a fresh perturbed board -- no extra memory -- so the
   only real limit is patience). One billion is effectively unlimited for any real
   run yet stays well within int; raise it if you ever need more. */
static const int max_restarts = 1000000000;
static const int pairs_uncapped = asize / 2;   /* 13: a board holds at most this */

/* A parsed --score/-S schedule is an ordered list of climb stages -- each a scoring
   model and a cap on the plug pairs it may set. Tokens are <letter><optional number>:
   model letters i/m/b/t/q (a stage, number = its pair cap, omitted = uncapped). The
   last model stage is the target/ranking model. The per-restart random kick and the
   partial exhaustion are separate options (--random / --exhaust), not schedule tokens. */
static const int max_stages = 16;
struct climbstage
{
  int model;   /* SCORE_* */
  int cap;     /* max plug pairs this stage may set (1..13; 13 = uncapped) */
};
static struct climbstage opt_stages[max_stages];
static int opt_nstages;    /* number of model stages in opt_stages[] */
static int opt_perturb;    /* --random K: random plug pairs injected per restart, 0..13
                              (K=0 is legal -- a no-perturbation control). */
static const int default_perturb = 10;  /* --random default kick: near the typical plug count */
static bool opt_random_set;             /* was --random passed explicitly? (errors without -c) */
static int opt_exhaust;    /* --exhaust E: partial plugboard exhaustion -- force E extra plug
                              pairs among the free letters (on top of any -s pairs), trying
                              every set of E disjoint pairs and keeping the best climb (0 = off).
                              Parallel over the first forced pair (exhaust_unit); exploration
                              tool, dominated by a high --restarts climb (see PERFORMANCE.md §3.6). */
static int opt_prefilter; /* key pre-filter: rank all keys by a cheap IC climb, then
                             run the full -c climb on only the top opt_prefilter keys
                             (0 = off; requires -c) */
static double opt_prefilter_frac; /* -F N% form: fraction of the resolved keyspace to
                             keep (0 = not used; when > 0 it overrides opt_prefilter) */
/* Plug-pair cap for the tier-1 IC filter climb. Capping the climb both speeds tier 1
   up (fewer passes per key) and improves rotor-key discrimination: an uncapped climb
   lets wrong keys overfit IC with surplus plugs and bury the true key, so a cap near
   the true plug count ranks it better. ~5 is the measured optimum (both-axes win vs
   uncapped; harmless on easy keyspaces) -- see archived/CODE_REVIEW_HISTORY.md §9 item 2. */
static const int filter_climb_cap = 5;
/* Simulated-annealing plugboard optimiser (-A N): N = total move budget (0 = off,
   use the greedy climb). An alternative to the greedy hill-climb that accepts
   worsening moves with a cooling probability to escape local optima. Needs -c; the
   move budget is SA's cost/quality knob (like -R for the greedy climb). See
   archived/SIMULATED_ANNEALING.md. */
static int opt_anneal;
static const int anneal_chain = 208;      /* moves per temperature level (8*26) */
static const int anneal_warmup = 200;     /* warm-up samples for T calibration */
/* chi0 is the initial worsening-move acceptance target; chi_end the final (near-greedy)
   one. chi0 = 0.12 was tuned by a quality-per-climb-time sweep (archived/SIMULATED_ANNEALING.md
   §15): the surface here is greedy-friendly, so a *cool* start (a mostly-downhill walk
   with occasional uphill escapes) matches or beats the greedy restart climb, whereas a
   hot start (chi0 = 0.8) wanders and loses ~2x. Higher chi0 and reheating were both
   measured worse and dropped. */
static const double anneal_chi0 = 0.12;
static const double anneal_chi_end = 0.001;
static int opt_threads;   /* worker threads for the search (default 1) */
static const int max_threads = 256;
/* Random seed for the plugboard restart perturbation. Mixed with the flat key index
   per key, so the restart RNG stays a pure function of (opt_seed, key) -- reproducible
   and independent of -T. Resolved as: -e <seed> > $ENIGMA_SEED > a fresh random draw.
   opt_seed == 0 reproduces the historical (pre-seed) behaviour exactly. */
static uint64_t opt_seed;
static bool opt_seed_set;

/* --true-key <reflector><3 wheels><3 ring><3 start> (standard Enigma, 10 chars,
   e.g. B241AAAQEW): a diagnostic for -F recall testing (CRACKQUALITY_TESTS.md §2).
   With -F set, after tier-1 ranks every key the search prints "true-key tier1 rank
   R of N" to stderr -- R = 1 + the number of keys whose tier-1 IC score is strictly
   higher, N = total keys -- so a harness can measure how often the pre-filter keeps
   the true key. Off by default; parsed into g_tk_* below. */
static const char * opt_true_key;
static int g_tk_u, g_tk_w[3], g_tk_r[3], g_tk_g[3];   /* parsed --true-key (numeric) */
static std::vector<float> g_tk_scores;                /* tier-1 IC score per flat key idx */
static std::atomic<size_t> g_tk_idx{static_cast<size_t>(-1)};   /* flat idx of the true key */

/* --dump-restarts: a diagnostic for restart-diversity testing (CRACKQUALITY_TESTS.md
   §3). With -c, every converged restart climb prints "restart <score> <board>" to
   stderr (under a mutex), so a harness can count how many distinct optima the restarts
   reach. Display-only -- it does not affect which candidate wins, so -T-deterministic
   results are preserved. Verbose; off by default. */
static bool opt_dump_restarts;
/* --restart-tt: maintain an in-binary Zobrist transposition table of converged restart
   boards and print a basin-collapse summary (distinct optima + hit histogram) at the end.
   Diagnostic only (never gates the winner), off by default. See the TT module below. */
static bool opt_restart_tt;
/* --score-tt: memoise score_iter in a per-worker plugboard score cache. Within one
   rotor key and one scoring model, score_iter is a pure function of the plugboard, so
   a board scored again returns the stored value instead of decoding afresh. Diagnostic
   / performance only -- a hit returns exactly what a miss would compute, so results
   (and -T-determinism) are byte-identical; only the number of real decodes falls, and
   the hit rate measures how much score_iter work the cache saves. Off by default. */
static bool opt_score_tt;

/* One direct-mapped slot of the --score-tt cache. Stores the Zobrist tag AND the exact
   board (a hash collision is then a miss, never a wrong score), plus the scoring model
   and the rotor-key generation the score was computed under -- both must match on a hit,
   which is how "everything but the plugboard is constant" is enforced. */
struct sc_slot
{
  uint64_t tag;                  /* board_hash of the stored board */
  double score;                  /* score_iter result for that board */
  uint32_t gen;                  /* rotor-key generation; 0 = empty slot */
  uint8_t scoring;               /* scoring model the score was computed under */
  unsigned char board[asize];    /* exact board -> a hash collision is a miss */
};

/* 2^18 slots (~12 MB/worker at 48 B/slot): comfortably larger than the distinct-board
   working set of a key's restarts, so eviction collisions are rare. Direct-mapped. */
static const int    sc_bits  = 18;
static const size_t sc_slots = static_cast<size_t>(1) << sc_bits;
static const size_t sc_mask  = sc_slots - 1;

static char ciphertext[maxlen+1];
static char altplaintext[maxlen+1];
static int textlength;
static unsigned char num_ciphertext[maxlen];

/* Read-only wiring tables, derived once by init() and shared by every search. */
static unsigned char rotor_fwd[rotor_count][asize];
static unsigned char rotor_rev[rotor_count][asize];
static unsigned char notch[rotor_count][asize];
static unsigned char reflector[reflector_count][asize];

/* --- per-search state: struct machine ----------------------------------- */

/* Per-search mutable machine state. Grouping it into one object (rather than
   file-scope globals) makes the search reentrant: a future worker thread can own
   its own machine while the read-only wiring tables, n-gram statistics and
   ciphertext stay shared. */
struct machine
{
  unsigned char steckerbrett[asize];    /* plugboard permutation */
  /* Per character position, the 26-byte rotor-stack substitution row. The
     scorers read rows[i][k]; rows[i] either points straight into the shared
     subst_array (the brute-force scan -- no copy) or into the contiguous
     mapping[] below (hill-climbing, which re-reads each row hundreds of times
     and benefits from the locality). */
  const unsigned char * rows[maxlen];
  unsigned char mapping[maxlen][asize]; /* hill-climb's contiguous row copies */
  char plaintext[maxlen+1];             /* candidate / result */

  int ukw;                              /* reflector index (thin UKW-b/c in M4) */
  int walzenlage[wheels];               /* wheel order (rotor indices) */
  unsigned char grundstellung[wheels];  /* start positions */
  unsigned char ringstellung[wheels];   /* ring positions */

  /* effective reflector actually applied by subst_rotors: a plain copy of
     reflector[ukw] normally, or greek-folded thin reflector in M4 mode (set once
     per task by set_effective_reflector, before precompute -- never in the hot
     loop, which reads the precomputed subst_array). */
  unsigned char reflector_eff[asize];
  int greek;          /* M4 Greek rotor index (Beta/Gamma), else -1 */
  int greek_offset;   /* M4 (Greek start - ring) mod 26, else 0 */
  int scoring;        /* active scoring model (= opt_scoring; varied per stage by
                         the staged plugboard climb, so it is per-machine not the
                         global -- a shared global would race across worker threads) */

  /* The 457 KB rotor-stack table (rebuilt once per wheel order by precompute,
     read per ring/start by setup_mapping) is heap-allocated separately and
     reached through its own base pointer. Keeping it out of the struct leaves
     every member above at a small offset -- so both the hot decode tables and
     subst_array itself get tight addressing on every compiler/target, rather
     than whichever one lands past the big array paying for large offsets (which
     ARM in particular handles poorly). */
  unsigned char (* subst_array)[asize][asize][asize];

  /* Diagnostic counter: number of plugboards scored (score_iter calls) by this
     worker. Bumped once per whole-message score -- not per character -- so it is
     out of the hot per-character loop, and placed last so it never pushes the hot
     tables above to large struct offsets. Summed across workers for the final line. */
  uint64_t plugboards_scored;

  /* Echo intermediate climb improvements as progress lines? Set by the workers:
     true in the main search and the -F tier-2 climb, false in the -F tier-1 filter
     (whose IC scores are not comparable with the ranking model). Cold -- read only
     on ACCEPTED climb moves (see report_climb_progress), never in the scoring scans. */
  bool report;

  /* Per-worker scratch for --exhaust (see the PLUG_FIXED_EX note below): a copy of the global
     plug_fixed (-s seed) plus this leaf's forced-pair letters, read ONLY by the EX=true climb
     instantiations. Under g++ this lives here, in the machine (a thread_local shifts g++'s
     whole-TU codegen); under clang it lives in a thread_local instead (a struct member shifts
     clang's climb codegen), so it is compiled out of the struct there. */
#if !defined(__clang__)
  bool plug_fixed_ex[asize];
#endif

  /* --score-tt plugboard score cache: heap-allocated per worker when the flag is on,
     else null. Reached through a pointer so it never enlarges the struct or pushes the
     hot decode tables above to large offsets. sc_gen is bumped by setup_mapping on every
     new rotor key (invalidating every stored score in O(1)); sc_hits/sc_lookups are the
     effectiveness counters, summed across workers for the final diagnostic. All cold --
     touched once per score_iter call, never in the per-character scoring loop. */
  sc_slot * sc_cache;
  uint32_t  sc_gen;
  uint64_t  sc_hits;
  uint64_t  sc_lookups;
};

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
static double ngram_scale[SCORE_ALL + 1];   /* per-model: 255/(vmax-vmin), full 0..255 range */
static double ngram_bias[SCORE_ALL + 1];    /* per-model vmin; indexed by SCORE_* */
static uint8_t mono8[asize];
static uint8_t bi8[asize][asize];
static uint8_t tri8[asize][asize][asize];
static uint8_t quad8[asize][asize][asize][asize];
/* SCORE_ALL ("weighted", -a): a quad-shaped table holding the log-linear symmetric
   mixture of all four orders (see load_table); the scorer/gainfix treat it like quad8. */
static uint8_t all8[asize][asize][asize][asize];

/* --- diagnostics and n-gram table loading ------------------------------- */

void fatal(const char * message)
{
  fprintf(stderr, "\nFatal error: %s\n", message);
  exit(1);
}

inline int char2num(char x)
{
  return x - 'A';
}

inline char num2char(int x)
{
  return static_cast<char>('A' + x);
}

/* Fold one Unicode code point to an A-Z letter index (0..25), or -1 if it is not
   a foldable Latin letter. Plain A-Z/a-z map directly; accented Latin letters
   fold to their base (diacritics removed: e-acute -> E, u-umlaut -> U, o-slash
   -> O, sharp-s -> S, ae/oe ligatures -> A/O). This lets the 26-letter machine
   use the accented n-grams in the non-English tables (and accented plaintext)
   instead of discarding them. */
static int fold_codepoint(unsigned cp)
{
  if ((cp >= 'a') && (cp <= 'z'))
    cp -= 32;
  if ((cp >= 'A') && (cp <= 'Z'))
    return static_cast<int>(cp - 'A');
  /* Latin-1 supplement letters U+00C0..U+00FF -> base letter (' ' = not a letter);
     lower half mirrors the upper except the final cell (sharp-s S vs y-diaeresis Y). */
  static const char lat1[] =
    "AAAAAAACEEEEIIIIDNOOOOO OUUUUY S"
    "AAAAAAACEEEEIIIIDNOOOOO OUUUUY Y";
  if ((cp >= 0xC0) && (cp <= 0xFF))
    {
      char b = lat1[cp - 0xC0];
      return (b == ' ') ? -1 : (b - 'A');
    }
  switch (cp)
    {
    case 0x0152: case 0x0153: return 'O' - 'A';   /* OE ligature */
    case 0x0178: return 'Y' - 'A';                /* Y with diaeresis */
    default: return -1;
    }
}

/* Fold a UTF-8 n-gram token (from the statistics files) to its A-Z base index.
   Returns the number of letters produced, or -1 if the token holds a code point
   that is not a foldable Latin letter. */
static int fold_gram(const char * s, int * index_out)
{
  const unsigned char * p = reinterpret_cast<const unsigned char *>(s);
  int idx = 0;
  int letters = 0;
  while (*p)
    {
      unsigned cp;
      if (*p < 0x80)
        { cp = *p; p += 1; }
      else if (((*p & 0xE0) == 0xC0) && ((p[1] & 0xC0) == 0x80))
        { cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
      else if (((*p & 0xF0) == 0xE0) && ((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80))
        { cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); p += 3; }
      else
        return -1;
      int b = fold_codepoint(cp);
      if (b < 0)
        return -1;
      idx = idx * asize + b;
      letters++;
    }
  * index_out = idx;
  return letters;
}

/* Read an n-gram statistics table from "<language>_<suffix>.txt" into 'itable', the
   flat backing store of the corresponding uint8 array (mono8 / bi8 / tri8 / quad8),
   contiguous and row-major so the n letters of a record map to the single index
   ((a*26 + b)*26 + ...) of size 26^n. Raw counts are accumulated in a transient uint32
   scratch buffer; each entry is then stored as the log10 probability log10(count /
   total) -- a per-gram log-likelihood -- quantised to int16 fixed-point (see the
   ngram_scale note), so the additive scorers sum a log-probability and the per-symbol
   average (score_iter) is a cross-entropy (dits/char). Unseen n-grams are floored at
   log10(1 / total) -- scored as a single occurrence -- so an unattested gram is
   penalised like the rarest attested one rather than ruled out. Parsing stops at end of
   file or the first malformed record. */
/* Read the raw n-gram counts for one order into `table` (pre-sized to asize^n by the
   caller); returns the total count. Extracted from ngrams_read so the file-read is a
   single-purpose helper, separate from the quantisation. */
static uint64_t load_counts(int n, std::vector<uint32_t> & table, const char * suffix)
{
  char filename[1024];
  int len = snprintf(filename, sizeof(filename), "%s/%s_%s.txt",
                     opt_datadir, opt_language, suffix);
  if ((len < 0) || (len >= static_cast<int>(sizeof(filename))))
    fatal("Data directory / language path too long");

  FILE * f = fopen(filename, "r");
  if (!f)
    {
      fprintf(stderr, "Fatal error: Unable to open the language statistics file %s\n",
              filename);
      exit(1);
    }

  uint64_t total = 0;   /* sum of all counts, in uint64 (can exceed uint32) */
  int nonmappable = 0;  /* records skipped because a gram char could not fold */
  char line[256];
  while (fgets(line, sizeof(line), f))
    {
      /* One record per line: "<GRAM> <count>". FOLD each gram to its A-Z base
         (fold_gram: accents removed, e.g. u-umlaut -> U) and ACCUMULATE counts,
         since several accented grams now collide onto one base gram. The tables
         are frequency sorted and the non-English languages interleave accented
         grams (German umlauts and eszett, Danish/French accents) from near the
         top; the original parser stopped at the first such record, truncating
         e.g. the german quadgram table to its first 29 of 366k entries (4.9% of
         the count) and crippling non-English scoring. Folding keeps the whole
         distribution the 26-letter machine can represent. A record whose gram
         does not fold to exactly n A-Z letters (e.g. a stray digit) is skipped. */
      char gram[16];
      unsigned count;
      if (sscanf(line, "%15s %u", gram, & count) != 2)
        continue;
      int index;
      int r = fold_gram(gram, & index);
      if (r < 0)          /* a code point we cannot fold to A-Z: warn + skip */
        {
          nonmappable++;
          continue;
        }
      if (r != n)         /* folds to the wrong number of letters: skip quietly */
        continue;
      table[index] += count;
      total += count;
    }

  fclose(f);
  if (nonmappable > 0)
    fprintf(stderr, "Note: %s: skipped %d record(s) with non-mappable characters.\n",
            filename, nonmappable);
  return total;
}

void ngrams_read(int n, uint8_t * itable, double * bias_out, double * scale_out,
                 const char * suffix,
                 const double * force_ll = nullptr, bool force_sym = false)
{
  int size = 1;
  for (int i = 0; i < n; i++)
    size *= asize;

  /* Raw counts are accumulated here (transient -- only itable outlives this call).
     uint32 holds every count exactly (the largest in the data is ~5.3e8, well inside
     the range); float would lose precision above 2^24 ~ 16.7M. */
  std::vector<uint32_t> table(size, 0);   /* unseen: count 0 until floored below */
  uint64_t total = load_counts(n, table, suffix);

  /* Quantise log10(count / total) into itable as uint8 fixed-point. Unseen grams are
     floored at count = floor_count = 1 (a hapax): not truly impossible (corpora have
     gaps, texts have typos), a deep floor was measured not to help, and this bounds the
     range to log10(max_count) so 8 bits suffice. The per-table bias is the minimum
     stored value (log10(min_effective_count/total) -- the floor when any gram is unseen,
     else the rarest seen), so q = round((v - bias) * ngram_scale) spends all 256 levels
     on the actual range. table[] held only the raw counts (scratch). */
  const double floor_count = 1.0;   /* unseen gram == a single occurrence (a hapax) */
  if (total == 0)
    total = 1;                        /* empty/degenerate table: avoid div-by-zero */

  /* RAISED FLAT FLOOR (env ENIGMA_FLOOR = T, default 0 -> byte-identical): merge every
     low-count gram (raw count <= T) onto ONE flat floor at the count-T level, on the theory
     that counts of 1 or 2 are corpus noise, so distinguishing them from "unseen" only feeds
     the wrong boards a noisy bottom-end gradient. This REMOVES gradient (unlike the graded
     probes, which add it). T=2 collapses {0,1,2} to the count-2 value; T=0 keeps the old
     behaviour (only count-0 unseen grams floored to a hapax). */
  const char * fl = getenv("ENIGMA_FLOOR");
  const int floor_t = (fl != nullptr) ? atoi(fl) : 0;
  const double floor_val = (floor_t >= 1) ? static_cast<double>(floor_t) : floor_count;

  /* SMOOTHING PROBE (env ENIGMA_SMOOTHING): how an unseen gram is scored.
       (default) flat floor -- every unseen gram == count floor_count = 1 (byte-identical).
       laplace   -- add-one: every gram (seen too) gets +1, total -> N + V; un-merges
                    hapax (2) from unseen (1). The blank/uniform pseudocount.
       background-- grade an unseen gram by its letter-composition prior q = p(A)p(B)p(C)p(D)
                    (monogram table, which is complete -> gap-free), mapped into a bounded
                    band [BG_LO, BG_HI] BELOW a hapax so seen grams always outrank unseen.
                    The wide letter-product range is why the adaptive scale is needed. */
  const char * sm = getenv("ENIGMA_SMOOTHING");
  const bool laplace    = (sm != nullptr) && (strcmp(sm, "laplace") == 0);
  const bool background = (sm != nullptr) && (strcmp(sm, "background") == 0);
  const bool overlap    = (sm != nullptr) && (strcmp(sm, "overlap") == 0) && (n == 4);
  const double BG_HI = 0.5, BG_LO = 1e-4;   /* unseen graded within [1e-4, 0.5] < hapax */
  /* laplace add-delta (Lidstone): delta < 1 penalises unseen harder while keeping a FLAT
     floor (all unseen == delta), un-merged from a hapax (1 + delta). ENIGMA_DELTA, default 1. */
  const char * ed = getenv("ENIGMA_DELTA");
  const double delta = (ed != nullptr) ? atof(ed) : 1.0;

  double p_letter[asize]; double max_qbg = 1.0;
  if (background)
    {
      std::vector<uint32_t> mono(asize, 0);
      uint64_t mt = load_counts(1, mono, "monograms");
      if (mt == 0) mt = 1;
      double mx = 0.0;
      for (int a = 0; a < asize; a++)
        { p_letter[a] = static_cast<double>(mono[a]) / static_cast<double>(mt);
          if (p_letter[a] > mx) mx = p_letter[a]; }
      for (int j = 0; j < n; j++) max_qbg *= mx;   /* (max letter prob)^n */
      if (max_qbg <= 0.0) max_qbg = 1.0;
    }

  /* overlap-corrected back-off (quad only): estimate an unseen quad ABCD's joint prob as
     p(ABC)*p(BCD)/p(BC) -- the linear-chain Markov estimate through the shared trigram
     overlap. The estimate is rank-mapped into the SAME [BG_LO, BG_HI] band below a hapax
     as background, so seen grams always outrank unseen; only the gradient WITHIN the floor
     band differs (a well-shaped tri/bi estimate vs background's crude monogram product).
     A gram with no lower-order support (tri or bi == 0) gets q = 0 -> BG_LO (deepest). */
  std::vector<uint32_t> tri_t, bi_t;
  double tri_total = 1.0, bi_total = 1.0, max_qov = 1.0;
  auto overlap_q = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize, a = idx / (asize * asize * asize);
    uint32_t t_abc = tri_t[(a * asize + b) * asize + c3];
    uint32_t t_bcd = tri_t[(b * asize + c3) * asize + d];
    uint32_t b_bc  = bi_t[b * asize + c3];
    if (t_abc == 0 || t_bcd == 0 || b_bc == 0) return 0.0;
    double p_abc = static_cast<double>(t_abc) / tri_total;
    double p_bcd = static_cast<double>(t_bcd) / tri_total;
    double p_bc  = static_cast<double>(b_bc) / bi_total;
    return p_abc * p_bcd / p_bc;
  };
  if (overlap)
    {
      tri_t.assign(static_cast<size_t>(asize) * asize * asize, 0);
      bi_t.assign(static_cast<size_t>(asize) * asize, 0);
      uint64_t tt = load_counts(3, tri_t, "trigrams");
      uint64_t bt = load_counts(2, bi_t, "bigrams");
      tri_total = tt ? static_cast<double>(tt) : 1.0;
      bi_total  = bt ? static_cast<double>(bt) : 1.0;
      double mx = 0.0;
      for (int i = 0; i < size; i++)
        if (table[i] == 0)
          { double q = overlap_q(i); if (q > mx) mx = q; }
      max_qov = (mx > 0.0) ? mx : 1.0;
    }

  /* Jelinek-Mercer per-gram interpolation (quad only, env ENIGMA_INTERP="l4,l3,l2,l1"):
     store the interpolated CONDITIONAL probability log10 P(D|ABC) with
       P(D|ABC) = l4*c(ABCD)/c(ABC) + l3*c(BCD)/c(BC) + l2*c(CD)/c(C) + l1*c(D)/N
     (weights normalised to sum 1). The mono term (complete table) keeps P>0 for every
     quad, so no floor is needed -- unseen quads back off smoothly to the lower orders.
     Unlike the floor probes this reshapes the SEEN scores, not just the tail. Note the
     model becomes conditional (vs the default joint quad), so "off" != any l4. */
  const char * ip = getenv("ENIGMA_INTERP");
  const bool interp = (ip != nullptr) && (n == 4);

  /* Log-linear interpolation (quad only, env ENIGMA_LOGLIN="a,b,c,d"): store a WEIGHTED SUM
     of the independent joint log-scores of the four orders that a window ABCD contains,
       v(ABCD) = a*log p(ABCD) + b*log p(BCD) + c*log p(CD) + d*log p(D),
     each order's own MLE joint log-prob with the usual hapax floor. Summed over the
     message's windows this is a weighted sum of the overall quad/tri/bi/mono scores (up to a
     3-letter boundary term). Stays JOINT -- no conditional reframing -- and weights (1,0,0,0)
     are byte-identical to the default quad. A geometric (log-linear) mixture of the models. */
  /* force_ll (the SCORE_ALL "-a" table) forces the log-linear symmetric fold with baked
     weights, independent of the ENIGMA_LOGLIN env (which stays an experimental override on
     the plain quad table). */
  const char * lp = getenv("ENIGMA_LOGLIN");
  const bool loglin = ((lp != nullptr) || (force_ll != nullptr)) && (n == 4) && !interp;
  /* symmetric folding (ENIGMA_LOGLIN_SYM): fold EVERY sub-gram a window contains (2 tris,
     3 bis, 4 monos) divided by its window-multiplicity (2/3/4), instead of only the trailing
     BCD/CD/D. Interior grams net the same weight; the difference is that the leading grams at
     the text start are now included (edge grams naturally down-weighted). Same one-lookup cost. */
  const bool loglin_sym = loglin &&
    (force_ll ? force_sym : (getenv("ENIGMA_LOGLIN_SYM") != nullptr));
  double lam[4] = {1.0, 0.0, 0.0, 0.0};   /* interp: normalised lambdas; loglin: raw weights */
  std::vector<uint32_t> mono_t;
  double mono_total = 1.0;
  if (interp || loglin)
    {
      double w[4] = {0.0, 0.0, 0.0, 0.0};
      if (force_ll)
        { for (int j = 0; j < 4; j++) w[j] = force_ll[j]; }
      else
        sscanf(interp ? ip : lp, "%lf,%lf,%lf,%lf", &w[0], &w[1], &w[2], &w[3]);
      double s = w[0] + w[1] + w[2] + w[3];
      if (interp)                          /* JM linear: normalise to sum 1 */
        {
          if (s <= 0.0) { w[0] = 1.0; s = 1.0; }
          for (int j = 0; j < 4; j++) lam[j] = w[j] / s;
        }
      else                                 /* log-linear: raw weights (overall scale is free) */
        {
          if (s <= 0.0) w[0] = 1.0;
          for (int j = 0; j < 4; j++) lam[j] = w[j];
        }
      if (tri_t.empty()) tri_t.assign(static_cast<size_t>(asize) * asize * asize, 0);
      if (bi_t.empty())  bi_t.assign(static_cast<size_t>(asize) * asize, 0);
      mono_t.assign(asize, 0);
      uint64_t tt = load_counts(3, tri_t, "trigrams");
      uint64_t bt = load_counts(2, bi_t, "bigrams");
      uint64_t mt = load_counts(1, mono_t, "monograms");
      tri_total  = tt ? static_cast<double>(tt) : 1.0;
      bi_total   = bt ? static_cast<double>(bt) : 1.0;
      mono_total = mt ? static_cast<double>(mt) : 1.0;
    }
  /* joint log10(count/total) of one order with the hapax floor (unseen -> single occurrence) */
  auto jlog = [](uint32_t c, double tot) -> double
  { return log10((c > 0 ? static_cast<double>(c) : 1.0) / tot); };
  auto loglin_v = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize;   /* v depends only on B,C,D (+ full quad) */
    double vq = jlog(table[idx], static_cast<double>(total));
    double vt = jlog(tri_t[(b * asize + c3) * asize + d], tri_total);
    double vb = jlog(bi_t[c3 * asize + d], bi_total);
    double vm = jlog(mono_t[d], mono_total);
    return lam[0] * vq + lam[1] * vt + lam[2] * vb + lam[3] * vm;
  };
  /* symmetric folding: all sub-grams of ABCD, each order divided by its window-multiplicity
     (tri/2, bi/3, mono/4) so interior grams net weight (b,c,d) and edge grams scale down. */
  auto loglin_v_sym = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize, a = idx / (asize * asize * asize);
    double vq = jlog(table[idx], static_cast<double>(total));
    double vt = jlog(tri_t[(a * asize + b) * asize + c3], tri_total)
              + jlog(tri_t[(b * asize + c3) * asize + d], tri_total);
    double vb = jlog(bi_t[a * asize + b], bi_total)
              + jlog(bi_t[b * asize + c3], bi_total)
              + jlog(bi_t[c3 * asize + d], bi_total);
    double vm = jlog(mono_t[a], mono_total) + jlog(mono_t[b], mono_total)
              + jlog(mono_t[c3], mono_total) + jlog(mono_t[d], mono_total);
    return lam[0] * vq + lam[1] * vt / 2.0 + lam[2] * vb / 3.0 + lam[3] * vm / 4.0;
  };
  auto interp_P = [&](int idx) -> double
  {
    int d = idx % asize, c3 = (idx / asize) % asize;
    int b = (idx / (asize * asize)) % asize, a = idx / (asize * asize * asize);
    uint32_t q_abcd = table[idx];
    uint32_t t_abc = tri_t[(a * asize + b) * asize + c3];
    uint32_t t_bcd = tri_t[(b * asize + c3) * asize + d];
    uint32_t b_bc  = bi_t[b * asize + c3];
    uint32_t b_cd  = bi_t[c3 * asize + d];
    double p4 = (t_abc > 0) ? static_cast<double>(q_abcd) / t_abc : 0.0;
    double p3 = (b_bc  > 0) ? static_cast<double>(t_bcd)  / b_bc  : 0.0;
    double p2 = (mono_t[c3] > 0) ? static_cast<double>(b_cd) / mono_t[c3] : 0.0;
    double p1 = static_cast<double>(mono_t[d]) / mono_total;
    double P = lam[0] * p4 + lam[1] * p3 + lam[2] * p2 + lam[3] * p1;
    return (P > 0.0) ? P : 1.0 / mono_total;   /* backstop (p1>0 whenever letter D is seen) */
  };

  /* effective count of gram idx (raw count c) under the selected smoothing */
  auto eff_count = [&](int idx, double c) -> double
  {
    if (laplace) return c + delta;
    if (background || overlap)
      {
        if (c > 0.0) return c;                       /* seen: keep MLE */
        double q, denom;
        if (overlap) { q = overlap_q(idx); denom = max_qov; }
        else                                         /* background: letter-prior product */
          {
            q = 1.0; int t = idx;
            for (int j = 0; j < n; j++) { q *= p_letter[t % asize]; t /= asize; }
            denom = max_qbg;
          }
        double bg = q * (BG_HI / denom);
        return (bg < BG_LO) ? BG_LO : (bg > BG_HI ? BG_HI : bg);
      }
    return (c > floor_t) ? c : floor_val;            /* default flat floor (raisable) */
  };

  const double eff_total = laplace ? static_cast<double>(total) + delta * size
                                   : static_cast<double>(total);
  const double log_total = log10(eff_total);

  /* per-gram stored log-value: interpolation stores log10 P(D|ABC) directly; every other
     mode stores log10(effective_count / total). Byte-identical to the old two-line form for
     the non-interp path (log is monotonic, so min/max of the count == min/max of the value). */
  auto logval = [&](int idx) -> double
  {
    if (interp) return log10(interp_P(idx));
    if (loglin) return loglin_sym ? loglin_v_sym(idx) : loglin_v(idx);
    return log10(eff_count(idx, table[idx])) - log_total;
  };

  double vmin = 1e300, vmax = -1e300;
  for (int i = 0; i < size; i++)
    {
      double v = logval(i);
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
  if (vmin > vmax) { vmin = 0.0; vmax = 0.0; }   /* degenerate guard (empty table) */
  double bias = vmin;
  *bias_out = bias;

  /* Adaptive per-table scale: map the whole [vmin, vmax] span onto the full 0..255 byte
     range. With graded smoothing / interpolation the span shifts, which is exactly why the
     scale must adapt. Guard span==0 with the old fixed 32. */
  double span = vmax - vmin;
  double scale = (span > 0.0) ? 255.0 / span : 32.0;
  *scale_out = scale;

  for (int i = 0; i < size; i++)
    {
      double q = (logval(i) - bias) * scale;
      if (q < 0.0)
        q = 0.0;
      else if (q > 255.0)
        q = 255.0;
      itable[i] = static_cast<uint8_t>(q < 0.0 ? q - 0.5 : q + 0.5);
    }
}


/* --- Restart transposition table (--restart-tt) -----------------------------
   A Zobrist-hashed table of converged restart plugboards, for measuring restart
   basin collapse in-binary: the distinct-optima dedup the external --dump-restarts
   harness does, plus the full hit count per basin. Diagnostic only -- it never
   affects which candidate wins, so results stay -T-deterministic; and since the
   multiset of converged boards is a deterministic function of the work items, the
   distinct-count and hit-histogram are themselves -T-invariant. The Zobrist words
   are a FIXED deterministic table (splitmix64 from a constant, not random_device
   or opt_seed) so the hash and every stat derived from it are reproducible. Each
   bucket stores the exact board (so a hash collision is a probe-on, never a false
   merge), its score, and how many restarts collapsed onto it. */
static const int g_npairs = asize * (asize - 1) / 2;   /* 325 unordered letter pairs */
static uint64_t g_zobrist[g_npairs];

static void init_zobrist()
{
  uint64_t s = 0x9E3779B97F4A7C15ULL;   /* fixed seed -> reproducible across runs/threads */
  for (int i = 0; i < g_npairs; i++)
    {
      s += 0x9E3779B97F4A7C15ULL;        /* splitmix64 */
      uint64_t z = s;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      z ^= z >> 31;
      g_zobrist[i] = z;
    }
}

/* Index of the unordered pair (a<b) in the lexicographic 0..324 order make_pairtab
   enumerates (i<j): pairs with first element < a, plus the offset within a. */
static inline int pair_index(int a, int b)
{
  return a * (asize - 1) - a * (a - 1) / 2 + (b - a - 1);
}

/* XOR of the Zobrist word of every set plug pair (each counted once, low<high). */
static uint64_t board_hash(const unsigned char * steck)
{
  uint64_t h = 0;
  for (int j = 0; j < asize; j++)
    if (steck[j] > j)
      h ^= g_zobrist[pair_index(j, steck[j])];
  return h;
}

struct tt_bucket
{
  unsigned char board[asize];
  double score;
  uint32_t count;
  bool occupied;
};

struct restart_tt
{
  tt_bucket * slots = nullptr;
  size_t mask = 0;          /* size - 1 (size is a power of two) */
  size_t nentries = 0;      /* distinct converged boards */
  size_t nclimbs = 0;       /* total touches */
  bool full = false;        /* set if we ever ran out of slots (then stop inserting) */
};

/* next power of two >= want, clamped to [1024, 4M slots (~160 MB)] */
static size_t tt_size_for(size_t want)
{
  size_t n = 1024;
  while (n < want && n < (static_cast<size_t>(1) << 22))
    n <<= 1;
  return n;
}

static void tt_alloc(restart_tt & tt, size_t want)
{
  size_t n = tt_size_for(want);
  tt.slots = new tt_bucket[n]();   /* value-init: occupied == false everywhere */
  tt.mask = n - 1;
  tt.nentries = 0;
  tt.nclimbs = 0;
  tt.full = false;
}

static void tt_free(restart_tt & tt)
{
  delete[] tt.slots;
  tt.slots = nullptr;
}

/* Insert the board or bump its counter. Open addressing, linear probe, exact compare. */
static void tt_touch(restart_tt & tt, const unsigned char * steck, double score)
{
  tt.nclimbs++;
  uint64_t h = board_hash(steck);
  size_t i = h & tt.mask;
  size_t probed = 0;
  while (tt.slots[i].occupied)
    {
      if (memcmp(tt.slots[i].board, steck, asize) == 0)
        {
          tt.slots[i].count++;
          return;
        }
      i = (i + 1) & tt.mask;
      if (++probed > tt.mask)          /* table full -> give up (should not happen if sized right) */
        { tt.full = true; return; }
    }
  memcpy(tt.slots[i].board, steck, asize);
  tt.slots[i].score = score;
  tt.slots[i].count = 1;
  tt.slots[i].occupied = true;
  tt.nentries++;
}

/* Render a board canonically (each pair low-high, pairs ordered by low letter). */
static void tt_board_str(const unsigned char * board, char * out)
{
  char * p = out;
  for (int j = 0; j < asize; j++)
    if (board[j] > j)
      {
        if (p > out)
          *p++ = ' ';
        *p++ = num2char(j);
        *p++ = num2char(static_cast<int>(board[j]));
      }
  *p = 0;
}

/* Env-gated detailed dump (ENIGMA_TT_DUMP): table load, per-basin score stats, the hit
   histogram (hits -> #basins), and the top basins by hit count. Off the hot path. */
static void tt_dump_verbose(const restart_tt & tt)
{
  std::vector<const tt_bucket *> b;
  for (size_t i = 0; i <= tt.mask; i++)
    if (tt.slots[i].occupied)
      b.push_back(& tt.slots[i]);
  if (b.empty())
    return;

  uint32_t maxc = 0;
  double smin = 1e300, smax = -1e300, ssum = 0.0;
  for (const tt_bucket * e : b)
    {
      if (e->count > maxc) maxc = e->count;
      if (e->score < smin) smin = e->score;
      if (e->score > smax) smax = e->score;
      ssum += e->score;
    }
  std::vector<size_t> hist(maxc + 1, 0);
  for (const tt_bucket * e : b)
    hist[e->count]++;

  std::sort(b.begin(), b.end(), [](const tt_bucket * x, const tt_bucket * y)
            { if (x->count != y->count) return x->count > y->count;
              return x->score > y->score; });

  size_t size = tt.mask + 1;
  fprintf(stderr, "  table: %zu slots, %zu entries, load %.3f%s\n",
          size, tt.nentries, static_cast<double>(tt.nentries) / static_cast<double>(size),
          tt.full ? " (FULL)" : "");
  fprintf(stderr, "  per-basin score: min %.4f  mean %.4f  max %.4f\n",
          smin, ssum / static_cast<double>(b.size()), smax);
  fprintf(stderr, "  hit histogram (hits:#basins):");
  for (uint32_t h = 1; h <= maxc; h++)
    if (hist[h])
      fprintf(stderr, " %u:%zu", h, hist[h]);
  fprintf(stderr, "\n");
  int K = b.size() < 10 ? static_cast<int>(b.size()) : 10;
  fprintf(stderr, "  top %d basins (hits  score  board):\n", K);
  char board[3 * 13];
  for (int k = 0; k < K; k++)
    {
      tt_board_str(b[k]->board, board);
      fprintf(stderr, "    %4u  %.4f  %s\n", b[k]->count, b[k]->score, board);
    }
}

/* Summarise basin collapse: distinct optima, the heaviest basin, and Shannon entropy of
   the hit distribution (uniform over N basins -> log2 N; all-collapsed -> 0). */
static void tt_report(const restart_tt & tt)
{
  if (tt.nclimbs == 0)
    return;
  uint32_t maxc = 0;
  double H = 0.0;
  double tot = static_cast<double>(tt.nclimbs);
  for (size_t i = 0; i <= tt.mask; i++)
    if (tt.slots[i].occupied)
      {
        uint32_t c = tt.slots[i].count;
        if (c > maxc)
          maxc = c;
        double p = static_cast<double>(c) / tot;
        H -= p * log2(p);
      }
  fprintf(stderr,
          "restart-tt: %zu distinct optima over %zu climbs "
          "(max %u hits on one basin, entropy %.2f bits%s)\n",
          tt.nentries, tt.nclimbs, maxc, H, tt.full ? ", TABLE FULL -- undercount" : "");
  if (getenv("ENIGMA_TT_DUMP") != nullptr)
    tt_dump_verbose(tt);
}

static restart_tt g_restart_tt;
static std::mutex g_tt_mutex;

/* --- machine model: setup, rotor stepping, precompute ------------------- */

void init()
{
  for (int i=0; i < rotor_count; i++)
    for (int j=0; j < asize; j++)
      {
        rotor_fwd[i][j] = char2num(rotor_string[i][j]);
        rotor_rev[i][j] = strchr(rotor_string[i],num2char(j)) - rotor_string[i];
        notch[i][j] = strchr(notch_string[i], num2char(j)) != NULL;
      }

  for (int i=0; i < reflector_count; i++)
    for (int j=0; j < asize; j++)
      reflector[i][j] = char2num(reflector_string[i][j]);

  init_zobrist();
}

/* Reset the plugboard to identity + the fixed -s pairs. Board-only (the fixed-letter set is
   the separate plug_fixed, below), so the init-dominated scan path pays no extra cost. */
void init_steckerbrett(machine & m, const char * steckerbrett_string)
{
  for (int j=0; j < asize; j++)
    m.steckerbrett[j] = static_cast<unsigned char>(j);

  int plug_count = static_cast<int>(strlen(steckerbrett_string) / 2);

  for (int i=0; i < plug_count; i++)
    {
      int a = char2num(steckerbrett_string[2*i+0]);
      int b = char2num(steckerbrett_string[2*i+1]);
      m.steckerbrett[a] = static_cast<unsigned char>(b);
      m.steckerbrett[b] = static_cast<unsigned char>(a);
    }
}

/* Letters the climb/SA must not rewire. plug_fixed is the fixed -s set: a plain read-only
   global, set once by init_plug_fixed before the search and shared by every worker (they only
   read it). The common (non-exhaust) climb reads this global directly, via a local
   `const bool * __restrict pf = plug_fixed` inside each climb function; that plain-global read
   is what clang and g++ both compile without reloading it after the steckerbrett stores in the
   move loop -- reading a struct member or thread_local there instead shifts a compiler's
   codegen ~18% (see CLAUDE.md and the PLUG_FIXED_EX note).

   --exhaust needs PER-WORKER forced pins (each parallel first-pair unit forces different pairs),
   so the EX=true climb instantiations read PLUG_FIXED_EX -- a per-worker copy of plug_fixed plus
   this leaf's forced pairs -- selected at compile time (EX ? PLUG_FIXED_EX : plug_fixed), so the
   common EX=false path folds to the plain global. WHERE that per-worker copy lives is compiler-
   dependent, and the two compilers disagree: clang wants it thread_local (a struct member costs
   its climb ~18%), g++ wants it a machine member (a thread_local costs g++'s whole-TU codegen
   ~19%). We give each what it wants; the exhaust path is a dominated exploration tool, so its
   own codegen does not matter -- only that the common path stays a plain global. */
static bool plug_fixed[asize];
#if defined(__clang__)
static thread_local bool plug_fixed_ex[asize];   /* clang: thread_local scratch */
#define PLUG_FIXED_EX plug_fixed_ex
#else
#define PLUG_FIXED_EX m.plug_fixed_ex           /* g++: the machine member declared above */
#endif

void init_plug_fixed(const char * steckerbrett_string)
{
  for (int j = 0; j < asize; j++)
    plug_fixed[j] = false;
  int plug_count = static_cast<int>(strlen(steckerbrett_string) / 2);
  for (int i = 0; i < plug_count; i++)
    {
      plug_fixed[char2num(steckerbrett_string[2*i+0])] = true;
      plug_fixed[char2num(steckerbrett_string[2*i+1])] = true;
    }
}

void init_walzen(machine & m, int u, int a, int b, int c)
{
  if (opt_norenigma)
    {
      m.ukw = norway_reflector_index + u;
      m.walzenlage[0] = norway_rotor_base + a;
      m.walzenlage[1] = norway_rotor_base + b;
      m.walzenlage[2] = norway_rotor_base + c;
    }
  else
    {
      m.ukw = u;
      m.walzenlage[0] = a;
      m.walzenlage[1] = b;
      m.walzenlage[2] = c;
    }
}

void init_ring_grund(machine & m, int a, int b, int c, int x, int y, int z)
{
  m.ringstellung[0] = a;
  m.ringstellung[1] = b;
  m.ringstellung[2] = c;
  m.grundstellung[0] = x;
  m.grundstellung[1] = y;
  m.grundstellung[2] = z;
}

int rotor_l(machine & m, int x, int rotor_no)
{
  int y = m.grundstellung[rotor_no] - m.ringstellung[rotor_no];
  x = (x + asize + y) % asize;
  x = rotor_fwd[m.walzenlage[rotor_no]][x];
  x = (x + asize - y) % asize;
  return x;
}

int rotor_r(machine & m, int x, int rotor_no)
{
  int y = m.grundstellung[rotor_no] - m.ringstellung[rotor_no];
  x = (x + asize + y) % asize;
  x = rotor_rev[m.walzenlage[rotor_no]][x];
  x = (x + asize - y) % asize;
  return x;
}

inline int mod26(int x)
{
  return (x+asize)%asize;
}

inline int subst_rotors(machine & m, int x)
{
  for (int r = wheels - 1; r >= 0; r--)
    x = rotor_l(m, x, r);

  x = m.reflector_eff[x];

  for(int r = 0; r < wheels; r++)
    x = rotor_r(m, x, r);

  return x;
}

/* Resolve the effective reflector for this machine's reflector / Greek wheel.
   Called once per task (before precompute), never per character. Standard and
   Norway modes just copy the wired reflector. In M4 the static Greek wheel folds
   into the thin reflector: the signal passes through the Greek wheel (at its fixed
   offset, forward like rotor_l), the thin reflector, then back through the Greek
   wheel (reverse like rotor_r). greek o thin o greek^-1 is the conjugate of an
   involution, so it is still a valid (involutory) reflector. */
void set_effective_reflector(machine & m)
{
  if (! opt_m4)
    {
      memcpy(m.reflector_eff, reflector[m.ukw], asize);
      return;
    }

  const int o = m.greek_offset;
  const unsigned char * thin = reflector[m.ukw];      /* ukw = thin index 4/5 */
  const unsigned char * gf = rotor_fwd[m.greek];
  const unsigned char * gr = rotor_rev[m.greek];
  for (int x = 0; x < asize; x++)
    {
      int a = mod26(gf[mod26(x + o)] - o);   /* Greek forward  (rotor_l style) */
      int b = thin[a];                       /* thin reflector                 */
      int c = mod26(gr[mod26(b + o)] - o);   /* Greek reverse  (rotor_r style) */
      m.reflector_eff[x] = static_cast<unsigned char>(c);
    }
}

void precompute(machine & m)
{
  int r1 = 0;
  int r2 = 0;
  int r3 = 0;
  for (int g1 = 0; g1 < asize; g1++)
    for (int g2 = 0; g2 < asize; g2++)
      for (int g3 = 0; g3 < asize; g3++)
        {
          init_ring_grund(m, r1, r2, r3, g1, g2, g3);
          for (int x = 0; x < asize; x++)
            m.subst_array[g1][g2][g3][x] = subst_rotors(m, x);
        }
}

/* Step the rotors over the message and record, per character position, a pointer
   to its rotor-stack substitution row (which depends only on the start-minus-ring
   offsets, so it is the same for all 26 input letters).

   The scan (no plugboard hill-climb) reads each position's row at most a couple
   of times and only ever at the single index steckerbrett[ciphertext[i]], so it
   just points rows[i] straight into the shared subst_array -- no per-position
   copy. Hill-climbing re-reads each row hundreds of times at varying indices as
   it permutes the plugboard, so with `copy_rows` it copies the 26-byte row into
   the contiguous mapping[] (better locality across the climb) and points there.

   The stepping state is held in plain locals for the duration of the loop rather
   than in m.grundstellung: the previous per-character read/modify/write through
   the struct could not be proven not to alias the stores, which serialised the
   loop and cost ~10-14% on the search path (worst on ARM). Locals let the
   compiler keep the rotor positions in registers; the final positions are
   written back once at the end. */
void setup_mapping(machine & m, bool copy_rows)
{
  if (textlength > maxlen)
    fatal("Ciphertext too long");

  /* A new rotor stack is being installed: every score_iter value now differs, so bump
     the --score-tt generation to invalidate the whole cache in O(1). setup_mapping is
     called exactly once per key (its restarts reuse the same rows), so the cache
     persists across a key's restarts -- which is where the memoisation pays off. */
  if (m.sc_cache)
    m.sc_gen++;

  const unsigned char (* __restrict sa)[asize][asize][asize] = m.subst_array;
  const unsigned char ** __restrict rows = m.rows;
  const int w1 = m.walzenlage[1];   /* middle rotor (notch checked for stepping) */
  const int w2 = m.walzenlage[2];   /* right rotor  */
  const int r0 = m.ringstellung[0];
  const int r1 = m.ringstellung[1];
  const int r2 = m.ringstellung[2];
  int g0 = m.grundstellung[0];
  int g1 = m.grundstellung[1];
  int g2 = m.grundstellung[2];

  for (int i = 0; i < textlength; i++)
    {
      /* stepping schedule including the Enigma double-stepping anomaly: the
         middle rotor advances (carrying the left one) when it sits on its own
         notch, as well as on the usual right-rotor carry */
      if (notch[w1][g1])
        {
          g0 = mod26(1 + g0);
          g1 = mod26(1 + g1);
        }
      else if (notch[w2][g2])
        {
          g1 = mod26(1 + g1);
        }
      g2 = mod26(1 + g2);

      const unsigned char * row =
        sa[mod26(g0 - r0)][mod26(g1 - r1)][mod26(g2 - r2)];
      if (copy_rows)
        {
          memcpy(m.mapping[i], row, asize);
          rows[i] = m.mapping[i];
        }
      else
        rows[i] = row;
    }

  m.grundstellung[0] = static_cast<unsigned char>(g0);
  m.grundstellung[1] = static_cast<unsigned char>(g1);
  m.grundstellung[2] = static_cast<unsigned char>(g2);
}

/* Decode one ciphertext position: plugboard -> per-position rotor-stack row ->
   plugboard. A tiny inline so decode() and the scorers share one copy of the
   formula. rows[i] is the position's substitution row (in subst_array for the
   scan, or the contiguous mapping[] for hill-climb). The scorers fuse this into
   their loops (see below) so the decoded text is never materialised in a scratch
   array. The base pointers are __restrict locals the callers have hoisted out of
   struct machine. */
inline int decode_at(const unsigned char * __restrict steck,
                     const unsigned char * const * __restrict rows,
                     const unsigned char * __restrict ct,
                     int i)
{
  return steck[rows[i][steck[ct[i]]]];
}

inline void decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  char * __restrict pt = m.plaintext;
  for (int i = 0; i < textlength; i++)
    pt[i] = num2char(decode_at(steck, rows, ct, i));
  pt[textlength] = 0;
}

/* Known-word ("crib") bonus for a converged board: sum the weights of every occurrence of
   a known word as a SUBSTRING of the decrypt. Substring (not token) matching is deliberate
   -- telegraphic traffic concatenates words within a clause (ROEMEINSBERTA, not
   ROEM.X.EINS.X.BERTA; X separates clauses, not words), so token matching would miss them.
   Read from m.plaintext, which the climb leaves holding the converged board's decrypt, so
   no extra decode. Deterministic (a pure function of the board and the fixed word list),
   hence -T-invariant. Only called when opt_crib is set, so the default path is untouched.
   Longer words carry more weight (see the crib file), which keeps rare short-word
   coincidences on garbage from swamping the genuine multi-word signal on the true board. */
static double crib_score(const machine & m)
{
  const char * __restrict pt = m.plaintext;
  double s = 0.0;
  for (const std::pair<std::string, double> & cw : g_cribs)
    {
      const int k = static_cast<int>(cw.first.size());
      const char * __restrict w = cw.first.data();
      for (int i = 0; i + k <= textlength; i++)
        if (memcmp(pt + i, w, static_cast<size_t>(k)) == 0)
          {
            s += cw.second;   /* presence: each distinct word counts once, so a garbage
                                 board cannot win by repeating one coincidental short match */
            break;
          }
    }
  return s;
}

/* Load the known-word list for --crib-file: one word per line, an optional weight after
   it (default 1.0); '#' starts a comment. Words are folded to A-Z uppercase (matching the
   ciphertext/plaintext readers). Populates g_cribs and sets opt_crib. */
static void load_cribs(const char * fname)
{
  FILE * f = fopen(fname, "r");
  if (f == nullptr)
    fatal("Cannot open crib file");
  char line[256];
  while (fgets(line, sizeof line, f) != nullptr)
    {
      char * p = line;
      while ((*p == ' ') || (*p == '\t'))
        p++;
      if ((*p == '#') || (*p == '\n') || (*p == '\0'))
        continue;
      std::string word;
      while ((*p != '\0') && (*p != ' ') && (*p != '\t') && (*p != '\n'))
        {
          int u = toupper(static_cast<unsigned char>(*p));
          if ((u >= 'A') && (u <= 'Z'))
            word += static_cast<char>(u);
          p++;
        }
      double w = 1.0;
      while ((*p == ' ') || (*p == '\t'))
        p++;
      if ((*p != '\0') && (*p != '\n') && (*p != '#'))
        {
          char * end = nullptr;
          double v = strtod(p, &end);
          if (end != p)
            w = v;
        }
      if (! word.empty())
        g_cribs.emplace_back(word, w);
    }
  fclose(f);
  opt_crib = g_cribs.empty() ? 0 : 1;
}

/* --- plaintext scoring models ------------------------------------------- */

/* The four n-gram scorers fuse decoding into the score loop: each character is
   decoded once, on the fly, into a small sliding window of the last n decoded
   letters that indexes the n-gram table -- so the decoded message is never
   written to and re-read from a scratch array. The quadgram scorer is ~99% of
   hill-climb runtime. The short-text guards (textlength < n) keep the n-1
   pre-roll decodes in bounds and reproduce the old `i < textlength-(n-1)` loops
   (which simply ran zero times for shorter input). */

double quadgram_score_decode(machine & m)
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
double allgram_score_decode(machine & m)
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

double trigram_score_decode(machine & m)
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

double bigram_score_decode(machine & m)
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

double monogram_score_decode(machine & m)
{
  const unsigned char * __restrict ct = num_ciphertext;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;

  long isum = 0;
  for (int i = 0; i < textlength; i++)
    isum += mono8[decode_at(steck, rows, ct, i)];
  return static_cast<double>(isum) / ngram_scale[SCORE_MONO] + textlength * ngram_bias[SCORE_MONO];
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

  double score = 0.0;
  for(int j=0; j<asize; j++)
    score += static_cast<double>(freq[j]) * (freq[j] - 1);
  return (textlength > 1)
    ? score / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;
}

/* Progress-line columns (shared by the header and the lines): score %7.4f,
   reflector+wheels up to 5 chars (M4 "bB123"), ring and start up to 4 (M4),
   plugboard up to 13 pairs = 38 chars, then the first preview_len characters
   of the decoded text. Worst case 7+1+5+1+4+1+4+1+38+1+15 = 78 chars, inside
   a 79-column terminal. */
static const int preview_len = 15;
static const char progress_fmt[] = "%7s %-5s %-4s %-4s %-38s %s\n";

/* Column header, printed once before the first progress line of a search. */
void showconfig_header(void)
{
  fprintf(stderr, progress_fmt, "Score", "W", "R", "G", "S", "Text");
}

void showconfig(machine & m, double score)
{
  char w[8], r[8], g[8], s[3 * 13], text[preview_len + 1];

  if (opt_m4)
    {
      /* M4: thin reflector (b/c) + static Greek wheel (B/G). Only the Greek
         (start - ring) offset is identifiable, so it is shown as start=offset,
         ring=A. The reflector/Greek/ring/start columns list the Greek first. */
      /* wheel numbers are single digits (1-8), printed as chars so the
         compiler can see the buffer cannot truncate */
      snprintf(w, sizeof(w), "%c%c%c%c%c",
               (m.ukw == m4_thin_base) ? 'b' : 'c',
               (m.greek == greek_base) ? 'B' : 'G',
               '1' + m.walzenlage[0],
               '1' + m.walzenlage[1],
               '1' + m.walzenlage[2]);
      snprintf(r, sizeof(r), "A%c%c%c",
               num2char(m.ringstellung[0]),
               num2char(m.ringstellung[1]),
               num2char(m.ringstellung[2]));
      snprintf(g, sizeof(g), "%c%c%c%c",
               num2char(m.greek_offset),
               num2char(m.grundstellung[0]),
               num2char(m.grundstellung[1]),
               num2char(m.grundstellung[2]));
    }
  else
    {
      /* display wheel numbers 1..N: standard rotors are index+1, Norway wheels
         are index - norway_rotor_base + 1; the reflector prints as its letter
         (N for Norway, else A/B/C). */
      int wheel_offset = opt_norenigma ? 1 - norway_rotor_base : 1;
      snprintf(w, sizeof(w), "%c%c%c%c",
               opt_norenigma ? 'N' : num2char(m.ukw),
               static_cast<char>('0' + m.walzenlage[0] + wheel_offset),
               static_cast<char>('0' + m.walzenlage[1] + wheel_offset),
               static_cast<char>('0' + m.walzenlage[2] + wheel_offset));
      snprintf(r, sizeof(r), "%c%c%c",
               num2char(m.ringstellung[0]),
               num2char(m.ringstellung[1]),
               num2char(m.ringstellung[2]));
      snprintf(g, sizeof(g), "%c%c%c",
               num2char(m.grundstellung[0]),
               num2char(m.grundstellung[1]),
               num2char(m.grundstellung[2]));
    }

  char * p = s;
  for (int j = 0; j < asize; j++)
    if (m.steckerbrett[j] > j)
      {
        if (p > s)
          *p++ = ' ';
        *p++ = num2char(j);
        *p++ = num2char(m.steckerbrett[j]);
      }
  *p = 0;

  /* Decode the text preview on the fly from the machine's CURRENT board --
     m.plaintext can be stale here (inside a running climb it still holds an
     earlier candidate). */
  const unsigned char * steck = m.steckerbrett;
  const unsigned char * const * rows = m.rows;
  int n = (textlength < preview_len) ? textlength : preview_len;
  for (int i = 0; i < n; i++)
    text[i] = num2char(decode_at(steck, rows, num_ciphertext, i));
  text[n] = 0;

  char scorebuf[16];
  snprintf(scorebuf, sizeof(scorebuf), "%.4f", score);
  fprintf(stderr, progress_fmt, scorebuf, w, r, g, s, text);
}

double score_iter(machine & m)
{
  m.plugboards_scored++;   /* diagnostic count (once per whole-message score) */

  /* --score-tt: consult the plugboard score cache first. The key is the Zobrist hash
     of the plugboard; a hit additionally requires the exact board, the current rotor-key
     generation and the scoring model to match, so the stored value is EXACTLY what this
     call would otherwise compute (a hash collision or a stale generation is a miss, never
     a wrong score). On a hit the whole decode/score loop is skipped -- that is the work
     the cache saves. Direct-mapped: `slot` is the (possibly evicted) home slot, reused
     for the store below so the board hash is computed only once. */
  sc_slot * slot = nullptr;
  uint64_t h = 0;
  if (m.sc_cache)
    {
      h = board_hash(m.steckerbrett);
      slot = & m.sc_cache[h & sc_mask];
      m.sc_lookups++;
      if ((slot->gen == m.sc_gen) && (slot->tag == h)
          && (slot->scoring == m.scoring)
          && (memcmp(slot->board, m.steckerbrett, asize) == 0))
        {
          m.sc_hits++;
          return slot->score;
        }
    }

  double score = 0;
  int nterms = 0;   /* number of n-gram terms; 0 = no per-symbol normalisation (IC) */

  switch(m.scoring)
    {
    case SCORE_IC:
      score = ic_score_decode(m);   /* already a normalised ratio; left as-is */
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

  /* Store the freshly computed score in its home slot (evicting whatever aliased there).
     Correctness never depends on the stored contents -- the gen/tag/board/model guard on
     lookup rejects any mismatch -- so eviction only ever costs a recomputation. */
  if (slot)
    {
      slot->tag = h;
      slot->score = score;
      slot->gen = m.sc_gen;
      slot->scoring = static_cast<uint8_t>(m.scoring);
      memcpy(slot->board, m.steckerbrett, asize);
    }

  return score;
}

/* --- plugboard hill-climb (steckerbrett) -------------------------------- */

/* Last-resort "re-pair" move: take two existing plugs {a-x},{b-y} to the OTHER
   pairing of their four letters ({a-b,x-y} or {a-y,x-b}), keeping the plug count. A
   single switch cannot reach these (it would first drop to one plug, often a worse
   intermediate the greedy climb never takes), so this crosses a barrier two single
   moves cannot. It is run only once the cheap swap/remove moves have converged -- a
   handful of times per climb, not every pass -- so its O(plugs^2) cost is small.
   Applies and returns true iff the single best re-pair strictly beats cur_score. */
/* Defined after best_result (below the search structs); climbs call it on every
   accepted move to echo intermediate plugboard improvements. */
static void report_climb_progress(machine & m, double score);

template<bool EX>
static bool try_repair(machine & m, double cur_score)
{
  const bool * __restrict pf = EX ? PLUG_FIXED_EX : plug_fixed;
  int plo[asize / 2];
  int phi[asize / 2];
  int np = 0;
  for (int a = 0; a < asize; a++)
    if ((m.steckerbrett[a] > a) && ! pf[a])   /* never rewire a fixed -s plug */
      {
        plo[np] = a;
        phi[np] = m.steckerbrett[a];
        np++;
      }

  double best = cur_score;
  int rp_pos[4] = { 0, 0, 0, 0 };
  int rp_val[4] = { 0, 0, 0, 0 };
  bool found = false;

  for (int i = 0; i < np; i++)
    for (int j = i + 1; j < np; j++)
      {
        int a = plo[i], x = phi[i], b = plo[j], y = phi[j];

        /* M1: {a-b, x-y} */
        m.steckerbrett[a] = b; m.steckerbrett[b] = a;
        m.steckerbrett[x] = y; m.steckerbrett[y] = x;
        double s1 = score_iter(m);
        if (s1 > best)
          {
            best = s1; found = true;
            rp_pos[0] = a; rp_val[0] = b; rp_pos[1] = b; rp_val[1] = a;
            rp_pos[2] = x; rp_val[2] = y; rp_pos[3] = y; rp_val[3] = x;
          }

        /* M2: {a-y, x-b} */
        m.steckerbrett[a] = y; m.steckerbrett[y] = a;
        m.steckerbrett[x] = b; m.steckerbrett[b] = x;
        double s2 = score_iter(m);
        if (s2 > best)
          {
            best = s2; found = true;
            rp_pos[0] = a; rp_val[0] = y; rp_pos[1] = y; rp_val[1] = a;
            rp_pos[2] = x; rp_val[2] = b; rp_pos[3] = b; rp_val[3] = x;
          }

        /* restore {a-x, b-y} */
        m.steckerbrett[a] = x; m.steckerbrett[x] = a;
        m.steckerbrett[b] = y; m.steckerbrett[y] = b;
      }

  if (found)
    {
      for (int k = 0; k < 4; k++)
        m.steckerbrett[rp_pos[k]] = static_cast<unsigned char>(rp_val[k]);
      report_climb_progress(m, best);
    }
  return found;
}

/* The 8 GENUINE rematchings of three plugs, indexing the six letters L[] =
   {a,x, b,y, c,z} (original pairs (0,1),(2,3),(4,5)). Each row is three pairs of L[]
   indices forming a perfect matching that shares NO edge with the original -- i.e. every
   letter changes partner. (The other six of the 15 matchings keep one original pair and
   are just a two-plug re-pair, already covered by try_repair, so they are omitted.) */
static const unsigned char REMATCH3[8][6] =
{
  { 0, 2, 1, 4, 3, 5 }, { 0, 2, 1, 5, 3, 4 },
  { 0, 3, 1, 4, 2, 5 }, { 0, 3, 1, 5, 2, 4 },
  { 0, 4, 1, 3, 2, 5 }, { 0, 4, 1, 2, 3, 5 },
  { 0, 5, 1, 3, 2, 4 }, { 0, 5, 1, 2, 3, 4 },
};

/* try_repair_3 (--repair3): the 3-plug generalisation of try_repair, tried only as a
   last-resort barrier cross once the cheap climb AND try_repair have both converged.
   Takes three existing (non-fixed) plugs -- six letters -- and scores every genuine
   count-neutral rematch (the 8 above); keeps the single best strictly-improving one.
   Count-neutral, so no cap gating is needed. Cost O(C(np,3)*8) score_iter per call
   (960 at 10 plugs), paid only at convergence, so ~zero amortised -- like try_repair. */
template<bool EX>
static bool try_repair_3(machine & m, double cur_score)
{
  const bool * __restrict pf = EX ? PLUG_FIXED_EX : plug_fixed;
  int plo[asize / 2];
  int phi[asize / 2];
  int np = 0;
  for (int a = 0; a < asize; a++)
    if ((m.steckerbrett[a] > a) && ! pf[a])   /* never rewire a fixed -s plug */
      {
        plo[np] = a;
        phi[np] = m.steckerbrett[a];
        np++;
      }

  double best = cur_score;
  int best_L[6] = { 0, 0, 0, 0, 0, 0 };
  int best_mm = -1;
  bool found = false;

  for (int i = 0; i < np; i++)
    for (int j = i + 1; j < np; j++)
      for (int k = j + 1; k < np; k++)
        {
          const int L[6] = { plo[i], phi[i], plo[j], phi[j], plo[k], phi[k] };
          for (int mm = 0; mm < 8; mm++)
            {
              for (int q = 0; q < 6; q += 2)   /* the three pairs of the rematch */
                {
                  int u = L[REMATCH3[mm][q]];
                  int v = L[REMATCH3[mm][q + 1]];
                  m.steckerbrett[u] = static_cast<unsigned char>(v);
                  m.steckerbrett[v] = static_cast<unsigned char>(u);
                }

              double s = score_iter(m);
              if (s > best)
                {
                  best = s;
                  found = true;
                  best_mm = mm;
                  for (int t = 0; t < 6; t++)
                    best_L[t] = L[t];
                }

              /* restore the three original plugs */
              m.steckerbrett[L[0]] = static_cast<unsigned char>(L[1]);
              m.steckerbrett[L[1]] = static_cast<unsigned char>(L[0]);
              m.steckerbrett[L[2]] = static_cast<unsigned char>(L[3]);
              m.steckerbrett[L[3]] = static_cast<unsigned char>(L[2]);
              m.steckerbrett[L[4]] = static_cast<unsigned char>(L[5]);
              m.steckerbrett[L[5]] = static_cast<unsigned char>(L[4]);
            }
        }

  if (found)
    {
      for (int q = 0; q < 6; q += 2)   /* the three pairs of the winning rematch */
        {
          int u = best_L[REMATCH3[best_mm][q]];
          int v = best_L[REMATCH3[best_mm][q + 1]];
          m.steckerbrett[u] = static_cast<unsigned char>(v);
          m.steckerbrett[v] = static_cast<unsigned char>(u);
        }
      report_climb_progress(m, best);
    }
  return found;
}

/* --gainfix tuning: candidate shortlist size and plug1 beam width. Plug2 is scored
   over the whole shortlist per plug1, so cascade cost is ~CAP + N1*CAP score_iter. */
static const int GAINFIX_CAP = 25;
static const int GAINFIX_N1  = 6;
static const int GAINFIX_N2  = 6;   /* --gainfix3: intermediate plug2 beam */
static const int GAINFIX_K3  = 8;   /* --gainfix3: # of sacrifice pairs reclimbed */

/* Form plug a-b in place, ejecting a's and b's old partners to self-steckered
   (an "add-with-eject" — a free endpoint is a no-op eject). */
static inline void gainfix_apply(unsigned char * steck, int a, int b)
{
  int pa = steck[a], pb = steck[b];
  steck[pa] = static_cast<unsigned char>(pa);
  steck[pb] = static_cast<unsigned char>(pb);
  steck[a] = static_cast<unsigned char>(b);
  steck[b] = static_cast<unsigned char>(a);
}

/* Generate the gain-vote candidate shortlist for the current board. For each
   position, find the best single-letter quad improvement (skipping the current
   letter and ct[j] — Enigma never self-encrypts), then vote its gain onto TWO
   candidate plugs: the EXIT re-plug {steck[pt[j]], bx} and the reciprocal ENTRY
   re-plug {ct[j], core_j(steck[bx])}. Writes the top `cap` plugs (endpoints a<b)
   by descending vote into ca[]/cb[]; returns the count. */
template<bool EX>
static int gainfix_candidates(machine & m, unsigned char * ca, unsigned char * cb, int cap)
{
  const bool * __restrict pf = EX ? PLUG_FIXED_EX : plug_fixed;
  const unsigned char * __restrict steck = m.steckerbrett;
  const unsigned char * const * __restrict rows = m.rows;
  const unsigned char * __restrict ct = num_ciphertext;
  const int n = textlength;
  /* score gains against the ACTIVE model's table (all8 for -a, else quad8) so the
     gain cascade stays consistent with the scorer for the weighted model too. */
  const uint8_t (* __restrict qt)[asize][asize][asize] =
    (m.scoring == SCORE_ALL) ? all8 : quad8;

  unsigned char pt[maxlen];
  for (int i = 0; i < n; i++)
    pt[i] = static_cast<unsigned char>(decode_at(steck, rows, ct, i));

  long votes[asize][asize];
  for (int a = 0; a < asize; a++)
    for (int b = 0; b < asize; b++)
      votes[a][b] = 0;

  for (int j = 0; j < n; j++)
    {
      int lo = j - 3; if (lo < 0) lo = 0;
      int hi = j;     if (hi > n - 4) hi = n - 4;
      if (hi < lo) continue;
      const int cj = ct[j];
      long cur = 0;
      for (int i = lo; i <= hi; i++)
        cur += qt[pt[i]][pt[i + 1]][pt[i + 2]][pt[i + 3]];
      int orig = pt[j], bx = orig;
      long bs = cur;
      for (int x = 0; x < asize; x++)
        {
          if (x == orig || x == cj) continue;   /* no-self-encryption prune */
          long s = 0;
          for (int i = lo; i <= hi; i++)
            {
              unsigned char q0 = pt[i], q1 = pt[i + 1], q2 = pt[i + 2], q3 = pt[i + 3];
              switch (j - i)
                {
                  case 0:  q0 = static_cast<unsigned char>(x); break;
                  case 1:  q1 = static_cast<unsigned char>(x); break;
                  case 2:  q2 = static_cast<unsigned char>(x); break;
                  default: q3 = static_cast<unsigned char>(x); break;
                }
              s += qt[q0][q1][q2][q3];
            }
          if (s > bs) { bs = s; bx = x; }
        }
      if (bs <= cur || bx == orig || bx == cj) continue;
      const long g = bs - cur;
      int r = steck[pt[j]];                      /* exit lever */
      if (r != bx && ! pf[r] && ! pf[bx])
        votes[r < bx ? r : bx][r < bx ? bx : r] += g;
      int y = rows[j][steck[bx]];                /* entry lever (reciprocal) */
      if (y != cj && ! pf[cj] && ! pf[y])
        votes[cj < y ? cj : y][cj < y ? y : cj] += g;
    }

  unsigned char ta[asize * (asize - 1) / 2], tb[asize * (asize - 1) / 2];
  long tv[asize * (asize - 1) / 2];
  int tot = 0;
  for (int a = 0; a < asize; a++)
    for (int b = a + 1; b < asize; b++)
      if (votes[a][b] > 0)
        {
          ta[tot] = static_cast<unsigned char>(a);
          tb[tot] = static_cast<unsigned char>(b);
          tv[tot] = votes[a][b];
          tot++;
        }
  int out = tot < cap ? tot : cap;
  for (int k = 0; k < out; k++)          /* partial selection sort: top `out` by vote */
    {
      int bi = k;
      for (int i = k + 1; i < tot; i++)
        if (tv[i] > tv[bi]) bi = i;
      long sv = tv[k]; tv[k] = tv[bi]; tv[bi] = sv;
      unsigned char sa = ta[k]; ta[k] = ta[bi]; ta[bi] = sa;
      unsigned char sb = tb[k]; tb[k] = tb[bi]; tb[bi] = sb;
      ca[k] = ta[k]; cb[k] = tb[k];
    }
  return out;
}

/* --gainfix: the 2-ply gain cascade barrier cross (PERFORMANCE.md 4.10). Quad-only,
   run at convergence once the cheap climb / re-pairs have stalled. Ranks the shortlist
   by the full re-decode score; then for each of the top-N1 plug1 candidates, applies it
   (even if it does not improve — that un-masks a masked second plug) and scores every
   plug2 candidate of the resulting board; keeps the (plug1, plug2) pair whose combined
   score most beats the converged score. Returns true (and installs the pair) iff such a
   strictly-improving pair exists, so the cheap climb resumes from it. Deterministic
   (no RNG, fixed candidate order), so -T-independent. */
template<bool EX>
static bool gain_cascade(machine & m, double cur_score)
{
  if ((m.scoring != SCORE_QUAD && m.scoring != SCORE_ALL) || textlength < 8)
    return false;
  if (cur_score < opt_gainfix_gate)             /* near-solution gate: skip junk boards */
    return false;

  unsigned char * steck = m.steckerbrett;
  unsigned char ca[GAINFIX_CAP], cb[GAINFIX_CAP];
  int nc = gainfix_candidates<EX>(m, ca, cb, GAINFIX_CAP);
  if (nc == 0)
    return false;

  unsigned char saveS[asize];
  for (int i = 0; i < asize; i++) saveS[i] = steck[i];

  /* rank plug1 candidates by the full re-decode score */
  double sc1[GAINFIX_CAP];
  int order[GAINFIX_CAP];
  for (int k = 0; k < nc; k++)
    {
      gainfix_apply(steck, ca[k], cb[k]);
      sc1[k] = score_iter(m);
      for (int i = 0; i < asize; i++) steck[i] = saveS[i];
      order[k] = k;
    }
  int n1 = nc < GAINFIX_N1 ? nc : GAINFIX_N1;
  for (int k = 0; k < n1; k++)           /* partial selection of the top-N1 plug1 */
    {
      int bi = k;
      for (int i = k + 1; i < nc; i++)
        if (sc1[order[i]] > sc1[order[bi]]) bi = i;
      int so = order[k]; order[k] = order[bi]; order[bi] = so;
    }

  double best = cur_score;
  bool found = false;
  int ba1 = 0, bb1 = 0, ba2 = 0, bb2 = 0;
  unsigned char saveS1[asize], ca2[GAINFIX_CAP], cb2[GAINFIX_CAP];
  for (int t = 0; t < n1; t++)
    {
      int k1 = order[t];
      for (int i = 0; i < asize; i++) steck[i] = saveS[i];
      gainfix_apply(steck, ca[k1], cb[k1]);                /* board -> S1 (may be downhill) */
      for (int i = 0; i < asize; i++) saveS1[i] = steck[i];
      int nc2 = gainfix_candidates<EX>(m, ca2, cb2, GAINFIX_CAP);
      for (int k = 0; k < nc2; k++)
        {
          gainfix_apply(steck, ca2[k], cb2[k]);
          double s = score_iter(m);
          for (int i = 0; i < asize; i++) steck[i] = saveS1[i];
          if (s > best)
            {
              best = s; found = true;
              ba1 = ca[k1]; bb1 = cb[k1]; ba2 = ca2[k]; bb2 = cb2[k];
            }
        }
    }

  for (int i = 0; i < asize; i++) steck[i] = saveS[i];      /* restore original board */
  if (found)
    {
      gainfix_apply(steck, ba1, bb1);
      gainfix_apply(steck, ba2, bb2);
      report_climb_progress(m, best);
    }
  return found;
}

template<bool EX> static double hillclimb(machine & m, int max_pairs);   /* fwd: reclimb below */

/* --gainfix3 ("sacrifice + reclimb"): a deeper escalation for 3-plug tangles the 2-ply pair
   can't cross, tried only when the 2-ply cascade found nothing. Rank the (plug1,plug2)
   SACRIFICE pairs (both plugs, possibly downhill) by their 2-plug score, and for the top-K
   commit the sacrifice and run a full PLAIN reclimb -- letting the ordinary climb find the
   completing plug(s) AND shed spurious ones -- keeping the best-scoring result. No explicit
   plug3 search: the completing plug is the top improving move after the sacrifice, so the
   reclimb finds it (measured), which is both simpler and recovers MORE than committing one
   fixed completing plug (a full climb per sacrifice beats a single triple; PERFORMANCE.md
   4.11). The reclimb runs with gainfix off -> no recursion, capped at the same max_pairs.
   template<bool EX>/plug_fixed like the rest; -T-deterministic. */
template<bool EX>
static bool gain_cascade_3ply(machine & m, double cur_score, int max_pairs)
{
  if ((m.scoring != SCORE_QUAD && m.scoring != SCORE_ALL) || textlength < 8)
    return false;
  if (cur_score < opt_gainfix_gate)             /* near-solution gate: skip junk boards */
    return false;

  unsigned char * steck = m.steckerbrett;
  unsigned char ca[GAINFIX_CAP], cb[GAINFIX_CAP];
  int nc = gainfix_candidates<EX>(m, ca, cb, GAINFIX_CAP);
  if (nc == 0)
    return false;

  unsigned char saveS[asize];
  for (int i = 0; i < asize; i++) saveS[i] = steck[i];

  /* rank plug1 by the full re-decode score, take the top-N1 */
  double sc1[GAINFIX_CAP];
  int order1[GAINFIX_CAP];
  for (int k = 0; k < nc; k++)
    {
      gainfix_apply(steck, ca[k], cb[k]);
      sc1[k] = score_iter(m);
      for (int i = 0; i < asize; i++) steck[i] = saveS[i];
      order1[k] = k;
    }
  int n1 = nc < GAINFIX_N1 ? nc : GAINFIX_N1;
  for (int k = 0; k < n1; k++)
    {
      int bi = k;
      for (int i = k + 1; i < nc; i++)
        if (sc1[order1[i]] > sc1[order1[bi]]) bi = i;
      int so = order1[k]; order1[k] = order1[bi]; order1[bi] = so;
    }

  /* build the (plug1,plug2) sacrifice boards + their 2-plug scores */
  const int MAXP = GAINFIX_N1 * GAINFIX_N2;
  double pscore[MAXP];
  unsigned char pboard[MAXP][asize];
  int npair = 0;
  unsigned char saveS1[asize], ca2[GAINFIX_CAP], cb2[GAINFIX_CAP];
  double sc2[GAINFIX_CAP];
  int order2[GAINFIX_CAP];
  for (int t = 0; t < n1; t++)
    {
      int k1 = order1[t];
      for (int i = 0; i < asize; i++) steck[i] = saveS[i];
      gainfix_apply(steck, ca[k1], cb[k1]);               /* plug1 -> S1 (may be downhill) */
      for (int i = 0; i < asize; i++) saveS1[i] = steck[i];
      int nc2 = gainfix_candidates<EX>(m, ca2, cb2, GAINFIX_CAP);
      for (int k = 0; k < nc2; k++)                       /* rank plug2 by 2-plug score */
        {
          gainfix_apply(steck, ca2[k], cb2[k]);
          sc2[k] = score_iter(m);
          for (int i = 0; i < asize; i++) steck[i] = saveS1[i];
          order2[k] = k;
        }
      int n2 = nc2 < GAINFIX_N2 ? nc2 : GAINFIX_N2;
      for (int k = 0; k < n2; k++)
        {
          int bi = k;
          for (int i = k + 1; i < nc2; i++)
            if (sc2[order2[i]] > sc2[order2[bi]]) bi = i;
          int so = order2[k]; order2[k] = order2[bi]; order2[bi] = so;
        }
      for (int u = 0; u < n2; u++)
        {
          int k2 = order2[u];
          if (ca2[k2] == ca[k1] && cb2[k2] == cb[k1])     /* skip plug2 == plug1 */
            continue;
          for (int i = 0; i < asize; i++) steck[i] = saveS1[i];
          gainfix_apply(steck, ca2[k2], cb2[k2]);         /* plug2 -> S2 (may be downhill) */
          pscore[npair] = sc2[k2];
          for (int i = 0; i < asize; i++) pboard[npair][i] = steck[i];
          npair++;
        }
    }

  /* rank the sacrifice pairs by 2-plug score, take the top-K */
  int po[MAXP];
  for (int k = 0; k < npair; k++) po[k] = k;
  int K = npair < GAINFIX_K3 ? npair : GAINFIX_K3;
  for (int k = 0; k < K; k++)
    {
      int bi = k;
      for (int i = k + 1; i < npair; i++)
        if (pscore[po[i]] > pscore[po[bi]]) bi = i;
      int so = po[k]; po[k] = po[bi]; po[bi] = so;
    }

  /* commit each top-K sacrifice and run a PLAIN reclimb (gainfix off -> no recursion), keep
     the best result -- the reclimb finds the completing plug(s) and sheds spurious ones */
  double best = cur_score;
  bool found = false;
  unsigned char bestboard[asize];
  int save_gf = opt_gainfix, save_gf3 = opt_gainfix3;
  opt_gainfix = 0; opt_gainfix3 = 0;
  for (int k = 0; k < K; k++)
    {
      for (int i = 0; i < asize; i++) steck[i] = pboard[po[k]][i];
      double s = hillclimb<EX>(m, max_pairs);
      if (s > best)
        {
          best = s; found = true;
          for (int i = 0; i < asize; i++) bestboard[i] = steck[i];
        }
    }
  opt_gainfix = save_gf; opt_gainfix3 = save_gf3;

  for (int i = 0; i < asize; i++) steck[i] = saveS[i];     /* restore original board */
  if (found)
    {
      for (int i = 0; i < asize; i++) steck[i] = bestboard[i];
      report_climb_progress(m, best);
    }
  return found;
}

/* Lexicographic table of the C(26,2)=325 unordered letter pairs, built once. */
struct pairtab { unsigned char a[asize * (asize - 1) / 2], b[asize * (asize - 1) / 2]; };
static pairtab make_pairtab()
{
  pairtab t = {};   /* zero-init: the loop fills every entry, but this lets cppcheck prove it */
  int k = 0;
  for (int i = 0; i < asize; i++)
    for (int j = i + 1; j < asize; j++)
      { t.a[k] = static_cast<unsigned char>(i); t.b[k] = static_cast<unsigned char>(j); k++; }
  return t;
}

/* --- Circular first-improvement climb (-I) ------------------------------------

   Steepest ascent full-scans all 325 toggle moves per accepted move and applies the single
   best. First-improvement instead applies the FIRST move that improves and keeps going.
   The ordering is *circular*: a cursor sweeps a fixed move list and CONTINUES from where
   it accepted (never restarts at the top), so each move is examined ~once per sweep --
   this both avoids re-scanning the moves that didn't change and spreads attention evenly
   around the 26 letters instead of always favouring low letters (the two problems of
   naive restart-from-top first-improvement). Convergence: a full cycle of all `nmoves`
   with no accepted move = a local optimum.

   Move list (fixed indices, so the cursor is well-defined): the 325 unordered letter
   pairs, each a "toggle a-b" (already paired -> REMOVE it; else force a-b, i.e. ADD /
   MOVE an endpoint / MERGE) -- the same unified operator the steepest-ascent scan uses,
   so removal is the already-paired toggle rather than a separate move list. Deterministic
   (no RNG, fixed order and acceptance rule) so the result is -T-independent; the trajectory
   differs from steepest ascent, so this is NOT byte-identical and must be judged on
   recovery, not equality. */
template<bool EX>
static void firstimprove_sweep(machine & m, int max_pairs)
{
  const bool * __restrict pf = EX ? PLUG_FIXED_EX : plug_fixed;
  static const int nmoves = asize * (asize - 1) / 2;   /* 325 pair-toggles */
  static const pairtab P = make_pairtab();

  unsigned char * __restrict steck = m.steckerbrett;
  double cur = score_iter(m);

  int pairs = 0;
  for (int j = 0; j < asize; j++)
    if (steck[j] > j)
      pairs++;

  /* Is the toggle on (a,b) blocked by the plug cap? A REMOVE (already paired) is always
     allowed (-1). At/over the cap an ADD (both ends free) is blocked, and with -M a
     count-preserving MOVE (one end free) too, so only the count-reducing merge/remove
     survive -- matching the steepest-ascent scan's cap rule. */
  auto cap_blocks = [&](int a, int b) -> bool
  {
    if (pairs < max_pairs) return false;
    if (steck[a] == b) return false;                        /* REMOVE: allowed */
    bool a_free = (steck[a] == a), b_free = (steck[b] == b);
    if (a_free && b_free) return true;                      /* block ADD */
    if (opt_capmerge && (a_free || b_free)) return true;    /* -M: block MOVE */
    return false;
  };

  /* Score the toggle on (a,b) against the current board, leaving the board unchanged. */
  auto probe_toggle = [&](int a, int b) -> double
  {
    double s;
    if (steck[a] == b)                             /* REMOVE a-b */
      {
        steck[a] = static_cast<unsigned char>(a);
        steck[b] = static_cast<unsigned char>(b);
        s = score_iter(m);
        steck[a] = static_cast<unsigned char>(b);
        steck[b] = static_cast<unsigned char>(a);
      }
    else                                           /* force a-b: ADD / MOVE / MERGE */
      {
        int x = steck[a], y = steck[b];
        int xx = steck[x], yy = steck[y];
        steck[x] = static_cast<unsigned char>(x);
        steck[y] = static_cast<unsigned char>(y);
        steck[a] = static_cast<unsigned char>(b);
        steck[b] = static_cast<unsigned char>(a);
        s = score_iter(m);
        steck[a] = static_cast<unsigned char>(x);
        steck[b] = static_cast<unsigned char>(y);
        steck[x] = static_cast<unsigned char>(xx);
        steck[y] = static_cast<unsigned char>(yy);
      }
    return s;
  };

  /* Move-visit order. Default: lexicographic (visit[i]=i). Dynamic (-J): score every move
     once from the starting board and visit them best-score-first, then circularly -- the
     "first round: score all, sort, then process in order" idea. The order is derived per
     climb from the (perturbed) starting board, so it differs per restart; deterministic
     (fixed board + tie-break) -> -T-independent. Costs one extra full scan per climb. */
  const bool dyn_order = (opt_dynorder != 0);
  const bool infl_order = (opt_inflorder != 0);
  int visit[nmoves];
  if (infl_order)
    {
      /* --infl-order: rank moves by board-state influence (§4.6), ~free. Two 26-bin
         histograms -- ciphertext letters and current-decrypt letters -- then
         w(a,b) = cc[a]+cc[b]+pc[a]+pc[b] (sum form of the influence weight). Order once
         from the starting board (per-restart, like -J), no per-move scoring. */
      const unsigned char * const * __restrict rows = m.rows;
      const unsigned char * __restrict ct = num_ciphertext;
      int cc[asize] = { 0 };
      int pc[asize] = { 0 };
      for (int i = 0; i < textlength; i++)
        {
          cc[ct[i]]++;
          pc[decode_at(steck, rows, ct, i)]++;
        }
      int infl[nmoves];
      for (int mv = 0; mv < nmoves; mv++)
        {
          int a = P.a[mv], b = P.b[mv];
          infl[mv] = cc[a] + cc[b] + pc[a] + pc[b];
          visit[mv] = mv;
        }
      std::sort(visit, visit + nmoves, [&](int i, int j)
      {
        if (infl[i] != infl[j]) return infl[i] > infl[j];   /* most influential first */
        return i < j;                                        /* deterministic tie-break */
      });
    }
  else if (dyn_order)
    {
      double sc[nmoves];
      for (int mv = 0; mv < nmoves; mv++)
        {
          int a = P.a[mv], b = P.b[mv];
          double s = -1e300;   /* invalid moves sort last */
          if (! (pf[a] || pf[b]) && ! cap_blocks(a, b))
            s = probe_toggle(a, b);
          sc[mv] = s;
          visit[mv] = mv;
        }
      std::sort(visit, visit + nmoves, [&](int i, int j)
      {
        if (sc[i] != sc[j]) return sc[i] > sc[j];   /* best score first */
        return i < j;                               /* deterministic tie-break */
      });
    }
  else
    for (int i = 0; i < nmoves; i++)
      visit[i] = i;

  int cursor = 0;
  int stale = 0;
  while (stale < nmoves)
    {
      int mv = visit[cursor];
      cursor++;
      if (cursor == nmoves)
        cursor = 0;

      int a = P.a[mv], b = P.b[mv];
      if ((pf[a] || pf[b]) || cap_blocks(a, b))
        { stale++; continue; }

      bool improved = false;

      if (steck[a] == b)                             /* REMOVE a-b */
        {
          steck[a] = static_cast<unsigned char>(a);
          steck[b] = static_cast<unsigned char>(b);
          double s = score_iter(m);
          if (s > cur)
            { cur = s; improved = true; }
          else
            {
              steck[a] = static_cast<unsigned char>(b);
              steck[b] = static_cast<unsigned char>(a);
            }
        }
      else                                           /* force a-b: ADD / MOVE / MERGE */
        {
          int x = steck[a], y = steck[b];
          int xx = steck[x], yy = steck[y];
          steck[x] = static_cast<unsigned char>(x);
          steck[y] = static_cast<unsigned char>(y);
          steck[a] = static_cast<unsigned char>(b);
          steck[b] = static_cast<unsigned char>(a);
          double s = score_iter(m);
          if (s > cur)
            { cur = s; improved = true; }
          else
            {
              steck[a] = static_cast<unsigned char>(x);
              steck[b] = static_cast<unsigned char>(y);
              steck[x] = static_cast<unsigned char>(xx);
              steck[y] = static_cast<unsigned char>(yy);
            }
        }

      if (improved)
        {
          stale = 0;
          report_climb_progress(m, cur);
          pairs = 0;   /* recompute the plug count (only on acceptance, ~cheap) */
          for (int j = 0; j < asize; j++)
            if (steck[j] > j)
              pairs++;
        }
      else
        stale++;
    }
}

/* Climb the steckerbrett for the current scoring model until no move improves it,
   but never letting the board exceed max_pairs plug pairs (the staged climb caps the
   low-order pre-pass to its first few plugs; pass pairs_uncapped for an unconstrained
   climb -- a board can hold at most 13 pairs anyway). The cheap "switch" and "remove"
   moves are run to convergence; then a single best "re-pair" is tried as a barrier
   cross, and if it improves the cheap climb resumes from the new board. */
template<bool EX>
static double hillclimb(machine & m, int max_pairs)
{
  const bool * __restrict pf = EX ? PLUG_FIXED_EX : plug_fixed;
  /* -I: circular first-improvement instead of steepest ascent (off by default, so the
     baseline is byte-identical). */
  const bool firstimp = (opt_firstimprove != 0);

  bool progress;
  do
    {
      progress = false;

      double cur;   /* converged score, handed to the re-pair barrier */

      if (firstimp)
        {
          firstimprove_sweep<EX>(m, max_pairs);
          cur = score_iter(m);
        }
      else
        {
          double best_score;
          double last_best;

          /* Cheap moves to convergence: each pass takes the single best of all "switch
             a-b" moves (force a-b, ejecting conflicts -- adds / moves an endpoint /
             merges two plugs into one) and all "remove" moves (free an existing pair). */
          do
            {
              best_score = score_iter(m);
              last_best = best_score;

              /* current plug-pair count: at the cap, moves that would add a brand-new
                 pair (both endpoints currently unplugged) are skipped below */
              int pairs = 0;
              for (int j = 0; j < asize; j++)
                if (m.steckerbrett[j] > j)
                  pairs++;

              double move_score = best_score;
              int move_kind = 0;        /* 0 = switch, 1 = remove */
              int move_a = 0;
              int move_b = 0;

              /* One "toggle a-b" operator over all 325 letter pairs expresses every plug move
                 by the current state of a and b: both ends free -> ADD a-b (+1 pair); exactly
                 one end plugged -> MOVE that plug's endpoint (0); both ends plugged to different
                 partners -> MERGE two plugs into one (-1); a-b already a pair -> REMOVE it (-1).
                 Steepest ascent takes the single best improving toggle per pass. The plug cap
                 gates by count-effect: at/over the cap an ADD is always blocked, and with -M
                 (opt_capmerge) a count-preserving MOVE too, so only the count-reducing MERGE and
                 REMOVE survive -- the cap becomes a strict descent target. (Folding removal in as
                 the already-paired toggle case is what lets a single scan replace the old
                 separate switch-scan + removal-loop pair.) */
              for(int a=0; a<asize; a++)
                for(int b=a+1; b<asize; b++)
                  {
                    /* never reassign a fixed -s plug (a fixed letter keeps its partner) */
                    if (pf[a] || pf[b])
                      continue;

                    int sa = m.steckerbrett[a];
                    int sb = m.steckerbrett[b];
                    bool a_free = (sa == a);
                    bool b_free = (sb == b);
                    bool paired = (sa == b);   /* a-b already a plug -> this toggle REMOVES it */

                    /* cap gate by count-effect (a REMOVE is -1, so always allowed) */
                    if ((pairs >= max_pairs) && ! paired)
                      {
                        if (a_free && b_free)
                          continue;                    /* block ADD (+1) */
                        if (opt_capmerge && (a_free || b_free))
                          continue;                    /* -M: block count-preserving MOVE (0) */
                      }

                    int new_kind, x = 0, y = 0, xx = 0, yy = 0;
                    if (paired)
                      {
                        m.steckerbrett[a] = a;         /* REMOVE a-b */
                        m.steckerbrett[b] = b;
                        new_kind = 1;
                      }
                    else
                      {
                        x = sa; y = sb;
                        xx = m.steckerbrett[x];
                        yy = m.steckerbrett[y];
                        m.steckerbrett[x] = x;         /* force a-b: ADD / MOVE / MERGE */
                        m.steckerbrett[y] = y;
                        m.steckerbrett[a] = b;
                        m.steckerbrett[b] = a;
                        new_kind = 0;
                      }

                    double score = score_iter(m);

                    /* steepest ascent; on an equal score a switch (add/move/merge) wins the tie
                       over a removal, so a converged board keeps the plugs the score justifies. */
                    if ((score > move_score) ||
                        ((score == move_score) && (score > best_score) &&
                         (new_kind == 0) && (move_kind == 1)))
                      {
                        move_score = score;
                        move_kind = new_kind;
                        move_a = a;
                        move_b = b;
                      }

                    if (paired)
                      {
                        m.steckerbrett[a] = b;         /* restore REMOVE */
                        m.steckerbrett[b] = a;
                      }
                    else
                      {
                        m.steckerbrett[a] = sa;        /* restore force */
                        m.steckerbrett[b] = sb;
                        m.steckerbrett[x] = xx;
                        m.steckerbrett[y] = yy;
                      }
                  }

              if (move_score - best_score > 0)
                {
                  int a = move_a;
                  int b = move_b;

                  if (move_kind == 1)
                    {
                      /* remove the a-b plug, freeing both ends */
                      m.steckerbrett[a] = a;
                      m.steckerbrett[b] = b;
                    }
                  else
                    {
                      /* switch plugs */
                      int x = m.steckerbrett[a];
                      int y = m.steckerbrett[b];
                      m.steckerbrett[x] = x;
                      m.steckerbrett[y] = y;
                      m.steckerbrett[a] = b;
                      m.steckerbrett[b] = a;
                    }

                  best_score = move_score;
                  report_climb_progress(m, best_score);
                }
            }
          while (best_score > last_best);
          cur = best_score;
        }

      /* Cheap moves converged: one last-resort re-pair barrier cross. If it
         improves, loop back and let the cheap climb resume from the new board.
         With --repair3, and only when the 2-plug re-pair also found nothing, try the
         deeper 3-plug reshuffle as a further barrier cross. */
      /* short-circuit: try_repair_3 runs only when the 2-plug re-pair found nothing */
      if ((! opt_no_repair && try_repair<EX>(m, cur))
          || (opt_repair3 && try_repair_3<EX>(m, cur))
          || (opt_gainfix && gain_cascade<EX>(m, cur))
          || (opt_gainfix3 && gain_cascade_3ply<EX>(m, cur, max_pairs)))
        progress = true;
    }
  while (progress);

  decode(m);

  return score_iter(m);
}

/* splitmix64: a tiny, well-distributed deterministic PRNG. Seeded per key (not
   from the clock or thread id) so a random-restart search stays reproducible and
   independent of the thread count. */
static inline uint64_t splitmix64(uint64_t * s)
{
  uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

/* Inject exactly k random plug pairs into the current plugboard, drawing only from
   letters that are still unplugged (so any fixed -s pairs are preserved). This is
   the per-restart perturbation: a kick of k random plugs (default_perturb, or an rN
   token) into a new basin, near the typical plug count so the staged climb need not
   tear down a near-saturated board (CODE_REVIEW §9). With k=0 it is a no-op (so r0
   makes restarts identical -- a useful control). */
void perturb_steckerbrett(machine & m, uint64_t * rng, int k)
{
  unsigned char freelet[asize];
  int nfree = 0;
  for (int i = 0; i < asize; i++)
    if (m.steckerbrett[i] == i)
      freelet[nfree++] = static_cast<unsigned char>(i);

  int want = k * 2;                  /* free letters to pair up */
  if (want > nfree)
    want = nfree - (nfree & 1);      /* clamp to an even count of free letters */

  /* partial Fisher-Yates: shuffle the first `want` free letters, then pair them */
  for (int i = 0; i < want; i++)
    {
      int j = i + static_cast<int>(splitmix64(rng) %
                                   static_cast<uint64_t>(nfree - i));
      unsigned char t = freelet[i]; freelet[i] = freelet[j]; freelet[j] = t;
    }
  for (int i = 0; i + 1 < want; i += 2)
    {
      int a = freelet[i], b = freelet[i + 1];
      m.steckerbrett[a] = static_cast<unsigned char>(b);
      m.steckerbrett[b] = static_cast<unsigned char>(a);
    }
}

/* Load the n-gram table backing a scoring model (IC needs none). */
void load_table(int model)
{
  switch (model)
    {
    case SCORE_MONO: ngrams_read(1, mono8, & ngram_bias[SCORE_MONO], & ngram_scale[SCORE_MONO], "monograms"); break;
    case SCORE_BI:   ngrams_read(2, & bi8[0][0], & ngram_bias[SCORE_BI], & ngram_scale[SCORE_BI], "bigrams"); break;
    case SCORE_TRI:  ngrams_read(3, & tri8[0][0][0], & ngram_bias[SCORE_TRI], & ngram_scale[SCORE_TRI], "trigrams"); break;
    case SCORE_QUAD: ngrams_read(4, & quad8[0][0][0][0], & ngram_bias[SCORE_QUAD], & ngram_scale[SCORE_QUAD], "quadgrams"); break;
    case SCORE_ALL:
      {
        /* the weighted all-order model: log-linear symmetric mixture of quad/tri/bi/mono,
           weights tuned across four languages (PR #106): quad 1, tri .6, bi .3, mono .15. */
        static const double AW[4] = { 1.0, 0.6, 0.3, 0.15 };
        ngrams_read(4, & all8[0][0][0][0], & ngram_bias[SCORE_ALL], & ngram_scale[SCORE_ALL],
                    "quadgrams", AW, true);
        break;
      }
    default: break;   /* IC: no table */
    }
}

/* Map a scoring-model letter (i/m/b/t/q) to its SCORE_* value. */
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
    default:  return SCORE_IC;
    }
}

/* Record a bare model selector (-i/-m/-b/-t/-q) as a single uncapped --score <model>
   stage (REDESIGN Part C). Two selectors that disagree (e.g. -m -q) make the intended
   model ambiguous, so reject them; repeats that agree (-q -q) are fine. Sets opt_scoring
   so a run with no --score ranks by the selected model. */
static void select_model(int model)
{
  if ((opt_model_selector != -1) && (opt_model_selector != model))
    fatal("Conflicting scoring models: the -i/-m/-b/-t/-q selectors disagree; "
          "pick one");
  opt_model_selector = model;
  opt_scoring = model;
}

/* Parse the --score/-S schedule string into opt_stages[]/opt_nstages, and set
   opt_scoring to the target (last model stage). Tokens are <letter><optional int>:
   model letters i/m/b/t/q (a climb stage; the number caps its plug pairs, omitted =
   uncapped). On a syntax error it calls fatal(). With no --score the schedule is the
   single -i/-m/.../-q target, uncapped. The per-restart random kick (--random) and
   the partial exhaustion (--exhaust) are separate options, not schedule tokens. */
void parse_schedule()
{
  opt_nstages = 0;

  if (! opt_staged)
    {
      opt_stages[0].model = opt_scoring;
      opt_stages[0].cap = pairs_uncapped;
      opt_nstages = 1;
      return;
    }

  for (const char * p = opt_staged; *p; )
    {
      char letter = *p++;
      int n = -1;                       /* -1 = no explicit number */
      if (isdigit(static_cast<unsigned char>(*p)))
        {
          n = 0;
          while (isdigit(static_cast<unsigned char>(*p)))
            {
              n = n * 10 + (*p++ - '0');
              if (n > pairs_uncapped)
                break;                  /* range-checked below; avoid overflow */
            }
        }

      if (strchr("imbtqa", letter))
        {
          if (opt_nstages >= max_stages)
            fatal("Illegal --score schedule: too many stages (max 16)");
          int cap = (n < 0) ? pairs_uncapped : n;
          if ((cap < 1) || (cap > pairs_uncapped))
            fatal("Illegal --score stage cap (1 to 13 plug pairs; omit for no cap)");
          opt_stages[opt_nstages].model = model_of(letter);
          opt_stages[opt_nstages].cap = cap;
          opt_nstages++;
        }
      else
        fatal("Illegal --score schedule (tokens are i/m/b/t/q/a + optional cap, "
              "e.g. --score m4a10; use --random for the kick, --exhaust for forcing)");
    }

  if (opt_nstages < 1)
    fatal("Illegal --score schedule: needs at least one model stage (i/m/b/t/q/a)");

  /* the last model stage is the target/ranking model */
  opt_scoring = opt_stages[opt_nstages - 1].model;
}

/* Does the parsed --score schedule carry climb-only detail -- i.e. more than one
   stage, or any stage capped below the board maximum? Such detail is meaningful only
   during a plugboard climb (-c); a bare rotor scan just ranks by the target model.
   Used to warn when a climb schedule is given without -c. */
static bool schedule_is_climb_only()
{
  if (opt_nstages > 1)
    return true;
  for (int i = 0; i < opt_nstages; i++)
    if (opt_stages[i].cap < pairs_uncapped)
      return true;
  return false;
}

/* Number of distinct sets of `pairs` disjoint plug pairs drawable from `free` letters:
   free! / (2^p p! (free-2p)!). Returned as a double (the count explodes fast). Used for the
   exhaustion combo count and for the restart pigeonhole warning (a kick of K pairs among the
   free letters has this many distinct outcomes). */
static double disjoint_pair_combinations(int free_letters, int pairs)
{
  double combos = 1.0;
  for (int i = 0; i < pairs; i++)
    combos *= static_cast<double>((free_letters - 2*i) * (free_letters - 2*i - 1))
              / (2.0 * (i + 1));
  return combos;
}

/* Staged plugboard climb: run each schedule stage in order, capping the plug pairs
   it may set. A lower-order model has a far smoother surface when only a plug or two
   are set, so an early stage steers the first plugs into a good basin that a
   single-model climb navigates poorly -- staging reshapes the *search* landscape
   (complementary to random restarts). The returned score and m.plaintext are in the
   target (last) model, so cross-key comparison is unaffected. With no -S this is a
   single uncapped climb in the -i/-m/.../-q model. */
template<bool EX>
static double run_stages(machine & m)
{
  double s = 0.0;
  for (int i = 0; i < opt_nstages; i++)
    {
      m.scoring = opt_stages[i].model;
      s = hillclimb<EX>(m, opt_stages[i].cap);
    }
  return s;   /* opt_nstages >= 1, so s is the target-model score */
}


/* --exhaust E partial plugboard exhaustion (PROTOTYPE, exploration tool only -- dominated by
   a high --restarts greedy climb at equal compute; see PERFORMANCE.md §3.6). E is the number
   of EXTRA plug pairs forced among the free letters, on top of any -s pairs. Instead of one
   climb from the seed, try every set of E disjoint pairs among the free letters -- pin them
   (as -s pins plugs) and run the staged climb from that seed -- and keep the best board. E=1
   tries each of the 325 first pairs; larger E is exponentially more work (combos(free,E) =
   free!/(2^E E! (free-2E)!) sets: ~45k for E=2, ~3.5M for E=3 with no -s). It composes with
   the kick and restarts: for each forced combo, --restarts N runs N kicked climbs (the kick
   perturbs only the still-free letters, leaving -s and the forced pairs intact), keeping the
   best.

   Parallel (REDESIGN Part D): the FIRST forced pair (the combo's minimum-low-letter pair)
   is the unit of work -- there are at most C(free,2) <= 325 of them, listed in
   g_exhaust_firsts, and every combo belongs to exactly one (its remaining pairs all use
   letters above the first pair's low letter). Each unit runs on any thread against its own
   PLUG_FIXED_EX pin set (per-thread under clang, per-machine under g++ -- no shared mutable
   state), and its best merges into the global best exactly like a restart. So exhaustion now
   scales with -T and stays -T-independent (each (unit, restart) climb is seeded only by
   key + restart). */
static inline uint64_t restart_seed(size_t key_index, int restart);   /* defined below */

/* Flat list of the free first-pair choices (x,y), two bytes each; built once before the
   search by build_exhaust_firsts(), read-only during it. */
static std::vector<unsigned char> g_exhaust_firsts;

struct exhaust_ctx
{
  int a[pairs_uncapped];   /* the currently-chosen forced pairs (depth of them) */
  int b[pairs_uncapped];
  int target;              /* E forced pairs */
  size_t key_index;        /* for the per-restart RNG seed */
  bool used[asize];        /* letters consumed by -s + forced-so-far (enumeration only) */
  double best;
  bool have;
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
};

/* At a full combo (the E forced pairs in c.a/c.b): run every restart's climb from the seed
   board -- identity + -s + the forced pairs -- plus this restart's --random kick over the
   still-free letters, and keep the best in c. The forced pairs are pinned in plug_fixed so
   the climb (and the kick, which draws only from self-steckered letters) leave them intact. */
static void exhaust_leaf(machine & m, exhaust_ctx & c)
{
  const bool kicked = (opt_restarts >= 1);
  const int climbs = kicked ? opt_restarts : 1;
  for (int r = 0; r < climbs; r++)
    {
      init_steckerbrett(m, opt_steckerbrett);   /* board = identity + -s */
      memcpy(PLUG_FIXED_EX, plug_fixed, asize);   /* per-worker pins = -s seed ... */
      for (int i = 0; i < c.target; i++)
        {
          m.steckerbrett[c.a[i]] = static_cast<unsigned char>(c.b[i]);
          m.steckerbrett[c.b[i]] = static_cast<unsigned char>(c.a[i]);
          PLUG_FIXED_EX[c.a[i]] = PLUG_FIXED_EX[c.b[i]] = true;   /* ... plus the forced pair */
        }
      if (kicked)
        {
          uint64_t rng = restart_seed(c.key_index, r);
          perturb_steckerbrett(m, & rng, opt_perturb);
        }
      double s = run_stages<true>(m);
      if (! c.have || (s > c.best))
        {
          c.best = s;
          c.have = true;
          memcpy(c.best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
          memcpy(c.best_steck, m.steckerbrett, asize);
        }
    }
}

/* Choose the remaining forced pairs (from depth up to c.target), each pair's low letter in
   increasing order (`first`) so every combo is enumerated exactly once. c.used excludes the
   -s letters and the pairs chosen so far. At full depth, climb (exhaust_leaf). */
static void exhaust_recurse(machine & m, exhaust_ctx & c, int depth, int first)
{
  if (depth == c.target)
    {
      exhaust_leaf(m, c);
      return;
    }
  for (int x = first; x < asize; x++)
    {
      if (c.used[x])
        continue;
      for (int y = x + 1; y < asize; y++)
        {
          if (c.used[y])
            continue;
          c.a[depth] = x;
          c.b[depth] = y;
          c.used[x] = c.used[y] = true;
          exhaust_recurse(m, c, depth + 1, x + 1);
          c.used[x] = c.used[y] = false;
        }
    }
}

/* Initialise c for one rotor key: E forced pairs, -s letters marked used. */
static void exhaust_ctx_init(exhaust_ctx & c, size_t key_index)
{
  c.target = opt_exhaust;
  c.key_index = key_index;
  c.best = 0.0;
  c.have = false;
  for (int j = 0; j < asize; j++)
    c.used[j] = false;
  int fixed = static_cast<int>(strlen(opt_steckerbrett) / 2);
  for (int i = 0; i < fixed; i++)
    {
      c.used[char2num(opt_steckerbrett[2*i+0])] = true;
      c.used[char2num(opt_steckerbrett[2*i+1])] = true;
    }
}

/* One parallel exhaustion unit: all combos whose first forced pair is g_exhaust_firsts[fi],
   over all restarts. Leaves m at the unit's best board/plaintext and returns its score, or
   a sentinel below any real score if the first pair leaves no room for E-1 more pairs. */
static double exhaust_unit(machine & m, size_t key_index, size_t fi)
{
  exhaust_ctx c;
  exhaust_ctx_init(c, key_index);
  int x = g_exhaust_firsts[2*fi];
  int y = g_exhaust_firsts[2*fi + 1];
  c.a[0] = x;
  c.b[0] = y;
  c.used[x] = c.used[y] = true;
  exhaust_recurse(m, c, 1, x + 1);   /* remaining E-1 pairs use letters above x */
  if (c.have)
    {
      memcpy(m.plaintext, c.best_pt, static_cast<size_t>(textlength) + 1);
      memcpy(m.steckerbrett, c.best_steck, asize);
      return c.best;
    }
  return -1e300;   /* no valid combo under this first pair: never wins the merge */
}

/* Whole-key exhaustion (used by the -F tier-2 climb, which parallelises over keys): every
   first-pair unit, keeping the best board/plaintext. Validation guarantees >= 1 combo. */
static double exhaust_all_combos(machine & m, size_t key_index)
{
  double best = 0.0;
  bool have = false;
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
  size_t nfirsts = g_exhaust_firsts.size() / 2;
  for (size_t fi = 0; fi < nfirsts; fi++)
    {
      double s = exhaust_unit(m, key_index, fi);
      if ((! have || (s > best)) && (s > -1e300))
        {
          best = s;
          have = true;
          memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
          memcpy(best_steck, m.steckerbrett, asize);
        }
    }
  if (have)
    {
      memcpy(m.plaintext, best_pt, static_cast<size_t>(textlength) + 1);
      memcpy(m.steckerbrett, best_steck, asize);
    }
  return best;
}

/* Enumerate the free first-pair choices (x,y) into g_exhaust_firsts: every pair of letters
   not pinned by -s, low letter first. Built once before the search (read-only after). Bounded
   by C(free,2) <= 325 regardless of E, so it never explodes in memory (unlike a full combo
   list); each unit does its own bounded sub-exhaustion. */
static void build_exhaust_firsts()
{
  bool sfixed[asize];
  for (int j = 0; j < asize; j++)
    sfixed[j] = false;
  int fixed = static_cast<int>(strlen(opt_steckerbrett) / 2);
  for (int i = 0; i < fixed; i++)
    {
      sfixed[char2num(opt_steckerbrett[2*i+0])] = true;
      sfixed[char2num(opt_steckerbrett[2*i+1])] = true;
    }
  g_exhaust_firsts.clear();
  for (int x = 0; x < asize; x++)
    {
      if (sfixed[x])
        continue;
      for (int y = x + 1; y < asize; y++)
        {
          if (sfixed[y])
            continue;
          g_exhaust_firsts.push_back(static_cast<unsigned char>(x));
          g_exhaust_firsts.push_back(static_cast<unsigned char>(y));
        }
    }
}

/* Hill-climb the plugboard with optional random restarts. --restarts 0 runs a single climb
   from the configured seed (identity or -s pairs), no kick -- fully deterministic. --restarts
   N runs N climbs, each from the seed plus a fresh --random kick (opt_perturb plug pairs, a
   moderate kick near the typical plug count), keeping the best; the un-kicked seed climb is not
   additionally run (REDESIGN Option A). The rotor-stack mapping[] depends only on the key (not
   the plugboard), so it is reused across restarts; only the steckerbrett is reset each time.
   The RNG is seeded from the flat key index, so the result is independent of -T. Each start
   runs the staged climb. */
/* A uniform double in [0, 1) from the splitmix64 stream (top 53 bits). */
static inline double uniform01(uint64_t * rng)
{
  return (splitmix64(rng) >> 11) * (1.0 / 9007199254740992.0);   /* 2^53 */
}

/* Draw two distinct letters a != b uniformly in 0..25 (two RNG draws). */
static inline void random_pair(uint64_t * rng, int & a, int & b)
{
  a = static_cast<int>(splitmix64(rng) % asize);
  b = static_cast<int>(splitmix64(rng) % (asize - 1));
  if (b >= a)
    b++;
}

/* Number of plug pairs currently set on the involution board. */
static inline int plug_count(const machine & m)
{
  int n = 0;
  for (int i = 0; i < asize; i++)
    if (m.steckerbrett[i] > i)
      n++;
  return n;
}

/* The single SA move: toggle the plug between a and b on the involution steckerbrett.
   If a-b is already a plug, remove it; otherwise force a-b, ejecting each endpoint's
   old partner to self-steckered. Reaches any involution from any other (ergodic).
   When cap < 13 (a known plug count, from the -S target-stage cap), a *connect* that
   would raise the pair count above cap is a no-op -- a connect grows the count only
   when both endpoints are currently self-steckered; removes and re-pairings never do,
   so every board with <= cap pairs stays reachable. A move touching a fixed -s letter
   is also a no-op, so preset plugs survive annealing. */
static inline void apply_toggle(machine & m, int a, int b, int cap)
{
  if (plug_fixed[a] || plug_fixed[b])         /* never disturb a fixed -s plug */
    return;
  if (m.steckerbrett[a] == b)                 /* already paired -> remove */
    {
      m.steckerbrett[a] = static_cast<unsigned char>(a);
      m.steckerbrett[b] = static_cast<unsigned char>(b);
    }
  else                                        /* connect, ejecting old partners */
    {
      bool grows = (m.steckerbrett[a] == a) && (m.steckerbrett[b] == b);
      if (grows && (plug_count(m) >= cap))
        return;                               /* would exceed the cap -> no-op */
      int ap = m.steckerbrett[a];
      int bp = m.steckerbrett[b];
      m.steckerbrett[ap] = static_cast<unsigned char>(ap);
      m.steckerbrett[bp] = static_cast<unsigned char>(bp);
      m.steckerbrett[a] = static_cast<unsigned char>(b);
      m.steckerbrett[b] = static_cast<unsigned char>(a);
    }
}

/* One simulated-annealing trajectory on the current board (Phase 1: full rescore per
   move, flat target model). Calibrates the temperature from a warm-up sample so it is
   length/model-robust (archived/SIMULATED_ANNEALING.md §4), cools geometrically, tracks the best
   board seen (incumbent), then finishes with a greedy quench so the result is at least
   a local optimum. Leaves m at the best board (m.plaintext set by the quench's decode).
   All randomness comes from the per-key *rng stream, so it is -T-independent. */
static double anneal_once(machine & m, uint64_t * rng)
{
  /* The whole trajectory honours the -S target-stage plug cap (uncapped = 13 by
     default). When you know the true plug count is below the maximum (e.g. -S q8 for
     an 8-plug board), capping keeps SA from adding spurious plugs that a short, noisy
     quad score would otherwise reward -- a measured win on short messages at modest
     budgets, neutral once the message/budget is large enough to recover the true board
     unaided. Set below the true count it clips and hurts, so it is a user-supplied
     prior (archived/SIMULATED_ANNEALING.md §16). */
  int cap = opt_stages[opt_nstages - 1].cap;

  /* IC pre-pass: greedy-climb under the index of coincidence to seed a decent board
     before annealing the target model. The quad surface is nearly flat with only a
     plug or two set, so annealing it from an empty board wanders; IC is far smoother
     and places the first plugs well (the same insight as the -S iq staged climb). */
  int target_model = m.scoring;
  if (target_model != SCORE_IC)
    {
      m.scoring = SCORE_IC;
      hillclimb<false>(m, cap);
      m.scoring = target_model;
    }

  double cur = score_iter(m);
  double best = cur;
  unsigned char best_board[asize];
  unsigned char saved[asize];
  memcpy(best_board, m.steckerbrett, asize);

  /* Warm-up: sample K random moves from the start board, average the magnitude of the
     worsening ones, and set T0/Tend so a worsening move is accepted with probability
     ~chi0 initially and ~chi_end at the end. */
  double sum_neg = 0.0;
  int n_neg = 0;
  for (int i = 0; i < anneal_warmup; i++)
    {
      int a, b;
      random_pair(rng, a, b);
      memcpy(saved, m.steckerbrett, asize);
      apply_toggle(m, a, b, cap);
      double d = score_iter(m) - cur;
      memcpy(m.steckerbrett, saved, asize);   /* sampling only -- always restore */
      if (d < 0.0)
        {
          sum_neg += -d;
          n_neg++;
        }
    }
  double meanabs = (n_neg > 0) ? sum_neg / n_neg : 1e-9;
  double T0 = meanabs / log(1.0 / anneal_chi0);
  double Tend = meanabs / log(1.0 / anneal_chi_end);
  if (T0 < 1e-12)
    T0 = 1e-12;
  if (Tend < 1e-12 || Tend > T0)
    Tend = T0 * 1e-6;

  int total = (opt_anneal > 0) ? opt_anneal : 1;
  double alpha = pow(Tend / T0, static_cast<double>(anneal_chain) / total);

  double T = T0;
  int moves = 0;
  while (moves < total)
    {
      for (int i = 0; (i < anneal_chain) && (moves < total); i++)
        {
          int a, b;
          random_pair(rng, a, b);
          memcpy(saved, m.steckerbrett, asize);
          apply_toggle(m, a, b, cap);
          double d = score_iter(m) - cur;
          if ((d >= 0.0) || (uniform01(rng) < exp(d / T)))
            {
              cur += d;
              if (cur > best)
                {
                  best = cur;
                  memcpy(best_board, m.steckerbrett, asize);
                  report_climb_progress(m, best);
                }
            }
          else
            memcpy(m.steckerbrett, saved, asize);   /* reject -> undo */
          moves++;
        }
      T *= alpha;
    }

  memcpy(m.steckerbrett, best_board, asize);
  hillclimb<false>(m, cap);   /* greedy quench under the target model, same cap */
  return score_iter(m);
}

/* Optimise the plugboard for the current key from the current board: simulated
   annealing (-A) or the staged greedy climb (default). */
static double optimize_once(machine & m, uint64_t * rng)
{
  if (opt_anneal > 0)
    return anneal_once(m, rng);
  return run_stages<false>(m);
}

/* Independent RNG seed for one restart, mixed from opt_seed, the flat key index and the
   restart index with a splitmix64 finaliser. Each restart draws from its OWN stream --
   not a single stream advanced sequentially through the restarts -- so restarts are
   order-independent and can run in any order / on any thread and still be reproducible
   (the precondition for parallelising them; see hillclimb_one). opt_seed==0 keeps the
   historical seedless-but-deterministic behaviour. */
static inline uint64_t restart_seed(size_t key_index, int restart)
{
  uint64_t z = opt_seed + 0x0123456789abcdefULL
             + static_cast<uint64_t>(key_index) * 0x9E3779B97F4A7C15ULL
             + static_cast<uint64_t>(restart)   * 0xC2B2AE3D27D4EB4FULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* --dump-restarts: emit one converged restart's (score, plugboard) to stderr for the
   restart-diversity diagnostic. The board is written canonically (each pair low-high,
   pairs ordered by low letter) so a harness can dedupe boards by string equality. Under
   a mutex; display-only, so results stay -T-deterministic. */
static std::mutex g_dump_mutex;
static void dump_restart(machine & m, double score)
{
  char board[3 * 13];
  char * p = board;
  for (int j = 0; j < asize; j++)
    if (m.steckerbrett[j] > j)
      {
        if (p > board)
          *p++ = ' ';
        *p++ = num2char(j);
        *p++ = num2char(m.steckerbrett[j]);
      }
  *p = 0;
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  fprintf(stderr, "restart %.4f %s\n", score, board);
}

/* One plugboard-recovery climb from the seed board. In kicked mode (--restarts N>=1) every
   climb -- including index 0 -- injects a fresh --random kick first, so the un-kicked seed
   climb is not run (REDESIGN Option A). With --restarts 0 there is a single un-kicked climb.
   Each restart draws from its own independent (key,restart) stream, so it is a self-contained
   unit of work; leaves m at this climb's converged board + plaintext and returns its score. */
static double hillclimb_one(machine & m, size_t key_index, int restart)
{
  init_steckerbrett(m, opt_steckerbrett);
  uint64_t rng = restart_seed(key_index, restart);
  if (opt_restarts >= 1)
    perturb_steckerbrett(m, & rng, opt_perturb);
  double score = optimize_once(m, & rng);
  if (opt_dump_restarts)
    dump_restart(m, score);
  if (opt_restart_tt)
    {
      std::lock_guard<std::mutex> lock(g_tt_mutex);
      tt_touch(g_restart_tt, m.steckerbrett, score);
    }
  return score;
}

/* Run all the climbs for one key sequentially, keeping the best (used where the search
   parallelises over keys rather than restarts -- the -F tier-2 climb). --restarts 0 is a
   single un-kicked seed climb; --restarts N is N kicked climbs (indices 0..N-1). search_worker's
   main path instead spreads the individual restarts across threads via hillclimb_one, so
   both share the same per-restart seeding and reach the same best. */
double hillclimb_restarts(machine & m, size_t key_index)
{
  const int climbs = (opt_restarts >= 1) ? opt_restarts : 1;
  double best = hillclimb_one(m, key_index, 0);
  if (climbs <= 1)
    return best;

  /* Keep the best restart's plaintext AND its plugboard together: each restart leaves
     m.steckerbrett at its own converged board, so without saving/restoring the board
     the machine would end up holding the LAST restart's plugboard while the returned
     score and plaintext are the best restart's -- showconfig() would then print a
     plugboard that does not match the winning decrypt (the reported bug). */
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
  memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
  memcpy(best_steck, m.steckerbrett, asize);

  for (int r = 1; r < climbs; r++)
    {
      double s = hillclimb_one(m, key_index, r);
      if (s > best)
        {
          best = s;
          memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
          memcpy(best_steck, m.steckerbrett, asize);
        }
    }
  memcpy(m.plaintext, best_pt, static_cast<size_t>(textlength) + 1);
  memcpy(m.steckerbrett, best_steck, asize);   /* restore the best board to match */
  return best;
}




/* The reflector x wheel-order combinations are the unit of parallelism: each is
   independent (its own precompute + ring/start sweep). The ring/start ranges are
   identical for every task. */
struct wheel_task
{
  int u;
  int w[wheels];
  int greek;        /* M4 Greek rotor index, else -1 */
  int greek_off;    /* M4 (Greek start - ring) mod 26, else 0 */
};

struct search_range
{
  int r_min[wheels], r_max[wheels];
  int g_min[wheels], g_max[wheels];
};

/* Best result so far, shared across worker threads. It is updated (and the live
   progress line printed) under the mutex only when a worker beats the current
   global best; improvements are rare, so contention is negligible. */
struct best_result
{
  std::mutex mutex;
  double score = score_min;
  size_t idx = static_cast<size_t>(-1);   /* work index of the best (for the tie-break) */
  bool found = false;
  char plaintext[maxlen+1];
  /* Winning plugboard, recorded at the merge so a post-search --gainfix-best pass can
     reconstruct the machine (via the key from `idx`) and finish the single best board. */
  unsigned char steckerbrett[asize];
  /* Highest score already ECHOED as a progress line -- display state only, never read
     by the merge logic, so it cannot affect which candidate wins (the -T-determinism
     contract is untouched). It can run ahead of `score`: an intermediate plugboard
     inside a still-running climb echoes as soon as it beats every line shown so far,
     while `score` only advances when a finished work item merges. Atomic so climbing
     workers can pre-check it cheaply (and race-free) before taking the mutex. */
  std::atomic<double> shown{score_min};
  /* Column header emitted before the first progress line (display state, only
     touched under the mutex). */
  bool header_shown = false;
};

/* The live search's shared best, exported for report_climb_progress (the climbs sit
   above bruteforce() in the file and know nothing else about the search phase).
   Set by bruteforce() before the workers start; null outside a search. */
static best_result * g_progress = nullptr;

/* Print one progress line under the best-result mutex, emitting the column
   header before the first line of the run. */
static void progress_line(best_result & b, machine & m, double score)
{
  if (! b.header_shown)
    {
      b.header_shown = true;
      showconfig_header();
    }
  showconfig(m, score);
}

/* Echo an intermediate plugboard improvement from inside a climb: the same
   progress line the key-level merge prints, but at the granularity the user
   actually watches -- every accepted climb move that beats everything echoed so
   far, not just every finished climb. Gated to the TARGET scoring model (a staged
   pre-pass and the -F tier-1 filter climb score in a different model, so their values
   are not comparable with the ranking scores) and to workers that opted in
   (m.report). Costs one relaxed atomic load per ACCEPTED move -- nothing on the
   325-move scoring scans -- so the hot path is untouched. The worker restored the
   machine's grundstellung right after setup_mapping (climb paths only), so
   showconfig() prints the true start positions here. */
static void report_climb_progress(machine & m, double score)
{
  best_result * bp = g_progress;
  if ((bp == nullptr) || (! m.report) || (m.scoring != opt_scoring))
    return;
  if (score <= bp->shown.load(std::memory_order_relaxed))
    return;
  std::lock_guard<std::mutex> lock(bp->mutex);
  if (score <= bp->shown.load(std::memory_order_relaxed))
    return;   /* another thread echoed something at least as good meanwhile */
  bp->shown.store(score, std::memory_order_relaxed);
  progress_line(* bp, m, score);
}

/* Deterministic ordering of candidates: higher score wins, ties broken by lower work
   index. Parallel restarts of one key often converge to the same score, so an explicit
   tie-break is what keeps the global best independent of thread count and merge order
   (the -T-independence contract) rather than "first thread to merge wins". */
static inline bool better_cand(double s1, size_t i1, double s2, size_t i2)
{
  return (s1 > s2) || ((s1 == s2) && (i1 < i2));
}

/* --- parallel search -------------------------------------------------------

   The search runs in two parallel phases over a fixed pool of per-thread
   machines:

   1. Precompute the rotor-stack table for every (reflector x wheel-order) once,
      into one big shared read-only block. (A table depends only on the reflector
      and wheel order, and serves every ring/start of that wheel order via the
      start-minus-ring offset; brute force has no early exit, so every table is
      needed anyway.)
   2. Sweep the whole flat (wheel-order x ring x start) key space: an atomic
      counter hands out adaptive-sized chunks, each worker decodes and scores its
      keys against the shared tables using its own private mapping.

   Parallelising the flat key space (not just the wheel order) means a search
   with the wheels fixed but ring/start wildcarded uses every thread -- the old
   wheel-order-only scheme left exactly that case single-threaded. */

/* Accounting for the final diagnostic (set by bruteforce). */
static size_t g_table_count = 0;
static size_t g_table_bytes = 0;
static size_t g_keys_analysed = 0;       /* rotor combinations examined */
static uint64_t g_plugboards_scored = 0; /* total score_iter calls across workers */
static uint64_t g_sc_hits = 0;           /* --score-tt: score_iter calls served from cache */
static uint64_t g_sc_lookups = 0;        /* --score-tt: cache lookups (== calls with cache on) */

/* base pointer into the rotor-stack table block: the same type as
   machine::subst_array, so 'all + i*asize' is task i's [asize]^4 table */
typedef unsigned char (* subst_table)[asize][asize][asize];

/* Phase 1: fill the table for each wheel-order task pulled off the counter.
   all + i*asize is task i's table (asize rows of [asize][asize][asize]). */
void precompute_worker(machine & m,
                       const std::vector<wheel_task> & tasks,
                       std::atomic<size_t> & next_task,
                       subst_table all)
{
  size_t i;
  while ((i = next_task.fetch_add(1)) < tasks.size())
    {
      const wheel_task & t = tasks[i];
      init_walzen(m, t.u, t.w[0], t.w[1], t.w[2]);
      m.greek = t.greek;
      m.greek_offset = t.greek_off;
      set_effective_reflector(m);   /* fold in the Greek wheel (M4) once per task */
      m.subst_array = all + i * asize;
      precompute(m);
    }
}

/* Phase 2: decode + score a slice of the flat key space. A flat index decodes to
   (wheel-order, ring combo, start combo) by mixed radix over the per-position
   ranges; the worker points its machine at the already-computed table for that
   wheel order (no recompute) and re-reads the wheel order's settings only when
   it changes from one key to the next. */
void search_worker(machine & m,
                   const std::vector<wheel_task> & tasks,
                   const search_range & range,
                   const int * rc, const int * gc,
                   subst_table all,
                   size_t rsize, size_t gsize,
                   std::atomic<size_t> & next_key,
                   size_t chunk,
                   size_t restarts,
                   best_result & best)
{
  const size_t rg = rsize * gsize;
  const size_t nkeys = tasks.size() * rg;
  /* Work items = keys x restarts. With -c the R restarts of a key are independent, so
     each is its own item (restart innermost, so consecutive items share a key and reuse
     setup_mapping); this is what lets a fully-specified rotor key still fill every thread.
     For the plain scan restarts==1, so the space is just the keys, exactly as before. */
  const size_t total = nkeys * restarts;
  const size_t rc12 = static_cast<size_t>(rc[1]) * rc[2];
  const size_t gc12 = static_cast<size_t>(gc[1]) * gc[2];

  m.scoring = opt_scoring;   /* per-machine; the staged climb varies it transiently */
  m.report = (opt_hillclimb != 0);   /* echo intermediate climb improvements */

  double local_best = score_min;
  size_t local_best_idx = static_cast<size_t>(-1);
  size_t cur_wo = static_cast<size_t>(-1);
  size_t cur_key = static_cast<size_t>(-1);
  int r1 = 0, r2 = 0, r3 = 0, g1 = 0, g2 = 0, g3 = 0;   /* current key's ring/start */

  size_t start;
  while ((start = next_key.fetch_add(chunk)) < total)
    {
      size_t end = start + chunk;
      if (end > total)
        end = total;

      for (size_t idx = start; idx < end; idx++)
        {
          size_t keyidx = idx / restarts;
          int restart = static_cast<int>(idx % restarts);

          if (keyidx != cur_key)   /* new key: (re)build the rotor stack, reused by its restarts */
            {
              cur_key = keyidx;
              size_t wo = keyidx / rg;
              size_t rem = keyidx % rg;
              size_t rflat = rem / gsize;
              size_t gflat = rem % gsize;

              if (wo != cur_wo)
                {
                  cur_wo = wo;
                  const wheel_task & t = tasks[wo];
                  m.subst_array = all + wo * asize;
                  init_walzen(m, t.u, t.w[0], t.w[1], t.w[2]);
                  m.greek = t.greek;            /* for showconfig of a new best (M4) */
                  m.greek_offset = t.greek_off;
                }

              r1 = range.r_min[0] + static_cast<int>(rflat / rc12);
              int rr = static_cast<int>(rflat % rc12);
              r2 = range.r_min[1] + rr / rc[2];
              r3 = range.r_min[2] + rr % rc[2];
              g1 = range.g_min[0] + static_cast<int>(gflat / gc12);
              int gg = static_cast<int>(gflat % gc12);
              g2 = range.g_min[1] + gg / gc[2];
              g3 = range.g_min[2] + gg % gc[2];

              init_ring_grund(m, r1, r2, r3, g1, g2, g3);
              /* hill-climb re-reads each row many times -> copy into contiguous
                 mapping[]; the scan reads straight from the shared subst_array */
              setup_mapping(m, opt_hillclimb != 0);
              /* setup_mapping stepped grundstellung; on the climb path restore the
                 start positions now, so an intermediate progress line (echoed from
                 inside the climb, where r1..g3 are out of reach) shows the true
                 config. The scan keeps the lazy restore below (no mid-key echoes,
                 and no extra per-key writes on its init-dominated path). */
              if (opt_hillclimb)
                init_ring_grund(m, r1, r2, r3, g1, g2, g3);
            }

          /* Run one work unit: a restart climb, an --exhaust first-pair unit, or one scan
             score. Both hillclimb_one and exhaust_unit draw only from their own
             (keyidx, restart)/(keyidx) streams, so the result is independent of which thread
             runs the unit. For --exhaust the per-key units are the first-pair choices, so
             `restart` here indexes g_exhaust_firsts. The scan does not decode per key (the
             fused scorer reads each row once, straight from subst_array); the plaintext is
             materialised only for a new best, below. */
          double score;
          if (opt_hillclimb)
            score = opt_exhaust ? exhaust_unit(m, keyidx, static_cast<size_t>(restart))
                                : hillclimb_one(m, keyidx, restart);
          else
            {
              init_steckerbrett(m, opt_steckerbrett);
              score = score_iter(m);
            }

          /* Crib finisher: rank the converged board by n-gram score + known-word bonus.
             m.plaintext holds this board's decrypt on the climb path, so no extra decode. */
          if (opt_crib && opt_hillclimb)
            score += opt_crib_weight * crib_score(m);

          if (better_cand(score, idx, local_best, local_best_idx))
            {
              std::lock_guard<std::mutex> lock(best.mutex);
              if (better_cand(score, idx, best.score, best.idx))
                {
                  if (! opt_hillclimb)
                    decode(m);   /* fill m.plaintext for this winning key */
                  best.score = score;
                  best.idx = idx;
                  best.found = true;
                  memcpy(best.plaintext, m.plaintext, textlength + 1);
                  memcpy(best.steckerbrett, m.steckerbrett, asize);   /* for --gainfix-best */
                  /* Echo the new best -- unless a progress line already showed this
                     score (a climb's last accepted move IS its converged board, so
                     reprinting it here would just duplicate the line). Ties that
                     win the merge on the idx tie-break are display-identical, so
                     they stay silent too. */
                  if (score > best.shown.load(std::memory_order_relaxed))
                    {
                      best.shown.store(score, std::memory_order_relaxed);
                      /* setup_mapping stepped grundstellung (scan path only; the
                         climb path restored it right after setup_mapping) */
                      init_ring_grund(m, r1, r2, r3, g1, g2, g3);
                      progress_line(best, m, score);
                    }
                }
              local_best = best.score;         /* track the global best for the filter */
              local_best_idx = best.idx;
            }
        }
    }
}

/* --- key pre-filter (-F) ---------------------------------------------------

   With -c, the full plugboard climb (-R restarts x -S stages) is paid on *every*
   key. The pre-filter instead ranks all keys by a single cheap index-of-coincidence
   climb -- which, unlike a plugboard-free IC scan, partially recovers the stecker
   and so discriminates the true rotor key even under a full 10-pair board -- and
   then runs the expensive climb only on the top -F keys. */

/* Decode a flat key index and configure the machine for it: switch to the wheel
   order's table only when it changes from cur_wo, set ring/start, reset the
   plugboard and build mapping[]. Fills rg6 = {r1,r2,r3,g1,g2,g3} for showconfig. */
static void key_to_machine(machine & m, size_t idx,
                           const std::vector<wheel_task> & tasks,
                           const search_range & range, const int * rc, const int * gc,
                           subst_table all, size_t rg, size_t gsize,
                           size_t rc12, size_t gc12, size_t & cur_wo, int rg6[6])
{
  size_t wo = idx / rg;
  size_t rem = idx % rg;
  size_t rflat = rem / gsize;
  size_t gflat = rem % gsize;

  if (wo != cur_wo)
    {
      cur_wo = wo;
      const wheel_task & t = tasks[wo];
      m.subst_array = all + wo * asize;
      init_walzen(m, t.u, t.w[0], t.w[1], t.w[2]);
      m.greek = t.greek;
      m.greek_offset = t.greek_off;
    }

  int r1 = range.r_min[0] + static_cast<int>(rflat / rc12);
  int rr = static_cast<int>(rflat % rc12);
  int r2 = range.r_min[1] + rr / rc[2];
  int r3 = range.r_min[2] + rr % rc[2];
  int g1 = range.g_min[0] + static_cast<int>(gflat / gc12);
  int gg = static_cast<int>(gflat % gc12);
  int g2 = range.g_min[1] + gg / gc[2];
  int g3 = range.g_min[2] + gg % gc[2];

  init_ring_grund(m, r1, r2, r3, g1, g2, g3);
  init_steckerbrett(m, opt_steckerbrett);
  setup_mapping(m, true);
  /* restore the start positions setup_mapping stepped, so mid-climb progress lines
     (finish_worker) echo the true config; rg6 carries them to the callers */
  init_ring_grund(m, r1, r2, r3, g1, g2, g3);
  rg6[0] = r1; rg6[1] = r2; rg6[2] = r3; rg6[3] = g1; rg6[4] = g2; rg6[5] = g3;
}

struct scored_key { double score; size_t idx; };

/* A min-heap that keeps the top-N keys: top() is the eviction candidate -- the
   lowest score, ties broken by the largest idx (so equal scores keep the lower idx,
   which makes the kept set deterministic and -T-independent). */
struct keep_worse
{
  bool operator()(const scored_key & a, const scored_key & b) const
  {
    if (a.score != b.score)
      return a.score > b.score;   /* top() = smallest score */
    return a.idx < b.idx;         /* tie: top() = largest idx */
  }
};

/* Tier 1: rank a slice of the flat key space by a cheap IC climb; keep the
   thread-local top-N, then merge into the shared candidate list. When show_progress
   is set (stderr is a terminal) it also updates a live "\r" progress line: the shared
   'progress' counter tracks keys ranked, and because each atomic add owns a disjoint
   range of that counter, exactly one thread crosses each 1%-of-total boundary and
   prints it -- so the line advances once per percent with no races or duplicates. */
void filter_worker(machine & m,
                   const std::vector<wheel_task> & tasks,
                   const search_range & range, const int * rc, const int * gc,
                   subst_table all, size_t rsize, size_t gsize,
                   std::atomic<size_t> & next_key, size_t chunk, size_t topn,
                   std::mutex & cand_mutex, std::vector<scored_key> & cand,
                   std::atomic<size_t> & progress, bool show_progress)
{
  const size_t rg = rsize * gsize;
  const size_t total = tasks.size() * rg;
  const size_t rc12 = static_cast<size_t>(rc[1]) * rc[2];
  const size_t gc12 = static_cast<size_t>(gc[1]) * gc[2];
  const size_t step = (total >= 100) ? total / 100 : 1;   /* progress granularity */

  m.report = false;   /* tier-1 filter scores are not ranking scores; stay quiet */
  m.scoring = SCORE_IC;   /* the cheap, smooth-surface filter model */
  const int cap = filter_climb_cap;

  std::priority_queue<scored_key, std::vector<scored_key>, keep_worse> heap;
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];

  size_t start;
  while ((start = next_key.fetch_add(chunk)) < total)
    {
      size_t end = start + chunk;
      if (end > total)
        end = total;

      for (size_t idx = start; idx < end; idx++)
        {
          key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                         rc12, gc12, cur_wo, rg6);
          double s = hillclimb<false>(m, cap);   /* single capped IC climb */

          if (opt_true_key)   /* --true-key: record every key's tier-1 score, and this
                                 flat idx if it is the true key (for the rank print) */
            {
              g_tk_scores[idx] = static_cast<float>(s);
              const wheel_task & t = tasks[cur_wo];
              if ((t.u == g_tk_u)
                  && (t.w[0] == g_tk_w[0]) && (t.w[1] == g_tk_w[1]) && (t.w[2] == g_tk_w[2])
                  && (rg6[0] == g_tk_r[0]) && (rg6[1] == g_tk_r[1]) && (rg6[2] == g_tk_r[2])
                  && (rg6[3] == g_tk_g[0]) && (rg6[4] == g_tk_g[1]) && (rg6[5] == g_tk_g[2]))
                g_tk_idx.store(idx, std::memory_order_relaxed);
            }

          if (heap.size() < topn)
            heap.push(scored_key{s, idx});
          else
            {
              const scored_key & w = heap.top();
              if ((s > w.score) || ((s == w.score) && (idx < w.idx)))
                {
                  heap.pop();
                  heap.push(scored_key{s, idx});
                }
            }
        }

      if (show_progress)
        {
          size_t before = progress.fetch_add(end - start);
          size_t after = before + (end - start);
          /* print on each 1% boundary, and always on the final key so it reaches 100% */
          if (((after / step) != (before / step)) || (after == total))
            {
              std::lock_guard<std::mutex> lock(cand_mutex);
              fprintf(stderr, "\rPre-filter: ranking %3zu%% (%zu / %zu keys)",
                      (after * 100) / total, after, total);
              fflush(stderr);
            }
        }
    }

  std::lock_guard<std::mutex> lock(cand_mutex);
  while (! heap.empty())
    {
      cand.push_back(heap.top());
      heap.pop();
    }
}

/* Tier 2: run the full -R/-S plugboard climb on the shortlisted keys only, merging
   the global best exactly like search_worker's hill-climb path. */
void finish_worker(machine & m,
                   const std::vector<wheel_task> & tasks,
                   const search_range & range, const int * rc, const int * gc,
                   subst_table all, size_t rsize, size_t gsize,
                   const std::vector<size_t> & shortlist,
                   std::atomic<size_t> & next, best_result & best)
{
  const size_t rg = rsize * gsize;
  const size_t rc12 = static_cast<size_t>(rc[1]) * rc[2];
  const size_t gc12 = static_cast<size_t>(gc[1]) * gc[2];

  m.scoring = opt_scoring;
  m.report = (opt_hillclimb != 0);   /* echo intermediate climb improvements */

  double local_best = score_min;
  size_t local_best_idx = static_cast<size_t>(-1);
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];

  size_t k;
  while ((k = next.fetch_add(1)) < shortlist.size())
    {
      size_t idx = shortlist[k];
      key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                     rc12, gc12, cur_wo, rg6);

      double score = opt_exhaust ? exhaust_all_combos(m, idx)
                                  : hillclimb_restarts(m, idx);

      /* Crib finisher (see search_worker): rank by n-gram score + known-word bonus. */
      if (opt_crib && opt_hillclimb)
        score += opt_crib_weight * crib_score(m);

      if (better_cand(score, idx, local_best, local_best_idx))
        {
          std::lock_guard<std::mutex> lock(best.mutex);
          if (better_cand(score, idx, best.score, best.idx))
            {
              best.score = score;
              best.idx = idx;
              best.found = true;
              memcpy(best.plaintext, m.plaintext, textlength + 1);
              /* echo only if no progress line already showed this score (see the
                 matching note in search_worker) */
              if (score > best.shown.load(std::memory_order_relaxed))
                {
                  best.shown.store(score, std::memory_order_relaxed);
                  progress_line(best, m, score);
                }
            }
          local_best = best.score;
          local_best_idx = best.idx;
        }
    }
}

/* Resolve the search ranges from the options, enumerate the reflector x
   wheel-order tasks, precompute their rotor tables in parallel, then sweep the
   flat (wheel-order x ring x start) key space in parallel. The best decryption
   is written to 'result'. */
/* All the derived search dimensions for one run, built from the opt_* CLI state: the
   task list (reflector x Greek wheel x Greek offset x wheel order) and the ring/start
   ranges and counts. Bundled so bruteforce() reads as phases rather than one long
   setup block. */
struct key_space
{
  std::vector<wheel_task> tasks;
  search_range range;
  int rc[wheels], gc[wheels];   /* per-position ring / start counts */
  size_t rsize, gsize;          /* ring-combo and start-combo counts */
  size_t total_keys;            /* tasks.size() * rsize * gsize */
};

static key_space build_key_space()
{
  key_space ks;

  int u_min, u_max;
  if (opt_norenigma)
    {
      u_min = 0;
      u_max = 0;
    }
  else if (opt_m4)
    {
      /* thin reflector index: B -> m4_thin_base (UKW-b), C -> +1 (UKW-c) */
      if (opt_ukw[0] == '.')
        {
          u_min = m4_thin_base;
          u_max = m4_thin_base + 1;
        }
      else
        u_min = u_max = m4_thin_base + char2num(opt_ukw[0]) - 1;
    }
  else
    {
      if (opt_ukw[0] == '.')
        {
          u_min = 0;
          u_max = 2;
        }
      else
        u_min = u_max = char2num(opt_ukw[0]);
    }

  int w_min[wheels], w_max[wheels];
  for(int i=0; i<wheels; i++)
    {
      if (opt_walzen[i] == '.')
        {
          w_min[i] = 0;
          w_max[i] = opt_maxwheel - 1;
        }
      else
        {
          w_min[i] = w_max[i] = opt_walzen[i] - '1';
        }

      if (opt_ringstellung[i] == '.')
        {
          ks.range.r_min[i] = 0;
          ks.range.r_max[i] = 25;
        }
      else
        {
          ks.range.r_min[i] = ks.range.r_max[i] = char2num(opt_ringstellung[i]);
        }

      if (opt_grundstellung[i] == '.')
        {
          ks.range.g_min[i] = 0;
          ks.range.g_max[i] = 25;
        }
      else
        {
          ks.range.g_min[i] = ks.range.g_max[i] = char2num(opt_grundstellung[i]);
        }
    }

  for (int i = 0; i < wheels; i++)
    {
      ks.rc[i] = ks.range.r_max[i] - ks.range.r_min[i] + 1;
      ks.gc[i] = ks.range.g_max[i] - ks.range.g_min[i] + 1;
    }
  ks.rsize = static_cast<size_t>(ks.rc[0]) * ks.rc[1] * ks.rc[2];
  ks.gsize = static_cast<size_t>(ks.gc[0]) * ks.gc[1] * ks.gc[2];

  /* M4 adds two outer dimensions: the Greek wheel (Beta/Gamma) and its fixed
     offset. Only the (start - ring) offset of the static Greek wheel is
     identifiable, so the pos/ring ranges collapse to the set of distinct offsets
     (<= 26, not 26x26). Non-M4 searches use the single sentinels {-1} / {0}. */
  std::vector<int> greek_list;
  std::vector<int> offset_list;
  if (opt_m4)
    {
      if (opt_greek_walzen == '.')
        {
          greek_list.push_back(greek_base);
          greek_list.push_back(greek_base + 1);
        }
      else
        greek_list.push_back(greek_base + (opt_greek_walzen == 'B' ? 0 : 1));

      int gp_min, gp_max, gr_min, gr_max;
      if (opt_greek_grundstellung == '.') { gp_min = 0; gp_max = 25; }
      else gp_min = gp_max = char2num(opt_greek_grundstellung);
      if (opt_greek_ringstellung == '.') { gr_min = 0; gr_max = 25; }
      else gr_min = gr_max = char2num(opt_greek_ringstellung);

      bool seen[asize];
      for (int i = 0; i < asize; i++)
        seen[i] = false;
      for (int gp = gp_min; gp <= gp_max; gp++)
        for (int gr = gr_min; gr <= gr_max; gr++)
          seen[mod26(gp - gr)] = true;
      for (int off = 0; off < asize; off++)   /* ascending: deterministic order */
        if (seen[off])
          offset_list.push_back(off);
    }
  else
    {
      greek_list.push_back(-1);
      offset_list.push_back(0);
    }

  for (int u1 = u_min; u1 <= u_max; u1++)
    for (int gi : greek_list)
      for (int off : offset_list)
        for (int w1 = w_min[0]; w1 <= w_max[0]; w1++)
          for (int w2 = w_min[1]; w2 <= w_max[1]; w2++)
            for (int w3 = w_min[2]; w3 <= w_max[2]; w3++)
              if ((w1 != w2) && (w1 != w3) && (w2 != w3))
                ks.tasks.push_back(wheel_task{u1, {w1, w2, w3}, gi, off});

  /* The option validation should make this unreachable, but never run an empty
     search and emit uninitialised output. */
  if (ks.tasks.empty())
    fatal("No machine configuration was searched "
          "(check the -u / -w / -x settings)");

  ks.total_keys = ks.tasks.size() * ks.rsize * ks.gsize;
  return ks;
}

/* Allocate the shared read-only rotor-table block: one [asize]^4 (457 KB) table per
   task, all resident. A clean fatal() beats a std::terminate if the allocator refuses
   the block. (Under Linux overcommit a too-large request may instead succeed here and
   be OOM-killed later while precompute touches the pages.) */
static subst_table allocate_subst_tables(size_t nwo)
{
  try
    {
      return new unsigned char[nwo * asize][asize][asize][asize];
    }
  catch (const std::bad_alloc &)
    {
      char msg[160];
      double gb = nwo * static_cast<double>(asize) * asize * asize * asize / 1e9;
      snprintf(msg, sizeof msg,
               "Could not allocate %.1f GB for the rotor tables "
               "(narrow -u / -w / -x, or fix the M4 Greek wheel/position)", gb);
      fatal(msg);
    }
  return nullptr;   /* unreachable: fatal() exits */
}

/* Run per_thread(t) for t in [0, nthreads): inline when single-threaded, otherwise on
   a thread pool joined before returning. Every search phase uses this, so the
   spawn/join boilerplate lives in one place. Objects the per_thread lambda captures by
   reference outlive the join, so no std::ref wrapping is needed. */
template <typename F>
static void run_parallel(int nthreads, F per_thread)
{
  if (nthreads <= 1)
    {
      per_thread(0);
      return;
    }
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(nthreads));
  for (int t = 0; t < nthreads; t++)
    pool.emplace_back(per_thread, t);
  for (std::thread & th : pool)
    th.join();
}

void bruteforce(char * result)
{
  key_space ks = build_key_space();
  const std::vector<wheel_task> & tasks = ks.tasks;
  const search_range & range = ks.range;
  const int * rc = ks.rc;
  const int * gc = ks.gc;
  size_t rsize = ks.rsize;
  size_t gsize = ks.gsize;
  size_t nwo = tasks.size();
  size_t total_keys = ks.total_keys;

  /* memory accounting for the final diagnostic (one [asize]^4 (457 KB) table per
     task; a full M4 wildcard is ~14.9 GiB, every other mode far smaller) */
  g_table_count = nwo;
  g_table_bytes = nwo * static_cast<size_t>(asize) * asize * asize * asize;

  /* With -c and no -F, the per-key work units are independent, so the parallel space is
     total_keys x units -- this is what lets a fully-specified rotor key (total_keys==1) still
     use every thread. For a plain climb the units are the restarts: --restarts 0 is one
     (un-kicked) climb per key, --restarts N is N (kicked) climbs. For --exhaust the units are
     the first-pair choices (each runs its own sub-exhaustion x restarts; REDESIGN Part D), so
     exhaustion now scales with -T too. The plain scan and the -F tiers keep one item per key
     (restarts_par==1). */
  const size_t climbs_per_key =
    (opt_restarts >= 1) ? static_cast<size_t>(opt_restarts) : 1;
  const size_t units_per_key =
    opt_exhaust ? (g_exhaust_firsts.size() / 2) : climbs_per_key;
  size_t restarts_par =
    (opt_hillclimb && (opt_prefilter <= 0) && (opt_prefilter_frac <= 0.0))
      ? units_per_key : 1;
  size_t work_items = total_keys * restarts_par;

  if (opt_restart_tt)
    tt_alloc(g_restart_tt, work_items);

  /* never start more threads than there is work to hand out */
  int nthreads = opt_threads;
  if (work_items < static_cast<size_t>(nthreads))
    nthreads = static_cast<int>(work_items);
  if (nthreads < 1)
    nthreads = 1;

  subst_table all = allocate_subst_tables(nwo);

  std::vector<machine *> machines(static_cast<size_t>(nthreads));
  for (int t = 0; t < nthreads; t++)
    {
      machines[t] = new machine();   /* subst_array is pointed at 'all' per task */
      /* --score-tt: give each worker its own zero-initialised score cache (gen 0 marks
         every slot empty; setup_mapping bumps to 1 on the first key). Per-worker so no
         locking is needed and the memoisation stays a pure-local optimisation. */
      if (opt_score_tt)
        machines[t]->sc_cache = new sc_slot[sc_slots]();
    }

  /* phase 1: precompute every wheel order's table once, in parallel */
  std::atomic<size_t> next_task{0};
  run_parallel(nthreads, [&](int t)
    { precompute_worker(*machines[t], tasks, next_task, all); });

  /* phase 2: sweep the flat key space in adaptive chunks (~16 per thread: enough to
     balance the tail, few enough to amortise the atomic). The -F tiers are keyed over
     total_keys; the non-F sweep is over work_items (keys x restarts), so it gets its own
     chunk below. */
  best_result best;
  g_progress = & best;   /* climbs echo intermediate improvements against this */
  size_t chunk = total_keys / (static_cast<size_t>(nthreads) * 16);
  if (chunk < 1)
    chunk = 1;

  if ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0))
    {
      /* Tier 1: rank every key by a cheap IC climb, keep the top -F. The -F N% form
         resolves to a fraction of the (now known) keyspace; the absolute form is used
         as given. Either way keep at least 1 key and at most the whole keyspace. */
      size_t topn = (opt_prefilter_frac > 0.0)
        ? static_cast<size_t>(ceil(opt_prefilter_frac * static_cast<double>(total_keys)))
        : static_cast<size_t>(opt_prefilter);
      if (topn < 1)
        topn = 1;
      if (topn > total_keys)
        topn = total_keys;

      std::vector<scored_key> cand;
      std::mutex cand_mutex;
      std::atomic<size_t> fnext{0};
      std::atomic<size_t> fprogress{0};
      bool show_progress = isatty(fileno(stderr)) != 0;   /* live line only on a TTY */
      if (opt_true_key)   /* --true-key: size the per-key tier-1 score store */
        {
          g_tk_scores.assign(total_keys, 0.0f);
          g_tk_idx.store(static_cast<size_t>(-1), std::memory_order_relaxed);
        }
      run_parallel(nthreads, [&](int t)
        { filter_worker(*machines[t], tasks, range, rc, gc, all, rsize, gsize,
                        fnext, chunk, topn, cand_mutex, cand, fprogress,
                        show_progress); });
      if (show_progress)
        fprintf(stderr, "\n");   /* finish the live \r progress line */

      if (opt_true_key)   /* report the true key's tier-1 rank among all keys */
        {
          size_t tki = g_tk_idx.load(std::memory_order_relaxed);
          if (tki == static_cast<size_t>(-1))
            fprintf(stderr, "true-key tier1 rank: not in the searched keyspace (of %zu keys)\n",
                    total_keys);
          else
            {
              float ts = g_tk_scores[tki];
              size_t better = 0;
              for (size_t i = 0; i < total_keys; i++)
                if (g_tk_scores[i] > ts)
                  better++;
              fprintf(stderr, "true-key tier1 rank %zu of %zu\n", better + 1, total_keys);
            }
          g_tk_scores.clear();
          g_tk_scores.shrink_to_fit();
        }

      /* deterministic global top-N: highest score first, ties by lowest idx */
      std::sort(cand.begin(), cand.end(),
                [](const scored_key & a, const scored_key & b)
                {
                  if (a.score != b.score) return a.score > b.score;
                  return a.idx < b.idx;
                });
      if (cand.size() > topn)
        cand.resize(topn);
      std::vector<size_t> shortlist;
      shortlist.reserve(cand.size());
      for (const scored_key & sk : cand)
        shortlist.push_back(sk.idx);

      fprintf(stderr,
              "Pre-filter: ranked %zu keys by a cheap IC climb, "
              "running the full climb on the top %zu\n",
              total_keys, shortlist.size());

      /* Tier 2: full -R / -S climb on the shortlist only. */
      std::atomic<size_t> snext{0};
      run_parallel(nthreads, [&](int t)
        { finish_worker(*machines[t], tasks, range, rc, gc, all, rsize, gsize,
                        shortlist, snext, best); });
    }
  else
    {
      size_t schunk = work_items / (static_cast<size_t>(nthreads) * 16);
      if (schunk < 1)
        schunk = 1;
      std::atomic<size_t> next_key{0};
      run_parallel(nthreads, [&](int t)
        { search_worker(*machines[t], tasks, range, rc, gc, all,
                        rsize, gsize, next_key, schunk, restarts_par, best); });
    }

  /* --gainfix-best: an alternative to the per-convergence --gainfix. Instead of firing
     the gated cascade at every near-solution convergence, run ONE unconditional gain
     cascade + finishing climb on the single best board after all restarts. Reconstruct
     that board's machine from its key (best.idx) and the recorded steckerbrett. Only
     the simple sweep records best.idx as key*restarts+restart, so it is guarded to
     that path (no -F, no --exhaust). */
  if ((opt_gainfix_best || opt_gainfix_best3) && best.found)
    {
      machine & m = *machines[0];
      size_t rg = rsize * gsize;
      size_t rc12b = static_cast<size_t>(rc[1]) * rc[2];
      size_t gc12b = static_cast<size_t>(gc[1]) * gc[2];
      size_t cur_wo = static_cast<size_t>(-1);
      int rg6[6];
      key_to_machine(m, best.idx / restarts_par, tasks, range, rc, gc, all, rg, gsize,
                     rc12b, gc12b, cur_wo, rg6);
      for (int i = 0; i < asize; i++)
        m.steckerbrett[i] = best.steckerbrett[i];
      m.scoring = opt_scoring;
      m.report = false;
      int save_gf = opt_gainfix;
      int save_gf3 = opt_gainfix3;
      double save_gate = opt_gainfix_gate;
      opt_gainfix = 1;
      opt_gainfix3 = opt_gainfix_best3;   /* --gainfix-best3 also enables the 3-ply escalation */
      opt_gainfix_gate = score_min;   /* unconditional cascade on the one best board */
      /* Cap the finishing climb at the TARGET-STAGE cap, not asize/2 (uncapped) -- like
         every other finisher/quench in the tool (the staged tail at opt_stages[last].cap,
         the -A quench). An uncapped finish let gainfix-best add spurious plugs 11..cap that
         raise the noisy short-message quad score while hurting the truth (the over-plugging
         avenue of the saturation exact-loss, PERFORMANCE.md 4.10). */
      int fin_cap = opt_stages[opt_nstages - 1].cap;
      double s = hillclimb<false>(m, fin_cap);
      opt_gainfix = save_gf;
      opt_gainfix3 = save_gf3;
      opt_gainfix_gate = save_gate;
      /* Monotonic by construction: replace the best board ONLY when the finish scores
         strictly higher, so gainfix-best never returns a worse-scoring board than the
         search already found (a truth-vs-score chase at the information floor is a
         separate matter -- unfixable by a score-only rule; see PERFORMANCE.md 4.10). */
      if (s > best.score)
        {
          best.score = s;
          decode(m);
          memcpy(best.plaintext, m.plaintext, textlength + 1);
        }
    }

  /* diagnostics: every rotor combination is analysed (brute force has no early
     exit), and each worker counted the plugboards it scored -- sum them up */
  g_keys_analysed = total_keys;
  g_plugboards_scored = 0;
  g_sc_hits = 0;
  g_sc_lookups = 0;
  for (int t = 0; t < nthreads; t++)
    {
      g_plugboards_scored += machines[t]->plugboards_scored;
      g_sc_hits += machines[t]->sc_hits;
      g_sc_lookups += machines[t]->sc_lookups;
    }

  if (opt_restart_tt)
    {
      tt_report(g_restart_tt);
      tt_free(g_restart_tt);
    }

  for (int t = 0; t < nthreads; t++)
    {
      delete[] machines[t]->sc_cache;
      delete machines[t];
    }
  delete[] all;

  if (! best.found)
    fatal("No machine configuration produced a score");

  memcpy(result, best.plaintext, textlength + 1);
}

/* --- input, output, help, and CLI --------------------------------------- */

/* Streaming UTF-8 text filter shared by the ciphertext and plaintext readers.
   Carries the decode state (cp/need) across read() calls so a multi-byte code
   point split over a buffer boundary still decodes. Each letter is folded to its
   A-Z base (fold_codepoint); whitespace is dropped silently; every other
   non-mappable code point is dropped and counted so the reader can warn. */
struct textfilter
{
  unsigned cp;                 /* UTF-8 code-point accumulator */
  int need;                    /* continuation bytes still expected */
  unsigned long accented;      /* non-A-Z letters folded to a base letter */
  unsigned long skipped;       /* non-mappable, non-whitespace code points dropped */
};

static void filter_bytes(textfilter * st, const unsigned char * buf, ssize_t len,
                         char * out, int * j, const char * toolong)
{
  for (ssize_t i = 0; i < len; i++)
    {
      unsigned char b = buf[i];
      int done = -1;
      if (st->need > 0)
        {
          if ((b & 0xC0) == 0x80)
            {
              st->cp = (st->cp << 6) | (b & 0x3Fu);
              if (--st->need == 0)
                done = static_cast<int>(st->cp);
            }
          else
            {
              st->need = 0;   /* malformed: drop partial, reprocess b as a lead */
              i--;
              continue;
            }
        }
      else if (b < 0x80)
        done = b;
      else if ((b & 0xE0) == 0xC0) { st->cp = b & 0x1Fu; st->need = 1; }
      else if ((b & 0xF0) == 0xE0) { st->cp = b & 0x0Fu; st->need = 2; }
      else if ((b & 0xF8) == 0xF0) { st->cp = b & 0x07u; st->need = 3; }
      /* else: invalid lead byte -- ignored */

      if (done < 0)
        continue;

      unsigned u = static_cast<unsigned>(done);
      int base = fold_codepoint(u);
      if (base >= 0)
        {
          if (! (((u >= 'A') && (u <= 'Z')) || ((u >= 'a') && (u <= 'z'))))
            st->accented++;
          if (*j >= maxlen)
            fatal(toolong);
          out[(*j)++] = num2char(base);
        }
      else if ((u == ' ') || (u == '\t') || (u == '\n') || (u == '\r')
               || (u == '\f') || (u == '\v'))
        { /* whitespace: silently skipped */ }
      else
        st->skipped++;   /* non-mappable, non-whitespace: skip and count */
    }
}

static void warn_filtered(const textfilter * st, const char * what)
{
  if (st->accented > 0)
    fprintf(stderr, "Note: %s contained %lu non-A-Z letter(s); folded accents to "
            "their A-Z base form.\n", what, st->accented);
  if (st->skipped > 0)
    fprintf(stderr, "Note: %s contained %lu non-mappable character(s) (skipped).\n",
            what, st->skipped);
}

void readciphertext()
{
  unsigned char buffer[65536];
  ssize_t len;
  int j = 0;
  textfilter st = {0, 0, 0, 0};
  char toolong[64];
  snprintf(toolong, sizeof(toolong),
           "Ciphertext too long (maximum is %d letters)", maxlen);

  /* read() may return short; loop until EOF, filtering as we go */
  while ((len = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0)
    filter_bytes(& st, buffer, len, ciphertext, & j, toolong);

  if (len < 0)
    fatal("Error reading ciphertext from standard input");

  ciphertext[j] = 0;
  textlength = j;
  warn_filtered(& st, "ciphertext input");
}

void readplaintext(char * filename, const char * result)
{
  unsigned char buffer[65536];
  ssize_t len;
  int j = 0;

  int fd = open(filename, O_RDONLY);
  if (fd < 0)
    fatal("Unable to open plaintext file");

  textfilter st = {0, 0, 0, 0};
  char toolong[64];
  snprintf(toolong, sizeof(toolong),
           "Plaintext file too long (maximum is %d letters)", maxlen);

  /* read() may return short; loop until EOF, filtering as we go */
  while ((len = read(fd, buffer, sizeof(buffer))) > 0)
    filter_bytes(& st, buffer, len, altplaintext, & j, toolong);

  int read_error = (len < 0);
  close(fd);
  if (read_error)
    fatal("Error reading plaintext file");

  altplaintext[j] = 0;
  warn_filtered(& st, "plaintext file");

  if (textlength != j)
    fatal("Plaintext not same length as ciphertext");

  int identical = 0;
  for (int i=0; i<textlength; i++)
    if (result[i] == altplaintext[i])
      identical++;

  fprintf(stderr,
          "%d of %d letters (%.1f%%) identical to given plaintext\n",
          identical,
          textlength,
          100.0 * identical / textlength);
}

void alltoupper(char * text)
{
  int len = strlen(text);
  for(int i=0; i<len; i++)
    text[i] = toupper(text[i]);
}

/* version()/help() take the output stream: explicit -h/-v write to stdout and
   exit 0, while usage errors (no arguments, bad option) write to stderr and
   exit 1. */
void version(FILE * out)
{
  fprintf(out, "Enigma cipher tool version 1.1.0\n");
  fprintf(out, "Copyright (C) 2017-2026 Torbjørn Rognes\n");
  fprintf(out, "\n");
}

void help(FILE * out)
{
  version(out);
  fprintf(out, "Usage: enigma [OPTIONS]\n");
  fprintf(out, "\n");

  /* Options are grouped basic/advanced; every option shows its short flag and
     its long alias. Unambiguous long-name prefixes (e.g. --lang, --restart) also
     work. Descriptions are aligned in a 24-column spec field (continuation lines
     pass an empty spec) and kept within 79 columns. */
  fprintf(out, "Basic options:\n");
  fprintf(out, "  %-24s %s\n", "-h, --help", "Show help information");
  fprintf(out, "  %-24s %s\n", "-v, --version", "Show version information");
  fprintf(out, "  %-24s %s\n", "-u, --reflector X",
          "Reflector (umkehrwalze); A-C, N, M4 b/c, or . [.]");
  fprintf(out, "  %-24s %s\n", "-w, --wheels XYZ", "Wheels (walzen); 1-8 or . [...]");
  fprintf(out, "  %-24s %s\n", "-r, --rings XYZ",
          "Ring positions (ringstellung); A-Z or . [AA.]");
  fprintf(out, "  %-24s %s\n", "-g, --start-position XYZ",
          "Start positions (grundstellung); A-Z or . [...]");
  fprintf(out, "  %-24s %s\n", "-s, --plugboard AB...",
          "Plugboard (steckerbrett) A-Z letter pairs [none];");
  fprintf(out, "  %-24s %s\n", "", "held fixed; the -c/-A climb finds the rest");
  fprintf(out, "  %-24s %s\n", "-n, --norway",
          "Norway Enigma: reflector N and wheels (1-5)");
  fprintf(out, "  %-24s %s\n", "-4, --m4", "M4 (4-rotor naval) mode. -u selects the thin");
  fprintf(out, "  %-24s %s\n", "", "reflector b/c; -w/-r/-g take 4 chars (Greek");
  fprintf(out, "  %-24s %s\n", "", "wheel/ring/start first)");
  fprintf(out, "  %-24s %s\n", "-c, --climb",
          "Perform hill climbing to find plugboard settings");
  fprintf(out, "  %-24s %s\n", "-R, --restarts N",
          "Random restart attempts: 0 = one deterministic");
  fprintf(out, "  %-24s %s\n", "", "climb; N = N kicked climbs, keep best [0]");
  fprintf(out, "  %-24s %s\n", "-S, --score schedule",
          "Staged plugboard climb: <letter><cap> tokens,");
  fprintf(out, "  %-24s %s\n", "", "models i/m/b/t/q/a (number caps plug pairs; the last");
  fprintf(out, "  %-24s %s\n", "", "stage is the target/ranking model). E.g. --score");
  fprintf(out, "  %-24s %s\n", "", "m4a10 (mono pre-pass then weighted, both capped).");
  fprintf(out, "  %-24s %s\n", "", "Without -c only the target model is used (to rank).");
  fprintf(out, "  %-24s %s\n", "-l, --language language",
          "Scoring language (english/german/danish/french);");
  fprintf(out, "  %-24s %s\n", "", "required for -m/-b/-t/-q (no default); not for -i");
  fprintf(out, "  %-24s %s\n", "-i, --ic",
          "Index of coincidence (IC); needs no -l [default]");
  fprintf(out, "  %-24s %s\n", "-m, --mono", "Monogram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-b, --bi", "Bigram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-t, --tri", "Trigram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-q, --quad", "Quadgram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-a, --weighted",
          "Weighted all-order score (log-linear mix of");
  fprintf(out, "  %-24s %s\n", "", "quad/tri/bi/mono); sharper on short messages.");
  fprintf(out, "  %-24s %s\n", "", "Recommended: -c -S m4a10 -J --gainfix-best3");
  fprintf(out, "  %-24s %s\n", "-d, --ngrams directory",
          "Dir with n-gram files (or $ENIGMA_DATA) [ngrams]");
  fprintf(out, "  %-24s %s\n", "-T, --threads N",
          "Worker threads for the search (1-256) [1]");
  fprintf(out, "\n");
  fprintf(out, "Advanced options:\n");
  fprintf(out, "  %-24s %s\n", "-x, --max-wheel N", "Highest wheel number to use (3-8) [5]");
  fprintf(out, "  %-24s %s\n", "-A, --anneal N",
          "Recover the plugboard by simulated annealing");
  fprintf(out, "  %-24s %s\n", "", "instead of the greedy climb; N = move budget");
  fprintf(out, "  %-24s %s\n", "", "(needs -c) [off]. Honours the -S target cap:");
  fprintf(out, "  %-24s %s\n", "", "-A N -S qK caps it at K plugs");
  fprintf(out, "  %-24s %s\n", "-J, --dynamic-order",
          "Like -I with dynamic best-first move ordering;");
  fprintf(out, "  %-24s %s\n", "", "wins ~10-plug, may lose few-plug (implies -I) [off]");
  fprintf(out, "  %-24s %s\n", "-M, --cap-target",
          "Make the plug cap a strict descent target: only");
  fprintf(out, "  %-24s %s\n", "", "merge/remove at/over the cap; pair with a tight");
  fprintf(out, "  %-24s %s\n", "", "-S cap (needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "--gainfix-best3",
          "Gain cascade once on the best board, plus a deeper");
  fprintf(out, "  %-24s %s\n", "", "3-ply cascade for 3-plug tangles; the recommended");
  fprintf(out, "  %-24s %s\n", "", "finisher, near-free at K=8 (needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "--crib-file F",
          "Known-word (crib) finisher: rank converged boards");
  fprintf(out, "  %-24s %s\n", "", "by score + weight*(known words present); measured");
  fprintf(out, "  %-24s %s\n", "", "neutral/dominated (needs -c) [off], not recommended");
  fprintf(out, "  %-24s %s\n", "--crib-weight X",
          "Weight of the crib bonus vs the n-gram score [0.5]");
  fprintf(out, "  %-24s %s\n", "-e, --seed N", "Random seed for restarts/annealing (also");
  fprintf(out, "  %-24s %s\n", "", "$ENIGMA_SEED); default fresh each run, echoed");
  fprintf(out, "  %-24s %s\n", "-p, --compare filename",
          "Plaintext file to compare the result against");
  fprintf(out, "  %-24s %s\n", "--random K",
          "Random-kick size: plug pairs injected per restart");
  fprintf(out, "  %-24s %s\n", "", "(needs -c; 0 = no kick, a control) [10]");
  fprintf(out, "\n");
  fprintf(out, "Non-recommended options (opt-in; dominated, ablation, or only\n");
  fprintf(out, "situational -- not proven to beat the recommended knobs above):\n");
  fprintf(out, "  %-24s %s\n", "-I, --first-improve",
          "First-improvement climb: ~2.8x cheaper per climb,");
  fprintf(out, "  %-24s %s\n", "", "pair with more -R (needs -c) [off]; prefer -J");
  fprintf(out, "  %-24s %s\n", "-F, --prefilter N[%]",
          "Key pre-filter: rank by a cheap IC climb, then");
  fprintf(out, "  %-24s %s\n", "", "run the full -c climb on only the top N keys, or");
  fprintf(out, "  %-24s %s\n", "", "top N% of the keyspace (needs -c) [off];");
  fprintf(out, "  %-24s %s\n", "", "long messages only, weak on short");
  fprintf(out, "  %-24s %s\n", "--repair3",
          "Last-resort 3-plug reshuffle at convergence (a");
  fprintf(out, "  %-24s %s\n", "", "deeper try_repair; needs -c) [off]; dominated");
  fprintf(out, "  %-24s %s\n", "--no-repair",
          "Disable the 2-plug re-pair barrier cross");
  fprintf(out, "  %-24s %s\n", "", "(ablation/measurement flag; needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "--gainfix-best",
          "Gain cascade once on the best board; superseded");
  fprintf(out, "  %-24s %s\n", "", "by --gainfix-best3, near-free (needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "--gainfix[=GATE]",
          "Quadgram-gain 2-ply directed-repair cascade at");
  fprintf(out, "  %-24s %s\n", "", "convergence; GATE = near-solution per-symbol");
  fprintf(out, "  %-24s %s\n", "", "score threshold (needs -c; quad-only) [off];");
  fprintf(out, "  %-24s %s\n", "", "prefer --gainfix-best3 (kept for -F/--exhaust)");
  fprintf(out, "  %-24s %s\n", "--exhaust E",
          "Force E extra plug pairs among the free letters,");
  fprintf(out, "  %-24s %s\n", "", "try every combination, keep the best climb. An");
  fprintf(out, "  %-24s %s\n", "", "exploration tool (E=1 = 325 climbs; E>1 explodes;");
  fprintf(out, "  %-24s %s\n", "", "needs -c; a high -R dominates it) [off]");
  fprintf(out, "\n");
  fprintf(out, "Diagnostic options (internal/experimental; for measurement):\n");
  fprintf(out, "  %-24s %s\n", "--true-key KEY",
          "With -F, print the tier-1 rank of the given");
  fprintf(out, "  %-24s %s\n", "", "standard key (e.g. B241AAAQEW = reflector, 3");
  fprintf(out, "  %-24s %s\n", "", "wheels, 3 ring, 3 start) among all keys [off]");
  fprintf(out, "  %-24s %s\n", "--dump-restarts",
          "With -c, print each converged restart's score");
  fprintf(out, "  %-24s %s\n", "", "and board to stderr (verbose) [off]");
  fprintf(out, "  %-24s %s\n", "--restart-tt",
          "With -c, hash converged restart boards into a");
  fprintf(out, "  %-24s %s\n", "", "transposition table; print a basin-collapse");
  fprintf(out, "  %-24s %s\n", "", "summary (distinct optima + hits) at the end [off]");
  fprintf(out, "  %-24s %s\n", "--score-tt",
          "With -c, memoise score_iter in a per-worker");
  fprintf(out, "  %-24s %s\n", "", "plugboard score cache; report the % of scores");
  fprintf(out, "  %-24s %s\n", "", "served from cache (results unchanged) [off]");
  fprintf(out, "  %-24s %s\n", "--infl-order",
          "Experimental: influence-ordered first-improvement");
  fprintf(out, "  %-24s %s\n", "", "(implies -I; measured, dominated by -J;");
  fprintf(out, "  %-24s %s\n", "", "needs -c) [off]");
  fprintf(out, "\n");
  fprintf(out, "Defaults are indicated in [square brackets].\n");
  fprintf(out, "\n");
  fprintf(out, "The ciphertext is read from standard input. The final plaintext is written\n");
  fprintf(out, "to standard output.\n");
  fprintf(out, "\n");
  fprintf(out, "For the reflector, wheels, ring position and start position, a dot (.)\n");
  fprintf(out, "works as a wild card, leaving it unspecified. When these settings are not\n");
  fprintf(out, "specified, the program will try all combinations to find the settings\n");
  fprintf(out, "resulting in the highest plaintext score. If asked for, a hill climbing\n");
  fprintf(out, "algorithm will be used to try to determine the plugboard settings.\n");
  fprintf(out, "\n");
  fprintf(out, "Recommended for short messages with a standard ~10-plug board (raise -R for\n");
  fprintf(out, "harder ones; the two are matched-compute peers -- SA tends to win the very\n");
  fprintf(out, "shortest/hardest lengths, the greedy climb the slightly longer ones):\n");
  fprintf(out, "  greedy: -c -J --gainfix-best3 --score m4a10 --random 10 -R 40 -a -l english\n");
  fprintf(out, "  SA:     -c -A 12000 --score a10 -R 12 -a -l english\n");
  fprintf(out, "-a (weighted all-order) is the recommended scoring model; -R is the main\n");
  fprintf(out, "quality dial (use -T to keep it cheap); the gainfix finishers are a\n");
  fprintf(out, "near-free bump, not a substitute for more restarts.\n");
  fprintf(out, "\n");
}

void removespaces(char * p)
{
  char * q = p;
  while(char c = *p++)
    if (c != ' ')
      *q++ = c;
  *q=0;
}

/* Echo the resolved run configuration to stderr so it is never a mystery what
   scoring model / language / settings a run is actually using. A dot (.) in the
   reflector/wheels/ring/start fields means that position is being searched. */
void show_settings()
{
  static const char * const scoring_name[] =
    { "index of coincidence", "monograms", "bigrams", "trigrams", "quadgrams",
      "weighted all-orders" };

  fprintf(stderr, "Ciphertext: %d letters\n", textlength);

  fprintf(stderr, "Scoring:    %s", scoring_name[opt_scoring]);
  if (opt_scoring == 0)
    fprintf(stderr, " (language-independent)\n");
  else
    fprintf(stderr, " (language: %s; n-gram files in %s)\n",
            opt_language, opt_datadir);

  /* One clause per continuation line so the line stays within 79 columns even when
     several are active (the default random seed is a full 20-digit uint64). The seed
     is shown once -- it drives both the SA trajectory and the restart perturbation. */
  fprintf(stderr, "Hillclimb:  %s\n", opt_hillclimb ? "yes" : "no");
  if (opt_hillclimb && (opt_anneal > 0))
    fprintf(stderr, "            simulated annealing, %d moves\n", opt_anneal);
  if (opt_hillclimb && (opt_restarts >= 1))
    fprintf(stderr, "            %d restarts, %d-pair kick\n",
            opt_restarts, opt_perturb);
  if (opt_hillclimb && opt_staged && (opt_anneal == 0))
    fprintf(stderr, "            staged: %s\n", opt_staged);
  if (opt_hillclimb && opt_exhaust)
    {
      int fixed_pairs = static_cast<int>(strlen(opt_steckerbrett) / 2);
      int free_letters = asize - 2 * fixed_pairs;
      double combos = disjoint_pair_combinations(free_letters, opt_exhaust);
      fprintf(stderr, "            partial exhaustion: %d forced pair(s) on top of %d "
              "-s pair(s) (%.0f combinations)\n", opt_exhaust, fixed_pairs, combos);
    }
  if (opt_hillclimb && opt_capmerge)
    fprintf(stderr, "            cap as strict descent target (merge/remove only at cap)\n");
  if (opt_hillclimb && opt_repair3)
    fprintf(stderr, "            3-plug re-pair barrier cross at convergence\n");
  if (opt_hillclimb && opt_no_repair)
    fprintf(stderr, "            2-plug re-pair barrier cross disabled (--no-repair)\n");
  if (opt_hillclimb && opt_gainfix)
    fprintf(stderr, "            quadgram-gain directed-repair cascade at convergence "
            "(--gainfix, near-solution gate %.2f)\n", opt_gainfix_gate);
  if (opt_hillclimb && opt_gainfix_best)
    fprintf(stderr, "            quadgram-gain cascade once on the best board (--gainfix-best)\n");
  if (opt_hillclimb && opt_gainfix_best3)
    fprintf(stderr, "            quadgram-gain 2-ply+3-ply cascade once on the best board "
            "(--gainfix-best3)\n");
  if (opt_hillclimb && opt_firstimprove)
    fprintf(stderr, "            first-improvement climb%s\n",
            opt_dynorder ? " (dynamic move order)" :
            opt_inflorder ? " (influence move order)" : "");
  if (opt_hillclimb && ((opt_anneal > 0) || (opt_restarts >= 1)))
    fprintf(stderr, "            seed: %llu\n",
            static_cast<unsigned long long>(opt_seed));

  if (opt_prefilter_frac > 0.0)
    fprintf(stderr, "Pre-filter: top %g%% of keys\n", opt_prefilter_frac * 100.0);
  else if (opt_prefilter > 0)
    fprintf(stderr, "Pre-filter: top %d keys\n", opt_prefilter);

  fprintf(stderr, "Threads:    %d\n", opt_threads);

  /* Split over two lines so it stays within 79 columns (M4 in particular is wide, and
     the "(max wheel N)" clause pushes even the standard line over). */
  if (opt_m4)
    {
      /* opt_ukw is upper-cased (B/C); echo the thin reflector in lower case. Line 1:
         the M4-specific parts (thin reflector + the static Greek wheel and its offset);
         line 2: the three stepping rotors. */
      fprintf(stderr,
              "Machine:    M4 Enigma; thin reflector %c, Greek wheel %c, ring %c, "
              "start %c\n",
              (opt_ukw[0] == '.') ? '.' : (opt_ukw[0] == 'B' ? 'b' : 'c'),
              opt_greek_walzen, opt_greek_ringstellung, opt_greek_grundstellung);
      fprintf(stderr, "            wheels %s", opt_walzen);
      if (strchr(opt_walzen, '.'))
        fprintf(stderr, " (max wheel %d)", opt_maxwheel);
      fprintf(stderr, ", ring %s, start %s\n", opt_ringstellung, opt_grundstellung);
    }
  else
    {
      fprintf(stderr, "Machine:    %s Enigma; reflector %s, wheels %s",
              opt_norenigma ? "Norway" : "standard", opt_ukw, opt_walzen);
      if (strchr(opt_walzen, '.'))
        fprintf(stderr, " (max wheel %d)", opt_maxwheel);
      fprintf(stderr, "\n            ring %s, start %s\n",
              opt_ringstellung, opt_grundstellung);
    }

  fprintf(stderr, "Plugboard:  ");
  if (opt_steckerbrett[0])
    {
      /* print the de-spaced pairs with a space between each pair (AB CD ...) */
      for (int i = 0; opt_steckerbrett[i]; i++)
        {
          if ((i > 0) && (i % 2 == 0))
            fputc(' ', stderr);
          fputc(opt_steckerbrett[i], stderr);
        }
      fputc('\n', stderr);
    }
  else
    fprintf(stderr, "(none)\n");
}

int main(int argc, char * * argv)
{
  auto t_start = std::chrono::steady_clock::now();

  if (argc == 1)
    {
      help(stderr);
      exit(1);
    }

  /* set default arguments. The reflector/wheels/ring/start defaults depend on the
     mode (standard vs M4's extra Greek position), so they are left null here and
     resolved after parsing, once -4 is known. */
  opt_ukw = 0;
  opt_walzen = 0;
  opt_ringstellung = 0;
  opt_grundstellung = 0;
  opt_steckerbrett = "";
  opt_language = 0;   /* no default; required for n-gram scoring (-m/-b/-t/-q) */
  opt_datadir = 0;    /* resolved after parsing: -d > $ENIGMA_DATA > "ngrams" */
  opt_plaintext = 0;
  opt_maxwheel = 5;
  opt_hillclimb = 0;
  opt_firstimprove = 0;
  opt_dynorder = 0;
  opt_inflorder = 0;
  opt_capmerge = 0;
  opt_repair3 = 0;
  opt_no_repair = 0;
  opt_gainfix = 0;
  opt_gainfix_gate = -4.9;   /* English-quad-calibrated near-solution gate (tunable) */
  opt_gainfix_best = 0;
  opt_gainfix3 = 0;
  opt_gainfix_best3 = 0;
  opt_crib_file = nullptr;
  opt_crib_weight = 0.5;
  opt_crib = 0;
  g_cribs.clear();
  opt_restart_tt = false;
  opt_score_tt = false;
  opt_restarts = 0;   /* new default: one deterministic seed climb, no kick (REDESIGN B) */
  opt_perturb = default_perturb;   /* --random kick size (default 10); K=0 is a legal control */
  opt_random_set = false;
  opt_exhaust = 0;    /* --exhaust E forced pairs, 0 = off */
  opt_staged = 0;   /* --score schedule string, or 0 for the single-model climb */
  opt_scoring = SCORE_IC;   /* default: the only model needing no -l (see help) */
  opt_model_selector = -1;  /* no -i/-m/-b/-t/-q selector seen yet */
  opt_norenigma = 0;
  opt_m4 = 0;
  opt_threads = 1;
  opt_prefilter = 0;
  opt_prefilter_frac = 0.0;
  opt_seed = 0;
  opt_seed_set = false;
  opt_anneal = 0;

  /* get arguments */

  /* Long-only option identifiers (no short form): values above the byte range so they
     never collide with a short flag char. --random and --exhaust are the seed-pipeline
     options introduced in REDESIGN Part B. */
  enum { OPT_RANDOM = 256, OPT_EXHAUST, OPT_TRUEKEY, OPT_DUMP, OPT_INFLORDER, OPT_REPAIR3,
         OPT_NO_REPAIR, OPT_GAINFIX, OPT_GAINFIX_BEST, OPT_GAINFIX_BEST3, OPT_RESTART_TT,
         OPT_SCORE_TT, OPT_CRIB, OPT_CRIBWEIGHT };

  /* Long-option aliases for the short flags (Part A of archived/REDESIGN.md), plus the two
     long-only options above (Part B). Each aliased long name maps onto its short value,
     so the switch below is shared. Unambiguous prefixes (e.g. --lang, --restart) are
     accepted natively by getopt_long. */
  static const struct option long_options[] =
    {
      { "reflector",      required_argument, nullptr, 'u' },
      { "wheels",         required_argument, nullptr, 'w' },
      { "rings",          required_argument, nullptr, 'r' },
      { "start-position", required_argument, nullptr, 'g' },
      { "plugboard",      required_argument, nullptr, 's' },
      { "compare",        required_argument, nullptr, 'p' },
      { "language",       required_argument, nullptr, 'l' },
      { "max-wheel",      required_argument, nullptr, 'x' },
      { "threads",        required_argument, nullptr, 'T' },
      { "restarts",       required_argument, nullptr, 'R' },
      { "score",          required_argument, nullptr, 'S' },
      { "prefilter",      required_argument, nullptr, 'F' },
      { "seed",           required_argument, nullptr, 'e' },
      { "anneal",         required_argument, nullptr, 'A' },
      { "ngrams",         required_argument, nullptr, 'd' },
      { "first-improve",  no_argument,       nullptr, 'I' },
      { "dynamic-order",  no_argument,       nullptr, 'J' },
      { "cap-target",     no_argument,       nullptr, 'M' },
      { "ic",             no_argument,       nullptr, 'i' },
      { "mono",           no_argument,       nullptr, 'm' },
      { "bi",             no_argument,       nullptr, 'b' },
      { "tri",            no_argument,       nullptr, 't' },
      { "quad",           no_argument,       nullptr, 'q' },
      { "weighted",       no_argument,       nullptr, 'a' },
      { "climb",          no_argument,       nullptr, 'c' },
      { "norway",         no_argument,       nullptr, 'n' },
      { "m4",             no_argument,       nullptr, '4' },
      { "version",        no_argument,       nullptr, 'v' },
      { "help",           no_argument,       nullptr, 'h' },
      { "random",         required_argument, nullptr, OPT_RANDOM  },
      { "exhaust",        required_argument, nullptr, OPT_EXHAUST },
      { "true-key",       required_argument, nullptr, OPT_TRUEKEY },
      { "dump-restarts",  no_argument,       nullptr, OPT_DUMP    },
      { "restart-tt",     no_argument,       nullptr, OPT_RESTART_TT },
      { "score-tt",       no_argument,       nullptr, OPT_SCORE_TT },
      { "infl-order",     no_argument,       nullptr, OPT_INFLORDER },
      { "repair3",        no_argument,       nullptr, OPT_REPAIR3 },
      { "no-repair",      no_argument,       nullptr, OPT_NO_REPAIR },
      { "gainfix",        optional_argument, nullptr, OPT_GAINFIX },
      { "gainfix-best",   no_argument,       nullptr, OPT_GAINFIX_BEST },
      { "gainfix-best3",  no_argument,       nullptr, OPT_GAINFIX_BEST3 },
      { "crib-file",      required_argument, nullptr, OPT_CRIB },
      { "crib-weight",    required_argument, nullptr, OPT_CRIBWEIGHT },
      { nullptr,          0,                 nullptr, 0   }
    };

  int c;
  while ((c = getopt_long(argc, argv,
                          "u:w:r:g:s:p:l:x:T:R:S:F:e:A:d:IJMimbtqacvhn4",
                          long_options, nullptr)) != -1)
    {
      switch (c)
        {
        case 'u':
          alltoupper(optarg);
          opt_ukw = optarg;
          break;
        case 'w':
          opt_walzen = optarg;
          break;
        case 'r':
          alltoupper(optarg);
          opt_ringstellung = optarg;
          break;
        case 'g':
          alltoupper(optarg);
          opt_grundstellung = optarg;
          break;
        case 's':
          alltoupper(optarg);
          removespaces(optarg);
          opt_steckerbrett = optarg;
          break;
        case 'p':
          opt_plaintext = optarg;
          break;
        case 'i':
          select_model(SCORE_IC);
          break;
        case 'm':
          select_model(SCORE_MONO);
          break;
        case 'b':
          select_model(SCORE_BI);
          break;
        case 't':
          select_model(SCORE_TRI);
          break;
        case 'q':
          select_model(SCORE_QUAD);
          break;
        case 'a':
          select_model(SCORE_ALL);
          break;
        case 'c':
          opt_hillclimb = 1;
          break;
        case 'I':
          opt_firstimprove = 1;
          break;
        case 'J':
          opt_firstimprove = 1;   /* -J implies first-improvement */
          opt_dynorder = 1;
          break;
        case OPT_INFLORDER:
          opt_firstimprove = 1;   /* --infl-order implies first-improvement */
          opt_inflorder = 1;
          break;
        case OPT_REPAIR3:
          opt_repair3 = 1;
          break;
        case OPT_NO_REPAIR:
          opt_no_repair = 1;
          break;
        case OPT_GAINFIX:
          opt_gainfix = 1;
          if (optarg != nullptr)
            opt_gainfix_gate = strtod(optarg, nullptr);
          break;
        case OPT_GAINFIX_BEST:
          opt_gainfix_best = 1;
          break;
        case OPT_GAINFIX_BEST3:
          opt_gainfix_best3 = 1;
          break;
        case 'M':
          opt_capmerge = 1;
          break;
        case 'S':
          opt_staged = optarg;
          break;
        case 'x':
          opt_maxwheel = atoi(optarg);
          break;
        case 'T':
          opt_threads = atoi(optarg);
          break;
        case 'R':
          opt_restarts = atoi(optarg);
          break;
        case OPT_RANDOM:
          opt_perturb = atoi(optarg);
          opt_random_set = true;
          break;
        case OPT_EXHAUST:
          opt_exhaust = atoi(optarg);
          break;
        case OPT_TRUEKEY:
          alltoupper(optarg);
          opt_true_key = optarg;
          break;
        case OPT_DUMP:
          opt_dump_restarts = true;
          break;
        case OPT_RESTART_TT:
          opt_restart_tt = true;
          break;
        case OPT_SCORE_TT:
          opt_score_tt = true;
          break;
        case OPT_CRIB:
          opt_crib_file = optarg;
          break;
        case OPT_CRIBWEIGHT:
          opt_crib_weight = strtod(optarg, nullptr);
          break;
        case 'e':
          opt_seed = strtoull(optarg, nullptr, 10);
          opt_seed_set = true;
          break;
        case 'A':
          opt_anneal = atoi(optarg);
          break;
        case 'F':
          {
            /* -F N keeps the top N keys; -F N% keeps the top N% of the resolved
               keyspace (atof stops at the '%'). The fraction, if given, wins. */
            size_t flen = strlen(optarg);
            if ((flen > 0) && (optarg[flen - 1] == '%'))
              opt_prefilter_frac = atof(optarg) / 100.0;
            else
              opt_prefilter = atoi(optarg);
          }
          break;
        case 'l':
          opt_language = optarg;
          break;
        case 'd':
          opt_datadir = optarg;
          break;
        case 'v':
          version(stdout);
          exit(0);
          break;
        case 'h':
          help(stdout);
          exit(0);
          break;
        case 'n':
          opt_norenigma = 1;
          break;
        case '4':
          opt_m4 = 1;
          break;
        default:
          fprintf(stderr, "\n");
          help(stderr);
          exit(1);
          break;
        }
    }

  if (opt_norenigma && opt_m4)
    fatal("-n (Norway) and -4 (M4) are mutually exclusive");

  /* resolve the mode-dependent reflector/wheels/ring/start defaults now that the
     mode flags are known. M4 takes a 4th (Greek) character first in -w/-r/-g; the
     Greek ring defaults to A and its position is wildcarded, so all 26 effective
     reflectors are tried. */
  if (opt_m4)
    {
      if (! opt_ukw)           opt_ukw = ".";
      if (! opt_walzen)        opt_walzen = "....";
      if (! opt_ringstellung)  opt_ringstellung = "AAA.";
      if (! opt_grundstellung) opt_grundstellung = "....";
    }
  else
    {
      if (! opt_ukw)           opt_ukw = ".";
      if (! opt_walzen)        opt_walzen = "...";
      if (! opt_ringstellung)  opt_ringstellung = "AA.";
      if (! opt_grundstellung) opt_grundstellung = "...";
    }

  /* resolve the n-gram data directory: -d wins, else $ENIGMA_DATA, else the
     bundled "ngrams" subdirectory (found when run from the repo root) */
  if (! opt_datadir)
    opt_datadir = getenv("ENIGMA_DATA");
  if ((! opt_datadir) || (! opt_datadir[0]))
    opt_datadir = "ngrams";

  /* validate arguments */

  if (opt_norenigma)
    {
      if ((strlen(opt_ukw) != 1) ||
          (strspn(opt_ukw, "N.") != 1))
        fatal("Illegal ukw string (must be N or .)");

      if ((strlen(opt_walzen) != wheels) ||
          (strspn(opt_walzen, "12345.") != wheels))
        fatal("Illegal walzen string (must be 3 digits (1-5) or .)");

      if ((opt_maxwheel < wheels) || (opt_maxwheel > 5))
        fatal("Illegal max wheel (must be 3 to 5)");
    }
  else if (opt_m4)
    {
      /* M4: thin reflector (b/c, case-insensitive -> B/C), and a 4-character
         -w/-r/-g whose first character is the static Greek wheel/ring/start. */
      if ((strlen(opt_ukw) != 1) ||
          (strspn(opt_ukw, "BC.") != 1))
        fatal("Illegal ukw string (M4: must be b, c or .)");

      if (strlen(opt_walzen) != wheels + 1)
        fatal("Illegal walzen string (M4: 4 chars: Greek (B/G/.) + 3 wheels "
              "(1-8/.))");
      if (! strchr("BGbg.", opt_walzen[0]))
        fatal("Illegal Greek wheel (M4: must be B (Beta), G (Gamma) or .)");
      if (strspn(opt_walzen + 1, "12345678.") != wheels)
        fatal("Illegal walzen string (M4: the 3 wheels must be digits (1-8) "
              "or .)");

      if (strlen(opt_ringstellung) != wheels + 1)
        fatal("Illegal ringstellung string (M4: 4 letters (A-Z) or .)");
      if (strlen(opt_grundstellung) != wheels + 1)
        fatal("Illegal grundstellung string (M4: 4 letters (A-Z) or .)");

      if ((opt_maxwheel < wheels) || (opt_maxwheel > 8))
        fatal("Illegal max wheel (must be 3-8)");

      /* split off the Greek (first) char of -w/-r/-g; the remaining 3-character
         tails then pass through the shared checks below exactly like a standard
         machine. -r/-g were already upper-cased; normalise the Greek wheel. */
      opt_greek_walzen = (opt_walzen[0] == 'b') ? 'B'
                       : (opt_walzen[0] == 'g') ? 'G'
                       : opt_walzen[0];
      opt_greek_ringstellung = opt_ringstellung[0];
      opt_greek_grundstellung = opt_grundstellung[0];
      opt_walzen += 1;
      opt_ringstellung += 1;
      opt_grundstellung += 1;
    }
  else
    {
      if ((strlen(opt_ukw) != 1) ||
          (strspn(opt_ukw, "ABC.") != 1))
        fatal("Illegal ukw string (must be A, B, C or .)");

      if ((strlen(opt_walzen) != wheels) ||
          (strspn(opt_walzen, "12345678.") != wheels))
        fatal("Illegal walzen string (must be 3 digits (1-8) or .)");

      if ((opt_maxwheel < wheels) || (opt_maxwheel > 8))
        fatal("Illegal max wheel (must be 3-8)");
    }

  /* A wheel cannot occupy two positions at once. Reject any explicitly named
     (non-'.') wheel repeated across positions: otherwise the permutation guard
     in bruteforce() skips every combination and the search silently finds
     nothing. */
  for (int i = 0; i < wheels; i++)
    for (int j = i + 1; j < wheels; j++)
      if ((opt_walzen[i] != '.') && (opt_walzen[i] == opt_walzen[j]))
        fatal("Illegal walzen string (a wheel cannot be used in two positions)");

  if ((strlen(opt_ringstellung) != wheels) ||
      (strspn(opt_ringstellung, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.") != wheels))
    fatal("Illegal ringstellung string (must be 3 letters (A-Z) or .)");

  if ((strlen(opt_grundstellung) != wheels) ||
      (strspn(opt_grundstellung, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.") != wheels))
    fatal("Illegal grundstellung string (must be 3 letters (A-Z) or .)");

  if ((strlen(opt_steckerbrett) > asize) ||
      (strspn(opt_steckerbrett, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") <
       strlen(opt_steckerbrett)))
    fatal("Illegal steckerbrett string (must be up to 13 letter pairs)");

  /* --restarts 0 (the new default) is legal: one deterministic climb from the seed, no
     kick. --restarts N>=1 runs N kicked climbs. */
  if ((opt_restarts < 0) || (opt_restarts > max_restarts))
    fatal("Illegal restart count (--restarts must be 0 to 1000000000)");

  /* --random K is the kick size (plug pairs injected per restart); K=0 is a legal control. */
  if ((opt_perturb < 0) || (opt_perturb > pairs_uncapped))
    fatal("Illegal kick size (--random must be 0 to 13 plug pairs)");

  /* Expand the --score schedule into opt_stages[] and set opt_scoring to the target
     (last) stage. Validates the schedule syntax; fatal() on error. With no --score
     this builds the single -i/-m/.../-q stage. */
  parse_schedule();

  /* A model selector (-i/-m/-b/-t/-q) is a --score <model> alias, so if BOTH are given
     they must agree on the target/ranking model: after parse_schedule() opt_scoring is the
     --score target, so a selector naming a different model is genuinely ambiguous -- reject
     it (REDESIGN Part C). Agreement (e.g. -q --score i4q10, or -q --score q) is fine. When
     no --score is given, opt_scoring already equals the selector, so this never fires. */
  if ((opt_model_selector != -1) && opt_staged && (opt_model_selector != opt_scoring))
    {
      static const char * const model_name[] =
        { "IC", "monograms", "bigrams", "trigrams", "quadgrams", "weighted" };
      char msg[128];
      snprintf(msg, sizeof msg,
               "Conflicting scoring models: selector picks %s but --score targets %s; "
               "pick one", model_name[opt_model_selector], model_name[opt_scoring]);
      fatal(msg);
    }

  if ((opt_threads < 1) || (opt_threads > max_threads))
    fatal("Illegal thread count (must be 1 to 256)");

  /* Resolve the restart RNG seed: an explicit -e wins; otherwise $ENIGMA_SEED (handy
     for reproducible tests/benchmarks without a flag); otherwise a fresh random draw,
     so by default every run explores different restarts. The chosen seed is echoed by
     show_settings() when restarts are active, so a random run can be reproduced. */
  if (! opt_seed_set)
    {
      const char * seed_env = getenv("ENIGMA_SEED");
      if (seed_env && *seed_env)
        opt_seed = strtoull(seed_env, nullptr, 10);
      else
        {
          std::random_device rd;
          opt_seed = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        }
    }


  /* The key pre-filter ranks every key by a cheap plugboard climb and runs the full
     climb only on the top -F keys, so it is only meaningful with -c. -F takes either
     an absolute count (opt_prefilter) or a percentage of the resolved keyspace
     (opt_prefilter_frac, which overrides). */
  if (opt_prefilter < 0)
    fatal("Illegal pre-filter count (-F must be >= 1)");
  if ((opt_prefilter_frac < 0.0) || (opt_prefilter_frac > 1.0))
    fatal("Illegal pre-filter percentage (-F N% must be 0 < N <= 100)");
  if (((opt_prefilter > 0) || (opt_prefilter_frac > 0.0)) && (! opt_hillclimb))
    fatal("The key pre-filter (-F) needs the plugboard hill-climb (-c)");

  /* Simulated annealing is an alternative plugboard optimiser, so it needs -c; the
     move budget must be non-negative. */
  if (opt_anneal < 0)
    fatal("Illegal anneal move budget (-A must be >= 1)");
  if ((opt_anneal > 0) && (! opt_hillclimb))
    fatal("Simulated annealing (-A) needs the plugboard hill-climb (-c)");

  /* -I is a hill-climb strategy, so it needs -c. */
  if (opt_firstimprove && (! opt_hillclimb))
    fatal("First-improvement (-I) needs the plugboard hill-climb (-c)");

  /* --infl-order and -J are two different move orders for the first-improvement climb;
     asking for both is ambiguous. */
  if (opt_inflorder && opt_dynorder)
    fatal("--infl-order and -J (dynamic order) are mutually exclusive");

  /* -M changes the plug-cap rule in the climb, so it needs -c. */
  if (opt_capmerge && (! opt_hillclimb))
    fatal("Cap-as-target (-M) needs the plugboard hill-climb (-c)");

  /* --repair3 is a climb barrier-cross move, so it needs -c. */
  if (opt_repair3 && (! opt_hillclimb))
    fatal("3-plug re-pair (--repair3) needs the plugboard hill-climb (-c)");

  /* --no-repair disables a climb move, so it only means anything with -c. */
  if (opt_no_repair && (! opt_hillclimb))
    fatal("Disabling the 2-plug re-pair (--no-repair) needs the plugboard hill-climb (-c)");

  /* --gainfix is a climb barrier-cross move, so it needs -c. */
  if (opt_gainfix && (! opt_hillclimb))
    fatal("Gain-cascade repair (--gainfix) needs the plugboard hill-climb (-c)");

  /* --gainfix-best finishes the best board post-search; needs -c, and the simple sweep
     (its best.idx = key*restarts+restart reconstruction does not hold under -F/--exhaust). */
  if (opt_gainfix_best && (! opt_hillclimb))
    fatal("Gain-cascade best-board finish (--gainfix-best) needs the plugboard hill-climb (-c)");
  if (opt_gainfix_best && opt_gainfix)
    fatal("--gainfix-best and --gainfix are alternatives; pick one");
  if (opt_gainfix_best && ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0) || opt_exhaust))
    fatal("--gainfix-best is not supported with -F or --exhaust");

  /* --gainfix-best3 = the best-board finisher with the 3-ply escalation; same guards. */
  if (opt_gainfix_best3 && (! opt_hillclimb))
    fatal("Gain-cascade 3-ply best-board finish (--gainfix-best3) needs the plugboard hill-climb (-c)");
  if (opt_gainfix_best3 && (opt_gainfix || opt_gainfix_best))
    fatal("--gainfix-best3, --gainfix-best and --gainfix are alternatives; pick one");
  if (opt_gainfix_best3 && ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0) || opt_exhaust))
    fatal("--gainfix-best3 is not supported with -F or --exhaust");

  /* --random and --exhaust are plugboard operations: they can do nothing in a bare rotor
     scan, so passing them without -c is an error (fail fast rather than silently ignore). */
  if (opt_random_set && (! opt_hillclimb))
    fatal("The random kick (--random) needs the plugboard hill-climb (-c)");
  if (opt_exhaust && (! opt_hillclimb))
    fatal("Partial exhaustion (--exhaust) needs the plugboard hill-climb (-c)");

  /* --dump-restarts is a per-restart climb diagnostic, so it needs -c. */
  if (opt_dump_restarts && (! opt_hillclimb))
    fatal("--dump-restarts needs the plugboard hill-climb (-c)");
  if (opt_restart_tt && (! opt_hillclimb))
    fatal("--restart-tt needs the plugboard hill-climb (-c)");
  /* --score-tt memoises the plugboard scoring, which only happens under the climb. */
  if (opt_score_tt && (! opt_hillclimb))
    fatal("--score-tt needs the plugboard hill-climb (-c)");

  /* The crib finisher re-ranks converged plugboards, so it needs the climb. */
  if (opt_crib_file && (! opt_hillclimb))
    fatal("The crib finisher (--crib-file) needs the plugboard hill-climb (-c)");

  /* --true-key reports the true key's tier-1 rank, so it needs the pre-filter (-F);
     it is a standard-Enigma diagnostic and parses into g_tk_* here. */
  if (opt_true_key)
    {
      if (opt_norenigma || opt_m4)
        fatal("--true-key is only supported for the standard Enigma (not -n / -4)");
      if ((opt_prefilter <= 0) && (opt_prefilter_frac <= 0.0))
        fatal("--true-key reports the tier-1 rank, so it needs the pre-filter (-F)");
      if (strlen(opt_true_key) != 10)
        fatal("--true-key needs 10 chars: <reflector><3 wheels><3 ring><3 start>, e.g. B241AAAQEW");
      g_tk_u = char2num(opt_true_key[0]);
      if ((g_tk_u < 0) || (g_tk_u > 2))
        fatal("--true-key reflector must be A/B/C");
      for (int i = 0; i < 3; i++)
        {
          if ((opt_true_key[1 + i] < '1') || (opt_true_key[1 + i] > '8'))
            fatal("--true-key wheels must be digits 1-8");
          g_tk_w[i] = opt_true_key[1 + i] - '1';
          g_tk_r[i] = char2num(opt_true_key[4 + i]);
          g_tk_g[i] = char2num(opt_true_key[7 + i]);
        }
    }

  /* --exhaust E forces E extra plug pairs among the free letters (on top of any -s pairs); it
     runs the greedy staged climb (not SA). E is bounded by the free plug pairs (13 minus the
     -s pins). It now parallelises over the first forced pair (REDESIGN Part D), so -T > 1 is
     fine; each worker climbs against its own PLUG_FIXED_EX pin set. */
  if (opt_exhaust && (opt_anneal > 0))
    fatal("Partial exhaustion (--exhaust) is not supported with simulated annealing (-A)");
  if (opt_exhaust)
    {
      int fixed_pairs = static_cast<int>(strlen(opt_steckerbrett) / 2);
      int free_pairs = pairs_uncapped - fixed_pairs;   /* free letters / 2 */
      if (opt_exhaust < 1)
        fatal("Illegal partial exhaustion (--exhaust must be >= 1 forced plug pairs)");
      if (opt_exhaust > free_pairs)
        fatal("Partial exhaustion (--exhaust E): E exceeds the free plug pairs (13 minus -s pairs)");
      build_exhaust_firsts();   /* the parallel first-pair work list (read-only after) */
    }

  /* Non-fatal warning: if --restarts N asks for more kicked restarts than there are distinct
     K-pair kicks among the free letters, the restarts must repeat by pigeonhole. free letters =
     26 - 2*(-s pairs + --exhaust forced pairs); the kick is clamped to at most free/2 pairs.
     Mainly catches the small-K / high-N footgun (e.g. --random 1 --restarts 1000). */
  if (opt_hillclimb && (opt_restarts >= 1))
    {
      int fixed_pairs = static_cast<int>(strlen(opt_steckerbrett) / 2);
      int free_letters = asize - 2 * (fixed_pairs + opt_exhaust);
      if (free_letters < 0)
        free_letters = 0;
      int keff = opt_perturb;
      if (keff > free_letters / 2)
        keff = free_letters / 2;
      double distinct = disjoint_pair_combinations(free_letters, keff);
      if (static_cast<double>(opt_restarts) > distinct)
        fprintf(stderr, "Warning: --restarts %d exceeds the %.0f distinct %d-pair kick(s) "
                "among %d free letters; restarts will repeat\n",
                opt_restarts, distinct, keff, free_letters);
    }

  /* Non-fatal warning: a --score schedule with climb-only detail (more than one stage, or any
     cap) does nothing in a bare rotor scan -- there is no climb to apply the stages/caps to.
     Flag a forgotten -c (or a pasted climb recipe) but proceed, ranking by the target model. */
  if ((! opt_hillclimb) && opt_staged && schedule_is_climb_only())
    {
      static const char * const model_name[] =
        { "IC", "monograms", "bigrams", "trigrams", "quadgrams", "weighted" };
      fprintf(stderr, "Warning: --score climb schedule ignored without -c; "
              "ranking by %s\n", model_name[opt_scoring]);
    }

  /* Scoring only happens when the run ranks candidates -- a '.' wildcard in the
     reflector/wheels/ring/start -- or hill-climbs the plugboard (-c). A fully
     specified machine with no -c just enciphers its input: there is a single
     candidate and its decode is the output, so no score, and hence no scoring
     language, is needed. In that case fall back to the index of coincidence (which
     needs neither a table nor -l) so plain encryption/decryption works with no
     scoring options at all. (Note the default ring is "AA.", so an explicit -r is
     needed to encrypt -- otherwise the wildcard makes it a search.) */
  bool has_wildcard =
      strchr(opt_ukw, '.') || strchr(opt_walzen, '.') ||
      strchr(opt_ringstellung, '.') || strchr(opt_grundstellung, '.') ||
      (opt_m4 && (opt_greek_walzen == '.' ||
                  opt_greek_ringstellung == '.' ||
                  opt_greek_grundstellung == '.'));
  bool needs_scoring = has_wildcard || opt_hillclimb;
  /* A fully specified machine with no search still scores its single decrypt for the
     diagnostic line. Honour the requested model when it can be satisfied -- an n-gram
     model needs -l -- but fall back to IC (which needs no table) so a bare decrypt
     needs no scoring options at all (the default model is quad, yet `enigma -u B -w
     123 -r AAA -g AAA` must work with no -l). */
  if (! needs_scoring && (opt_scoring != SCORE_IC) && ! opt_language)
    opt_scoring = SCORE_IC;

  /* The n-gram scoring models (mono/bi/tri/quad) need a language, with no default;
     the index of coincidence (-i) is language-independent. Every stage that reads an
     n-gram table -- pre-pass or target -- needs -l. Only enforce this when scoring
     actually runs. */
  if (needs_scoring && ! opt_language)
    for (int i = 0; i < opt_nstages; i++)
      if (opt_stages[i].model != SCORE_IC)
        fatal("A scoring language is required: add -l <language> "
              "(e.g. -l english), or use -i for the language-independent "
              "index of coincidence");

  if (opt_language &&
      ((strlen(opt_language) < 1) ||
       (strlen(opt_language) > 32) ||
       (strspn(opt_language,
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") <
        strlen(opt_language))))
    fatal("Illegal language name (must be 1-32 letters, e.g. english)");


  /* Load the n-gram tables scoring will use (none for IC), target first, so a
     missing or mistyped -l fails immediately (with the offending filename) before
     we read and consume standard input. Also loads when a fully specified decrypt
     asked for an n-gram model (opt_scoring left non-IC above); skipped for a bare
     decrypt (which fell back to IC and needs no table). */
  if (needs_scoring || (opt_scoring != SCORE_IC))
    {
      bool table_loaded[SCORE_ALL + 1] = { false, false, false, false, false, false };
      load_table(opt_scoring);
      table_loaded[opt_scoring] = true;
      for (int i = 0; i < opt_nstages; i++)
        {
          int model = opt_stages[i].model;
          if (! table_loaded[model])
            {
              load_table(model);
              table_loaded[model] = true;
            }
        }
    }

  /* Load the known-word list for the crib finisher (sets opt_crib), before consuming
     stdin so a missing/empty file fails fast. */
  if (opt_crib_file != nullptr)
    load_cribs(opt_crib_file);

  /* read ciphertext */

  readciphertext();

  show_settings();

  if (textlength < 1)
    fatal("Ciphertext is empty (no A-Z letters on standard input)");

  for(int i=0; i< textlength; i++)
    num_ciphertext[i] = char2num(ciphertext[i]);

  init();

  init_plug_fixed(opt_steckerbrett);   /* -s seed each worker thread copies into plug_fixed */

  /* try all combinations (bruteforce allocates one machine per worker thread) */

  char result[maxlen+1];
  bruteforce(result);

  /* write plaintext */

  fprintf(stderr, "\n");
  printf("%s\n", result);

  /* read plaintext to compare to, if given */

  if (opt_plaintext)
    readplaintext(opt_plaintext, result);

  /* final diagnostic: wall-clock time and memory use */
  double secs = std::chrono::duration<double>
    (std::chrono::steady_clock::now() - t_start).count();
  struct rusage ru;
  double peak_mb = 0.0;
  if (getrusage(RUSAGE_SELF, & ru) == 0)
    {
      /* ru_maxrss is kilobytes on Linux but bytes on macOS/BSD */
#ifdef __APPLE__
      peak_mb = ru.ru_maxrss / (1024.0 * 1024.0);
#else
      peak_mb = ru.ru_maxrss / 1024.0;
#endif
    }
  fprintf(stderr,
          "Analysed %zu rotor combination%s, scored %llu plugboard%s\n",
          g_keys_analysed, (g_keys_analysed == 1) ? "" : "s",
          static_cast<unsigned long long>(g_plugboards_scored),
          (g_plugboards_scored == 1) ? "" : "s");
  if (opt_score_tt)
    {
      /* Effectiveness of the --score-tt plugboard cache: what fraction of the logical
         score_iter calls were served from the cache and so skipped a real decode. */
      double pct = (g_sc_lookups > 0)
                 ? 100.0 * static_cast<double>(g_sc_hits)
                         / static_cast<double>(g_sc_lookups)
                 : 0.0;
      fprintf(stderr,
              "score-tt: %llu/%llu score_iter served from cache (%.1f%% saved)\n",
              static_cast<unsigned long long>(g_sc_hits),
              static_cast<unsigned long long>(g_sc_lookups), pct);
    }
  fprintf(stderr, "Finished in %.2f s using %d thread%s\n",
          secs, opt_threads, (opt_threads == 1) ? "" : "s");
  fprintf(stderr, "Precomputed %zu rotor table%s (%.1f MB); peak memory %.0f MB\n",
          g_table_count, (g_table_count == 1) ? "" : "s",
          g_table_bytes / (1024.0 * 1024.0), peak_mb);
}
