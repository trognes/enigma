#include "cli.h"

#include "common.h"
#include "options.h"
#include "text.h"
#include "wiring.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

/* Still defined in enigma.cc; they move to the crib modules in step 4, which is
   why they are declared here rather than included from a header. Both are read
   only by show_settings(), to report how much work a crib option resolved to. */
extern std::vector<std::string> g_crib_list;
extern int g_selfcrib_nhyps;

/* Likewise: the --exhaust pigeonhole warning needs the count of distinct
   K-pair kicks among the free letters. Moves to the exhaust module. */
double disjoint_pair_combinations(int free_letters, int pairs);

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
  fprintf(out, "  %-24s %s\n", "--soft-plug AB...",
          "Plugboard pairs GUESSED rather than known: the");
  fprintf(out, "  %-24s %s\n", "", "climb starts from them each restart but may");
  fprintf(out, "  %-24s %s\n", "", "move or drop them, unlike -s (needs -c) [none]");
  fprintf(out, "  %-24s %s\n", "--self-crib-seeds K",
          "Seed the climb from a DOUBLED WORD in the message");
  fprintf(out, "  %-24s %s\n", "", "(X RENNER X RENNER): deduce the boards it");
  fprintf(out, "  %-24s %s\n", "", "allows per key, rank by IC, climb the top K");
  fprintf(out, "  %-24s %s\n", "", "(needs -c; 0 = off) [0]");
  fprintf(out, "  %-24s %s\n", "--self-crib-length L",
          "Shortest doubled word to hypothesise [6]");
  fprintf(out, "  %-24s %s\n", "--self-crib-signature",
          "Assert the doubled word CLOSES the message (a");
  fprintf(out, "  %-24s %s\n", "", "signed surname): ~15x cheaper, but only wins");
  fprintf(out, "  %-24s %s\n", "", "when that holds [off]");
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
  fprintf(out, "  %-24s %s\n", "", "models i/m/b/t/q/a/f/k (k = mono+IC, experimental;");
  fprintf(out, "  %-24s %s\n", "", "number caps plug pairs; last");
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
  fprintf(out, "  %-24s %s\n", "--confidence N",
          "Sample N keys BEFORE the sweep to measure what");
  fprintf(out, "  %-24s %s\n", "",
          "this model scores with NO signal, and report how");
  fprintf(out, "  %-24s %s\n", "",
          "far the winner sits above it -- against what the");
  fprintf(out, "  %-24s %s\n", "",
          "BEST of the analysed keys reaches by chance, which");
  fprintf(out, "  %-24s %s\n", "",
          "grows with the keyspace. Read the MARGIN, not the");
  fprintf(out, "  %-24s %s\n", "",
          "raw score. Samples are climbed when -c is on, so");
  fprintf(out, "  %-24s %s\n", "",
          "the null matches the search. N buys precision in");
  fprintf(out, "  %-24s %s\n", "",
          "the null and nothing else: below 128 a signal-FREE");
  fprintf(out, "  %-24s %s\n", "",
          "ciphertext can report a positive margin, and above");
  fprintf(out, "  %-24s %s\n", "",
          "512 there is nothing left to buy. Free without -c;");
  fprintf(out, "  %-24s %s\n", "",
          "~1.7ms/sample with it. Needs a key space to sample:");
  fprintf(out, "  %-24s %s\n", "",
          "with the rotor key fully given there is no null, and");
  fprintf(out, "  %-24s %s\n", "",
          "the run says so and reports raw scores instead");
  fprintf(out, "  %-24s %s\n", "",
          "[0 = off, use 256]");
  fprintf(out, "  %-24s %s\n", "--tune-phase N",
          "Hill-climb the rotor PHASE instead of enumerating");
  fprintf(out, "  %-24s %s\n", "",
          "it: keep N starting phases per wheel, climb the");
  fprintf(out, "  %-24s %s\n", "",
          "plugboard, then scan all 26x26 middle/right ring");
  fprintf(out, "  %-24s %s\n", "",
          "positions with that board FROZEN (offsets held, so");
  fprintf(out, "  %-24s %s\n", "",
          "only the notch timing moves) and re-climb, until");
  fprintf(out, "  %-24s %s\n", "",
          "neither improves. Needs -c and -r/-g wildcarding");
  fprintf(out, "  %-24s %s\n", "",
          "the middle and right positions; no --ring-stride,");
  fprintf(out, "  %-24s %s\n", "",
          "-F, --exhaust, --crib or -A. An APPROXIMATION:");
  fprintf(out, "  %-24s %s\n", "",
          "capture radius ~0.4*L/26, so it wants long");
  fprintf(out, "  %-24s %s\n", "", "messages [0..26, 0 = off]");
    fprintf(out, "  %-24s %s\n", "--self-crib-tandem",
          "Also hypothesise a doubled word with NO separator");
  fprintf(out, "  %-24s %s\n", "",
          "(SIEGFRIEDSIEGFRIED). Roughly doubles the");
  fprintf(out, "  %-24s %s\n", "",
          "hypotheses, so opt-in; reaches ~5% more messages [off]");
