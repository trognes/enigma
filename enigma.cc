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

/* The --ring-stride refinement's middle-wheel offset window (mid_ring_window = 2) USED to
   live here. It is gone because the refinement now DERIVES that offset from the coarse
   winner's and the candidate's step schedules instead of banding it (refinement.md): the
   quantity the band was guessing at is computable from the two keys, with no knowledge of
   the truth. The bound the band rested on still holds -- a ring2/start2 shift moves the
   middle wheel's schedule by at most 2, 1 from the ordinary time shift plus 1 when double
   stepping straddles the wheel's own notch, established by enumerating every rotor pair x
   26 start1 x 26 start2 x every shift at L=600 -- but nothing depends on it any more, which
   is what makes the refinement correct for two-notch right wheels and straddled double
   steps rather than merely usually right. */
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
enum scoring { SCORE_IC, SCORE_MONO, SCORE_BI, SCORE_TRI, SCORE_QUAD, SCORE_ALL,
               SCORE_FUSED };

static const char * opt_ukw;
static const char * opt_walzen;
static const char * opt_ringstellung;
static const char * opt_grundstellung;
static const char * opt_steckerbrett;
/* Letters that are part of a fixed (-s) plug pair are never rewired by the hill-climb,
   re-pair or SA toggle, so -s pairs are *known* plugs that survive the climb rather than
   a mere seed (see plug_fixed / PLUG_FIXED_EX below for the storage and the parallel
   --exhaust arrangement, REDESIGN Part D). */
/* --no-plug LETTERS: letters KNOWN to carry no cable. The other half of what -s
   expresses -- -s says "these two are plugged to each other", --no-plug says "this one is
   plugged to nothing" -- and until now there was no way to say it except by inventing a
   fake pair. Both end up in the same place: the letter is marked in plug_fixed[], so the
   climb, the SA toggle, the re-pair and the --random kick all leave it alone. The
   difference is only in the board they start from -- -s pairs its letters, --no-plug
   leaves its letters self-steckered.
     A cable has two ends, so knowing a letter is unsteckered constrains the board twice
   over: it removes 25 candidate plugs, not one. That shrinks the climb's move set
   quadratically (each marked letter drops 25 of the 325 toggles) and, more to the point,
   stops the climb spending moves on plugs that cannot exist. */
static const char * opt_no_plug;
/* --crib TEXT / --crib-at N: a guess at part of the plaintext, together with where it
   sits. Given one that is right, part of the plugboard follows by ARITHMETIC instead of
   search, and rotor settings that cannot produce it are rejected without ever being
   scored -- Turing's menu and Welchman's diagonal board, on the machine equation this
   file already computes. See cribs.md; the deduction itself is crib_deduce() below.
     Step 3 of cribs.md 12: one crib at one alignment, used as a KEY FILTER. The
   alignment sweep and the seeded climb are later steps, so --crib-at is required. */
static const char * opt_crib_text;
static int opt_crib_at = -1;
static bool opt_crib_dump;              /* print each surviving hypothesis (diagnostic) */
/* The menu, built once by init_crib(). A menu is a property of the crib AND the place it
   sits, so everything except the crib letters themselves is per ALIGNMENT: the ciphertext
   letters it pairs with, and therefore the anchor letter the 26 hypotheses are about.
     DELIBERATELY DECLARED HERE, beside the cold option globals, and not next to the
   deduction code further down -- that would put several KB of arrays a few dozen lines
   from plug_fixed, the hot climb-loop global whose placement this file is already
   documented as sensitive to (see the struct-layout note in CLAUDE.md). Keeping cold data
   away from it costs nothing and removes a whole class of accidental regression. */
static unsigned char crib_p[maxlen];    /* the crib's letters, one per menu edge */
static int crib_edges = 0;              /* = the crib length */
static int crib_align[maxlen];          /* viable alignments, in order */
static unsigned char crib_anchor_at[maxlen];   /* anchor letter of each one's menu */
static int crib_aligns = 0;
static std::atomic<size_t> g_crib_rejected{0};   /* keys the crib proved impossible */
static char * opt_plaintext; /* plaintext to compare to */
static const char * opt_language; /* english, german, danish, french, swedish, finnish,
                                      icelandic, polish, spanish, wehrmacht; no default */
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
/* Circular first-improvement climb instead of steepest ascent: applies the FIRST improving
   move (a cursor sweeps a fixed move list and continues from where it accepted), so it does
   far fewer score_iter calls per climb -- ~2.8x cheaper -- at the cost of recovering worse
   per restart. Set only by -J (the bare first-improvement flag was removed as dominated),
   so it is always paired with the dynamic move order below. */
static int opt_firstimprove;
/* -J: first-improvement with DYNAMIC best-first move ordering. Each climb first scores every
   move once against the starting (perturbed) board, sorts, and then runs the circular
   first-improvement in that order. The order is rebuilt per restart, so it front-loads good
   moves without collapsing restart diversity (unlike the rejected static order). Measured win
   on the realistic ~10-plug regime (+2-6pp mean at matched compute); a loss when few plugs are
   truly set. Off by default; needs -c. */
static int opt_dynorder;
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
/* --no-repair: disable the default 2-plug re-pair barrier cross (try_repair), for
   ablation/measurement. Off by default (baseline byte-identical); needs -c. */
static int opt_no_repair;
/* --cascade: quadgram-gain directed-repair barrier cross, tried at quad convergence.
   A 2-ply "cascade" that uses per-position gain to propose plug corrections (both
   plugboard contacts, self-encryption pruned), ranks them by the full re-decode
   score, applies the best pair even when the first plug is downhill (which un-masks
   the second), and keeps it only if the pair nets an improvement. Off by default
   (baseline byte-identical); needs -c; quad-only. See gain_cascade(); archived/PERFORMANCE.md 4.10. */
static int opt_cascade;
/* --cascade near-solution gate: the cascade only fires on a converged board whose
   per-symbol quad score clears this threshold, so it skips the ~76% junk boards and
   spends its compute only on promising ones. Default -4.9 (English-quad calibrated:
   junk ~-5.3, near-solution 60%+ ~-4.8..-4.2); tune per language via --cascade=VALUE. */
static double opt_cascade_gate;
/* Internal: enable the 3-ply gain cascade (a deeper escalation, tried only when the 2-ply
   cascade found nothing). --polish turns it on for the single best-board finisher. */
static int opt_cascade3;
static int opt_polish;
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
   answer is the artifact). Off by default (no crib file ->
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
                              tool, dominated by a high --restarts climb (see archived/PERFORMANCE.md §3.6). */
/* --ring-stride K (default 1 = off): sparse ring sampling for the rightmost stepping
   wheel (walzenlage[2]). Unlike the leftmost wheel's unconditional exact collapse
   (build_key_space(), archived/PERFORMANCE.md §7.10), the rightmost wheel's own notch gates
   further stepping, so a ring+start shift is only an APPROXIMATION -- measured small
   and smoothly growing with the shift (archived/PERFORMANCE.md §7.11). K>1 tests only every
   Kth ring value (0, K, 2K, ...) in the main search, then runs one refinement pass over
   EVERY skipped value (bruteforce(), after the main search) to recover the exact key.
   Requires both -r and -g to wildcard the rightmost wheel's position (else every value
   is a distinct, necessary key, exactly like the leftmost-wheel collapse's precondition).

   Legal range is 1..26, but the cost curve FLATTENS AT 13 and the useful range is much
   smaller. The coarse set is {v : v < 26, v = 0 mod K}, so it holds 2 values for every
   K in 13..25 -- K=14 samples {0,14} and costs exactly what K=13's {0,13} costs, with
   no compensating gain -- and 1 value only at K=26. Measured on the tool's bare-default
   keyspace: 79092 keys at K=1, 42003 at K=3, 20709 flat across K=13..25, 17667 at K=26.
   Accuracy tracks that: on authentic telegraphic German (L=60, 120 paired trials) K=3
   and K=13 both match the unstrided baseline, while K=26's single coarse anchor loses
   ~10pp of exact recovery. K=2/K=3 stay the recommendation; K=13 is the largest stride
   that is still a uniform sampling; past it only K=26 changes anything, and it changes
   accuracy more than cost. */
static int opt_ring_stride;
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
   e.g. B241AAAQEW): a diagnostic for -F recall testing (archived/CRACKQUALITY_TESTS.md §2).
   With -F set, after tier-1 ranks every key the search prints "true-key tier1 rank
   R of N" to stderr -- R = 1 + the number of keys whose tier-1 IC score is strictly
   higher, N = total keys -- so a harness can measure how often the pre-filter keeps
   the true key. Off by default; parsed into g_tk_* below. */
static const char * opt_true_key;
static int g_tk_u, g_tk_w[3], g_tk_r[3], g_tk_g[3];   /* parsed --true-key (numeric) */
static std::vector<float> g_tk_scores;                /* tier-1 IC score per flat key idx */
static std::atomic<size_t> g_tk_idx{static_cast<size_t>(-1)};   /* flat idx of the true key */

/* --dump-all: with -c, print the FULL setting of every converged (rotor
   key, restart) climb -- "dumpall <refl+wheels> <ring> <start> <score> <plugboard>" -- so a
   wildcarded search (not just a fixed key) can be inspected key-by-key. Display-only under
   the same mutex, so it never affects which candidate wins (-T-deterministic results are
   preserved; only the line ORDER is thread-timing dependent). Very verbose; off by default. */
static bool opt_dump_all;

/* --full-text: print the WHOLE decrypted message with each progress line, instead of the
   19-character preview the fixed-width line has room for. The preview is enough to notice
   a board turning into German and not enough to decide a run is finished, which matters
   when the stop criterion is a person reading the output rather than a score threshold.
     Printed on its own wrapped, indented lines BELOW the progress line rather than by
   widening it: the columns are budgeted to land exactly on 80 (see progress_fmt_3), and
   they have to keep lining up whether this is on or off.
     Not a hot-path concern: a progress line is emitted only when a board beats everything
   echoed so far, so this prints once per improvement -- tens of times in a run -- not once
   per board scored. Off by default. */
static bool opt_full_text;

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
static double ngram_scale[SCORE_FUSED + 1];   /* per-model: 255/(vmax-vmin), full 0..255 range */
static double ngram_bias[SCORE_FUSED + 1];    /* per-model vmin; indexed by SCORE_* */
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
   -> O, sharp-s -> S, ae/oe ligatures -> A/O, thorn -> T [pairs with eth -> D as
   the voiceless/voiced dental-fricative counterpart, Icelandic]). This lets the
   26-letter machine use the accented n-grams in the non-English tables (and
   accented plaintext) instead of discarding them. Added for Swedish/Finnish
   (A-ring, A/O-diaeresis -- already Latin-1), Icelandic (thorn) and Polish (the
   Latin Extended-A ogonek/stroke/acute/dot-above letters below): dropping them
   loses up to ~20% of a table's mass (Polish quadgrams), not a rounding error. */
