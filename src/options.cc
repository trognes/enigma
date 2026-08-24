#include "options.h"

#include "common.h"

#include <stdint.h>

/* Every command-line setting, with the rationale for what it does and what was
   measured about it. Declarations are in options.h; this file is the reference
   for what each one MEANS.

   Definitions carry their defaults, so the default configuration of the whole
   program is readable in one place. */

const char * opt_ukw;
const char * opt_walzen;
const char * opt_ringstellung;
const char * opt_grundstellung;
const char * opt_steckerbrett;
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
const char * opt_no_plug;

/* --soft-plug PAIRS: the same shape as -s, and the opposite contract. -s says "these
   plugs are KNOWN", marks their letters in plug_fixed[] and forbids every move that would
   touch them; --soft-plug says "these plugs are a good GUESS" -- it lays them on the board
   each restart starts from and then gets out of the way, so the climb may move, merge or
   remove them like any other plug.
     The distinction is worth an option because the two failure modes are not comparable.
   A wrong -s pin cannot be undone by anything downstream (the pins deliberately survive
   --polish), so a bad guess poisons the whole run; a wrong --soft-plug guess costs only
   whatever the climb spends walking back out of it. That is exactly the trade a DEDUCED
   seed needs -- a deduction that is right most of the time but not always, such as the
   terminal-signature self-crib (ENHANCEMENTS.md item 5), where pinning measured WORSE than
   not seeding at all on the ~28% of messages where the ranking picks the wrong seed.
     The kick needs no change and that is not luck: perturb_steckerbrett() draws only from
   SELF-STECKERED letters, so a soft-seeded pair is invisible to it -- the kick adds pairs
   among the letters the seed left alone instead of scattering the seed itself. Seeding
   therefore happens BEFORE the kick, and every restart starts from seed + its own kick. */
const char * opt_soft_plug;

/* --crib TEXT / --crib-at N: a guess at part of the plaintext, together with where it
   sits. Given one that is right, part of the plugboard follows by ARITHMETIC instead of
   search, and rotor settings that cannot produce it are rejected without ever being
   scored -- Turing's menu and Welchman's diagonal board, on the machine equation this
   file already computes. See archived/cribs.md; the deduction itself is crib_deduce() below.
     Step 3 of archived/cribs.md 12: one crib at one alignment, used as a KEY FILTER. The
   alignment sweep and the seeded climb are later steps, so --crib-at is required. */
const char * opt_crib_text;

/* --self-crib-seeds K / --self-crib-length L: the TERMINAL-SIGNATURE self-crib.

   A doubled word is a SELF-crib: it does not say what the plaintext letter is, only that
   two positions carry the SAME one. Decryption is p_i = steck[core_i[steck[c_i]]], so
   p_i == p_j cancels the unknown letter from both sides and leaves

       steck[c_j] = core_j[core_i[steck[c_i]]]

   -- computable from the rotor key alone, exactly like --crib's rule but with no known
   plaintext anywhere in it. As a FILTER this is worthless (measured: 0 of 160 wrong keys
   rejected, because a sweep is dominated by its weakest alignment). As a SEEDER it is the
   only thing this repo has measured that beats -R at matched compute.

   What makes it work is pinning the ALIGNMENT rather than sweeping it: half the corpus's
   doublings are a signed surname closing the message (`... X RENNER X RENNER`), so only
   the word's LENGTH is unknown and the hypothesis set is ~20 rather than ~2800. The
   separator and the left flank are then real known-plaintext X's -- ordinary anchor edges,
   the same kind --crib uses -- which is what anchors the otherwise-floating equalities.

   Per key: deduce under all 26 guesses for steck[X] over every hypothesis, keep the
   distinct surviving boards (~28), rank them by the INDEX OF COINCIDENCE of their decrypt,
   and climb the top K with the deduced plugs pinned. IC ranks them as well as the fused
   model does (150/200 against 144/200 top-1) and needs no language, which is why the
   ranking is free. See ENHANCEMENTS.md item 5. */
