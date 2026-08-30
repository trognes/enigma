#include "args.h"

#include "cli.h"
#include "common.h"
#include "crib.h"
#include "exhaust.h"
#include "keyspace.h"
#include "machine.h"
#include "ngrams.h"
#include "options.h"
#include "plugboard.h"
#include "progress.h"
#include "scoring.h"
#include "schedule.h"
#include "search.h"
#include "text.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <getopt.h>
#include <stdint.h>
#include <algorithm>
#include <string>
#include <vector>
#include <random>

void parse_args(int argc, char * * argv)
{
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
  opt_soft_plug = "";
  opt_crib_text = nullptr;
  opt_crib_list = nullptr;
  g_crib_list.clear();
  opt_crib_reorder = true;
  opt_crib_at = -1;
  opt_crib_dump = false;
  opt_language = 0;   /* no default; required for n-gram scoring (-m/-b/-t/-q/-a) */
  opt_datadir = 0;    /* resolved after parsing: -d > $ENIGMA_DATA > "ngrams" */
  opt_plaintext = 0;
  opt_maxwheel = 5;
  opt_hillclimb = 0;
  opt_firstimprove = 0;
  opt_dynorder = 0;
  opt_ic_order = 0;
  opt_capmerge = 0;
  opt_no_repair = 0;
  opt_cascade = 0;
  opt_cascade_gate = -4.9;   /* English-quad-calibrated near-solution gate (tunable) */
  opt_cascade3 = 0;
  opt_polish = 0;
  opt_doubling_report = 0;
  opt_doubling_z = double_z_default;
  opt_doubling_z_set = 0;
  opt_doubling_mismatches = double_mismatches_default;
  opt_doubling_mismatches_set = 0;
  opt_crib_rerank = nullptr;
  opt_crib_weight = 0.5;
  opt_crib = 0;
  crib_words_clear();
  opt_dump_all = false;
  opt_full_text = false;
  opt_no_preflight = false;
  opt_crib_seeds = 0;
  opt_restarts = 0;   /* new default: one deterministic seed climb, no kick (REDESIGN B) */
  opt_perturb = default_perturb;   /* --random kick size (default 10); K=0 is a legal control */
  opt_biased_random = 0.0;         /* --biased-random T (0 = uniform kick) */
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
  opt_tune_phase = 0;
  opt_seed_dedup = 0;
  opt_seed_dedup_bits = 0;   /* 0 = unset; the default of 8 is applied below */
  opt_seed_dedup_max = 0;
  opt_confidence = 0;

  /* get arguments */

  /* Long-only option identifiers (no short form): values above the byte range so they
     never collide with a short flag char. --random and --exhaust are the seed-pipeline
     options introduced in REDESIGN Part B. */
  enum { OPT_RANDOM = 256, OPT_EXHAUST, OPT_TRUEKEY, OPT_NO_REPAIR, OPT_CASCADE,
         OPT_POLISH, OPT_CRIBRERANK, OPT_CRIBWEIGHT, OPT_DUMPALL, OPT_RINGSTRIDE,
         OPT_NOPLUG, OPT_SOFTPLUG, OPT_SCSEEDS, OPT_SCLEN, OPT_SCSIG,
         OPT_FULLTEXT, OPT_CRIBTEXT, OPT_CRIBAT, OPT_CRIBDUMP,
         OPT_CRIBLIST, OPT_NOCRIBREORDER, OPT_TUNEPHASE, OPT_CONFIDENCE,
         OPT_BIASEDRANDOM,
         OPT_DOUBLINGREPORT, OPT_DOUBLINGZ,
         OPT_DOUBLINGMM, OPT_NOPREFLIGHT, OPT_CRIBSEEDS, OPT_SCTANDEM,
         OPT_SEEDDEDUP, OPT_SEEDDEDUPBITS, OPT_SEEDDEDUPMAX };

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
      { "ic-order",       no_argument,       nullptr, 'K' },
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
      { "biased-random",  required_argument, nullptr, OPT_BIASEDRANDOM },
      { "exhaust",        required_argument, nullptr, OPT_EXHAUST },
      { "true-key",       required_argument, nullptr, OPT_TRUEKEY },
      { "dump-all",       no_argument,       nullptr, OPT_DUMPALL },
      { "no-repair",      no_argument,       nullptr, OPT_NO_REPAIR },
      { "cascade",        optional_argument, nullptr, OPT_CASCADE },
      { "polish",         no_argument,       nullptr, OPT_POLISH },
      { "crib-rerank",    required_argument, nullptr, OPT_CRIBRERANK },
      { "crib-weight",    required_argument, nullptr, OPT_CRIBWEIGHT },
      { "ring-stride",    required_argument, nullptr, OPT_RINGSTRIDE },
      { "tune-phase",     required_argument, nullptr, OPT_TUNEPHASE },
      { "confidence",     required_argument, nullptr, OPT_CONFIDENCE },
      { "no-plug",        required_argument, nullptr, OPT_NOPLUG },
      { "soft-plug",      required_argument, nullptr, OPT_SOFTPLUG },
      { "self-crib-seeds", required_argument, nullptr, OPT_SCSEEDS },
      { "self-crib-length", required_argument, nullptr, OPT_SCLEN },
      { "self-crib-signature", no_argument,   nullptr, OPT_SCSIG },
      { "self-crib-tandem", no_argument,      nullptr, OPT_SCTANDEM },
      { "full-text",      no_argument,       nullptr, OPT_FULLTEXT },
      { "no-preflight",   no_argument,       nullptr, OPT_NOPREFLIGHT },
      { "crib",           required_argument, nullptr, OPT_CRIBTEXT },
      { "crib-at",        required_argument, nullptr, OPT_CRIBAT },
      { "crib-dump",      no_argument,       nullptr, OPT_CRIBDUMP },
      { "crib-seeds",     required_argument, nullptr, OPT_CRIBSEEDS },
      { "crib-list",      required_argument, nullptr, OPT_CRIBLIST },
      { "no-crib-reorder", no_argument,      nullptr, OPT_NOCRIBREORDER },
      { "seed-dedup",     no_argument,       nullptr, OPT_SEEDDEDUP },
      { "seed-dedup-bits", required_argument, nullptr, OPT_SEEDDEDUPBITS },
      { "seed-dedup-max", required_argument, nullptr, OPT_SEEDDEDUPMAX },
      { "doubling-report", required_argument, nullptr, OPT_DOUBLINGREPORT },
      { "doubling-z",     required_argument, nullptr, OPT_DOUBLINGZ },
      { "doubling-mismatches", required_argument, nullptr, OPT_DOUBLINGMM },
      { nullptr,          0,                 nullptr, 0   }
    };

  int c;
  while ((c = getopt_long(argc, argv,
                          "u:w:r:g:s:p:l:x:T:R:S:F:e:A:d:JKMimbtqafcvhn4",
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
        case 'K':
          /* -K is -J with the move-ordering scan ranked by IC, so it IMPLIES
             -J rather than modifying it: the two are one climb rule each, not
             a rule and a switch. `-J -K` is agreement, not a conflict, and is
             accepted silently. */
          opt_firstimprove = 1;
          opt_dynorder = 1;
          opt_ic_order = 1;
          break;
        case OPT_NO_REPAIR:
          opt_no_repair = 1;
          break;
        case OPT_CASCADE:
          opt_cascade = 1;
          if (optarg != nullptr)
            opt_cascade_gate = parse_opt_double(optarg, "--cascade");
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
          opt_maxwheel = parse_opt_int(optarg, "-x");
          break;
        case 'T':
          opt_threads = parse_opt_int(optarg, "-T");
          break;
        case 'R':
          opt_restarts = parse_opt_int(optarg, "-R");
          break;
        case OPT_RANDOM:
          opt_perturb = parse_opt_int(optarg, "--random");
          opt_random_set = true;
          break;
        case OPT_BIASEDRANDOM:
          opt_biased_random = parse_opt_double(optarg, "--biased-random");
          break;
        case OPT_EXHAUST:
          opt_exhaust = parse_opt_int(optarg, "--exhaust");
          break;
        case OPT_TRUEKEY:
          alltoupper(optarg);
          opt_true_key = optarg;
          break;
        case OPT_DUMPALL:
          opt_dump_all = true;
          break;
        case OPT_RINGSTRIDE:
          opt_ring_stride = parse_opt_int(optarg, "--ring-stride");
          break;
        case OPT_SEEDDEDUP:
          opt_seed_dedup = 1;
          break;
        case OPT_SEEDDEDUPBITS:
          opt_seed_dedup_bits = parse_opt_int(optarg, "--seed-dedup-bits");
          break;
        case OPT_SEEDDEDUPMAX:
          opt_seed_dedup_max = parse_opt_bytes(optarg, "--seed-dedup-max");
          break;
        case OPT_TUNEPHASE:
          opt_tune_phase = parse_opt_int(optarg, "--tune-phase");
          break;
        case OPT_CONFIDENCE:
          opt_confidence = parse_opt_int(optarg, "--confidence");
          break;
        case OPT_DOUBLINGREPORT:
          opt_doubling_report = parse_opt_int(optarg, "--doubling-report");
          break;
        case OPT_DOUBLINGMM:
          opt_doubling_mismatches = parse_opt_int(optarg, "--doubling-mismatches");
          opt_doubling_mismatches_set = 1;
          break;
        case OPT_DOUBLINGZ:
          /* This was the one option that checked its parse (0.0 is a legal,
             very loose gate here, so a typo would have looked deliberate).
             parse_opt_double now does the same for every numeric option --
             see the note on it in common.h. */
          opt_doubling_z = parse_opt_double(optarg, "--doubling-z");
          opt_doubling_z_set = 1;
          break;
        case OPT_NOPLUG:
          alltoupper(optarg);
          opt_no_plug = optarg;
          break;
        case OPT_SOFTPLUG:
          alltoupper(optarg);
          opt_soft_plug = optarg;
          break;
        case OPT_SCSEEDS:
          opt_self_crib_seeds = parse_opt_int(optarg, "--self-crib-seeds");
          break;
        case OPT_SCLEN:
          opt_self_crib_length = parse_opt_int(optarg, "--self-crib-length");
          break;
        case OPT_SCSIG:
          opt_self_crib_signature = true;
          break;

        case OPT_SCTANDEM:
          opt_self_crib_tandem = true;
          break;
        case OPT_FULLTEXT:
          opt_full_text = true;
          break;

        case OPT_NOPREFLIGHT:
          opt_no_preflight = true;
          break;
        case OPT_CRIBTEXT:
          alltoupper(optarg);
          opt_crib_text = optarg;
          break;
        case OPT_CRIBAT:
          /* 1-BASED on the command line -- "the crib starts at the Nth letter" is
             how a person reads a message. Converted here to the 0-based index the
             menu and the alignment sweep use, so only this one line and the two
             display sites below know about the offset.
               Rejected HERE rather than in validation because 0 - 1 == -1 is the
             "not given" sentinel: a --crib-at 0 that fell through would silently
             mean "sweep every alignment" instead of erroring. */
          {
            const int at = parse_opt_int(optarg, "--crib-at");
            if (at < 1)
              fatal("--crib-at is 1-based: the first position is 1, not 0");
            opt_crib_at = at - 1;
          }
          break;
        case OPT_CRIBSEEDS:
          opt_crib_seeds = parse_opt_int(optarg, "--crib-seeds");
          break;

        case OPT_CRIBDUMP:
          opt_crib_dump = true;
          break;
        case OPT_CRIBLIST:
          opt_crib_list = optarg;
          break;
        case OPT_NOCRIBREORDER:
          opt_crib_reorder = false;
          break;
        case OPT_CRIBRERANK:
          opt_crib_rerank = optarg;
          break;
        case OPT_CRIBWEIGHT:
          opt_crib_weight = parse_opt_double(optarg, "--crib-weight");
          break;
        case 'e':
          opt_seed = parse_opt_u64(optarg, "-e");
          opt_seed_set = true;
          break;
        case 'A':
          opt_anneal = parse_opt_int(optarg, "-A");
          break;
        case 'F':
          {
            /* -F N keeps the top N keys; -F N% keeps the top N% of the
               resolved keyspace. The '%' is the one legal trailing character
               in any option argument, so it is stripped into a local copy
               before parsing rather than tolerated by the parser -- which
               would re-admit "10x" everywhere else. The fraction, if given,
               wins. */
            const size_t flen = strlen(optarg);
            if ((flen > 0) && (optarg[flen - 1] == '%'))
              {
                char pct[64];
                if (flen >= sizeof(pct))
                  fatal("Illegal value for -F: too long");
                memcpy(pct, optarg, flen - 1);
                pct[flen - 1] = 0;
                opt_prefilter_frac = parse_opt_double(pct, "-F") / 100.0;
              }
            else
              opt_prefilter = parse_opt_int(optarg, "-F");
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

  /* --seed-dedup: skip the target climb when this restart's stage-0 seed has
     already been climbed for this key.

     It needs a CHEAP PREFIX to key on, so a single-stage schedule is refused
     rather than silently doing nothing -- with one stage the "seed" would be
     the converged board itself, by which point the expensive work is already
     paid for (which is exactly what the removed --restart-tt measured down).

     Everything else refused here either installs its own starting board (so
     the board after stage 0 is not a function of (key, restart) alone), or
     re-encodes the work index so that "one key per pass" stops holding.
     --ring-stride is NOT in that list: its coarse pass is an ordinary
     restart-major sweep and dedups like any other, and its refinement simply
     runs with the filter off. */
  if (opt_seed_dedup)
    {
      if (! opt_hillclimb)
        fatal("--seed-dedup needs -c (there is no climb to skip without it)");
      if (opt_seed_dedup_bits == 0)
        opt_seed_dedup_bits = 8;
      if ((opt_seed_dedup_bits < 4) || (opt_seed_dedup_bits > 24))
        fatal("Illegal bits per item (--seed-dedup-bits must be 4 to 24; "
              "below 4 the false-positive rate costs more coverage than the "
              "skipping saves)");
      if (opt_anneal > 0)
        fatal("--seed-dedup does not work with -A (annealing has no staged "
              "seed to key on)");
      if (opt_exhaust > 0)
        fatal("--seed-dedup does not work with --exhaust");
      if ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0))
        fatal("--seed-dedup does not work with -F");
      if (opt_tune_phase > 0)
        fatal("--seed-dedup does not work with --tune-phase");
      if ((opt_crib_text != nullptr) || (opt_crib_list != nullptr))
        fatal("--seed-dedup does not work with --crib or --crib-list");
      if (opt_self_crib_seeds > 0)
        fatal("--seed-dedup does not work with --self-crib-seeds");
    }
  else if ((opt_seed_dedup_max > 0) || (opt_seed_dedup_bits != 0))
    fatal("--seed-dedup-bits / --seed-dedup-max need --seed-dedup");

  /* --tune-phase N: keep N starting phases per wheel instead of enumerating all
     26, and let tune_phase() find the rest by scanning with the plugboard
     frozen. It REPLACES the outer enumeration of ring1/ring2, so it needs those
     two positions -- and the starts that pair with them -- wildcarded, exactly
     as --ring-stride needs the rightmost pair. It is a plugboard-climb step
     (the phase carries no signal without a recovered board), so it needs -c. */
  if ((opt_tune_phase < 0) || (opt_tune_phase > asize))
    fatal("Illegal phase count (--tune-phase must be 0 to 26)");

  /* --confidence N: N is a sample count, so the only wrong values are negative and
     absurd. It composes with everything -- it samples fresh key indices rather than
     re-reading best.idx, so it carries none of --polish's encoding fragility. */
  if ((opt_confidence < 0) || (opt_confidence > 1000000))
    fatal("Illegal sample count (--confidence must be 0 to 1000000)");
  if (opt_tune_phase > 0)
    {
      if (! opt_hillclimb)
        fatal("Rotor phase tuning (--tune-phase) needs the plugboard "
              "hill-climb (-c) -- with no board recovered the phase is noise");
      for (int i = 1; i < 3; i++)
        if ((opt_ringstellung[i] != '.') || (opt_grundstellung[i] != '.'))
          fatal("--tune-phase needs both -r and -g to wildcard the middle and "
                "rightmost wheels' positions (e.g. -r A.. -g ...)");
      if (opt_ring_stride > 1)
        fatal("--tune-phase and --ring-stride are alternatives: both "
              "reparameterise the ring positions the search enumerates");
      if (opt_crib_text || opt_crib_list)
        fatal("--tune-phase is not supported with --crib (the crib deduction "
              "is a per-key test, and the tuning moves the key under it)");
      if (opt_anneal > 0)
        fatal("--tune-phase is not supported with -A (the alternation is "
              "defined against the greedy climb)");
    }

  if ((strlen(opt_steckerbrett) > asize) ||
      (strspn(opt_steckerbrett, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") <
       strlen(opt_steckerbrett)))
    fatal("Illegal steckerbrett string (must be up to 13 letter pairs)");

  /* --crib TEXT / --crib-at N: archived/cribs.md 12 step 3 is one crib at one alignment, so the
     position is required -- the sweep is step 4. The combination rules follow archived/cribs.md 8:
     the crib composes with the climb options, and is rejected against the search modes
     whose key handling it would have to be reconciled with. */
  if (opt_crib_text && opt_crib_list)
    fatal("--crib and --crib-list are alternatives: give one crib or a library of them");
  if (opt_crib_text || opt_crib_list)
    {
      if (opt_crib_text)
        {
          size_t n = strlen(opt_crib_text);
          if ((n < 2) || (n > static_cast<size_t>(maxlen)) ||
              (strspn(opt_crib_text, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") < n))
            fatal("Illegal --crib string (must be at least 2 letters A-Z)");
        }
      if (opt_crib_at == -1)
        { /* no --crib-at: sweep every alignment (archived/cribs.md 12 step 4) */ }
      /* Negative and zero are rejected at parse time (see OPT_CRIBAT). */
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
      /* A library holds cribs of many lengths, so one --crib-at cannot be right for
         all of them; and the cost estimate is what makes a library affordable. */
      if (opt_crib_list && (opt_crib_at >= 0))
        fatal("--crib-at pins ONE alignment, so it cannot apply to a whole "
              "--crib-list (the cribs differ in length and position)");
      /* With one crib there is no order to choose, so a request for one would
         silently do nothing -- say so rather than accept and ignore it. */
      if ((! opt_crib_reorder) && (opt_crib_list == nullptr))
        fatal("--no-crib-reorder needs --crib-list (there is nothing to order)");
      /* --crib-seeds picks WHICH hypotheses to climb, so with no climb to seed it
         would silently do nothing -- the same contract --self-crib-seeds has. */
      if ((opt_crib_seeds > 0) && ! opt_hillclimb)
        fatal("--crib-seeds needs -c (it chooses which plugboard climbs to run)");
      if ((opt_crib_seeds < 0) || (opt_crib_seeds > 10000))
        fatal("--crib-seeds must be 0 (off) to 10000");
    }
  else if ((opt_crib_at >= 0) || opt_crib_dump)
    fatal("--crib-at and --crib-dump need --crib");
  else if (opt_crib_seeds > 0)
    fatal("--crib-seeds needs --crib or --crib-list");
  else if (! opt_crib_reorder)
    fatal("--no-crib-reorder needs --crib-list (there is nothing to order)");

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

  /* --soft-plug PAIRS: a GUESS at part of the board, laid on each restart's starting
     position and then left free. Same well-formedness rules as -s, plus the two
     contradictions: a letter -s already pins (that letter's partner is asserted KNOWN, so
     starting it somewhere else is incoherent) and a letter --no-plug marks as carrying no
     cable (which the guess would immediately plug). Without -c there is no climb to start,
     so the "soft" half is meaningless and the caller wants -s. */
  if ((strlen(opt_soft_plug) > asize) ||
      (strspn(opt_soft_plug, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") < strlen(opt_soft_plug)))
    fatal("Illegal --soft-plug string (must be letters A-Z)");
  if (strlen(opt_soft_plug) & 1)
    fatal("Illegal --soft-plug string (needs an even number of letters -- it is pairs)");
  {
    bool seen[asize];
    for (int j = 0; j < asize; j++)
      seen[j] = false;
    for (const char * p = opt_soft_plug; *p != 0; p++)
      {
        if (seen[char2num(*p)])
          fatal("Illegal --soft-plug string (a letter is repeated)");
        seen[char2num(*p)] = true;
      }
    size_t nplugged = strlen(opt_steckerbrett);   /* letters, not pairs */
    for (size_t i = 0; i < nplugged; i++)
      if (seen[char2num(opt_steckerbrett[i])])
        fatal("A letter is both pinned by -s and guessed by --soft-plug");
    for (const char * p = opt_no_plug; *p != 0; p++)
      if (seen[char2num(*p)])
        fatal("A letter is both marked unplugged by --no-plug and guessed by "
              "--soft-plug");
  }
  if (opt_soft_plug[0] && (! opt_hillclimb))
    fatal("--soft-plug needs -c (it seeds the plugboard climb; without one the "
          "plugboard is fixed, which is what -s is for)");
  /* Every other seeding mechanism installs its own starting board at its own site, so
     combining them would silently let one overwrite the other: --exhaust pins its forced
     pairs, --crib pins what it deduces, and -A seeds itself with an IC pre-pass. */
  if (opt_soft_plug[0] && (opt_exhaust > 0))
    fatal("--soft-plug cannot be combined with --exhaust (both seed the board)");
  if (opt_soft_plug[0] && (opt_crib_text || opt_crib_list))
    fatal("--soft-plug cannot be combined with --crib/--crib-list (both seed the board)");
  if (opt_soft_plug[0] && (opt_anneal > 0))
    fatal("--soft-plug cannot be combined with -A (SA seeds itself with an IC pre-pass)");

  /* --self-crib-seeds K / --self-crib-length L. K is the number of IC-ranked seeds climbed
     per key, so it is the cost: per-key work is the deduction plus K climbs. L is the
     shortest signature hypothesised -- raising it drops the weak short hypotheses (an
     L=4 menu rejects nothing and deduces almost no plugs) at the price of missing a
     message actually signed with a short name.
       Every rejection below is a mode that installs its own starting board at its own
     site, or that re-encodes the work index: letting two of them run would silently have
     one overwrite the other. */
  if ((opt_self_crib_seeds < 0) || (opt_self_crib_seeds > 10000))
    fatal("Illegal --self-crib-seeds (must be 0 to 10000; 0 is off)");
  if ((opt_self_crib_length < 2) || (opt_self_crib_length > selfcrib_maxlen))
    fatal("Illegal --self-crib-length (must be 2 to 13)");
  if (opt_self_crib_signature && (opt_self_crib_seeds == 0))
    fatal("--self-crib-signature needs --self-crib-seeds (it only narrows where the "
          "doubled word is hypothesised)");
  if (opt_self_crib_tandem && (opt_self_crib_seeds == 0))
    fatal("--self-crib-tandem needs --self-crib-seeds (it only adds hypotheses for "
          "the seeder to rank)");
  /* --signature says the doubled word CLOSES the message, which fixes where the
     separator sits; --tandem says there is no separator at all. Both at once is a
     contradiction rather than a narrowing, so it is refused rather than silently
     preferring one. */
  if (opt_self_crib_tandem && opt_self_crib_signature)
    fatal("--self-crib-tandem and --self-crib-signature contradict each other "
          "(one says the copies are separated by an X closing the message, the "
          "other that they are not separated at all)");
  if (opt_self_crib_seeds > 0)
    {
      if (! opt_hillclimb)
        fatal("--self-crib-seeds needs -c (it seeds the plugboard climb)");
      if (opt_crib_text || opt_crib_list)
        fatal("--self-crib-seeds cannot be combined with --crib/--crib-list "
              "(both seed the board from a deduction)");
      if (opt_exhaust)
        fatal("--self-crib-seeds cannot be combined with --exhaust (both force plugs "
              "from outside the climb)");
      if (opt_anneal > 0)
        fatal("--self-crib-seeds cannot be combined with -A (SA seeds itself)");
      if (opt_soft_plug[0])
        fatal("--self-crib-seeds cannot be combined with --soft-plug (both seed the "
              "starting board)");
      if ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0))
        fatal("--self-crib-seeds is not supported with -F (tier 1 could filter out the "
              "very key the deduction settles)");
      if (opt_tune_phase > 0)
        fatal("--self-crib-seeds cannot be combined with --tune-phase (which moves the "
              "key the deduction was computed for)");
    }

  /* --restarts 0 (the new default) is legal: one deterministic climb from the seed, no
     kick. --restarts N>=1 runs N kicked climbs. */
  if ((opt_restarts < 0) || (opt_restarts > max_restarts))
    fatal("Illegal restart count (--restarts must be 0 to 1000000000)");

  /* --random K is the kick size (plug pairs injected per restart); K=0 is a legal control. */
  if ((opt_perturb < 0) || (opt_perturb > pairs_uncapped))
    fatal("Illegal kick size (--random must be 0 to 13 plug pairs)");

  /* --biased-random T: the kick, drawn from exp(z / T) over the single-plug IC
     scores instead of uniformly. Rejected wherever the kick is not the thing
     that starts the climb -- those sites call perturb_steckerbrett() at their
     own seeding points, so the flag would silently do nothing rather than
     being wrong, which is the worse failure of the two. */
  if (opt_biased_random != 0.0)
    {
      if ((opt_biased_random < 0.01) || (opt_biased_random > 100.0))
        fatal("Illegal --biased-random temperature (must be 0.01 to 100, or 0 "
              "for the uniform kick)");
      if (! opt_hillclimb)
        fatal("--biased-random needs -c (it biases the plugboard kick, and a "
              "bare rotor scan has none)");
      if (opt_restarts < 1)
        fatal("--biased-random needs --restarts 1 or more (with -R 0 there is "
              "no kick to bias)");
      if (opt_perturb < 1)
        fatal("--biased-random needs --random 1 or more (with a kick of zero "
              "pairs there is nothing to draw)");
      if (opt_anneal > 0)
        fatal("--biased-random is not supported with -A (simulated annealing "
              "seeds itself and never calls the kick)");
      if (opt_crib_text || opt_crib_list)
        fatal("--biased-random is not supported with --crib/--crib-list (the "
              "crib deduction installs its own starting board)");
      if (opt_exhaust > 0)
        fatal("--biased-random is not supported with --exhaust (which pins its "
              "own forced pairs)");
      if (opt_self_crib_seeds > 0)
        fatal("--biased-random is not supported with --self-crib-seeds (the "
              "deduction installs its own starting board)");
      /* $ENIGMA_KICK_RANK=k ranks the 325 single plugs by mono+IC instead of
         IC, which reads the monogram table and so needs a language.  IC needs
         neither, which is why the default asks for neither.  Refuse rather
         than rank on an all-zero mono8: that degenerates to IC exactly, so a
         forgotten -l would read as "k makes no difference" instead of as a
         mistake -- the worst possible failure for a flag that exists to be
         A/B'd. */
      if (kick_rank_model() == SCORE_MONOIC)
        {
          if (opt_language == nullptr)
            fatal("$ENIGMA_KICK_RANK=k ranks the kick by mono+IC, which needs "
                  "-l <language> for the monogram table (the default IC "
                  "ranking needs no language)");
          load_table(SCORE_MONO);
        }
    }

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
        opt_seed = parse_opt_u64(seed_env, "$ENIGMA_SEED");
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

  /* --doubling-report reports converged CLIMBS gated on z, so it needs both halves:
     -c for something to converge, and --confidence for the null that defines z.
     Neither is defaultable -- a bare rotor scan has no plugboard to recover, and
     silently sampling a null would spend real time (each sample is a whole climb
     under -c) on a run that never asked for it. */
  if (opt_doubling_report < 0)
    fatal("Illegal doubling length (--doubling-report must be >= 1)");
  if (opt_doubling_report > doubling_maxlen)
    fatal("Illegal doubling length (--doubling-report exceeds the longest "
          "doubling the scan looks for)");
  if ((opt_doubling_report > 0) && (! opt_hillclimb))
    fatal("Doubling reports (--doubling-report) need the plugboard hill-climb (-c)");
  if ((opt_doubling_report > 0) && (opt_confidence <= 0))
    fatal("Doubling reports (--doubling-report) need a null to gate on: add "
          "--confidence 256");
  /* --doubling-z alone changes nothing, and silently ignoring it would hide a
     typo on the flag that actually enables the report. */
  if (opt_doubling_z_set && (opt_doubling_report <= 0))
    fatal("--doubling-z sets the gate for --doubling-report, which is not on");
  if (opt_doubling_mismatches_set && (opt_doubling_report <= 0))
    fatal("--doubling-mismatches applies to --doubling-report, which is not on");
  if (opt_doubling_mismatches < 0)
    fatal("Illegal mismatch budget (--doubling-mismatches must be >= 0)");
  /* At N >= L every pair of equal-length X-free runs matches, so the test stops
     testing anything -- a vacuous setting, refused rather than run. */
  if ((opt_doubling_report > 0) && (opt_doubling_mismatches >= opt_doubling_report))
    fatal("Illegal mismatch budget (--doubling-mismatches must be below "
          "--doubling-report, or every pair matches)");

  /* Simulated annealing is an alternative plugboard optimiser, so it needs -c; the
     move budget must be non-negative. */
  if (opt_anneal < 0)
    fatal("Illegal anneal move budget (-A must be >= 1)");
  if ((opt_anneal > 0) && (! opt_hillclimb))
    fatal("Simulated annealing (-A) needs the plugboard hill-climb (-c)");

  /* -K is a climb rule, so it needs a climb. It sets opt_dynorder itself, so
     there is no "needs -J" case to reject. */
  if (opt_ic_order)
    {
      if (! opt_hillclimb)
        fatal("IC-ordered first-improvement climb (-K) needs the plugboard "
              "hill-climb (-c)");
      /* A stage whose model has a histogram form already scores each of the
         325 moves in O(26), and exactly -- so there the IC ranking is not
         merely cheap, it is inert. With EVERY stage low-order, -K is exactly
         -J, which a run that asked for -K should be told rather than left to
         infer from an unchanged answer. Non-fatal: the request is coherent,
         it just has no work to do. */
      bool any_full_scan = false;
      for (int i = 0; i < opt_nstages; i++)
        {
          const int md = opt_stages[i].model;
          if ((md != SCORE_IC) && (md != SCORE_MONO) && (md != SCORE_MONOIC))
            any_full_scan = true;
        }
      if (! any_full_scan)
        fprintf(stderr, "WARNING: -K is exactly -J with a low-order schedule "
                "-- every stage already ranks its moves in O(26) exactly\n");
    }

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
  /* -F ranks keys by a rotor-key-indexed tier 1, and --exhaust re-encodes the
     work index as forced pairs; --tune-phase moves the key out from under
     both. */
  if ((opt_tune_phase > 0) &&
      ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0) || opt_exhaust))
    fatal("--tune-phase is not supported with -F or --exhaust");

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
  if (opt_crib_rerank && (! opt_hillclimb))
    fatal("The crib finisher (--crib-rerank) needs the plugboard hill-climb (-c)");

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

  /* Checked here rather than with the other --seed-dedup rules above, because
     the stage count is not known until the schedule has been parsed. One stage
     means there is no cheap prefix to key on: the "seed" would be the converged
     board, and skipping on that saves nothing (the removed --restart-tt
     measured exactly this). Refused rather than quietly inert. */
  if (opt_seed_dedup && (opt_nstages < 2))
    fatal("--seed-dedup needs a staged --score schedule with a cheap first "
          "stage to key on (e.g. -S k4f10); with one stage there is no seed "
          "before the expensive climb");

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
      /* Sized by the LAST model, not by a literal list -- adding SCORE_MONOIC
         left the old seven-element initialiser one short, and only UBSan saw
         it (a plain build read past the array and happened to work). */
      bool table_loaded[SCORE_MONOIC + 1] = { false };
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
  if (opt_crib_rerank != nullptr)
    load_cribs(opt_crib_rerank);

  /* Same reason: read the crib library before stdin, so a missing or empty file is
     reported immediately rather than after the ciphertext has been consumed. */
  if (opt_crib_list != nullptr)
    load_crib_list(opt_crib_list);}