static int fold_codepoint(unsigned cp)
{
  if ((cp >= 'a') && (cp <= 'z'))
    cp -= 32;
  if ((cp >= 'A') && (cp <= 'Z'))
    return static_cast<int>(cp - 'A');
  /* Latin-1 supplement letters U+00C0..U+00FF -> base letter (' ' = not a letter);
     lower half mirrors the upper except the final cell (sharp-s S vs y-diaeresis Y). */
  static const char lat1[] =
    "AAAAAAACEEEEIIIIDNOOOOO OUUUUYTS"
    "AAAAAAACEEEEIIIIDNOOOOO OUUUUYTY";
  if ((cp >= 0xC0) && (cp <= 0xFF))
    {
      char b = lat1[cp - 0xC0];
      return (b == ' ') ? -1 : (b - 'A');
    }
  switch (cp)
    {
    case 0x0152: case 0x0153: return 'O' - 'A';   /* OE ligature */
    case 0x0178: return 'Y' - 'A';                /* Y with diaeresis */
    /* Latin Extended-A: Polish diacritics, diacritic stripped to the base letter
       (Z-acute and Z-dot-above both fold to Z, same "closest base letter"
       convention as sharp-s -> S above). */
    case 0x0104: case 0x0105: return 'A' - 'A';   /* A-ogonek (Ą/ą) */
    case 0x0106: case 0x0107: return 'C' - 'A';   /* C-acute (Ć/ć) */
    case 0x0118: case 0x0119: return 'E' - 'A';   /* E-ogonek (Ę/ę) */
    case 0x0141: case 0x0142: return 'L' - 'A';   /* L-stroke (Ł/ł) */
    case 0x0143: case 0x0144: return 'N' - 'A';   /* N-acute (Ń/ń) */
    case 0x015A: case 0x015B: return 'S' - 'A';   /* S-acute (Ś/ś) */
    /* Z-acute and Z-dot-above both fold to Z, so they share one case list. */
    case 0x0179: case 0x017A: case 0x017B: case 0x017C: return 'Z' - 'A';
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
  int overflowed = 0;   /* records whose count exceeded UINT32_MAX and was clamped */
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
         does not fold to exactly n A-Z letters (e.g. a stray digit) is skipped.

         `count` is parsed as unsigned long long (not directly into the uint32_t
         `table` cell): a generated table can contain a count that overflows 32
         bits -- the wehrmacht_quadgrams.txt generator (eval/build_telegraphic_ngrams.py)
         once emitted values up to ~8.3e20 from an unclamped reweighting ratio, and
         parsing that straight into `unsigned` via "%u" is undefined behaviour on
         overflow (glibc happened to saturate to UINT32_MAX, silently tying 843
         distinct quadgrams at one value -- see archived/PERFORMANCE.md 6.17). The generator
         is now capped and should never produce this again, but the loader clamps
         explicitly and audibly rather than depending on that, or on unspecified
         sscanf behaviour, to stay correct for any future/external table. */
      /* Hand-rolled instead of sscanf("%15s %llu", ...). sscanf reinterprets the format
         string and runs a general integer conversion for every one of the ~457k lines in
         the english quadgram table; measured on that file it is 59 ms of the 71 ms this
         loop costs, against 11 ms for the parse below (110 ms vs 27 ms under ASan). Since
         every invocation of the tool pays this before doing any work, it was ~30% of a
         short run and a third of the sanitizer CI job.

         Equivalent to the sscanf form on the bundled tables (verified byte-identical
         table hashes across every language x model), and deliberately STRICTER on
         malformed input in three places, none of them reachable from a well-formed table:
           - a token longer than 15 bytes is skipped here. sscanf would truncate it to 15
             and then usually fail the number conversion; in the one case it does not
             (chars 16+ happen to be digits) the record is still dropped below, because a
             >15-byte token folds to at least 6 letters and every n is <= 4.
           - a NEGATIVE count is skipped rather than wrapped. "%llu" accepts a sign and
             wraps, which the UINT32_MAX clamp below would then silently turn into the
             largest possible count -- the worst way to mishandle it.
           - a count exceeding 64 bits saturates rather than being undefined behaviour,
             which is what "%llu" overflow formally is (see the clamp comment above). */
      char gram[16];
      unsigned long long count64 = 0;
      {
        const char * p = line;
        while ((*p == ' ') || (*p == '\t') || (*p == '\n') || (*p == '\v')
               || (*p == '\f') || (*p == '\r'))
          p++;
        const char * gstart = p;
        while ((*p != '\0') && (*p != ' ') && (*p != '\t') && (*p != '\n')
               && (*p != '\v') && (*p != '\f') && (*p != '\r'))
          p++;
        size_t glen = static_cast<size_t>(p - gstart);
        if ((glen == 0) || (glen >= sizeof(gram)))
          continue;                       /* empty line, or an over-long token */
        memcpy(gram, gstart, glen);
        gram[glen] = '\0';
        while ((*p == ' ') || (*p == '\t') || (*p == '\n') || (*p == '\v')
               || (*p == '\f') || (*p == '\r'))
          p++;
        if (*p == '+')
          p++;
        if ((*p < '0') || (*p > '9'))
          continue;                       /* no count (or a negative one) */
        while ((*p >= '0') && (*p <= '9'))
          {
            unsigned d = static_cast<unsigned>(*p - '0');
            if (count64 > (UINT64_MAX - d) / 10)
              { count64 = UINT64_MAX; break; }   /* saturate, never wrap */
            count64 = count64 * 10 + d;
            p++;
          }
      }
      if (count64 > UINT32_MAX)
        {
          count64 = UINT32_MAX;
          overflowed++;
        }
      unsigned count = static_cast<unsigned>(count64);
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
  if (overflowed > 0)
    fprintf(stderr, "Warning: %s: clamped %d record(s) with a count exceeding %u.\n",
            filename, overflowed, UINT32_MAX);
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

  /* Evaluate logval ONCE per entry into a scratch array, rather than in both the min/max
     and the quantise loop below. The two loops used to call it 2 x size times (914k for
     quad), and under -a/-f each call recomputes the whole four-order log-linear mixture:
     measured 9.5 + 10.5 ms for -q but 77 + 77 ms for -f on the same table, i.e. the two
     passes cost more than parsing the file. Storing doubles (not floats) keeps every
     quantised byte identical -- the values feed a rounding boundary, so narrowing here
     would be a silent table change. 3.7 MB transient for quad, freed on return. */
  std::vector<double> vals(size);
  for (int i = 0; i < size; i++)
    vals[i] = logval(i);

  double vmin = 1e300, vmax = -1e300;
  for (int i = 0; i < size; i++)
    {
      double v = vals[i];
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
      double q = (vals[i] - bias) * scale;
      if (q < 0.0)
        q = 0.0;
      else if (q > 255.0)
        q = 255.0;
      itable[i] = static_cast<uint8_t>(q < 0.0 ? q - 0.5 : q + 0.5);
    }
}



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

void init_plug_fixed(const char * steckerbrett_string, const char * no_plug_string)
{
  for (int j = 0; j < asize; j++)
    plug_fixed[j] = false;
  int plug_count = static_cast<int>(strlen(steckerbrett_string) / 2);
  for (int i = 0; i < plug_count; i++)
    {
      plug_fixed[char2num(steckerbrett_string[2*i+0])] = true;
      plug_fixed[char2num(steckerbrett_string[2*i+1])] = true;
    }
  /* --no-plug letters are fixed in exactly the same sense -- the climb may not rewire
     them -- they are simply fixed to nothing rather than to a partner. Since they stay
     self-steckered, every move set that skips a fixed letter skips them too. */
  for (const char * p = no_plug_string; *p != 0; p++)
    plug_fixed[char2num(*p)] = true;
}

/* --- crib deduction (--crib): the menu and its closure ---------------------------
   Decryption of one character is  p = steck[core_i[steck[c]]], and the rotor core is its
   own inverse, so the same line rearranges to

       steck[p] = core_i[steck[c]]

   -- if you know what the ciphertext letter is plugged to, the core tells you what the
   plaintext letter is plugged to. That is the whole deduction step: one table lookup, on
   the rows[] table setup_mapping() already builds.

   The crib gives one such equation per position. Guess a single plug, chain the rule
   along every equation, and add RECIPROCITY -- steck[x] = y implies steck[y] = x, and no
   two letters may share a partner. That second part is Welchman's diagonal board, free
   here because the plugboard is stored as an involution, and measured to supply almost
   all of the rejecting power: a loop-free 12-letter menu still rejects 88% of rotor
   settings, against 0% without it (cribs.md 4.1).

   A contradiction kills the guess. Kill all 26 and the rotor setting cannot have produced
   the crib, so the search skips it without scoring anything.

   The menu is a property of the crib and the ciphertext, not of the key, so its edges and
   its anchor letter are built once at startup by init_crib(). */
/* Build the alignment list once, and each alignment's anchor.

   An alignment is VIABLE only if the crib disagrees with the ciphertext at every position:
   an Enigma never encrypts a letter to itself, so a match there proves the crib cannot sit
   at that offset. That test costs nothing and removes about half the alignments outright
   (cribs.md 6.6) -- it is pure arithmetic on the ciphertext, done here rather than per key.

   The anchor is the highest-degree letter of the menu's largest connected component, so
   one guess reaches as far as it can before a second would be needed. It depends on which
   ciphertext letters the crib lands on, hence one per alignment. */
static void init_crib()
{
  crib_edges = static_cast<int>(strlen(opt_crib_text));
  for (int j = 0; j < crib_edges; j++)
    crib_p[j] = static_cast<unsigned char>(char2num(opt_crib_text[j]));

  int first = (opt_crib_at >= 0) ? opt_crib_at : 0;
  int last = (opt_crib_at >= 0) ? opt_crib_at : textlength - crib_edges;
  crib_aligns = 0;
  for (int at = first; at <= last; at++)
    {
      bool viable = true;
      for (int j = 0; j < crib_edges; j++)
        if (crib_p[j] == num_ciphertext[at + j])
          {
            viable = false;
            break;
          }
      if (! viable)
        continue;

      /* components by union-find over this alignment's edges, then the largest
         component's busiest letter */
      int parent[asize], deg[asize];
      for (int i = 0; i < asize; i++)
        {
          parent[i] = i;
          deg[i] = 0;
        }
      for (int j = 0; j < crib_edges; j++)
        {
          int a = crib_p[j], b = num_ciphertext[at + j];
          deg[a]++;
          deg[b]++;
          while (parent[a] != a) a = parent[a];
          while (parent[b] != b) b = parent[b];
          if (a != b)
            parent[a] = b;
        }
      int size[asize], root[asize];
      for (int i = 0; i < asize; i++)
        size[i] = 0;
      for (int i = 0; i < asize; i++)
        {
          int r = i;
          while (parent[r] != r) r = parent[r];
          root[i] = r;
          if (deg[i])
            size[r]++;
        }
      int big = -1;
      for (int i = 0; i < asize; i++)
        if (deg[i] && ((big < 0) || (size[root[i]] > size[root[big]])))
          big = i;
      int anchor = -1;
      for (int i = 0; i < asize; i++)
        if (deg[i] && (root[i] == root[big])
            && ((anchor < 0) || (deg[i] > deg[anchor])))
          anchor = i;
      if (anchor < 0)
        continue;

      crib_align[crib_aligns] = at;
      crib_anchor_at[crib_aligns] = static_cast<unsigned char>(anchor);
      crib_aligns++;
    }
}

/* One hypothesis at one alignment: "the anchor letter is plugged to hyp". Propagates to a
   fixed point and
   returns false on contradiction. `board` is left holding the partial plugboard it
   deduced (-1 = still unknown), which is what the hybrid will seed a climb from.
     `board` is int, not signed char: it holds -1 alongside letter values coming out of
   the UNSIGNED char rows[] table, and mixing those two signednesses is the bug class
   clang-tidy's bugprone-signed-char-misuse exists to catch. 26 ints, read once per key,
   cost nothing. */
static bool crib_try(const machine & m, int at, int anchor, int hyp, int * board)
{
  for (int i = 0; i < asize; i++)
    board[i] = -1;
  const unsigned char * const * rows = m.rows;

  /* assign steck[x] = y, and its reciprocal, failing on any disagreement */
#define CRIB_SET(x, y)                                          \
  do {                                                          \
    int xx = (x), yy = (y);                                     \
    if ((board[xx] >= 0) && (board[xx] != yy)) return false;    \
    if ((board[yy] >= 0) && (board[yy] != xx)) return false;    \
    board[xx] = yy;                                             \
    board[yy] = xx;                                             \
  } while (0)

  CRIB_SET(anchor, hyp);
  bool changed = true;
  while (changed)
    {
      changed = false;
      for (int j = 0; j < crib_edges; j++)
        {
          const unsigned char * core = rows[at + j];
          int p = crib_p[j], c = num_ciphertext[at + j];
          if ((board[c] >= 0) && (board[p] < 0))
            {
              CRIB_SET(p, static_cast<int>(core[board[c]]));
              changed = true;
            }
          else if ((board[p] >= 0) && (board[c] < 0))
            {
              CRIB_SET(c, static_cast<int>(core[board[p]]));
              changed = true;
            }
          else if ((board[p] >= 0) && (board[c] >= 0))
            {
              if (static_cast<int>(core[board[c]]) != board[p])
                return false;
            }
        }
    }
#undef CRIB_SET
  return true;
}

/* --crib-dump: print every surviving hypothesis, the alignment it survived at, and the
   plugs it deduces, so a harness can check them against a known board (cribs.md 10.1).
   Display-only and under the same mutex as the progress lines, so it cannot affect which
   candidate wins. Very verbose; off by default.

   THE LINE FORMAT HAS TWO CONSUMERS that both parse it positionally --
   eval/crib_vectors_check.py and the --crib checks in tests/run_tests.sh -- so adding a
   field here silently breaks them until they are updated. Adding the alignment field did
   exactly that. Change both when changing this. */
static void crib_dump(machine & m, int r1, int r2, int r3, int g1, int g2, int g3);

/* The first alignment at which some hypothesis survives, or -1 when every hypothesis at
   every viable alignment contradicts -- in which case this rotor setting cannot have
   produced the crib ANYWHERE in the message, and the caller skips it without scoring.

   Returning the alignment rather than a bool is what step 5 will seed a climb from, and
   what the progress line reports now. The early exit matters and is asymmetric: a key that
   survives usually does so at one of the first alignments tried, while a REJECTED key pays
   the whole sweep -- and rejected keys are meant to be the common case.

   Pure function of the key: no shared state, so it is thread-safe and -T-deterministic. */
static int crib_first_stop(const machine & m)
{
  int board[asize];
  for (int a = 0; a < crib_aligns; a++)
    for (int h = 0; h < asize; h++)
      if (crib_try(m, crib_align[a], crib_anchor_at[a], h, board))
        return crib_align[a];
  return -1;
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

/* mod26() adds a SINGLE alphabet, so it is only correct for x >= -26 -- fine everywhere it
   is used on a position plus a step. The --ring-stride refinement derives offsets from a
   step-count difference that is not bounded that way (a candidate start1 far from the
   coarse winner's can differ by several steps, and the difference is subtracted from a
   position), so it uses this full-range form. Caught by UBSan as a negative subst_array
   index, which then read out of bounds. */
inline int mod26_full(int x)
{
  return ((x % asize) + asize) % asize;
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

/* Cumulative step counts of the middle and left wheels, per character position,
   for a key starting at (g1, g2). Mirrors setup_mapping()'s stepping exactly --
   including the double step, where the middle wheel sitting on its OWN notch
   advances both itself and the left wheel -- but records only the counts, since
   that is all the --ring-stride refinement's derivation needs.

   The refinement uses these to compute how far a candidate's schedule has
   drifted from the coarse winner's (refinement.md §4). The substitution consumes
   a_i = o0 + left(i), b_i = o1 + mid(i) and c_i = o2 + i, so a candidate whose
   step counts differ from the winner's by a constant reproduces the winner's
   alignment exactly when its ring offsets absorb that constant. w1/w2 are
   TRANSLATED rotor indices (as held in machine::walzenlage), since notch[] is
   indexed that way. */
static void step_counts(int w1, int w2, int g1, int g2,
                        unsigned short * mid, unsigned short * left)
{
  int nmid = 0, nleft = 0;
  for (int i = 0; i < textlength; i++)
    {
      if (notch[w1][g1])
        {
          nleft++;
          nmid++;
          g1 = mod26(1 + g1);
        }
      else if (notch[w2][g2])
        {
          nmid++;
          g1 = mod26(1 + g1);
        }
      g2 = mod26(1 + g2);
      mid[i] = static_cast<unsigned short>(nmid);
      left[i] = static_cast<unsigned short>(nleft);
    }
}

/* The distinct values of (a[i] - b[i]) over the message, ascending. At most
   `cap` are stored; the return value is how many. The refinement emits one
   candidate per distinct value rather than reducing them to a mode -- a mode is
   a guess that can be wrong on a short message where the two schedules alternate
   evenly, while enumerating a handful of values cannot be (refinement.md §4). */
static int step_deltas(const unsigned short * a, const unsigned short * b,
                       int * out, int cap)
{
  int n = 0;
  for (int i = 0; i < textlength; i++)
    {
      int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
      int j = 0;
      while ((j < n) && (out[j] != d))
        j++;
      if ((j == n) && (n < cap))
        out[n++] = d;
    }
  return n;
}

/* Add every value within `band` of one already in `out[0..n)`, deduplicated, and return
   the new count. The derivation is exact for the step-count term, but the coarse winner's
   own offset can be off for reasons no schedule explains (refinement.md §7.2), and a small
   band around each derived value covers that at a few times the candidate count -- which
   is still two orders of magnitude below the enumeration it replaced. */
static int widen_deltas(int * out, int n, int band, int cap)
{
  int base = n;
  for (int i = 0; i < base; i++)
    for (int d = -band; d <= band; d++)
      {
        if (d == 0)
          continue;
        int v = out[i] + d;
        int j = 0;
        while ((j < n) && (out[j] != v))
          j++;
        if ((j == n) && (n < cap))
          out[n++] = v;
      }
  return n;
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

/* Progress-line columns (shared by the header and the lines): score right-aligned in 8
   (a 4-decimal score reaches 8 chars, e.g. "-12.7393", so 8 keeps the columns from
   shifting), reflector+wheels up to 5 chars (M4 "bB123"), ring and start 3 each --
   M4's 4-char ring/start simply print their extra character -- then the plugboard and
   the first preview_len characters of the decoded text.

   The plugboard column always reserves the full 13 pairs (38 chars), and every field is
   exactly as wide as its content, so each column is separated by a single space.

   The key is wider on the 4-wheel M4 (reflector+Greek+3 wheels = 5 chars, and a 4-char
   ring/start) than on a 3-wheel machine (4/3/3), so the two use their own format and
   their own preview length, each budgeted to land exactly on 80 columns:

     3-wheel: 8+1+4+1+3+1+3+1+38+1+19 = 80
     M4:      8+1+5+1+4+1+4+1+38+1+16 = 80

   The preview is what absorbs the difference -- it is truncated to whatever the key
   leaves, so a line never exceeds 80 columns for either machine. The header uses the
   same format, so its columns line up with the lines below it. */
static const int preview_len_3 = 19;   /* 3-wheel (standard / Norway) */
static const int preview_len_4 = 16;   /* 4-wheel M4: the wider key costs 3 characters */
static const int preview_max = 19;     /* buffer size: the larger of the two */
static const char progress_fmt_3[] = "%8s %-4s %-3s %-3s %-38s %s\n";
static const char progress_fmt_4[] = "%8s %-5s %-4s %-4s %-38s %s\n";
/* --crib adds an "A" column: which alignment the crib survived at. A crib run produces
   lines from many alignments and they are otherwise indistinguishable, so the line has to
   say which. The width comes out of the preview, the field the 80-column budget already
   designates as absorbing the difference between the two key layouts:
     3-wheel + crib: 8+1+4+1+3+1+3+1+38+1+3+1+15 = 80
     M4      + crib: 8+1+5+1+4+1+4+1+38+1+3+1+12 = 80 */
static const int preview_len_3c = 15;
static const int preview_len_4c = 12;
static const char progress_fmt_3c[] = "%8s %-4s %-3s %-3s %-38s %3s %s\n";
static const char progress_fmt_4c[] = "%8s %-5s %-4s %-4s %-38s %3s %s\n";

/* The alignment the current key's crib survived at, for the progress line. Display-only
   and per worker: set by search_worker once per key, read by showconfig from both the
   key-level merge and from inside a climb. thread_local rather than a `machine` member so
   struct machine's layout -- which the hot loops are documented to be sensitive to -- is
   left alone; nothing here is on a hot path. */
static thread_local int g_crib_stop_shown = -1;

static inline const char * progress_fmt(void)
{
  if (opt_crib_text)
    return opt_m4 ? progress_fmt_4c : progress_fmt_3c;
  return opt_m4 ? progress_fmt_4 : progress_fmt_3;
}

static inline int preview_len(void)
{
  if (opt_crib_text)
    return opt_m4 ? preview_len_4c : preview_len_3c;
  return opt_m4 ? preview_len_4 : preview_len_3;
}

/* Column header, printed once before the first progress line of a search. */
void showconfig_header(void)
{
  if (opt_crib_text)
    fprintf(stderr, progress_fmt(), "Score", "W", "R", "G", "S", "A", "Text");
  else
    fprintf(stderr, progress_fmt(), "Score", "W", "R", "G", "S", "Text");
}

/* Format m's rotor key into w (reflector+wheels), r (ring), g (start) -- the columns
   showconfig prints, with the M4 Greek-offset and Norway-reflector handling. Shared with
   the --dump-all diagnostic so the two can never diverge. */
static void format_key(machine & m, char (&w)[8], char (&r)[8], char (&g)[8])
{
  if (opt_m4)
    {
      /* M4: thin reflector (b/c) + static Greek wheel (B/G). Only the Greek
         (start - ring) offset is identifiable, so it is shown as start=offset,
         ring=A. The reflector/Greek/ring/start columns list the Greek first.
         Wheel numbers are single digits (1-8), printed as chars. */
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
}

/* --full-text: print the whole decrypted message below the progress line, wrapped and
   indented so it reads as a continuation of that line rather than as another one.
     Decoded on the fly from m's CURRENT board for the same reason the preview is:
   m.plaintext holds an earlier candidate while a climb is running. Called from
   showconfig(), so it inherits the best-result mutex and cannot interleave with another
   thread's output. */
static const int full_text_indent = 2;
static const int full_text_width = 76;   /* + indent = 78, inside a 79-column terminal */

static void show_full_text(machine & m)
{
  const unsigned char * steck = m.steckerbrett;
  const unsigned char * const * rows = m.rows;
  char line[full_text_width + 1];
  for (int i = 0; i < textlength; i += full_text_width)
    {
      int n = textlength - i;
      if (n > full_text_width)
        n = full_text_width;
      for (int j = 0; j < n; j++)
        line[j] = num2char(decode_at(steck, rows, num_ciphertext, i + j));
      line[n] = 0;
      fprintf(stderr, "%*s%s\n", full_text_indent, "", line);
    }
}

/* Format m's plugboard into s: canonical (each pair low-high, pairs ordered by low
   letter), so a harness can dedupe boards by string equality. */
static void format_plugboard(machine & m, char (&s)[3 * 13])
{
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
}

void showconfig(machine & m, double score)
{
  char w[8], r[8], g[8], s[3 * 13], text[preview_max + 1];

  format_key(m, w, r, g);
  format_plugboard(m, s);

  /* Decode the text preview on the fly from the machine's CURRENT board --
     m.plaintext can be stale here (inside a running climb it still holds an
     earlier candidate). */
  const unsigned char * steck = m.steckerbrett;
  const unsigned char * const * rows = m.rows;
  const int plen = preview_len();
  int n = (textlength < plen) ? textlength : plen;
  for (int i = 0; i < n; i++)
    text[i] = num2char(decode_at(steck, rows, num_ciphertext, i));
  text[n] = 0;

  char scorebuf[16];
  snprintf(scorebuf, sizeof(scorebuf), "%.4f", score);
  if (opt_crib_text)
    {
      char at[8];
      snprintf(at, sizeof(at), "%d", g_crib_stop_shown);
      fprintf(stderr, progress_fmt(), scorebuf, w, r, g, s, at, text);
    }
  else
    fprintf(stderr, progress_fmt(), scorebuf, w, r, g, s, text);

  if (opt_full_text)
    show_full_text(m);
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
   to tune. ENIGMA_IC_BLEND overrides it for experiments, exactly as ENIGMA_LOGLIN
   overrides -a's weights. See archived/PERFORMANCE.md 6.4. */
static const double fused_lambda_default = 30.0;
static double g_fused_lambda = fused_lambda_default;

static void ic_blend_init()
{
  const char * s = getenv("ENIGMA_IC_BLEND");
  if (s)
    g_fused_lambda = atof(s);
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

  double coin = 0.0;
  for (int j = 0; j < asize; j++)
    coin += static_cast<double>(freq[j]) * (freq[j] - 1);
  *ic_out = (textlength > 1)
    ? coin / (static_cast<double>(textlength) * (textlength - 1)) : 0.0;

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

/* --cascade tuning: candidate shortlist size and plug1 beam width. Plug2 is scored
   over the whole shortlist per plug1, so cascade cost is ~CAP + N1*CAP score_iter. */
static const int GAINFIX_CAP = 25;
static const int GAINFIX_N1  = 6;
static const int GAINFIX_N2  = 6;   /* 3-ply: intermediate plug2 beam */
static const int GAINFIX_K3  = 8;   /* 3-ply: # of sacrifice pairs reclimbed */

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
    ((m.scoring == SCORE_ALL) || (m.scoring == SCORE_FUSED)) ? all8 : quad8;

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

/* --cascade: the 2-ply gain cascade barrier cross (archived/PERFORMANCE.md 4.10). Quad-only,
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
  if ((m.scoring != SCORE_QUAD && m.scoring != SCORE_ALL
       && m.scoring != SCORE_FUSED) || textlength < 8)
    return false;
  if (cur_score < opt_cascade_gate)             /* near-solution gate: skip junk boards */
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

/* 3-ply ("sacrifice + reclimb"): a deeper escalation for 3-plug tangles the 2-ply pair
   can't cross, tried only when the 2-ply cascade found nothing. Rank the (plug1,plug2)
   SACRIFICE pairs (both plugs, possibly downhill) by their 2-plug score, and for the top-K
   commit the sacrifice and run a full PLAIN reclimb -- letting the ordinary climb find the
   completing plug(s) AND shed spurious ones -- keeping the best-scoring result. No explicit
   plug3 search: the completing plug is the top improving move after the sacrifice, so the
   reclimb finds it (measured), which is both simpler and recovers MORE than committing one
   fixed completing plug (a full climb per sacrifice beats a single triple; archived/PERFORMANCE.md
   4.11). The reclimb runs with gainfix off -> no recursion, capped at the same max_pairs.
   template<bool EX>/plug_fixed like the rest; -T-deterministic. */
template<bool EX>
static bool gain_cascade_3ply(machine & m, double cur_score, int max_pairs)
{
  if ((m.scoring != SCORE_QUAD && m.scoring != SCORE_ALL
       && m.scoring != SCORE_FUSED) || textlength < 8)
    return false;
  if (cur_score < opt_cascade_gate)             /* near-solution gate: skip junk boards */
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
  int save_gf = opt_cascade, save_gf3 = opt_cascade3;
  opt_cascade = 0; opt_cascade3 = 0;
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
  opt_cascade = save_gf; opt_cascade3 = save_gf3;

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

/* --- Circular first-improvement climb (-J) ------------------------------------

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
  int visit[nmoves];
  if (dyn_order)
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
  /* -J: circular first-improvement instead of steepest ascent (off by default, so the
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
         improves, loop back and let the cheap climb resume from the new board. */
      if ((! opt_no_repair && try_repair<EX>(m, cur))
          || (opt_cascade && gain_cascade<EX>(m, cur))
          || (opt_cascade3 && gain_cascade_3ply<EX>(m, cur, max_pairs)))
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
   letters that are still unplugged AND not fixed (so -s pairs are preserved, and so are
   --no-plug letters, which are self-steckered and would otherwise look free here). This is
   the per-restart perturbation: a kick of k random plugs (default_perturb, or an rN
   token) into a new basin, near the typical plug count so the staged climb need not
   tear down a near-saturated board (CODE_REVIEW §9). With k=0 it is a no-op (so r0
   makes restarts identical -- a useful control). */
void perturb_steckerbrett(machine & m, uint64_t * rng, int k)
{
  unsigned char freelet[asize];
  int nfree = 0;
  for (int i = 0; i < asize; i++)
    if ((m.steckerbrett[i] == i) && (! plug_fixed[i]))
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
    case SCORE_FUSED:   /* -f reuses all8; only the IC term differs at score time */
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
    case 'f': return SCORE_FUSED;
    default:  return SCORE_IC;
    }
}

/* Record a bare model selector (-i/-m/-b/-t/-q/-a/-f) as a single uncapped --score <model>
   stage (REDESIGN Part C). Two selectors that disagree (e.g. -m -q) make the intended
   model ambiguous, so reject them; repeats that agree (-q -q) are fine. Sets opt_scoring
   so a run with no --score ranks by the selected model. */
static void select_model(int model)
{
  if ((opt_model_selector != -1) && (opt_model_selector != model))
    fatal("Conflicting scoring models: the -i/-m/-b/-t/-q/-a/-f selectors disagree; "
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

      if (strchr("imbtqaf", letter))
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
        fatal("Illegal --score schedule (tokens are i/m/b/t/q/a/f + optional cap, "
              "e.g. --score m4f10; use --random for the kick, --exhaust for forcing)");
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
   a high --restarts greedy climb at equal compute; see archived/PERFORMANCE.md §3.6). E is the number
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
  for (const char * p = opt_no_plug; *p != 0; p++)   /* --no-plug: not available to force */
    c.used[char2num(*p)] = true;
}

/* One parallel exhaustion unit: all combos whose first forced pair is g_exhaust_firsts[fi],
   over all restarts. Leaves m at the unit's best board/plaintext and returns its score, or
   a sentinel below any real score if the first pair leaves no room for E-1 more pairs. */
/* --- the hybrid: deduce, then climb (cribs.md 7, 12 step 5) --------------------------

   One work item at a key the crib did not reject: climb once from EVERY surviving
   hypothesis, seeded with the plugs that hypothesis deduces, and keep the best.

   The deduced plugs are HELD FIXED for the climb, in PLUG_FIXED_EX -- the same per-worker
   pin set --exhaust uses, because plug_fixed is a read-only global that no worker may
   touch. They stay fixed through --polish too: a deduced plug comes from arithmetic on the
   machine equation, while the finisher's cascade is score-driven local repair, so
   releasing them would let weaker evidence overwrite stronger (cribs.md 7b). A WRONG
   hypothesis needs no such rescue -- it loses on score to the other 25.

   Letters the deduction settles as carrying NO cable are pinned as well: board[x] == x is
   a real finding, not an absence of one, and marking it stops the climb wasting moves on a
   letter that cannot be plugged. That is the value cribs.md 7 wanted --no-plug for, had
   here for free.

   Cost is one climb per surviving hypothesis. With a long crib that is usually one; with a
   short one it is the several that cribs.md 7a's seed mode expects and prices. */
static void dump_all(machine & m, double score);   /* defined with the other diagnostics */

static double crib_unit(machine & m, size_t key_index, int restart)
{
  double best = 0.0;
  bool have = false;
  char best_pt[maxlen + 1];
  unsigned char best_steck[asize];
  int best_at = -1;
  int board[asize];

  for (int a = 0; a < crib_aligns; a++)
    for (int h = 0; h < asize; h++)
      {
        if (! crib_try(m, crib_align[a], crib_anchor_at[a], h, board))
          continue;
        init_steckerbrett(m, opt_steckerbrett);      /* board = identity + -s */
        memcpy(PLUG_FIXED_EX, plug_fixed, asize);    /* pins = -s / --no-plug ... */
        for (int x = 0; x < asize; x++)
          if (board[x] >= 0)
            {
              m.steckerbrett[x] = static_cast<unsigned char>(board[x]);
              PLUG_FIXED_EX[x] = true;               /* ... plus this deduction */
            }
        /* The kick is off by default here and should stay off: it can only scatter the
           letters the deduction did NOT settle, and a seeded climb starts near the answer
           (cribs.md 7b). -R N still asks for N kicked passes if that is wanted. */
        if (opt_restarts >= 1)
          {
            uint64_t rng = restart_seed(key_index, restart);
            perturb_steckerbrett(m, & rng, opt_perturb);
          }
        double sc = run_stages<true>(m);
        if (opt_dump_all)
          dump_all(m, sc);
        if (! have || (sc > best))
          {
            best = sc;
            have = true;
            best_at = crib_align[a];
            memcpy(best_pt, m.plaintext, static_cast<size_t>(textlength) + 1);
            memcpy(best_steck, m.steckerbrett, asize);
          }
      }

  if (! have)
    return -1e300;      /* no hypothesis survived: never wins the merge */
  memcpy(m.plaintext, best_pt, static_cast<size_t>(textlength) + 1);
  memcpy(m.steckerbrett, best_steck, asize);
  g_crib_stop_shown = best_at;   /* the progress line reports the winning alignment */
  return best;
}

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
  for (const char * p = opt_no_plug; *p != 0; p++)   /* --no-plug: never force a pair here */
    sfixed[char2num(*p)] = true;
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
/* ENIGMA_SA_STAGES probe: let -A run the leading --score stages as its pre-pass
   instead of the built-in IC one. Read once (the getenv is not on any hot path, but
   the answer is constant for a run and the SA path is per-restart). Off by default,
   so the shipped SA trajectory stays byte-identical. See archived/PERFORMANCE.md 3.11. */
static bool sa_staged_prepass()
{
  static const bool on = (getenv("ENIGMA_SA_STAGES") != nullptr);
  return on;
}


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
  if (sa_staged_prepass() && (opt_nstages > 1))
    {
      /* ENIGMA_SA_STAGES probe (archived/PERFORMANCE.md 3.11): honour the WHOLE --score
         schedule, not just its last stage's cap. By default SA ignores the leading
         stages and always seeds with IC, so `-A --score m4a10` is byte-identical to
         `-A --score a10` -- SA cannot use the mono pre-pass that is worth ~3-4pp over
         IC to the greedy climb, which is part of why greedy beats SA outright on
         telegraphic traffic. This runs each leading stage at its own cap instead. */
      for (int i = 0; i < opt_nstages - 1; i++)
        {
          m.scoring = opt_stages[i].model;
          hillclimb<false>(m, opt_stages[i].cap);
        }
      m.scoring = target_model;
    }
  else if (target_model != SCORE_IC)
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

/* Serialises the --dump-all diagnostic lines; display-only, so results stay
   -T-deterministic. */
static std::mutex g_dump_mutex;

/* --dump-all: emit one converged (rotor key, restart) climb's FULL setting -- the rotor
   key (reflector+wheels / ring / start), the score, and the plugboard -- so a wildcarded
   search can be inspected key-by-key. Reuses showconfig's format_key/format_plugboard so
   the rotor key matches the progress line exactly (the climb path restored the true start
   positions, so the key is correct here). Under the shared mutex; display-only. */
static void dump_all(machine & m, double score)
{
  char w[8], r[8], g[8], s[3 * 13];
  format_key(m, w, r, g);
  format_plugboard(m, s);
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  fprintf(stderr, "dumpall %s %s %s %.4f %s\n", w, r, g, score, s);
}

/* --crib-dump: one line per surviving hypothesis at this key -- "cribstop <key> <anchor>
   <letter> <plugs>" -- so a harness can check the deduced plugs against a known board
   (cribs.md 10.1) and count stops (10.3). The rotor key is rebuilt from the caller's
   ring/start rather than read from the machine, because on the scan path setup_mapping
   has already stepped grundstellung (the documented lazy restore). Under the same mutex
   as --dump-all; display-only, so results stay -T-deterministic. */
static void crib_dump(machine & m, int r1, int r2, int r3, int g1, int g2, int g3)
{
  char w[8], r[8], g[8];
  init_ring_grund(m, r1, r2, r3, g1, g2, g3);
  format_key(m, w, r, g);
  int board[asize];
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  for (int a = 0; a < crib_aligns; a++)
    for (int h = 0; h < asize; h++)
      {
        int anchor = crib_anchor_at[a];
        if (! crib_try(m, crib_align[a], anchor, h, board))
          continue;
        fprintf(stderr, "cribstop %s %s %s %d %c %c", w, r, g, crib_align[a],
                num2char(anchor), num2char(h));
        for (int x = 0; x < asize; x++)
          if (board[x] >= 0)
            fprintf(stderr, " %c%c", num2char(x), num2char(board[x]));
        fputc('\n', stderr);
      }
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
  if (opt_dump_all)
    dump_all(m, score);
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
  /* Ring position 2 (the rightmost wheel) is the one dimension that can be a
     NON-CONTIGUOUS set: --ring-stride's coarse pass samples {0, K, 2K, ...} and its
     refinement tests a wrapped window around the coarse winner with that winner
     removed. Both are expressed as an explicit ascending value list, so the decode is
     a plain lookup (r2_vals[i]) instead of arithmetic that has to know about strides.
     r_min[2]/r_max[2] still describe the caller's requested BOUNDS (build_key_space
     derives the list from them); everything that decodes a key reads the list, never
     the bounds. r2_n always equals rc[2]. Filled via set_ring2() below.

     unsigned char, not int, and this is load-bearing: search_worker() reads this
     struct in its per-key decode, so growing it pushes that decode across more cache
     lines. An int[26] list measured a REAL ~5% search regression under g++ -- against
     a base-vs-base noise floor of only 0.5% on that benchmark, so well outside the
     noise (the hillclimb tier's own floor is ~4.5%, which is why its numbers looked
     scattered and meant nothing). A byte holds 0..25 fine and keeps the struct near
     its original footprint. See archived/PERFORMANCE.md §7.11. */
  unsigned char r2_vals[asize];
  int r2_n;
};

/* Fill a search_range's ring-2 value list from a 26-bit mask (bit v = test ring2 v).
   A mask is how callers naturally express the set -- a stride, a wrapped window, a
   window minus its centre -- and expanding it once here keeps every decode site a
   simple indexed load. Ascending order makes the enumeration deterministic. */
static void set_ring2(search_range & r, unsigned int mask)
{
  r.r2_n = 0;
  for (int v = 0; v < asize; v++)
    if (mask & (1u << v))
      r.r2_vals[r.r2_n++] = static_cast<unsigned char>(v);
}

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
  /* Winning plugboard, recorded at the merge so the post-search --polish pass can
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

/* --- middle-wheel ring x start collapse (archived/PERFORMANCE.md §7.12) -------------
   Shifting ring1 and start1 together leaves mod26(g1-r1) -- the middle wheel's whole
   contribution to the substitution -- invariant, so two such pairs can only differ
   through notch[w1][g1], the middle notch that gates the left wheel and the double
   step. The middle wheel steps only ~once per 26 characters, so in a short message it
   visits ~L/26 positions and most start1 values never reach the notch at all: every one
   of those decodes identically. Measured 182 distinct of 676 at L=140 (3.71x), 130 at
   L=100 (5.20x), all exact duplicates.

   Exploited by SKIPPING keys whose start1 is not its class's canonical member. No
   reparameterisation is needed because the collapse is purely over start1: for a
   representative start1, ring1 ranging over all 26 already yields all 26 offsets. That
   also leaves the best.idx encoding untouched, so --polish / --ring-stride / -F keep
   working (a fused index would break all three).

   g_mid_rep_mask[(w1 * rotor_count + w2) * asize + start2] holds a 26-bit mask, bit s ==
   "start1 s is the canonical representative of its class". Indexed by the MIDDLE and
   RIGHT rotor plus start2 -- not by task -- because nothing else enters the stepping:
   the reflector, the left rotor and every ring setting are irrelevant, so a full
   wildcard's ~1000 tasks collapse onto at most 15x15 rotor pairs here. Null when the
   collapse is inactive -- it needs ring1 AND start1 both fully wildcarded, since with
   ring1 pinned each start1 carries a distinct offset and dropping any would lose keys.
   Read-only during the search; a plain global rather than a struct member or parameter,
   matching plug_fixed (see the aliasing note in the struct machine comments). */
static std::vector<uint32_t> g_mid_rep_store;
static const uint32_t * g_mid_rep_mask = nullptr;

/* First middle-notch firing index for (w1, w2, start1, start2), or -1 for "never
   within `limit` characters". Pure stepping: no ring setting, start0, reflector or
   plugboard enters a stepping decision, so those do not index this. */
static int mid_first_fire(int w1, int w2, int s1, int s2, int limit)
{
  int g1 = s1;
  int g2 = s2;
  for (int i = 0; i < limit; i++)
    {
      if (notch[w1][g1])
        return i;                    /* the firing that steps the left wheel */
      if (notch[w2][g2])
        g1 = mod26(1 + g1);
      g2 = mod26(1 + g2);
    }
  return -1;
}

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
  int crib_stop_at = -1;                /* --crib: alignment that survived at this key */
  const uint32_t * mid_row = nullptr;   /* §7.12 mask row for the current wheel order */
  bool key_skipped = false;             /* current key collapsed away (§7.12) */

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
                  /* §7.12 row for this wheel order: the collapse depends only on the
                     middle and right rotors, so this follows the wheel order, not the
                     task */
                  mid_row = g_mid_rep_mask
                    ? g_mid_rep_mask
                      + (static_cast<size_t>(t.w[1]) * rotor_count + t.w[2]) * asize
                    : nullptr;
                }

              r1 = range.r_min[0] + static_cast<int>(rflat / rc12);
              int rr = static_cast<int>(rflat % rc12);
              r2 = range.r_min[1] + rr / rc[2];
              /* ring2 can be a sparse set (--ring-stride); the range carries it as an
                 explicit list, so the decode is a lookup and needs no stride knowledge */
              r3 = range.r2_vals[rr % rc[2]];
              g1 = range.g_min[0] + static_cast<int>(gflat / gc12);
              int gg = static_cast<int>(gflat % gc12);
              g2 = range.g_min[1] + gg / gc[2];
              g3 = range.g_min[2] + gg % gc[2];

              /* Middle-wheel collapse (§7.12): skip start1 values that are not their
                 class's canonical member -- they decode byte-identically to one that is.
                 Latched per key rather than `continue`d here, because cur_key has
                 already advanced: a bare continue would let this key's remaining
                 restarts fall through and score against a stale machine. */
              key_skipped = (mid_row != nullptr)
                            && (((mid_row[g3] >> g2) & 1u) == 0);

              if (! key_skipped)
                {
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

                  /* --crib: reject keys the crib proves impossible at EVERY viable
                     alignment, before any scoring. rows[] is valid here (setup_mapping
                     just filled it) and the deduction reads nothing else, so this is a
                     pure per-key test and stays -T-deterministic. */
                  if (opt_crib_text)
                    crib_stop_at = g_crib_stop_shown = crib_first_stop(m);
                  if (opt_crib_text && (crib_stop_at < 0))
                    {
                      key_skipped = true;
                      /* Count only the key's FIRST work item. A key's restarts can
                         straddle a chunk boundary, in which case two workers each
                         see it as new and each evaluate it -- counting there would
                         make the total depend on -T. Every key has exactly one item
                         with restart == 0, so this is exact and thread-invariant. */
                      if (restart == 0)
                        g_crib_rejected.fetch_add(1, std::memory_order_relaxed);
                    }
                  else if (opt_crib_dump)
                    crib_dump(m, r1, r2, r3, g1, g2, g3);
                }
            }

          if (key_skipped)
            continue;

          /* Run one work unit: a restart climb, an --exhaust first-pair unit, or one scan
             score. Both hillclimb_one and exhaust_unit draw only from their own
             (keyidx, restart)/(keyidx) streams, so the result is independent of which thread
             runs the unit. For --exhaust the per-key units are the first-pair choices, so
             `restart` here indexes g_exhaust_firsts. The scan does not decode per key (the
             fused scorer reads each row once, straight from subst_array); the plaintext is
             materialised only for a new best, below. */
          double score;
          if (opt_hillclimb)
            score = opt_exhaust
                      ? exhaust_unit(m, keyidx, static_cast<size_t>(restart))
                      : (opt_crib_text ? crib_unit(m, keyidx, restart)
                                       : hillclimb_one(m, keyidx, restart));
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
                  memcpy(best.steckerbrett, m.steckerbrett, asize);   /* for --polish */
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
/* Decode a flat key index into `m`. Returns false if the key is collapsed away by the
   middle-wheel reduction (§7.12) -- it decodes byte-identically to a representative that
   IS searched -- in which case `m` is left untouched and no setup_mapping is done. The
   reconstruction callers (--polish, the --ring-stride refinement) always pass an index
   that survived the search, so they never see false. */
static bool key_to_machine(machine & m, size_t idx,
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
  /* see the matching comment in search_worker() */
  int r3 = range.r2_vals[rr % rc[2]];   /* see the matching comment in search_worker() */
  int g1 = range.g_min[0] + static_cast<int>(gflat / gc12);
  int gg = static_cast<int>(gflat % gc12);
  int g2 = range.g_min[1] + gg / gc[2];
  int g3 = range.g_min[2] + gg % gc[2];

  if (g_mid_rep_mask != nullptr)
    {
      const wheel_task & t = tasks[cur_wo];
      const uint32_t * row = g_mid_rep_mask
        + (static_cast<size_t>(t.w[1]) * rotor_count + t.w[2]) * asize;
      if (((row[g3] >> g2) & 1u) == 0)
        return false;                 /* collapsed away (§7.12) */
    }

  init_ring_grund(m, r1, r2, r3, g1, g2, g3);
  init_steckerbrett(m, opt_steckerbrett);
  setup_mapping(m, true);
  /* restore the start positions setup_mapping stepped, so mid-climb progress lines
     (finish_worker) echo the true config; rg6 carries them to the callers */
  init_ring_grund(m, r1, r2, r3, g1, g2, g3);
  rg6[0] = r1; rg6[1] = r2; rg6[2] = r3; rg6[3] = g1; rg6[4] = g2; rg6[5] = g3;
  return true;
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
          if (! key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                               rc12, gc12, cur_wo, rg6))
            continue;                 /* collapsed away (§7.12): a duplicate of a key
                                         this tier ranks anyway, so never shortlist it */
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
      /* shortlist entries all survived tier 1, so this never fires -- kept so a future
         change to the shortlist cannot silently score a collapsed key (§7.12) */
      if (! key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                           rc12, gc12, cur_wo, rg6))
        continue;

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
  size_t total_keys;            /* tasks.size() * rsize * gsize -- the INDEX space */
  size_t scored_keys;           /* keys actually scored (< total_keys under §7.12) */
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

  /* The LEFTMOST of the 3 stepping wheels (index 0) is the one place besides the
     M4 Greek wheel where a ring x start collapse is EXACT, not approximate, and
     unconditional -- not just "when it happens not to step" (some settings ARE
     merely unidentifiable per instance; this is a stronger, always-true fact).
     Nothing in setup_mapping() ever reads ringstellung[0] or grundstellung[0]
     except the final subst_array lookup mod26(g0-r0): wheel 0 has no notch
     check of its own (there is no wheel to its left to step), and its own
     stepping (driven entirely by wheel 1's notch) advances g0 by a pure
     additive constant untouched by r0 -- so shifting ring0 and start0 by the
     same delta leaves mod26(g0(i)-r0) identical at every character position i,
     for the ENTIRE message, regardless of length or how many times wheel 0
     steps (verified: -R/-g shifted together by 1..25 produced byte-identical
     decodes at 127 characters, vs. the middle/right wheels which visibly
     diverge after a handful of characters -- their own notch checks feed
     forward into further stepping, so they lack this property). Collapsing
     ring0's range to the single sentinel value 0 -- leaving grund0's 0..25
     range to enumerate the offsets directly, exactly like the M4 Greek wheel's
     offset_list above -- is therefore a lossless 26x reduction whenever BOTH
     are wildcarded (if only one is wildcarded there is no redundancy: every
     value of the wildcarded one is then a distinct, necessary offset). Reported
     ring position is always 'A' in this case, the direct analogue of the Greek
     wheel's unidentifiable ring. */
  if ((opt_ringstellung[0] == '.') && (opt_grundstellung[0] == '.'))
    ks.range.r_min[0] = ks.range.r_max[0] = 0;

  for (int i = 0; i < wheels; i++)
    {
      ks.rc[i] = ks.range.r_max[i] - ks.range.r_min[i] + 1;
      ks.gc[i] = ks.range.g_max[i] - ks.range.g_min[i] + 1;
    }

  /* --ring-stride K (archived/PERFORMANCE.md §7.11): the rightmost wheel lacks wheel 0's exact
     collapse above (its own notch feeds forward into further stepping, so a ring+start
     shift is only an approximation), but the corruption is small and grows smoothly, so
     testing only every Kth ring value -- {0, K, 2K, ...} -- still reliably lands near
     the truth; bruteforce()'s refinement pass afterward checks the skipped neighbours
     around the best coarse hit to recover the exact key. The sampled values become the
     range's explicit ring2 list, so rc[2] is just its length and the mixed-radix decode
     and parallel chunking carry the sparse set unchanged -- no stride arithmetic at any
     decode site. K=1 yields the full contiguous list, i.e. the unstrided search exactly.
     Validated by option parsing to fire only when opt_ringstellung[2]=='.' &&
     opt_grundstellung[2]=='.' (the same no-redundancy precondition as wheel 0's
     collapse). */
  unsigned int r2_mask = 0;
  for (int v = ks.range.r_min[2]; v <= ks.range.r_max[2]; v += opt_ring_stride)
    r2_mask |= 1u << v;
  set_ring2(ks.range, r2_mask);
  ks.rc[2] = ks.range.r2_n;

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

  /* Middle-wheel ring x start collapse (archived/PERFORMANCE.md §7.12). Only fires when ring1 and
     start1 are BOTH fully wildcarded: with ring1 pinned, each start1 carries a distinct
     offset1 and dropping any would lose real keys. Built per (middle, right) rotor pair
     -- the only things the stepping depends on besides the two start positions -- so a
     full wildcard's ~1000 tasks share at most 15x15 rows. Deterministic: the LOWEST
     start1 in each class is the representative, so the surviving key set (and hence the
     winner on a tie) does not depend on iteration order or thread count. */
  g_mid_rep_store.clear();
  g_mid_rep_mask = nullptr;
  /* --true-key ranks a specific key against the whole tier-1 keyspace; a collapsed key
     would simply be absent and never get a rank, so the diagnostic keeps the full sweep. */
  if ((ks.rc[1] == asize) && (ks.gc[1] == asize) && ! opt_true_key)
    {
      g_mid_rep_store.assign(static_cast<size_t>(rotor_count) * rotor_count * asize, 0);
      bool pair_done[rotor_count][rotor_count] = { { false } };
      for (const wheel_task & t : ks.tasks)
        {
          int w1 = t.w[1];
          int w2 = t.w[2];
          if (pair_done[w1][w2])
            continue;
          pair_done[w1][w2] = true;
          for (int s2 = 0; s2 < asize; s2++)
            {
              /* Class key: the first middle-notch firing index, or -1 for "never fires
                 in this message". A second firing needs ~26 further middle steps (~676
                 characters), so that one integer is the whole signature at any realistic
                 length -- verified against the binary in 7/7 configurations, including
                 the two-notch and double-step cases where a closed form fails (§7.12).
                 At most 26 classes, so a linear scan for "already seen" is both trivial
                 and obviously correct; -1 needs no special case. */
              int seen[asize];
              int nseen = 0;
              uint32_t mask = 0;
              for (int s1 = 0; s1 < asize; s1++)
                {
                  int f = mid_first_fire(w1, w2, s1, s2, textlength);
                  bool dup = false;
                  for (int k = 0; k < nseen; k++)
                    if (seen[k] == f)
                      {
                        dup = true;
                        break;
                      }
                  if (! dup)
                    {
                      seen[nseen++] = f;
                      mask |= 1u << s1;     /* lowest start1 of the class wins */
                    }
                }
              g_mid_rep_store[(static_cast<size_t>(w1) * rotor_count + w2) * asize + s2]
                = mask;
            }
        }
      g_mid_rep_mask = g_mid_rep_store.data();
    }

  ks.total_keys = ks.tasks.size() * ks.rsize * ks.gsize;

  /* Keys actually scored. The flat index space stays total_keys (the collapse skips
     during iteration rather than renumbering), so the diagnostic line would otherwise
     claim to have analysed keys it never touched. */
  ks.scored_keys = ks.total_keys;
  if (g_mid_rep_mask != nullptr)
    {
      ks.scored_keys = 0;
      for (const wheel_task & t : ks.tasks)
        {
          const uint32_t * row = g_mid_rep_mask
            + (static_cast<size_t>(t.w[1]) * rotor_count + t.w[2]) * asize;
          size_t reps = 0;
          for (int s2 = ks.range.g_min[2]; s2 <= ks.range.g_max[2]; s2++)
            reps += static_cast<size_t>(__builtin_popcount(row[s2]));
          ks.scored_keys += ks.rsize * static_cast<size_t>(ks.gc[0]) * reps;
        }
    }
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
  size_t scored_keys = ks.scored_keys;

  /* Echo the middle-wheel collapse (§7.12) when it is actually applied. Keyed on the
     mask itself rather than on a re-derived "ring1 and start1 wildcarded && !--true-key"
     test, so the line cannot drift from the real gate and claim a reduction that did not
     happen -- being truthful about what was searched is the whole point of printing it.
     That is also why it lives here rather than in show_settings(), which runs before
     build_key_space() has decided. Unlike --ring-stride this is LOSSLESS, so the wording
     reports a fact rather than a warning -- but it does explain a reported ring/start
     that differs from the key the message was enciphered with. */
  if ((g_mid_rep_mask != nullptr) && (scored_keys < total_keys))
    fprintf(stderr, "Collapse:   middle ring x start: %zu duplicate keys skipped "
            "(%.1fx);\n            reported ring/start may be an equivalent\n",
            total_keys - scored_keys,
            static_cast<double>(total_keys) / static_cast<double>(scored_keys));

  /* The "--ring-stride is not paying for itself" warning that used to live here is GONE,
     because the case it warned about no longer exists. It fired when the refinement's
     25 skipped ring2 values, re-searched over ring1 x start1 x start2, outweighed the
     26/K the coarse pass saved -- a real invocation (`-r A.. -g A..` at K=2 cost 1.46x
     MORE than not striding). Deriving the refinement's offsets instead of enumerating
     them shrank it from 25 x 130 x 26 to 25 x (start1 range), and that is now provably
     too small to lose:

       warn iff  total + refine > total/rc2 * 26,  refine = 25 * gc1 * (a small factor)
       total = T * rc2 with T = tasks*rc0*rc1*gc0*gc1*gc2, so gc1 cancels:
       warn iff  50 > tasks * rc0 * rc1 * gc0 * gc2 * (26 - rc2)

     Validation forces start2 wildcarded, so gc2 = 26, and rc2 <= 13 for any K >= 2 --
     the right-hand side is at least 26 * 13 = 338. The same keyspace that used to warn
     now analyses 363 keys against 676 unstrided, a 1.86x win. tests/run_tests.sh guards
     that inversion rather than the removed warning. */

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

  /* --polish and --ring-stride's refinement pass both need the winning board's full
     machine state reconstructed once from best.idx. Only the simple sweep records
     best.idx as key*restarts+restart, so both are guarded to that path (no -F, no
     --exhaust; enforced in option validation). Reconstructed once here and threaded
     through both steps in the right order -- rotor key first, then plugboard -- so
     neither silently reverts the other's improvement by re-deriving from the stale
     pre-refinement best.idx. */
  size_t extra_keys_analysed = 0;   /* --ring-stride's refinement pass, added below */
  if (best.found && (opt_polish || (opt_ring_stride > 1)))
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

      /* --ring-stride refinement (archived/PERFORMANCE.md §7.11): the coarse search only tested
         ring2 in {0, K, 2K, ...}; re-check the ring2 values it skipped around the best
         hit -- ALL of them by default, since a refinement ring2 value is orders of
         magnitude cheaper than a coarse one (see the window-width note below).
         ring0/start0 stay pinned to the coarse winner -- that pin is exact and
         ring2-independent (§7.10's unconditional offset collapse holds regardless of
         what ring2 is). ring1/start1 must NOT be pinned to the coarse winner: the coarse
         winner's ring1/start1 were only optimal for ITS (possibly off-by-one, corrupted)
         ring2 row, and a different ring2 nearby can have a different best-fitting
         ring1/start1 -- confirmed by manual testing, where pinning them missed the true
         key even though its ring2 fell inside the refinement window. So the refinement
         re-opens ring1/start1 to the ORIGINAL search's bounds (range.r_min/max[1],
         range.g_min/max[1] -- collapses back to a pin automatically if the caller had
         explicitly pinned ring1/start1 rather than wildcarding it), narrowing only ring2
         (to the skipped-neighbour window) and leaving start2 open, mirroring the
         measurement harness's per-candidate re-search (eval/ring_stride_probe.py). The
         window wraps at the 0/25 ring2 boundary and excludes the coarse winner itself
         (see the mask2 construction below): ring2 is circular, so a clamp would
         silently drop the wrapped-around neighbour, and the winner's own ring2 was
         already scored by the coarse pass over a SUPERSET of what phase 2 would search
         there. Because search_range carries ring2 as an explicit value list, that
         possibly-wrapped, centre-punctured set is one range and therefore ONE search --
         a small, self-contained reuse of search_worker (single-task, mostly-pinned
         key_space) so the skipped neighbours get the exact same treatment -- restarts,
         staged climb, everything -- as the coarse pass got. Reuses the already-
         precomputed subst_array (same wheel order, so no re-precompute); the local
         best_result keeps its (mini-range-relative) idx from leaking into the outer
         best.idx, which nothing reads again after this point. */
      if (opt_ring_stride > 1)
        {
          /* The refinement tests EVERY ring2 the coarse pass skipped -- all 25 of
             them, unconditionally. No window, no budget, no dependence on K.

             The earlier +/-K/2 window rested on the coarse winner landing within K/2 of
             the truth, which is exactly the assumption the measured stride-specific miss
             rate said fails. Refining every value drops the assumption: whatever ring2
             wins the coarse pass, all 26 are then tested exactly, under the winner's
             wheel order / reflector / ring0 / start0.

             This ran under a "25% of the coarse pass" budget for a while, on the theory
             that a keyspace narrow enough (single task AND start0 pinned) would see the
             refinement outcost the coarse pass. That was a ratio masquerading as a cost.
             The refinement is ONE pass over ONE task for the whole invocation. Its worst
             case is 25 * rc[1] * gc[1] * 26 keys, but do not price it from that bound:
             the case it describes -- ring1 and start1 BOTH wildcarded -- is the one where
             the offset band below applies, replacing the 26 x 26 (ring1, start1) pairs
             with 26 start1 x (2*mid_ring_window + 1) offsets = 130. So the realistic cost
             is 25 * 130 * 26 = 84500 index keys, and the middle-wheel collapse (7.12)
             then cuts what is actually scored to ~19000 at L=100. Measured on
             -r A.. -g A..: 18875 scored keys at BOTH K=2 and K=3, the refinement being
             K-independent. In the corner the budget was guarding, the whole run is
             988 keys against 676 unstrided -- microseconds. Trading predictable behaviour
             for that is a bad deal: a budget makes the same command do different work
             depending on an unrelated part of the keyspace, silently, with no way to
             adjust it. Cost is bounded and small; keep it fixed and explainable. */
          int center2 = m.ringstellung[2];

          /* Snapshot everything each segment pins (ring0/start0/wheel order/
             ring0/start0) BEFORE search_worker() touches m. The wheel order and
             reflector are NOT snapshotted here -- they come from tasks[cur_wo]
             verbatim, since m holds them already translated (see rtasks below).
             The plain-scan path leaves m's ringstellung/grundstellung in a stale,
             stepped state after scanning (a documented "lazy restore" perf
             optimisation below in search_worker() -- only the hillclimb path
             restores them per key), so re-reading m.ringstellung[0]/
             m.grundstellung[0] fresh from m between segments picks up whatever key
             the PRIOR segment's scan last touched, not the intended pin -- confirmed
             by a concrete miss during testing (start0 silently drifted by one
             wheel0 step between two searches, corrupting the second one's window
             even though the first found nothing better). The refinement is a single
             search now (the value list expresses the whole set at once), so only the
             ordering matters. */
          int fixed_ring0 = m.ringstellung[0];
          int fixed_start0 = m.grundstellung[0];

          /* THE OFFSETS ARE DERIVED, NOT SEARCHED (refinement.md).

             The substitution consumes a_i = o0 + left(i), b_i = o1 + mid(i) and
             c_i = o2 + i, where o_w = start_w - ring_w and left/mid are the wheels'
             cumulative step counts. Two things follow.

             c_i has no schedule term, so the right wheel's whole contribution is a
             function of o2 alone: every candidate carries the coarse winner's o2 exactly,
             start2 = ring2 + o2. Measured 0 losses in 600 paired trials.

             b_i and a_i DO have one, and it is not a small perturbation. Moving start2 by
             delta moves the turnover by delta MODULO 26, so it can carry a turnover across
             the START of the message and change the step count for the whole message
             rather than for a delta-length window. The offset then absorbs that difference
             -- which is why the coarse winner is not "the truth with a wrong ring2" but the
             truth with a wrong ring2 AND a compensating middle offset, and why it still
             decodes most of the message. Measured case: step positions [1,27,53] against
             [26,52], counts differing by 1 on 58 of 60 positions, offsets 7 against 8
             cancelling exactly, 58 of 60 characters correct.

             Both schedules follow from the two keys alone, with no knowledge of the truth,
             so the correction is COMPUTED: o1 = o1_coarse + (mid_coarse - mid_cand). That
             replaces the old +-mid_ring_window band -- a fixed guess at a quantity that can
             be derived -- and takes the candidate set from 25 x 130 x 26 = 84500 to
             25 x 26. The band's bound of 2 still holds (it is where mid_ring_window came
             from) but nothing here depends on it: the delta is measured, not assumed, which
             is what makes this correct for two-notch right wheels and straddled double
             steps rather than merely usually right.

             The LEFT wheel gets the same treatment, ungated: left() counts double steps, a
             ring2 shift moves those too, and one near either end of the message can be
             carried in or out of it. Its delta set is computed from the same schedule walk
             and is {0} whenever the schedules agree, so the derivation self-gates and an
             explicit "does the left wheel step?" condition would be one more thing to get
             wrong for no saving. (The old code pinned ring0/start0 outright, citing §7.10 --
             but §7.10 is the DEGENERACY, that shifting ring0 and start0 together is
             decode-identical, which is not the same claim as pinning o0 across a ring2
             change.) */
          int coarse_off1 = mod26(m.grundstellung[1] - m.ringstellung[1]);
          int coarse_off2 = mod26(m.grundstellung[2] - m.ringstellung[2]);
          int coarse_g1 = m.grundstellung[1];
          int coarse_g2 = m.grundstellung[2];
          /* TRANSLATED rotor indices: notch[] is indexed that way, unlike the §7.12 mask
             below, which is built and read by RAW index. */
          int mid_wheel = m.walzenlage[1], right_wheel = m.walzenlage[2];

          /* Derive an offset only where the caller left the freedom to. With ring1 pinned
             -- which includes the tool's own default -r AA. -- each start1 in the sweep
             already carries a determined offset start1 - ring1, the sweep covers every one
             of them, and deriving would override a constraint the caller stated. Wheel 0
             is the same rule: shift start0 when it is free (the usual case, since §7.10
             collapses ring0 to a sentinel and lets start0 enumerate the offsets), else
             shift ring0, else leave o0 alone. */
          bool derive_ring1 = (rc[1] == asize);
          /* Width of the band placed around each derived offset (see widen_deltas).
             MEASURED TO BUY NOTHING, so the shipped value is 0 -- the pure derivation.
             The band was built for refinement.md §7.2, the one failure the derivation
             cannot correct: a coarse winner whose own o1 is wrong for scoring rather than
             schedule reasons. Over 360 paired end-to-end trials it changed not a single
             recovery, because every key the derived set "lost" against the old enumerated
             band turned out to be one the EXHAUSTIVE K=1 search also fails -- a scoring
             failure, where the truth is not the top-scoring key and no search shape can
             help. ENIGMA_REFINE_BAND keeps it measurable without a rebuild. */
          int refine_band = 0;
          if (const char * bp = getenv("ENIGMA_REFINE_BAND"))
            refine_band = atoi(bp);
          if (refine_band < 0)
            refine_band = 0;
          bool shift_start0 = (gc[0] == asize);
          bool shift_ring0 = (! shift_start0) && (rc[0] == asize);

          std::vector<unsigned short> sched_c_mid(static_cast<size_t>(textlength));
          std::vector<unsigned short> sched_c_left(static_cast<size_t>(textlength));
          std::vector<unsigned short> sched_k_mid(static_cast<size_t>(textlength));
          std::vector<unsigned short> sched_k_left(static_cast<size_t>(textlength));
          step_counts(mid_wheel, right_wheel, coarse_g1, coarse_g2,
                      sched_c_mid.data(), sched_c_left.data());

          /* Every ring2 except the coarse winner. The winner needs no retest: the
             coarse pass already scored that exact ring2, and phase 2 pins ring0/start0
             to the winner's own values while opening ring1/start1/start2 to the same
             ranges phase 1 used -- so for ring2 == centre, phase 2's space is a SUBSET
             of what phase 1 already searched there, and re-running it can only reproduce
             the same winning score. (Caveat, deliberate: under -c the per-restart RNG
             seeds differ between the two searches, so a retest could stumble on a better
             plugboard. That is extra plugboard restarts smuggled into a rotor-key
             refinement, not refinement work; -R is the documented lever for that.)

             A full sweep also removes a whole class of subtlety this code used to carry.
             When it was a window it had to WRAP at the 0/25 boundary rather than clamp
             -- ring2 is circular, so a coarse winner at A(0) with the true ring2 at Z(25)
             was a documented-recoverable case a clamped window silently never checked
             (confirmed by a concrete miss during testing). With every value in the set
             there is no edge to fall off. search_range carries ring2 as an explicit value
             list, so the punctured set goes in as-is: one mask, one search. */
          unsigned int mask2 = ((1u << asize) - 1u) & ~(1u << center2);

          /* MEASUREMENT-ONLY override (ENIGMA_REFINE_WINDOW=k, unset/0/>=13 = off):
             restrict the refinement to the ring2 values within circular distance k of
             the coarse winner, so the width the full sweep replaced can be re-measured
             without rebuilding. This is what eval/ring_stride_window_probe.py sweeps;
             the shipped default is the full punctured set above, and with the variable
             unset this loop does not run. Circular by construction (the mask is a set,
             not an interval), so the wrap subtlety a clamped window used to have cannot
             come back through it. */
          if (const char * wp = getenv("ENIGMA_REFINE_WINDOW"))
            {
              int wk = atoi(wp);
              if ((wk > 0) && (wk < asize / 2))
                for (int v = 0; v < asize; v++)
                  {
                    int d = abs(v - center2);
                    if (d > asize - d)
                      d = asize - d;
                    if (d > wk)
                      mask2 &= ~(1u << v);
                  }
            }

          /* Reuse the winning task VERBATIM rather than rebuilding one from the
             machine's fields. wheel_task carries RAW wheel/reflector numbers, which
             init_walzen() translates on the way into a machine -- in Norway mode it adds
             norway_rotor_base / norway_reflector_index. Rebuilding from m.walzenlage[]
             therefore hands search_worker already-translated values that it translates a
             SECOND time, so the refinement searched the wrong rotors entirely; and the
             §7.12 collapse mask, which is built and looked up by raw index, hit a
             never-built all-zero row and skipped every key, leaving the refinement
             empty-handed. Both were invisible outside Norway mode, where raw ==
             translated. cur_wo was set by the key_to_machine() call above. */
          std::vector<wheel_task> rtasks(1, tasks[cur_wo]);
          search_range rrange;
          /* Every candidate pins all six positions, so each sub-search is a single key:
             the derived (ring2, start2) and (ring1, start1) are DIAGONALS, and
             search_range holds rectangles only. */
          rrange.r_min[2] = 0;                /* bounds unused: r2_vals below decodes */
          rrange.r_max[2] = asize - 1;
          int rrc[wheels] = { 1, 1, 1 };
          int rgc[wheels] = { 1, 1, 1 };
          size_t rrsize = 1;
          size_t rgsize = 1;
          size_t rwork = restarts_par;

          /* BUILD THE DERIVED CANDIDATES. One per (skipped ring2) x (start1 in the
             caller's range) x (delta the middle schedule actually drifted) x (ditto the
             left wheel's). start1 values the §7.12 collapse would skip are dropped here
             rather than handed to search_worker to reject, so the count below is what is
             actually scored -- and a class member and its representative share a schedule,
             hence the same derived offset, so dropping them loses nothing. */
          struct refine_cand { unsigned char r0, g0, r1, g1, r2, g2; };
          std::vector<refine_cand> cands;
          const uint32_t * mrow = nullptr;
          if (g_mid_rep_mask != nullptr)
            mrow = g_mid_rep_mask + (static_cast<size_t>(rtasks[0].w[1]) * rotor_count
                                     + rtasks[0].w[2]) * asize;
          for (int v = 0; v < asize; v++)
            {
              if (! ((mask2 >> v) & 1u))
                continue;
              int g2 = mod26(v + coarse_off2);
              for (int g1 = range.g_min[1]; g1 <= range.g_max[1]; g1++)
                {
                  if ((mrow != nullptr) && ! ((mrow[g2] >> g1) & 1u))
                    continue;
                  step_counts(mid_wheel, right_wheel, g1, g2,
                              sched_k_mid.data(), sched_k_left.data());
                  int dmid[asize], dleft[asize];
                  int nmid = derive_ring1
                    ? step_deltas(sched_c_mid.data(), sched_k_mid.data(), dmid, asize)
                    : 1;                      /* ring1 pinned: nothing to derive */
                  /* Widen each derived delta by +-refine_band. The derivation corrects the
                     SCHEDULE term exactly, but the coarse winner's own o1 can also be off
                     for scoring reasons -- the argmax on a partly-garbled decode need not
                     be the truth's middle setting -- and no schedule computation can see
                     that. Off by default: measured to change no recovery at all (see
                     refine_band above). */
                  if (derive_ring1 && (refine_band > 0))
                    nmid = widen_deltas(dmid, nmid, refine_band, asize);
                  int nleft = (shift_start0 || shift_ring0)
                    ? step_deltas(sched_c_left.data(), sched_k_left.data(), dleft, asize)
                    : 1;                      /* o0 fixed by the caller */
                  for (int a = 0; a < nmid; a++)
                    for (int b = 0; b < nleft; b++)
                      {
                        refine_cand c;
                        c.r1 = static_cast<unsigned char>
                          (derive_ring1 ? mod26_full(g1 - (coarse_off1 + dmid[a]))
                                        : range.r_min[1]);
                        c.g1 = static_cast<unsigned char>(g1);
                        c.r2 = static_cast<unsigned char>(v);
                        c.g2 = static_cast<unsigned char>(g2);
                        int d0 = (nleft > 1 || (shift_start0 || shift_ring0)) ? dleft[b] : 0;
                        c.r0 = static_cast<unsigned char>
                          (shift_ring0 ? mod26_full(fixed_ring0 - d0) : fixed_ring0);
                        c.g0 = static_cast<unsigned char>
                          (shift_start0 ? mod26_full(fixed_start0 + d0) : fixed_start0);
                        cands.push_back(c);
                      }
                }
            }
          /* Keys the refinement actually SCORES -- now simply the candidate count, since
             every candidate is one fully-pinned key and the §7.12 collapse was applied
             while building the list rather than left for search_worker to reject. The
             enumerated-vs-scored gap the old accounting had to correct for (439400 against
             106600 on a fully wildcarded keyspace) is gone with the enumeration. */
          extra_keys_analysed = cands.size();

          best_result rbest;
          /* Carry the display high-water mark into the refinement. Its best_result is a
             fresh one (so its mini-range-relative idx cannot leak into the outer best),
             which would otherwise restart the progress ladder from score_min and echo a
             full run of lines that do NOT beat what the coarse pass already found --
             ending on a line WORSE than the answer actually being returned. Since the
             last progress line is exactly what a reader takes for the result, that reads
             as the tool regressing. Seeding from best.shown means the refinement speaks
             only when it genuinely improves on what was already displayed. Display-only:
             the merge below still compares against best.score. */
          rbest.shown.store(best.shown.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
          /* The header was already printed by the coarse pass; a fresh best_result would
             otherwise re-emit it mid-run. */
          rbest.header_shown = true;
          /* Point the climb's accepted-move echo at rbest too. report_climb_progress()
             reads the g_progress global, which still addresses the OUTER best -- so the
             climb echo and this search's merge would gate on two independent `shown`
             fields, and a single improvement would print TWICE (once from each, the
             second dragging the header with it). One gate, one line. Safe to swap here:
             no workers are running at this point, and it is restored below. */
          best_result * save_progress = g_progress;
          g_progress = &rbest;
          int rnthreads = opt_threads;
          if (rwork < static_cast<size_t>(rnthreads))
            rnthreads = static_cast<int>(rwork);
          if (rnthreads < 1)
            rnthreads = 1;
          size_t rchunk = rwork / (static_cast<size_t>(rnthreads) * 16);
          if (rchunk < 1)
            rchunk = 1;
          /* One sub-search per derived candidate, sharing a single rbest. Everything each
             pins comes from the candidate list, never re-read from m: on the plain-scan
             path search_worker leaves m's ring/start in a stale stepped state (the
             documented lazy restore), which is how an earlier multi-search version here
             silently corrupted its own second pass. Ascending candidate order and a
             strictly-greater test keep the winner deterministic. */
          refine_cand won = cands.empty() ? refine_cand{0, 0, 0, 0, 0, 0} : cands[0];
          double prev_score = rbest.score;
          for (size_t i = 0; i < cands.size(); i++)
            {
              const refine_cand & c = cands[i];
              rrange.r_min[0] = rrange.r_max[0] = c.r0;
              rrange.g_min[0] = rrange.g_max[0] = c.g0;
              rrange.r_min[1] = rrange.r_max[1] = c.r1;
              rrange.g_min[1] = rrange.g_max[1] = c.g1;
              rrange.g_min[2] = rrange.g_max[2] = c.g2;
              set_ring2(rrange, 1u << c.r2);
              std::atomic<size_t> rnext_key{0};
              run_parallel(rnthreads, [&](int t)
                { search_worker(*machines[t], rtasks, rrange, rrc, rgc, m.subst_array,
                                rrsize, rgsize, rnext_key, rchunk, restarts_par, rbest); });
              /* rbest.idx is relative to whichever sub-search produced it, so remember the
                 candidate pinned when the score last improved; the reconstruction below
                 re-pins rrange to it. */
              if (rbest.found && (rbest.score > prev_score))
                {
                  prev_score = rbest.score;
                  won = c;
                }
            }
          rrange.r_min[0] = rrange.r_max[0] = won.r0;
          rrange.g_min[0] = rrange.g_max[0] = won.g0;
          rrange.r_min[1] = rrange.r_max[1] = won.r1;
          rrange.g_min[1] = rrange.g_max[1] = won.g1;
          rrange.g_min[2] = rrange.g_max[2] = won.g2;
          set_ring2(rrange, 1u << won.r2);

          g_progress = save_progress;
          /* Carry the refinement's display high-water mark back, so the merge echo below
             does not reprint a line rbest already showed during the search. */
          if (rbest.shown.load(std::memory_order_relaxed)
              > best.shown.load(std::memory_order_relaxed))
            best.shown.store(rbest.shown.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);

          if (rbest.found && (rbest.score > best.score))
            {
              size_t rrg = rrsize * rgsize;
              size_t rrc12 = static_cast<size_t>(rrc[1]) * rrc[2];
              size_t rgc12 = static_cast<size_t>(rgc[1]) * rgc[2];
              size_t rcur_wo = static_cast<size_t>(-1);
              int rrg6[6];
              key_to_machine(m, rbest.idx / restarts_par, rtasks, rrange, rrc, rgc,
                             m.subst_array, rrg, rgsize, rrc12, rgc12, rcur_wo, rrg6);
              for (int i = 0; i < asize; i++)
                m.steckerbrett[i] = rbest.steckerbrett[i];
              best.score = rbest.score;
              memcpy(best.plaintext, rbest.plaintext, textlength + 1);
              memcpy(best.steckerbrett, rbest.steckerbrett, asize);
              if (rbest.score > best.shown.load(std::memory_order_relaxed))
                {
                  best.shown.store(rbest.score, std::memory_order_relaxed);
                  progress_line(best, m, rbest.score);
                }
            }
        }

      /* Guarded by opt_polish. This block shares its enclosing `if` with the
         --ring-stride refinement above (both need best.idx reconstructed once), and
         used to run whenever EITHER was requested -- so a --ring-stride run got the
         plugboard finisher too, including with no -c at all. That is not a cosmetic
         leak: with no -c the tool must not touch the plugboard, and the finisher was
         adding spurious plugs to a board supplied with -s, corrupting the decrypt and
         lowering the score-vs-truth on exactly the runs 7.11 measured. It also charged
         --ring-stride for a cost it never asked for. --polish already requires -c
         (validated), so the flag is the whole guard needed. */
      if (opt_polish)
        {
        int save_gf = opt_cascade;
        int save_gf3 = opt_cascade3;
        double save_gate = opt_cascade_gate;
        opt_cascade = 1;
        opt_cascade3 = 1;   /* --polish also enables the 3-ply escalation */
        opt_cascade_gate = score_min;   /* unconditional cascade on the one best board */
        /* Cap the finishing climb at the TARGET-STAGE cap, not asize/2 (uncapped) -- like
           every other finisher/quench in the tool (the staged tail at opt_stages[last].cap,
           the -A quench). An uncapped finish let gainfix-best add spurious plugs 11..cap that
           raise the noisy short-message quad score while hurting the truth (the over-plugging
           avenue of the saturation exact-loss, archived/PERFORMANCE.md 4.10). */
        int fin_cap = opt_stages[opt_nstages - 1].cap;
        double s = hillclimb<false>(m, fin_cap);
        opt_cascade = save_gf;
        opt_cascade3 = save_gf3;
        opt_cascade_gate = save_gate;
        /* Monotonic by construction: replace the best board ONLY when the finish scores
           strictly higher, so gainfix-best never returns a worse-scoring board than the
           search already found (a truth-vs-score chase at the information floor is a
           separate matter -- unfixable by a score-only rule; see archived/PERFORMANCE.md 4.10). */
        if (s > best.score)
          {
            best.score = s;
            decode(m);
            memcpy(best.plaintext, m.plaintext, textlength + 1);
            /* Echo the improved board: without this the finisher silently replaced the
               winner, so the last progress line the user saw showed the PRE-finisher
               score/wheels/plugboard while stdout held a different (better) decrypt.
               The search threads are joined here and key_to_machine restored the true
               start positions, so m holds the correct config to display. Guarded by
               best.shown like every other echo, so a line already showing this score is
               not repeated; display-only, so -T-determinism is untouched. */
            if (s > best.shown.load(std::memory_order_relaxed))
              {
                best.shown.store(s, std::memory_order_relaxed);
                progress_line(best, m, s);
              }
          }
        }
    }

  /* diagnostics: every rotor combination is analysed (brute force has no early
     exit), and each worker counted the plugboards it scored -- sum them up */
  g_keys_analysed = scored_keys + extra_keys_analysed;
  g_plugboards_scored = 0;
  for (int t = 0; t < nthreads; t++)
    g_plugboards_scored += machines[t]->plugboards_scored;

  for (int t = 0; t < nthreads; t++)
    delete machines[t];
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
  fprintf(out, "Enigma cipher tool version 2.1.0\n");
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
  fprintf(out, "  %-24s %s\n", "--no-plug LETTERS",
          "Letters known to carry NO cable: the climb leaves");
  fprintf(out, "  %-24s %s\n", "", "them unplugged, as -s holds its pairs plugged");
  fprintf(out, "  %-24s %s\n", "", "(needs -c) [none]");
  fprintf(out, "  %-24s %s\n", "-n, --norway",
          "Norway Enigma: reflector N and wheels (1-5)");
  fprintf(out, "  %-24s %s\n", "-4, --m4", "M4 (4-rotor naval) mode. -u selects the thin");
  fprintf(out, "  %-24s %s\n", "", "reflector b/c; -w/-r/-g take 4 chars (Greek");
  fprintf(out, "  %-24s %s\n", "", "wheel/ring/start first)");
  fprintf(out, "  %-24s %s\n", "-c, --climb",
          "Hill-climb the plugboard for each candidate key.");
  fprintf(out, "  %-24s %s\n", "", "The climb rule is STEEPEST ASCENT by default:");
  fprintf(out, "  %-24s %s\n", "", "score all 325 plug toggles, apply the single");
  fprintf(out, "  %-24s %s\n", "", "best, repeat to convergence (see -J) [off]");
  fprintf(out, "  %-24s %s\n", "-R, --restarts N",
          "Random restart attempts: 0 = one deterministic");
  fprintf(out, "  %-24s %s\n", "", "climb; N = N kicked climbs, keep best [0]");
  fprintf(out, "  %-24s %s\n", "-S, --score schedule",
          "Staged plugboard climb: <letter><cap> tokens,");
  fprintf(out, "  %-24s %s\n", "", "models i/m/b/t/q/a/f (number caps plug pairs; last");
  fprintf(out, "  %-24s %s\n", "", "stage is the target/ranking model). E.g. --score");
  fprintf(out, "  %-24s %s\n", "", "m4f10 (mono pre-pass then fused, both capped).");
  fprintf(out, "  %-24s %s\n", "", "Without -c only the target model is used (to rank).");
  fprintf(out, "  %-24s %s\n", "-l, --language language",
          "Scoring language: english/german/danish/french/");
  fprintf(out, "  %-24s %s\n", "", "swedish/finnish/icelandic/polish/spanish, or");
  fprintf(out, "  %-24s %s\n", "", "wehrmacht (telegraphic military German -- X as");
  fprintf(out, "  %-24s %s\n", "", "word separator, Q for ch, spelled-out numbers;");
  fprintf(out, "  %-24s %s\n", "", "for real WWII traffic, NOT for prose German);");
  fprintf(out, "  %-24s %s\n", "", "required for -m/-b/-t/-q/-a/-f (no default); not -i");
  fprintf(out, "  %-24s %s\n", "-i, --ic",
          "Index of coincidence (IC); needs no -l [default]");
  fprintf(out, "  %-24s %s\n", "-m, --mono", "Monogram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-b, --bi", "Bigram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-t, --tri", "Trigram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-q, --quad", "Quadgram statistics for the plaintext score");
  fprintf(out, "  %-24s %s\n", "-a, --weighted",
          "Weighted all-order score (log-linear mix of");
  fprintf(out, "  %-24s %s\n", "", "quad/tri/bi/mono); sharper on short messages");
  fprintf(out, "  %-24s %s\n", "-f, --fused",
          "Weighted all-order score PLUS the index of");
  fprintf(out, "  %-24s %s\n", "", "coincidence. IC is language-independent and the");
  fprintf(out, "  %-24s %s\n", "", "plugboard cannot game it, so it adds gradient");
  fprintf(out, "  %-24s %s\n", "", "where the n-gram surface is flat -- a better");
  fprintf(out, "  %-24s %s\n", "", "CLIMB, not better discrimination. Recommended:");
  fprintf(out, "  %-24s %s\n", "", "-c -S m4f10 -J --polish");
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
          "Change -c's climb rule to FIRST-IMPROVEMENT in");
  fprintf(out, "  %-24s %s\n", "", "best-first order: apply the first improving");
  fprintf(out, "  %-24s %s\n", "", "toggle instead of scanning for the best, ~2.8x");
  fprintf(out, "  %-24s %s\n", "", "cheaper per climb -- so pair it with a larger -R.");
  fprintf(out, "  %-24s %s\n", "", "Wins the realistic ~10-plug case, may lose with");
  fprintf(out, "  %-24s %s\n", "", "few plugs (needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "-M, --cap-target",
          "Make the plug cap a strict descent target: only");
  fprintf(out, "  %-24s %s\n", "", "merge/remove at/over the cap; pair with a tight");
  fprintf(out, "  %-24s %s\n", "", "-S cap (needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "--polish",
          "Gain cascade once on the best board, plus a deeper");
  fprintf(out, "  %-24s %s\n", "", "3-ply cascade for 3-plug tangles. The recommended");
  fprintf(out, "  %-24s %s\n", "", "finisher: it runs once after all restarts, so its");
  fprintf(out, "  %-24s %s\n", "", "cost is fixed and negligible at a high -R, but is");
  fprintf(out, "  %-24s %s\n", "", "a few % of a low-R run (needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "--ring-stride K",
          "Sparse ring sampling for the rightmost wheel:");
  fprintf(out, "  %-24s %s\n", "", "test only every Kth ring value, then refine every");
  fprintf(out, "  %-24s %s\n", "", "skipped one (needs -r and -g to wildcard that");
  fprintf(out, "  %-24s %s\n", "", "wheel; no -F/--exhaust) [1..26, 1 = off].");
  fprintf(out, "  %-24s %s\n", "", "K=2/K=3 analyse 1.9x/2.6x fewer keys for ~0.5-2pp");
  fprintf(out, "  %-24s %s\n", "", "of exact recovery on telegraphic German; K=3 is");
  fprintf(out, "  %-24s %s\n", "", "the better pick. K>=5 is not recommended (2-8pp).");
  fprintf(out, "  %-24s %s\n", "", "Cost is flat over K=13..25 (2 coarse values each),");
  fprintf(out, "  %-24s %s\n", "", "so nothing above 13 pays until K=26, which is 15%");
  fprintf(out, "  %-24s %s\n", "", "cheaper than 13 but loses ~10pp. Still an");
  fprintf(out, "  %-24s %s\n", "", "APPROXIMATION (archived/PERFORMANCE.md 7.11)");
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
  fprintf(out, "  %-24s %s\n", "--full-text",
          "Print the whole decrypted message with each");
  fprintf(out, "  %-24s %s\n", "", "progress line, not just the first 19 letters [off]");
  fprintf(out, "  %-24s %s\n", "--crib TEXT",
          "Known plaintext at --crib-at: rotor settings that");
  fprintf(out, "  %-24s %s\n", "", "cannot produce it are rejected unscored. Needs");
  fprintf(out, "  %-24s %s\n", "", "--crib-at; no -F/--exhaust/--ring-stride/-A [off]");
  fprintf(out, "  %-24s %s\n", "--crib-at N",
          "Where the crib sits (0-based); omit to sweep every");
  fprintf(out, "  %-24s %s\n", "", "alignment -- but rejections multiply across");
  fprintf(out, "  %-24s %s\n", "", "them, so a swept crib needs 16+ letters");
  fprintf(out, "\n");
  fprintf(out, "Non-recommended options (opt-in; dominated, ablation, or only\n");
  fprintf(out, "situational -- not proven to beat the recommended knobs above):\n");
  fprintf(out, "  %-24s %s\n", "-F, --prefilter N[%]",
          "Key pre-filter: rank by a cheap IC climb, then");
  fprintf(out, "  %-24s %s\n", "", "run the full -c climb on only the top N keys, or");
  fprintf(out, "  %-24s %s\n", "", "top N% of the keyspace (needs -c) [off];");
  fprintf(out, "  %-24s %s\n", "", "long messages only, weak on short");
  fprintf(out, "  %-24s %s\n", "--no-repair",
          "Disable the 2-plug re-pair barrier cross");
  fprintf(out, "  %-24s %s\n", "", "(ablation/measurement flag; needs -c) [off]");
  fprintf(out, "  %-24s %s\n", "--cascade[=GATE]",
          "Quadgram-gain 2-ply directed-repair cascade at");
  fprintf(out, "  %-24s %s\n", "", "convergence; GATE = near-solution per-symbol");
  fprintf(out, "  %-24s %s\n", "", "score threshold (needs -c; quad-only) [off];");
  fprintf(out, "  %-24s %s\n", "", "prefer --polish (kept for -F/--exhaust)");
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
  fprintf(out, "  %-24s %s\n", "--dump-all",
          "With -c, print the full setting (rotor key, score,");
  fprintf(out, "  %-24s %s\n", "", "plugboard) of every key x restart (verbose) [off]");
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
  fprintf(out, "  greedy: -c -J --polish --score m4f10 --random 10 -R 40 -f -l english\n");
  fprintf(out, "  SA:     -c -A 12000 --score a10 -R 12 -a -l english\n");
  fprintf(out, "-f (weighted all-order + IC) is the recommended scoring model; -R is the main\n");
  fprintf(out, "quality dial (use -T to keep it cheap); the polisher is a small bump\n");
  fprintf(out, "on top, not a substitute for more restarts.\n");
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
      "weighted all-orders", "weighted all-orders + IC" };

  fprintf(stderr, "Ciphertext: %d letters\n", textlength);

  fprintf(stderr, "Scoring:    %s", scoring_name[opt_scoring]);
  if (opt_scoring == 0)
    fprintf(stderr, " (language-independent)\n");
  else
    /* The data directory is an arbitrary path, so it gets its own continuation
       line rather than a trailing clause that would push the line past 80. */
    fprintf(stderr, " (language: %s)\n            n-gram files in %s\n",
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
      int free_letters = asize - 2 * fixed_pairs
                         - static_cast<int>(strlen(opt_no_plug));
      double combos = disjoint_pair_combinations(free_letters, opt_exhaust);
      fprintf(stderr, "            partial exhaustion: %d forced pair(s) on top of %d "
              "-s pair(s) (%.0f combinations)\n", opt_exhaust, fixed_pairs, combos);
    }
  if (opt_hillclimb && opt_capmerge)
    fprintf(stderr, "            cap as strict descent target (merge/remove only at cap)\n");
  if (opt_hillclimb && opt_no_repair)
    fprintf(stderr, "            2-plug re-pair barrier cross disabled (--no-repair)\n");
  if (opt_hillclimb && opt_cascade)
    fprintf(stderr, "            quadgram-gain directed-repair cascade at convergence "
            "(--cascade, near-solution gate %.2f)\n", opt_cascade_gate);
  if (opt_hillclimb && opt_polish)
    fprintf(stderr, "            quadgram-gain 2-ply+3-ply cascade once on the best board "
            "(--polish)\n");
  if (opt_hillclimb && opt_firstimprove)
    fprintf(stderr, "            first-improvement climb%s\n",
            opt_dynorder ? " (dynamic move order)" : "");
  if (opt_hillclimb && ((opt_anneal > 0) || (opt_restarts >= 1)))
    fprintf(stderr, "            seed: %llu\n",
            static_cast<unsigned long long>(opt_seed));

  if (opt_prefilter_frac > 0.0)
    fprintf(stderr, "Pre-filter: top %g%% of keys\n", opt_prefilter_frac * 100.0);
  else if (opt_prefilter > 0)
    fprintf(stderr, "Pre-filter: top %d keys\n", opt_prefilter);

  /* --ring-stride makes the rotor-key search APPROXIMATE (it can miss the true key --
     ~10pp of exact recovery at K=2 on telegraphic German, archived/PERFORMANCE.md §7.11), so a
     run that used it must say so: otherwise a saved log is indistinguishable from an
     exhaustive one. Every other search-affecting option is echoed here; this was the
     only silent one. */
  if (opt_ring_stride > 1)
    fprintf(stderr, "Stride:     rightmost ring every %d (--ring-stride), then refine "
            "skipped\n            rings; approximate, may miss the true key\n",
            opt_ring_stride);

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
  if (opt_no_plug[0])
    fprintf(stderr, "            %s known unplugged (--no-plug)\n", opt_no_plug);
  if (opt_crib_text)
    {
      if (opt_crib_at >= 0)
        fprintf(stderr, "Crib:       %s at position %d\n",
                opt_crib_text, opt_crib_at);
      else
        fprintf(stderr, "Crib:       %s, sweeping every alignment\n", opt_crib_text);
    }
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
  opt_no_plug = "";
  opt_crib_text = nullptr;
  opt_crib_at = -1;
  opt_crib_dump = false;
  opt_language = 0;   /* no default; required for n-gram scoring (-m/-b/-t/-q/-a) */
  opt_datadir = 0;    /* resolved after parsing: -d > $ENIGMA_DATA > "ngrams" */
  opt_plaintext = 0;
  opt_maxwheel = 5;
  opt_hillclimb = 0;
  opt_firstimprove = 0;
  opt_dynorder = 0;
  opt_capmerge = 0;
  opt_no_repair = 0;
  opt_cascade = 0;
  opt_cascade_gate = -4.9;   /* English-quad-calibrated near-solution gate (tunable) */
  opt_cascade3 = 0;
  opt_polish = 0;
  opt_crib_file = nullptr;
  opt_crib_weight = 0.5;
  opt_crib = 0;
  g_cribs.clear();
  opt_dump_all = false;
  opt_full_text = false;
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
  opt_ring_stride = 1;

  /* get arguments */

  /* Long-only option identifiers (no short form): values above the byte range so they
     never collide with a short flag char. --random and --exhaust are the seed-pipeline
     options introduced in REDESIGN Part B. */
  enum { OPT_RANDOM = 256, OPT_EXHAUST, OPT_TRUEKEY, OPT_NO_REPAIR, OPT_CASCADE,
         OPT_POLISH, OPT_CRIB, OPT_CRIBWEIGHT, OPT_DUMPALL, OPT_RINGSTRIDE,
         OPT_NOPLUG, OPT_FULLTEXT, OPT_CRIBTEXT, OPT_CRIBAT, OPT_CRIBDUMP };

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
      { "dynamic-order",  no_argument,       nullptr, 'J' },
      { "cap-target",     no_argument,       nullptr, 'M' },
      { "ic",             no_argument,       nullptr, 'i' },
      { "mono",           no_argument,       nullptr, 'm' },
      { "bi",             no_argument,       nullptr, 'b' },
      { "tri",            no_argument,       nullptr, 't' },
      { "quad",           no_argument,       nullptr, 'q' },
      { "weighted",       no_argument,       nullptr, 'a' },
      { "fused",          no_argument,       nullptr, 'f' },
      { "climb",          no_argument,       nullptr, 'c' },
      { "norway",         no_argument,       nullptr, 'n' },
      { "m4",             no_argument,       nullptr, '4' },
      { "version",        no_argument,       nullptr, 'v' },
      { "help",           no_argument,       nullptr, 'h' },
      { "random",         required_argument, nullptr, OPT_RANDOM  },
      { "exhaust",        required_argument, nullptr, OPT_EXHAUST },
      { "true-key",       required_argument, nullptr, OPT_TRUEKEY },
      { "dump-all",       no_argument,       nullptr, OPT_DUMPALL },
      { "no-repair",      no_argument,       nullptr, OPT_NO_REPAIR },
      { "cascade",        optional_argument, nullptr, OPT_CASCADE },
      { "polish",         no_argument,       nullptr, OPT_POLISH },
      { "crib-file",      required_argument, nullptr, OPT_CRIB },
      { "crib-weight",    required_argument, nullptr, OPT_CRIBWEIGHT },
      { "ring-stride",    required_argument, nullptr, OPT_RINGSTRIDE },
      { "no-plug",        required_argument, nullptr, OPT_NOPLUG },
      { "full-text",      no_argument,       nullptr, OPT_FULLTEXT },
      { "crib",           required_argument, nullptr, OPT_CRIBTEXT },
      { "crib-at",        required_argument, nullptr, OPT_CRIBAT },
      { "crib-dump",      no_argument,       nullptr, OPT_CRIBDUMP },
      { nullptr,          0,                 nullptr, 0   }
    };

  int c;
  while ((c = getopt_long(argc, argv,
                          "u:w:r:g:s:p:l:x:T:R:S:F:e:A:d:JMimbtqafcvhn4",
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

        case 'f':
          select_model(SCORE_FUSED);
          break;
        case 'c':
          opt_hillclimb = 1;
          break;
        case 'J':
          opt_firstimprove = 1;   /* -J is the first-improvement climb, best-first order */
          opt_dynorder = 1;
          break;
        case OPT_NO_REPAIR:
          opt_no_repair = 1;
          break;
        case OPT_CASCADE:
          opt_cascade = 1;
          if (optarg != nullptr)
            opt_cascade_gate = strtod(optarg, nullptr);
          break;
        case OPT_POLISH:
          opt_polish = 1;
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
        case OPT_DUMPALL:
          opt_dump_all = true;
          break;
        case OPT_RINGSTRIDE:
          opt_ring_stride = atoi(optarg);
          break;
        case OPT_NOPLUG:
          alltoupper(optarg);
          opt_no_plug = optarg;
          break;
        case OPT_FULLTEXT:
          opt_full_text = true;
          break;
        case OPT_CRIBTEXT:
          alltoupper(optarg);
          opt_crib_text = optarg;
          break;
        case OPT_CRIBAT:
          opt_crib_at = atoi(optarg);
          break;
        case OPT_CRIBDUMP:
          opt_crib_dump = true;
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

  /* --ring-stride K: sparse ring sampling for the rightmost wheel (walzenlage[2]).
     Only meaningful -- and only lossless-by-design in its refinement pass -- when
     both -r and -g wildcard that wheel's position; otherwise every value tested is
     a distinct, necessary key and there is nothing to thin out (same precondition
     as the leftmost wheel's exact collapse in build_key_space()). */
  if ((opt_ring_stride < 1) || (opt_ring_stride > asize))
    fatal("Illegal ring stride (--ring-stride must be 1 to 26)");
  if ((opt_ring_stride > 1) &&
      ((opt_ringstellung[2] != '.') || (opt_grundstellung[2] != '.')))
    fatal("--ring-stride needs both -r and -g to wildcard the rightmost "
          "wheel's position (e.g. -r ..X -> -r ...)");

  if ((strlen(opt_steckerbrett) > asize) ||
      (strspn(opt_steckerbrett, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") <
       strlen(opt_steckerbrett)))
    fatal("Illegal steckerbrett string (must be up to 13 letter pairs)");

  /* --crib TEXT / --crib-at N: cribs.md 12 step 3 is one crib at one alignment, so the
     position is required -- the sweep is step 4. The combination rules follow cribs.md 8:
     the crib composes with the climb options, and is rejected against the search modes
     whose key handling it would have to be reconciled with. */
  if (opt_crib_text)
    {
      size_t n = strlen(opt_crib_text);
      if ((n < 2) || (n > static_cast<size_t>(maxlen)) ||
          (strspn(opt_crib_text, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") < n))
        fatal("Illegal --crib string (must be at least 2 letters A-Z)");
      if (opt_crib_at == -1)
        { /* no --crib-at: sweep every alignment (cribs.md 12 step 4) */ }
      else if (opt_crib_at < 0)
        fatal("--crib-at must not be negative");
      if ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0))
        fatal("--crib is not supported with -F (tier 1 could filter out the very key "
              "the crib settles)");
      if (opt_exhaust)
        fatal("--crib is not supported with --exhaust (both force plugs from outside "
              "the climb)");
      if (opt_ring_stride > 1)
        fatal("--crib is not supported with --ring-stride");
      if (opt_anneal > 0)
        fatal("--crib is not supported with -A (annealing seeds its own board)");
    }
  else if ((opt_crib_at >= 0) || opt_crib_dump)
    fatal("--crib-at and --crib-dump need --crib");

  /* --no-plug LETTERS: letters known to carry no cable. Three ways to get it wrong, all
     fatal because each means the command line says something the search cannot honour:
     a non-letter, the same letter twice (harmless but always a typo for a different
     letter), and a letter that -s also plugs -- that one is a contradiction, since -s
     says the letter carries a cable and --no-plug says it does not. */
  if ((strlen(opt_no_plug) > asize) ||
      (strspn(opt_no_plug, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") < strlen(opt_no_plug)))
    fatal("Illegal --no-plug string (must be letters A-Z)");
  {
    bool seen[asize];
    for (int j = 0; j < asize; j++)
      seen[j] = false;
    for (const char * p = opt_no_plug; *p != 0; p++)
      {
        if (seen[char2num(*p)])
          fatal("Illegal --no-plug string (a letter is repeated)");
        seen[char2num(*p)] = true;
      }
    size_t nplugged = strlen(opt_steckerbrett);   /* letters, not pairs */
    for (size_t i = 0; i < nplugged; i++)
      if (seen[char2num(opt_steckerbrett[i])])
        fatal("A letter is both plugged by -s and marked unplugged by --no-plug");
  }
  /* Without a climb the plugboard is whatever -s says and nothing searches for more, so
     there is nothing for --no-plug to constrain -- the same reason --random needs -c. */
  if (opt_no_plug[0] && (! opt_hillclimb))
    fatal("--no-plug needs -c (it constrains the plugboard climb; without one the "
          "plugboard is fixed anyway)");

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

  /* -J selects the first-improvement climb with dynamic move order, so it needs -c. */
  if (opt_firstimprove && (! opt_hillclimb))
    fatal("Dynamic move order (-J) needs the plugboard hill-climb (-c)");

  /* -M changes the plug-cap rule in the climb, so it needs -c. */
  if (opt_capmerge && (! opt_hillclimb))
    fatal("Cap-as-target (-M) needs the plugboard hill-climb (-c)");

  /* --no-repair disables a climb move, so it only means anything with -c. */
  if (opt_no_repair && (! opt_hillclimb))
    fatal("Disabling the 2-plug re-pair (--no-repair) needs the plugboard hill-climb (-c)");

  /* --cascade is a climb barrier-cross move, so it needs -c. */
  if (opt_cascade && (! opt_hillclimb))
    fatal("Gain cascade (--cascade) needs the plugboard hill-climb (-c)");

  /* --polish finishes the best board post-search; needs -c, and the simple sweep
     (its best.idx = key*restarts+restart reconstruction does not hold under -F/--exhaust). */
  /* --polish = the best-board finisher with the 3-ply escalation; same guards. */
  if (opt_polish && (! opt_hillclimb))
    fatal("Best-board finisher (--polish) needs the plugboard hill-climb (-c)");
  if (opt_polish && opt_cascade)
    fatal("--polish and --cascade are alternatives; pick one");
  if (opt_polish && ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0) || opt_exhaust))
    fatal("--polish is not supported with -F or --exhaust");
  /* the refinement pass reconstructs the winning key via key_to_machine(best.idx /
     restarts_par, ...), which only decodes the "simple sweep" index encoding -- the
     same fragility --polish has (see the guard above). */
  if ((opt_ring_stride > 1) &&
      ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0) || opt_exhaust))
    fatal("--ring-stride is not supported with -F or --exhaust");

  /* --random and --exhaust are plugboard operations: they can do nothing in a bare rotor
     scan, so passing them without -c is an error (fail fast rather than silently ignore). */
  if (opt_random_set && (! opt_hillclimb))
    fatal("The random kick (--random) needs the plugboard hill-climb (-c)");
  if (opt_exhaust && (! opt_hillclimb))
    fatal("Partial exhaustion (--exhaust) needs the plugboard hill-climb (-c)");

  /* --dump-all is a per-restart climb diagnostic, so it needs -c. */
  if (opt_dump_all && (! opt_hillclimb))
    fatal("--dump-all needs the plugboard hill-climb (-c)");

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
      /* --no-plug letters are unavailable to force a pair on, so they come off the
         free-letter count exactly as an -s pair's two letters do. */
      int free_pairs = (asize - 2 * fixed_pairs
                        - static_cast<int>(strlen(opt_no_plug))) / 2;
      if (opt_exhaust < 1)
        fatal("Illegal partial exhaustion (--exhaust must be >= 1 forced plug pairs)");
      if (opt_exhaust > free_pairs)
        fatal("Partial exhaustion (--exhaust E): E exceeds the free plug pairs "
              "(13 minus the -s pairs and half the --no-plug letters)");
      build_exhaust_firsts();   /* the parallel first-pair work list (read-only after) */
    }

  /* Non-fatal warning: if --restarts N asks for more kicked restarts than there are distinct
     K-pair kicks among the free letters, the restarts must repeat by pigeonhole. free letters =
     26 - 2*(-s pairs + --exhaust forced pairs); the kick is clamped to at most free/2 pairs.
     Mainly catches the small-K / high-N footgun (e.g. --random 1 --restarts 1000). */
  if (opt_hillclimb && (opt_restarts >= 1))
    {
      int fixed_pairs = static_cast<int>(strlen(opt_steckerbrett) / 2);
      int free_letters = asize - 2 * (fixed_pairs + opt_exhaust)
                         - static_cast<int>(strlen(opt_no_plug));
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
      bool table_loaded[SCORE_FUSED + 1] = { false, false, false, false, false, false, false };
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

  ic_blend_init();
  readciphertext();

  show_settings();

  if (textlength < 1)
    fatal("Ciphertext is empty (no A-Z letters on standard input)");

  for(int i=0; i< textlength; i++)
    num_ciphertext[i] = char2num(ciphertext[i]);

  init();

  init_plug_fixed(opt_steckerbrett, opt_no_plug);   /* -s pairs + --no-plug letters */

  /* --crib: the menu depends on the ciphertext, so it is built here rather than during
     option validation. The checks that need the ciphertext live here too. */
  if (opt_crib_text)
    {
      int n = static_cast<int>(strlen(opt_crib_text));
      if (n > textlength)
        fatal("--crib is longer than the ciphertext");
      if ((opt_crib_at >= 0) && (opt_crib_at + n > textlength))
        fatal("--crib runs past the end of the ciphertext (check --crib-at)");
      init_crib();
      /* An Enigma never encrypts a letter to itself, so an alignment where the crib
         matches the ciphertext is impossible. With --crib-at that kills the one alignment
         asked for, and every key would be rejected: say so rather than silently finding
         nothing. Sweeping, it is just the filter doing its job -- unless it removes
         everything, which means the crib cannot sit anywhere in this message. */
      if (crib_aligns == 0)
        fatal((opt_crib_at >= 0)
              ? "--crib matches the ciphertext at that position: an Enigma never "
                "encrypts a letter to itself, so the crib cannot sit there"
              : "--crib cannot sit anywhere in this ciphertext: every alignment has "
                "the crib matching the ciphertext, which an Enigma never does");
    }

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
  if (opt_crib_text)
    {
      size_t rej = g_crib_rejected.load(std::memory_order_relaxed);
      fprintf(stderr,
              "Crib: %d alignment%s, rejected %zu of %zu key%s (%.1f%%) unscored\n",
              crib_aligns, (crib_aligns == 1) ? "" : "s",
              rej, g_keys_analysed, (g_keys_analysed == 1) ? "" : "s",
              g_keys_analysed ? (100.0 * rej / g_keys_analysed) : 0.0);
    }
  fprintf(stderr, "Finished in %.2f s using %d thread%s\n",
          secs, opt_threads, (opt_threads == 1) ? "" : "s");
  fprintf(stderr, "Precomputed %zu rotor table%s (%.1f MB); peak memory %.0f MB\n",
          g_table_count, (g_table_count == 1) ? "" : "s",
          g_table_bytes / (1024.0 * 1024.0), peak_mb);
}