int opt_self_crib_seeds = 0;        /* K: seeds climbed per key; 0 = off */

int opt_self_crib_length = 6;   /* L: shortest doubled word hypothesised */
/* --self-crib-signature: assert the doubled word CLOSES the message (a signed surname),
   rather than hypothesising it anywhere.  The default assumes nothing and the flag adds
   knowledge, which is the same shape as --crib (sweeps every alignment) and --crib-at
   (pins one).  Restricting is ~15x cheaper -- 20 hypotheses against ~2200 -- but it only
   wins when the assumption holds: measured over every corpus message carrying a doubling
   anywhere, terminal breaks 16/40 against a swept 26/40 and a bare -R 16's 19/40. */
bool opt_self_crib_signature = false;

/* --self-crib-tandem: also hypothesise a doubled word with NO separator between the
   copies -- SIEGFRIEDSIEGFRIED rather than ENGELMANN X ENGELMANN. The default cannot
   see one at all: its 26 guesses are on steck[X] and the separator anchor is what
   carries that guess into the message.
     OPT-IN ON COST, not on whether it works. It works -- the equality edges never
   mentioned the plaintext, so guessing at a flank instead of the separator runs the
   same closure, and recall barely moves (a correct hypothesis exists in 195 of 200
   trials against the separated case's 197). What it costs is enumeration: gap 0 has
   as many alignments as gap 1, so switching it on roughly DOUBLES the hypothesis
   count (+101% over the corpus). Per-key cost tracks that count almost linearly
   (2196 hypotheses <-> 2428 us, 1328 <-> 1065), so on by default it would take the
   seeder from ~2428 us per key to ~4900 -- past the 2901 us of the -R 16 baseline it
   is measured against, i.e. it would cost the feature its headline. What it buys is
   3 of 66 corpus messages, +4.5pp (SIEGFRIED, OSTROW, ROSENOW). A doubling of cost
   for 4.5pp belongs behind a flag.
     MEASURED END TO END, and on those 3 messages it is decisive: over 60 paired
   676-key sweeps with the board hidden, 3/60 exact recoveries become 22/60 -- 19
   only-on against 0 only-off, McNemar p = 3.8e-6. Wall time is 2.6x, matching the
   doubled hypothesis count; plugboards SCORED fall (2.42M -> 2.31M), which is the
   score_iter-is-the-wrong-axis note again, since the extra cost is all in the
   uncounted deduction. On the risk population -- messages with a separated doubling
   and no tandem one, where every tandem hypothesis is wrong by construction and
   competes for the same K seed slots -- 38/60 becomes 36/60 (0 only-on, 2 only-off,
   p = 0.5, 95% CI [-7.9, +1.2]pp): no measurable loss, though the sign is the one
   crowding-out predicts. Corpus-weighted that is ~+0.6pp for 2.6x the time, which
   is the arithmetic that keeps it opt-in. eval/selfcrib_tandem_ab.py.
   ENHANCEMENTS.md item 5. */
bool opt_self_crib_tandem = false;