fprintf(out, "  %-24s %s\n", "--crib-rerank F",
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
  fprintf(out, "  %-24s %s\n", "--no-preflight",
          "Do not report whether the ciphertext looks like");
  fprintf(out, "  %-24s %s\n", "",
          "Enigma output at all. The report is ON by default");
  fprintf(out, "  %-24s %s\n", "",
          "for a search (a wildcarded key) [reporting on]");
  fprintf(out, "  %-24s %s\n", "--doubling-report L",
          "Report every converged climb past the z gate whose");
  fprintf(out, "  %-24s %s\n", "",
          "decrypt holds a word of L+ letters doubled around");
  fprintf(out, "  %-24s %s\n", "",
          "an X (\"ROMANOWO X ROMANOWO\"), telegraphic German's");
  fprintf(out, "  %-24s %s\n", "",
          "own error correction. One SUBSTITUTION is allowed,");
  fprintf(out, "  %-24s %s\n", "",
          "the error a garble makes; a dropped letter");
  fprintf(out, "  %-24s %s\n", "",
          "misaligns the copies and is missed. Marked \">>\";");
  fprintf(out, "  %-24s %s\n", "",
          "a CONFIRMATION only -- it never enters a ranking,");
  fprintf(out, "  %-24s %s\n", "",
          "so it cannot promote a wrong key, and a report is");
  fprintf(out, "  %-24s %s\n", "",
          "not a new best. Chance reports fall ~16x per extra");
  fprintf(out, "  %-24s %s\n", "",
          "letter: L=7 expects ~6 in a 230M-key sweep, L=6");
  fprintf(out, "  %-24s %s\n", "",
          "about 90 -- so raise L before touching the gate.");
  fprintf(out, "  %-24s %s\n", "",
          "Lengths above 30 are not searched (the longest in");
  fprintf(out, "  %-24s %s\n", "",
          "the corpus is 13; the cap is what keeps the scan");
  fprintf(out, "  %-24s %s\n", "",
          "cheap). Needs -c and --confidence (defines z) [off]");
  fprintf(out, "  %-24s %s\n", "--doubling-z Z",
          "Sigma threshold for --doubling-report. Below it a");
  fprintf(out, "  %-24s %s\n", "",
          "climb is not examined at all, which is what keeps");
  fprintf(out, "  %-24s %s\n", "",
          "the check free. Raw z over the --confidence null,");
  fprintf(out, "  %-24s %s\n", "",
          "NOT the margin the lines print. Lowering it finds");
  fprintf(out, "  %-24s %s\n", "",
          "nothing extra (the true key sits at z = 7..16) and");
  fprintf(out, "  %-24s %s\n", "", "multiplies false reports [3]");
  fprintf(out, "  %-24s %s\n", "--doubling-mismatches N",
          "Positions the two copies may differ in. The");
  fprintf(out, "  %-24s %s\n", "",
          "default 1 is the channel's error and no more --");
  fprintf(out, "  %-24s %s\n", "",
          "Enigma has no diffusion, so one garbled letter");
  fprintf(out, "  %-24s %s\n", "",
          "damages one copy. Measured on 2M null texts, N=2");
  fprintf(out, "  %-24s %s\n", "",
          "multiplies false reports ~50x and finds nothing");
  fprintf(out, "  %-24s %s\n", "",
          "extra (of 25 real doublings, 18 have no mismatch,");
  fprintf(out, "  %-24s %s\n", "",
          "7 have one, none has two). A letter of L divides");
  fprintf(out, "  %-24s %s\n", "",
          "the rate by 16, so N=2 needs L+2 to break even [1]");
  fprintf(out, "  %-24s %s\n", "--crib TEXT",
          "Known plaintext: rotor settings that cannot produce");
  fprintf(out, "  %-24s %s\n", "", "it are rejected unscored, and with -c the plugs");
  fprintf(out, "  %-24s %s\n", "", "it deduces seed the climb. No -F/--exhaust/");
  fprintf(out, "  %-24s %s\n", "", "--ring-stride/-A [off]");
  fprintf(out, "  %-24s %s\n", "--crib-at N",
          "Where the crib sits (1-based); omit to sweep every");
  fprintf(out, "  %-24s %s\n", "", "alignment -- but rejections multiply across");
  fprintf(out, "  %-24s %s\n", "", "them, so a swept crib needs 16+ letters");
  fprintf(out, "  %-24s %s\n", "--crib-dump",
          "Print every surviving crib hypothesis and the plugs");
  fprintf(out, "  %-24s %s\n", "", "it deduces (diagnostic; needs --crib) [off]");
  fprintf(out, "  %-24s %s\n", "--crib-seeds K",
          "Climb only the K crib hypotheses whose decrypt has");
  fprintf(out, "  %-24s %s\n", "",
          "the highest index of coincidence, instead of every");
  fprintf(out, "  %-24s %s\n", "",
          "survivor. For SHORT swept cribs, where a key can");
  fprintf(out, "  %-24s %s\n", "",
          "leave hundreds (needs -c; 0 = off) [0]");
  fprintf(out, "  %-24s %s\n", "--crib-list F",
          "Crib library, one per line ('#' comments); one rotor");
  fprintf(out, "  %-24s %s\n", "", "sweep each, best board kept [off]");
  fprintf(out, "  %-24s %s\n", "--no-crib-reorder",
          "Keep a --crib-list in file order. By default it is");
  fprintf(out, "  %-24s %s\n", "", "run cheapest-measured-cost first, since a long");
  fprintf(out, "  %-24s %s\n", "", "crib can cost ~2600x less than a short one [off]");
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


/* Echo the resolved run configuration to stderr so it is never a mystery what
   scoring model / language / settings a run is actually using. A dot (.) in the
   reflector/wheels/ring/start fields means that position is being searched. */
void show_settings()
{
  static const char * const scoring_name[] =
    { "index of coincidence", "monograms", "bigrams", "trigrams", "quadgrams",
      "weighted all-orders", "weighted all-orders + IC",
      "monograms + IC" };

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

  /* --confidence changes what the Score column MEANS, so a saved log has to say
     so up front: a reader who joins at the progress lines cannot otherwise tell
     a margin from a raw score, and the two differ by ~20 on the same run. N is
     echoed with it because the margin's precision is set by N and nothing else
     (below 128 a signal-free run can read positive -- see --help). */
  if (opt_confidence > 0)
    fprintf(stderr, "Confidence: %d null samples%s\n"
            "            the first column below is the MARGIN over chance, not "
            "a score\n",
            opt_confidence, opt_hillclimb ? ", each climbed" : "");

  /* The report fires on any key past the gate, not only on a new best, so a
     reader who does not know it is on would misread its lines as the search
     having improved. Say what marks them and what the gate was. */
  if (opt_doubling_report > 0)
    fprintf(stderr, "Doubling:   report doublings of %d+ letters at z >= %g, "
            "up to %d mismatch%s\n"
            "            marked \">>\" below; a report is NOT a new best\n",
            opt_doubling_report, opt_doubling_z, opt_doubling_mismatches,
            (opt_doubling_mismatches == 1) ? "" : "es");

  /* --ring-stride makes the rotor-key search APPROXIMATE (it can miss the true key --
     ~10pp of exact recovery at K=2 on telegraphic German, archived/PERFORMANCE.md §7.11), so a
     run that used it must say so: otherwise a saved log is indistinguishable from an
     exhaustive one. Every other search-affecting option is echoed here; this was the
     only silent one. */
  if (opt_ring_stride > 1)
    fprintf(stderr, "Stride:     rightmost ring every %d (--ring-stride), then refine "
            "skipped\n            rings; approximate, may miss the true key\n",
            opt_ring_stride);

  /* Same reason: --tune-phase replaces the exhaustive enumeration of the middle
     and right rings with N starting phases plus a frozen-board scan, so the run
     is approximate and a log of it must not read like an exhaustive sweep. */
  if (opt_tune_phase > 0)
    fprintf(stderr,
            "Phase:      %d starting phase%s per wheel, then tune ring1/ring2 "
            "with the\n            plugboard frozen (--tune-phase); "
            "approximate, may miss the true key\n",
            opt_tune_phase, (opt_tune_phase == 1) ? "" : "s");

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
  if (opt_soft_plug[0])
    fprintf(stderr, "            %s guessed, climb may revise (--soft-plug)\n",
            opt_soft_plug);
  if (opt_self_crib_seeds > 0)
    fprintf(stderr, "Self-crib:  top %d seed%s per key by IC, %d+ letters, "
            "%d hypotheses (%s)\n", opt_self_crib_seeds,
            (opt_self_crib_seeds == 1) ? "" : "s", opt_self_crib_length,
            g_selfcrib_nhyps,
            opt_self_crib_signature ? "signature"
              : (opt_self_crib_tandem ? "anywhere, separated or tandem"
                                      : "anywhere"));
  if (opt_crib_text)
    {
      if (opt_crib_at >= 0)
        fprintf(stderr, "Crib:       %s at position %d\n",
                opt_crib_text, opt_crib_at + 1);
      else
        fprintf(stderr, "Crib:       %s, sweeping every alignment\n", opt_crib_text);
    }
  if (opt_crib_list)
    {
      fprintf(stderr, "Crib list:  %s (%zu crib%s)\n", opt_crib_list,
              g_crib_list.size(), (g_crib_list.size() == 1) ? "" : "s");
      fprintf(stderr, "Crib order: %s\n",
              opt_crib_reorder ? "cheapest measured cost first"
                               : "file order (--no-crib-reorder)");
    }
  /* Reported for --crib and --crib-list alike: it changes how many climbs each key
     costs, which is the first thing to check when a crib run is slower or worse than
     expected. */
  if (opt_crib_seeds > 0)
    fprintf(stderr, "Crib seeds: top %d hypotheses per key by index of "
            "coincidence\n", opt_crib_seeds);
}