int opt_crib_at = -1;
bool opt_crib_dump;              /* print each surviving hypothesis (diagnostic) */
/* --crib-seeds K: IC-rank the surviving hypotheses and climb only the best K, exactly as
   --self-crib-seeds does. 0 = off, which climbs every survivor (the historical path, kept
   byte-identical).
     WHY IT EXISTS. crib_unit() runs a FULL plugboard climb per surviving (alignment,
   hypothesis) pair, and a SWEPT short crib leaves a great many: measured at the true key,
   438.6 survivors at 8 letters, 90.7 at 10, 8.3 at 12, 1.5 at 14. Long cribs reject
   nearly everything and leave nothing to rank; short ones leave hundreds of climbs per
   key, which is exactly why the swept crib has a documented floor of 16 letters.
     WHY IC RANKS THEM. A hypothesis that is right pins several correct plugs, and that
   lifts the index of coincidence of its decrypt before any climbing -- the same signal
   --self-crib-seeds ranks on, and it needs no language and no n-gram table.
     THE WINDOW IS NARROW AND BOUNDED ON BOTH SIDES, which is the measurement that should
   govern how this is used (eval/crib_ic_rank.py, 40 trials/length, true key, 10 plugs):

       crib   survivors   rank 1   top-10   median rank
          8       438.6    15/40    23/40             6
         10        90.7    25/40    37/40             1
         12         8.3    40/40    40/40             1
         14         1.5    40/40    40/40             1

   At 12+ ranking is perfect and pointless -- there is no population left to cut. At 8 the
   population explodes and IC degrades with it: the top 10 keeps only 57% of correct
   hypotheses, so a 44x cut costs 42% of them. Only at ~10 letters are both true at once:
   91 survivors, top-10 keeps 92.5%, a 9x cut for ~7% loss. Do not read this as a general
   speed-up; it is a way into the short-crib regime, and K should be raised when the crib
   is short. ENHANCEMENTS.md, Cribs item 12. */
int opt_crib_seeds = 0;

/* --crib-list FILE: a whole library of cribs, tried in file order, one full rotor
   sweep each (archived/cribs.md 6.7 -- crib-outer, because early exit is worth up to 50x while
   the shared setup_mapping/precompute a rotor-outer loop would save is 0.6%).
     You rarely know which phrase a message contains; you know the vocabulary of the
   network, which is what eval/build_cribs.py emits. A single --crib is for testing and
   for the case where you do know. */
const char * opt_crib_list = nullptr;

/* --no-crib-reorder: keep a --crib-list in file order instead of running it
   cheapest-measured-cost first. Reordering is the DEFAULT, so the flag names the
   exception -- and it can safely be a default because ordering discards nothing: the
   worst case is that the winner is found later, never that it is lost. See
   crib_cheaper() for why cheapest-first, and why it reverses what archived/cribs.md 5 step 5
   concluded from a modelled cost. */
bool opt_crib_reorder = true;

char * opt_plaintext; /* plaintext to compare to */
const char * opt_language; /* english, german, danish, french, swedish, finnish,
                                      icelandic, polish, spanish, wehrmacht; no default */

const char * opt_datadir;  /* directory holding the n-gram files (default "ngrams") */
int opt_norenigma; /* use the 5 Norenigma (Norway Enigma) wheels */
int opt_m4;        /* use M4 (4-rotor naval) mode */
char opt_greek_walzen = '.';      /* Greek wheel: B (Beta), G (Gamma) or . */
char opt_greek_ringstellung = '.';   /* Greek ring letter or . */
char opt_greek_grundstellung = '.';  /* Greek start letter or . */
int opt_maxwheel;
int opt_scoring;   /* the resolved ranking/target model (SCORE_*) */
/* The model chosen by a bare selector -i/-m/-b/-t/-q, or -1 if none was given. The
   selectors are thin aliases for a single uncapped --score <model> stage (REDESIGN
   Part C); this records the choice so conflicting models -- two disagreeing selectors,
   or a selector vs a different --score target -- can be rejected as a fatal error. */
int opt_model_selector;

int opt_hillclimb;
/* Circular first-improvement climb instead of steepest ascent: applies the FIRST improving
   move (a cursor sweeps a fixed move list and continues from where it accepted), so it does
   far fewer score_iter calls per climb -- ~2.8x cheaper -- at the cost of recovering worse
   per restart. Set only by -J (the bare first-improvement flag was removed as dominated),
   so it is always paired with the dynamic move order below. */
int opt_firstimprove;

/* -J: first-improvement with DYNAMIC best-first move ordering. Each climb first scores every
   move once against the starting (perturbed) board, sorts, and then runs the circular
   first-improvement in that order. The order is rebuilt per restart, so it front-loads good
   moves without collapsing restart diversity (unlike the rejected static order). Measured win
   on the realistic ~10-plug regime (+2-6pp mean at matched compute); a loss when few plugs are
   truly set. Off by default; needs -c. */
int opt_dynorder;

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
int opt_capmerge;

/* --no-repair: disable the default 2-plug re-pair barrier cross (try_repair), for
   ablation/measurement. Off by default (baseline byte-identical); needs -c. */
int opt_no_repair;

/* --cascade: quadgram-gain directed-repair barrier cross, tried at quad convergence.
   A 2-ply "cascade" that uses per-position gain to propose plug corrections (both
   plugboard contacts, self-encryption pruned), ranks them by the full re-decode
   score, applies the best pair even when the first plug is downhill (which un-masks
   the second), and keeps it only if the pair nets an improvement. Off by default
   (baseline byte-identical); needs -c; quad-only. See gain_cascade(); archived/PERFORMANCE.md 4.10. */
int opt_cascade;

/* --cascade near-solution gate: the cascade only fires on a converged board whose
   per-symbol quad score clears this threshold, so it skips the ~76% junk boards and
   spends its compute only on promising ones. Default -4.9 (English-quad calibrated:
   junk ~-5.3, near-solution 60%+ ~-4.8..-4.2); tune per language via --cascade=VALUE. */
double opt_cascade_gate;

/* Internal: enable the 3-ply gain cascade (a deeper escalation, tried only when the 2-ply
   cascade found nothing). --polish turns it on for the single best-board finisher. */
int opt_cascade3;

int opt_polish;
/* --doubling-report L: report, on stderr, every converged climb whose score clears
   the z gate below AND whose decrypt carries a doubled word of at least L
   letters around an X separator. A CONFIRMATION SIGNAL, not a score term: it
   never changes a ranking, so nothing it does can promote a wrong key. That is
   the one shape ENHANCEMENTS.md item 5 found defensible -- the score-bonus form
   (5(e)) was swept and measured down, because a bonus applied after the climb
   needs a trial where the climb recovered the plaintext and the score still
   lost, and in 140 genuine sweeps there were none. Reporting has no such
   dependency: it fires on the key that IS right and stays silent otherwise.
   0 = off. */
int opt_doubling_report;

/* --doubling-z Z: the z the report gates on -- the raw sigma count over the
   --confidence null, NOT the margin the progress lines print (margin =
   z - sqrt(2 ln K)).

   The DEFAULT of 3 is where it was measured, and both directions are worse. At
   z > 3 a full 230M-key rotor sweep expects ~6 spurious L>=7 doublings;
   loosening to z > 2 quadruples that while rescuing nothing extra, because the
   doublings are already concentrated in the tail. Tightening throws the true key
   out with the chaff -- it sits at z = 7..16 once its climb has recovered the
   plaintext, nowhere near the gate, while the keys below 3 are the ones whose
   climb failed, where there is no doubling to find anyway. The cheap lever is L
   instead: the chance rate falls ~16x per extra letter.

   It is a knob rather than a constant because the numbers above are for one
   corpus and one key space, and a much larger or much noisier sweep may want to
   move it -- but move L first. */
double opt_doubling_z;

int opt_doubling_z_set;
/* --doubling-mismatches N: positions where the two copies may differ. Default 1,
   which is the CHANNEL's error and no more: Enigma has no diffusion, so one
   corrupted ciphertext letter damages exactly one plaintext letter, in one copy
   and not the other.

   RAISING IT IS EXPENSIVE AND BUYS ALMOST NOTHING -- measured, not modelled, on
   2M synthetic texts drawn from the climbed-wrong-key letter statistics (X-rate
   2.41%, IC 0.0514). The generator validates against the documented operational
   null: it reads 6.0e-6 at L=6,N=1 where that null is 4.9e-6.

     L   N   false-positive rate   vs N=1   real doublings found (of 46)
     6   0             0                     8
     6   1       6.0e-06               1x   13
     6   2       2.9e-04              49x   13
     6   3       7.3e-03            1212x   14
     7   2       2.0e-05             ~53x   11  (same as N=1)
     8   2       2.0e-06                     7  (N=1 finds 6)

   So N=2 multiplies false reports by ~50x and finds nothing extra at L=6 or 7,
   one more at L=8. That matches the corpus: of the 25 real doublings 18 have no
   mismatch and 7 have exactly one, and NONE has two.

   N and L are not interchangeable levers. One extra LETTER divides the rate by
   ~16, one extra mismatch multiplies it by ~50, so a step in N costs about
   what 1.4 letters buy back -- if you want N=2, add 2 to L and you are back
   where you started. Kept as a knob because a heavily garbled message is a
   real case; the default is where the evidence is. */
int opt_doubling_mismatches;

int opt_doubling_mismatches_set;
/* --crib-rerank / --crib-weight: known-word ("crib") finisher -- Ostwald & Weierud's
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
const char * opt_crib_rerank = nullptr;

double opt_crib_weight = 0.5;   /* the least-harmful weight measured (still net ~0) */
int opt_crib = 0;               /* set once a non-empty crib file is loaded */
int opt_restarts;  /* --restarts/-R: number of randomised restart attempts.
                             0 (the default) = one deterministic climb from the seed,
                             no kick; N>=1 = exactly N kicked climbs, keep the best
                             (the un-kicked seed climb is not additionally run). */

const char * opt_staged;  /* raw --score/-S schedule string (e.g. "i4q10"), or 0;
                                    parse_schedule() expands it into opt_stages[] */

/* A parsed --score/-S schedule is an ordered list of climb stages -- each a scoring
   model and a cap on the plug pairs it may set. Tokens are <letter><optional number>:
   model letters i/m/b/t/q (a stage, number = its pair cap, omitted = uncapped). The
   last model stage is the target/ranking model. The per-restart random kick and the
   partial exhaustion are separate options (--random / --exhaust), not schedule tokens. */
/* max_stages and struct climbstage: options.h */
struct climbstage opt_stages[max_stages];

int opt_nstages;    /* number of model stages in opt_stages[] */
int opt_perturb;    /* --random K: random plug pairs injected per restart, 0..13
                              (K=0 is legal -- a no-perturbation control). */

/* --biased-random T: draw the restart kick's pairs with probability
   exp(z / T) over the 325 single-plug IC z-scores instead of uniformly
   (0 = off, the uniform kick).  T is in units of sd because the scores are
   z-scored per key -- IC's absolute level wanders by key while its spread is
   what carries the signal, so one temperature means the same thing
   everywhere.  Large T tends to the uniform kick and small T to the greedy
   argmax; the useful range is measured at 0.25..1, and BOTH ends are worse
   than the middle -- see eval/results-weighted-kick.txt. */
double opt_biased_random;

bool opt_random_set;             /* was --random passed explicitly? (errors without -c) */
int opt_exhaust;    /* --exhaust E: partial plugboard exhaustion -- force E extra plug
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
/* --confidence N (0 = off): sample N keys from the resolved key space, score each
   exactly as the search does, and report how far the winning score sits above that
   null -- both on its own and against what the BEST of the analysed keys reaches by
   chance. See report_confidence(). */
int opt_confidence;

int opt_ring_stride;
/* --seed-dedup: skip the target climb when this restart's stage-0 seed has
   already been climbed for this key.  --seed-dedup-bits is the Bloom filter's
   bits per item (default 8) and --seed-dedup-max an optional memory ceiling
   that REFUSES rather than thinning the filter.  See src/dedup.h. */
int opt_seed_dedup;
int opt_seed_dedup_bits;
uint64_t opt_seed_dedup_max;
/* --tune-phase N (0 = off): N starting phases per wheel for tune_phase() below.
   With it on, the sweep enumerates the middle and right wheels' OFFSETS only --
   26^3 per wheel order instead of 26^5 -- and their 26x26 phase subspace
   becomes a cheap per-key scan instead of enumerated keys. */
int opt_tune_phase;

int opt_prefilter; /* key pre-filter: rank all keys by a cheap IC climb, then
                             run the full -c climb on only the top opt_prefilter keys
                             (0 = off; requires -c) */

double opt_prefilter_frac; /* -F N% form: fraction of the resolved keyspace to
                             keep (0 = not used; when > 0 it overrides opt_prefilter) */

/* Simulated-annealing plugboard optimiser (-A N): N = total move budget (0 = off,
   use the greedy climb). An alternative to the greedy hill-climb that accepts
   worsening moves with a cooling probability to escape local optima. Needs -c; the
   move budget is SA's cost/quality knob (like -R for the greedy climb). See
   archived/SIMULATED_ANNEALING.md. */
int opt_anneal;

int opt_threads;   /* worker threads for the search (default 1) */
/* Random seed for the plugboard restart perturbation. Mixed with the flat key index
   per key, so the restart RNG stays a pure function of (opt_seed, key) -- reproducible
   and independent of -T. Resolved as: -e <seed> > $ENIGMA_SEED > a fresh random draw.
   opt_seed == 0 reproduces the historical (pre-seed) behaviour exactly. */
uint64_t opt_seed;

bool opt_seed_set;
/* --true-key <reflector><3 wheels><3 ring><3 start> (standard Enigma, 10 chars,
   e.g. B241AAAQEW): a diagnostic for -F recall testing (archived/CRACKQUALITY_TESTS.md §2).
   With -F set, after tier-1 ranks every key the search prints "true-key tier1 rank
   R of N" to stderr -- R = 1 + the number of keys whose tier-1 IC score is strictly
   higher, N = total keys -- so a harness can measure how often the pre-filter keeps
   the true key. Off by default; parsed into g_tk_* below. */
const char * opt_true_key;

/* --dump-all: with -c, print the FULL setting of every converged (rotor
   key, restart) climb -- "dumpall <refl+wheels> <ring> <start> <score> <plugboard>" -- so a
   wildcarded search (not just a fixed key) can be inspected key-by-key. Display-only under
   the same mutex, so it never affects which candidate wins (-T-deterministic results are
   preserved; only the line ORDER is thread-timing dependent). Very verbose; off by default. */
bool opt_dump_all;

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
bool opt_full_text;

/* --preflight / the always-on sanity warning: IS THIS CIPHERTEXT EVEN ENIGMA?
     Enigma is a permutation cipher, so its output is near-flat; a ciphertext
   carrying residual language structure was not produced by one, and no key
   exists to be found. That is not hypothetical -- a 28-hour, 75.2M-key sweep of
   the QTXMA challenge message returned nothing, and the reason was visible in
   the ciphertext before the search started (see eval/preflight_null.py and
   MODERN_BREAKING_NOTES 5l).
     Two statistics, both free: the index of coincidence, and how many letters
   of the alphabet never occur. THE NULL MUST BE LENGTH-DEPENDENT -- IC variance
   goes as 1/C(n,2), so short messages reach a high IC routinely; two of the
   four BROKEN (i.e. genuinely Enigma) 1941 messages sit at z = +4.2, at 47 and
   74 letters, and a fixed IC threshold would flag them.
     No tables are needed, because both have closed forms under a uniform
   multinomial that were checked against simulated Enigma encryptions and matched
   within 1-2% at every length from 40 to 600 (preflight_null.py 1). IC =
   P/C(n,2) where P counts same-letter position pairs; with uniform p those pair
   indicators are pairwise UNCORRELATED -- the shared-index covariance is
   sum p^3 - (sum p^2)^2 = 1/A^2 - 1/A^2 = 0 -- so E[IC] = 1/A and Var[IC] =
   q(1-q)/C(n,2), q = 1/A, with no dependence on the plaintext at all. */
bool opt_no_preflight;
