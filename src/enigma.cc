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
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <cmath>
#include <limits>
#include <vector>

#include "common.h"
#include "options.h"
#include "machine.h"
#include "scoring.h"
#include "plugboard.h"
#include "crib.h"
#include "progress.h"
#include "result.h"
#include "cli.h"
#include "preflight.h"
#include "ngrams.h"
#include "text.h"
#include "wiring.h"

/* uwwwrrrggg = 3*8*7*6*26*26*26*26*26*26 = 311 387 102 208 */

/* --- machine constants, command-line options, and global state ---------- */


/* The --ring-stride refinement's middle-wheel offset window (mid_ring_window = 2) USED to
   live here. It is gone because the refinement now DERIVES that offset from the coarse
   winner's and the candidate's step schedules instead of banding it (archived/refinement.md): the
   quantity the band was guessing at is computable from the two keys, with no knowledge of
   the truth. The bound the band rested on still holds -- a ring2/start2 shift moves the
   middle wheel's schedule by at most 2, 1 from the ordinary time shift plus 1 when double
   stepping straddles the wheel's own notch, established by enumerating every rotor pair x
   26 start1 x 26 start2 x every shift at L=600 -- but nothing depends on it any more, which
   is what makes the refinement correct for two-notch right wheels and straddled double
   steps rather than merely usually right. */









/* --doubling-report defaults: one tolerated substitution (Enigma has no
   diffusion, so one garbled ciphertext letter damages exactly one plaintext
   letter, in one copy of the doubling and not the other), and a z gate of 3
   over the calibrated null. */
static const int double_mismatches_default = 1;
static const double double_z_default = 3.0;

/* Upper bound on -R, purely a sanity guard against a typo (each restart just
   re-runs the hill-climb from a fresh perturbed board -- no extra memory -- so the
   only real limit is patience). One billion is effectively unlimited for any real
   run yet stays well within int; raise it if you ever need more. */
static const int max_restarts = 1000000000;

static const int pairs_uncapped = asize / 2;   /* 13: a board holds at most this */

static const int default_perturb = 10;  /* --random default kick: near the typical plug count */
/* Alternation cap for tune_phase(); it converges well before this. */
static const int tune_phase_rounds = 4;

/* Plug-pair cap for the tier-1 IC filter climb. Capping the climb both speeds tier 1
   up (fewer passes per key) and improves rotor-key discrimination: an uncapped climb
   lets wrong keys overfit IC with surplus plugs and bury the true key, so a cap near
   the true plug count ranks it better. ~5 is the measured optimum (both-axes win vs
   uncapped; harmless on easy keyspaces) -- see archived/CODE_REVIEW_HISTORY.md §9 item 2. */
static const int filter_climb_cap = 5;
static const int max_threads = 256;

static int g_tk_u, g_tk_w[3], g_tk_r[3], g_tk_g[3];   /* parsed --true-key (numeric) */
static std::vector<float> g_tk_scores;                /* tier-1 IC score per flat key idx */

static std::atomic<size_t> g_tk_idx{static_cast<size_t>(-1)};   /* flat idx of the true key */






























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
double disjoint_pair_combinations(int free_letters, int pairs)
{
  double combos = 1.0;
  for (int i = 0; i < pairs; i++)
    combos *= static_cast<double>((free_letters - 2*i) * (free_letters - 2*i - 1))
              / (2.0 * (i + 1));
  return combos;
}




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
      plug_fixed_ex_reset(m);   /* per-worker pins = -s seed ... */
      for (int i = 0; i < c.target; i++)
        {
          m.steckerbrett[c.a[i]] = static_cast<unsigned char>(c.b[i]);
          m.steckerbrett[c.b[i]] = static_cast<unsigned char>(c.a[i]);
          plug_fixed_ex_pin(m, c.a[i]);   /* ... plus the forced pair */
          plug_fixed_ex_pin(m, c.b[i]);
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
/* --- the hybrid: deduce, then climb (archived/cribs.md 7, 12 step 5) --------------------------

   One work item at a key the crib did not reject: climb once from EVERY surviving
   hypothesis, seeded with the plugs that hypothesis deduces, and keep the best.

   The deduced plugs are HELD FIXED for the climb, in PLUG_FIXED_EX -- the same per-worker
   pin set --exhaust uses, because plug_fixed is a read-only global that no worker may
   touch. They stay fixed through --polish too: a deduced plug comes from arithmetic on the
   machine equation, while the finisher's cascade is score-driven local repair, so
   releasing them would let weaker evidence overwrite stronger (archived/cribs.md 7b). A WRONG
   hypothesis needs no such rescue -- it loses on score to the other 25.

   Letters the deduction settles as carrying NO cable are pinned as well: board[x] == x is
   a real finding, not an absence of one, and marking it stops the climb wasting moves on a
   letter that cannot be plugged. That is the value archived/cribs.md 7 wanted --no-plug for, had
   here for free.

   Cost is one climb per surviving hypothesis. With a long crib that is usually one; with a
   short one it is the several that archived/cribs.md 7a's seed mode expects and prices. */




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
  return unit_no_score;   /* no valid combo under this first pair: never wins the merge */
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
      if ((! have || (s > best)) && (s > unit_no_score))
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




/* --tune-phase: after the plugboard climb, hold the board FIXED and scan the
   middle and right wheels' PHASE -- ring and start shifted together, so each
   wheel's OFFSET (and with it that wheel's whole contribution to the
   substitution) is unchanged and the only thing moving is when its own notch
   fires. Then re-climb the plugs at the winning phase and repeat: a
   block-coordinate ascent alternating (board, phase).

   THE ORDER IS LOAD-BEARING. Scoring a rotor key without a plugboard climb is
   noise -- a rotor-only decrypt under a full board is ~95% scrambled, the same
   reason -F's tier 1 is a climb and not a scan. The board must be recovered
   first, then frozen, before the phase carries any signal at all.

   SCANNED, NOT CLIMBED. The axis is only 26x26 and the score along it is not
   reliably monotone (measured: the peak is correct far more often than the path
   to it is uphill), so steepest ascent can stall short of the truth. An
   exhaustive scan removes that question and costs about half a plugboard climb
   -- against the 676 plugboard climbs it replaces, since the phase subspace no
   longer has to be enumerated by the outer sweep.

   MEASURED. With 10 plugs hidden and the board frozen, the score peaks at the
   TRUE phase in 8/8 trials at L=439 when the starting phase error is <= 5, with
   a wide margin (-4.98 against -7.1..-7.6). It degrades with distance -- 67% at
   7, 33% at 9, 25% at 13 -- because past a point the climb starts from too much
   corruption to recover a usable board at all (the failures all score badly
   even AT the true phase). The capture radius is ~0.4*L/26, so it widens with
   message length: 25% at L=439 against 67% at L=900 for distance 13. That is
   why the key space keeps several starting phases rather than one. */
static double tune_phase(machine & m, uint64_t * rng, double score)
{
  const int o1 = diff26(m.grundstellung[1], m.ringstellung[1]);
  const int o2 = diff26(m.grundstellung[2], m.ringstellung[2]);
  const int r0 = m.ringstellung[0];
  const int g0 = m.grundstellung[0];
  int best_p1 = m.ringstellung[1];
  int best_p2 = m.ringstellung[2];
  /* The tentative move below re-climbs the board at a candidate phase, and that
     climb is not guaranteed to end above `score` (a staged --score schedule
     optimises an earlier model first, so its target-model score can come out
     lower). Rejecting the move then has to restore the BOARD as well as the
     phase, or the returned score and the machine the caller merges would
     describe different things. */
  unsigned char save_board[asize];

  for (int round = 0; round < tune_phase_rounds; round++)
    {
      /* Scan every phase with the board frozen. copy_rows is false: each phase
         is scored once, so this reads rows straight from subst_array like the
         plain sweep, instead of paying the climb path's per-position copy. */
      double best = score;
      int found_p1 = best_p1, found_p2 = best_p2;
      for (int p1 = 0; p1 < asize; p1++)
        for (int p2 = 0; p2 < asize; p2++)
          {
            init_ring_grund(m, r0, p1, p2, g0, add26(o1, p1), add26(o2, p2));
            setup_mapping(m, false);
            const double sc = score_iter(m);
            if (sc > best)
              {
                best = sc;
                found_p1 = p1;
                found_p2 = p2;
              }
          }
      /* Phase did not move: the board is already the best one for it. */
      if ((found_p1 == best_p1) && (found_p2 == best_p2))
        break;

      /* Re-climb the plugs at the candidate phase. The machine changed under
         the board, so the board is stale; resuming the climb from it rather
         than reseeding is what makes the extra rounds cheap. */
      memcpy(save_board, m.steckerbrett, asize);
      init_ring_grund(m, r0, found_p1, found_p2, g0,
                      add26(o1, found_p1), add26(o2, found_p2));
      setup_mapping(m, true);
      /* setup_mapping stepped grundstellung; restore it exactly as
         search_worker's climb path does, so a progress line echoed from inside
         the climb -- and the key recorded at the merge -- carry the true start
         positions and not the ones the message stepped the wheels to. */
      init_ring_grund(m, r0, found_p1, found_p2, g0,
                      add26(o1, found_p1), add26(o2, found_p2));
      const double after = optimize_once(m, rng);
      if (after <= score)
        {
          memcpy(m.steckerbrett, save_board, asize);
          break;               /* converged; the move is not committed */
        }
      best_p1 = found_p1;
      best_p2 = found_p2;
      score = after;
    }

  /* Leave the machine on the winning phase with the board, the mapping and the
     plaintext all describing `score` -- the caller merges all four together --
     and on the UNSTEPPED start positions (see above), since the merge records
     them as the winning key. Both loop exits can land here with the machine set
     to a phase that was scanned or climbed and then rejected, so this is a
     restore, not a no-op. */
  init_ring_grund(m, r0, best_p1, best_p2, g0,
                  add26(o1, best_p1), add26(o2, best_p2));
  setup_mapping(m, true);
  init_ring_grund(m, r0, best_p1, best_p2, g0,
                  add26(o1, best_p1), add26(o2, best_p2));
  decode(m);
  return score;
}

/* One plugboard-recovery climb from the seed board. In kicked mode (--restarts N>=1) every
   climb -- including index 0 -- injects a fresh --random kick first, so the un-kicked seed
   climb is not run (REDESIGN Option A). With --restarts 0 there is a single un-kicked climb.
   Each restart draws from its own independent (key,restart) stream, so it is a self-contained
   unit of work; leaves m at this climb's converged board + plaintext and returns its score. */
static double hillclimb_one(machine & m, size_t key_index, int restart)
{
  init_steckerbrett(m, opt_steckerbrett);
  apply_soft_plug(m);            /* before the kick -- see the opt_soft_plug note */
  uint64_t rng = restart_seed(key_index, restart);
  if (opt_restarts >= 1)
    perturb_steckerbrett(m, & rng, opt_perturb);
  double score = optimize_once(m, & rng);
  if (opt_tune_phase > 0)
    score = tune_phase(m, & rng, score);
  if (opt_dump_all)
    dump_all(m, score);
  report_doubling(m, score);
  return score;
}

/* The per-key climb the search actually runs, in ONE place.

   --crib and --self-crib-seeds replace the plain climb with a deduction-seeded one, and a
   seeded climb is drawn from a completely different score distribution: its board starts
   pinned from a hypothesis, which lifts the true key and depresses wrong ones. So
   --confidence has to calibrate its null against the SAME unit, exactly as it already
   climbs its samples rather than scanning them when -c is on -- calibrating one against
   the other reports a margin for a distribution the search never samples. Routing both
   the sweep and calibrate_null() through this helper is what stops the two drifting.
   (--exhaust is not here: its work unit is a forced pair rather than a key, so the sweep
   calls exhaust_unit directly with a pair index.) */
static double climb_unit(machine & m, size_t key_index, int restart)
{
  if (opt_crib_text)
    return crib_unit(m, key_index, restart);
  if (opt_self_crib_seeds > 0)
    return self_crib_unit(m, key_index, restart);
  return hillclimb_one(m, key_index, restart);
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
  /* --tune-phase spaces ring1's starting phases this far apart; 1 otherwise.
     Deliberately here: r2_vals/r2_n leave alignment padding, so this costs the
     struct nothing -- which matters because search_worker() reads it in the
     per-key decode (see the r2_vals note above). */
  unsigned char r_phase_step;
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


/* --- middle-wheel ring x start collapse (archived/PERFORMANCE.md §7.12) -------------
   Shifting ring1 and start1 together leaves diff26(g1, r1) -- the middle wheel's whole
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

/* --- right-wheel ring x start collapse by 13 (two-notch wheels) ------------
   The companion to the collapse above, on the OTHER position and for a
   different reason. VI, VII and VIII notch at M(12) and Z(25), exactly 13
   apart, so their notch SET survives a shift of 13 -- and a stepping wheel's
   absolute position is read by nothing but that notch test (the offset, which
   is what the substitution consumes, is preserved by shifting ring and start
   together). So for such a wheel on the right, (ring2, start2) and
   (ring2+13, start2+13) decode byte-identically.

   Unlike §7.12's, this equivalence is UNCONDITIONAL: it has no length term, so
   it does not decay as the message grows -- 2x at L=40 and 2x at L=900 alike.
   §7.12's is the opposite, worth 7.4x at L=40 and 1.00x past L~676.

   Exploited by skipping ring2 >= 13, which is exactly one representative per
   class: every dropped (r2, g2) has its twin (r2-13, g2-13) still in the sweep,
   since start2 ranges over all 26. That needs ring2 AND start2 both fully
   wildcarded -- with either pinned the twin may be absent and the skip would
   lose a real key -- which is the same no-redundancy precondition wheel 0's
   collapse and --ring-stride carry. The rc/gc test below also excludes
   --ring-stride and --tune-phase for free, since both leave rc[2] short of 26.

   Whether it applies is per WHEEL ORDER, not per search, so the flag is only
   the enable; search_worker() tests notch_halfperiod[] against the task's own
   right wheel. Reported ring2/start2 may therefore be either member of the
   pair -- the same class-representative contract §7.12 and wheel 0 already
   carry, and harmless because the decode, and so the plaintext, is
   identical. */
static bool g_r2_halve = false;

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
        g1 = step26(g1);
      g2 = step26(g2);
    }
  return -1;
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
/* Work index -> key index. RESTART IS THE OUTER DIMENSION: idx = restart*keys
   + key, so the key is the remainder and the restart is the quotient. Every
   site that recovers a rotor key from a merged best.idx must go through this;
   getting it wrong prints a key that does not decrypt to the plaintext the run
   just wrote to stdout, which is the failure the --tune-phase notes record.
   `keys == 0` cannot happen for a live sweep and is only guarded so the helper
   is total. */
static inline size_t work_key(size_t idx, size_t keys)
{
  return (keys > 0) ? (idx % keys) : idx;
}

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
     each is its own item; this is what lets a fully-specified rotor key still fill every
     thread. For the plain scan restarts==1, so the space is just the keys, exactly as
     before.

     RESTART IS THE OUTER DIMENSION: the sweep does every key at restart 0, then every
     key at restart 1, and so on. Restart-innermost would let consecutive items share a
     key and reuse its setup_mapping, which is why it was built that way -- but that
     saving is under 1% (setup_mapping is <0.1% of a -c run by callgrind, and a direct
     -R 1 vs -R 8 timing cannot resolve it above thread jitter), while the ordering
     decides WHEN an answer appears. There is no early exit, so this does not shorten a
     run; it front-loads the probability, which is what lets a watcher kill a 28-hour
     sweep early. Taking the measured climb curve (87% at R=16, ~11.9% per restart):
     found by the quarter mark 40% against 22%, by halfway 64% against 44%, the same 87%
     at the end. */
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
  /* current key collapsed away (§7.12, or the right-wheel collapse by 13) */
  bool key_skipped = false;
  /* this wheel order's right wheel has a period-13 notch set */
  bool r2_halve_wo = false;

  /* Live progress is accounted here rather than per chunk, because a chunk is
     total/(threads*16) -- at -T 1 that is sixteen updates for the whole run,
     one every few minutes on the sweeps that need a progress line most. A local
     counter flushed every tick_block items costs one predictable branch per key
     when the line is off (g_sweep_total == 0 short-circuits it) and one relaxed
     atomic add per block when it is on.

     The BLOCK SIZE has to follow the regime, because an item costs four orders
     of magnitude more under -c than in a scan. A scanned key is ~0.3 us, so
     4096 of them is ~1 ms -- fine. A climbed key is ~1-2 ms, so 4096 of them is
     a thread reporting once every NINE SECONDS, which is half of why the line
     appeared to hang on long runs. 64 climbed items is ~100 ms of work, which
     the 250 ms redraw gate then paces; the extra atomic adds are ~10/s per
     thread against a climb rate of ~450/s, so they cost nothing measurable. */
  const bool ticking = sweep_progress_armed();
  const size_t tick_block = opt_hillclimb ? 64 : 4096;
  size_t since_tick = 0;

  size_t start;
  while ((start = next_key.fetch_add(chunk)) < total)
    {
      size_t end = start + chunk;
      if (end > total)
        end = total;

      for (size_t idx = start; idx < end; idx++)
        {
          if (ticking && (++since_tick >= tick_block))
            {
              sweep_progress_tick(since_tick, best);
              since_tick = 0;
            }
          size_t keyidx = work_key(idx, nkeys);
          int restart = static_cast<int>(idx / nkeys);

          /* --tune-phase leaves the machine on the phase IT found, not the
             one the work index encodes, so the "reused by its restarts"
             sharing below no longer holds: restart 1 would start from restart
             0's tuned phase, which both breaks the independence -R relies on
             and makes the result depend on which thread ran which restart.
             Rebuild the key for every work item instead -- one extra
             setup_mapping per restart, and only with the option on. */
          if ((opt_tune_phase > 0) && (keyidx == cur_key) && (! key_skipped))
            {
              init_ring_grund(m, r1, r2, r3, g1, g2, g3);
              setup_mapping(m, true);
              init_ring_grund(m, r1, r2, r3, g1, g2, g3);
            }

          /* Restart-major means consecutive items are consecutive KEYS, so this fires
             on every item and the rotor stack is rebuilt each time rather than shared
             across a key's restarts. That is the whole cost of the ordering, measured
             at under 1%. The wheel order still changes only every rg items, so the
             expensive part -- the shared subst_array swap -- is unaffected. */
          if (keyidx != cur_key)   /* new key: (re)build the rotor stack */
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
                  /* Right-wheel collapse by 13: applies per wheel order, so
                     it is latched here beside the §7.12 row. m.walzenlage is
                     the TRANSLATED rotor number, which is how notch[] and
                     notch_halfperiod[] are indexed. */
                  r2_halve_wo = g_r2_halve
                                && notch_halfperiod[m.walzenlage[2]] != 0;
                }

              r1 = range.r_min[0] + static_cast<int>(rflat / rc12);
              int rr = static_cast<int>(rflat % rc12);
              r2 = range.r_min[1] + (rr / rc[2]) * range.r_phase_step;
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
              key_skipped = ((mid_row != nullptr)
                             && (((mid_row[g3] >> g2) & 1u) == 0))
                            /* right-wheel collapse: ring2 >= 13 is the
                               non-canonical half of its pair (g_r2_halve) */
                            || (r2_halve_wo && (r3 >= asize / 2));

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
                    {
                      crib_stop_at = crib_first_stop(m);
                      set_crib_stop_shown(crib_stop_at);
                    }
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
                      : climb_unit(m, keyidx, restart);
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
                  for (int i = 0; i < 3; i++)
                    {
                      best.ringstellung[i] =
                        static_cast<unsigned char>(m.ringstellung[i]);
                      best.grundstellung[i] =
                        static_cast<unsigned char>(m.grundstellung[i]);
                    }
                  /* Echo the new best -- unless a progress line already showed this
                     score (a climb's last accepted move IS its converged board, so
                     reprinting it here would just duplicate the line). Ties that
                     win the merge on the idx tie-break are display-identical, so
                     they stay silent too. */
                  if (score > best.shown.load(std::memory_order_relaxed))
                    {
                      best.shown.store(score, std::memory_order_relaxed);
                      /* setup_mapping stepped grundstellung (scan path only; the
                         climb path restored it right after setup_mapping).
                         --tune-phase is the one case where m holds a DIFFERENT
                         key than the work index encodes -- it left the machine
                         on the winning phase, consistent with the board and
                         the plaintext -- so restoring r1..g3 there would echo
                         the phase this climb started from against the tuned
                         phase's decrypt. */
                      if (opt_tune_phase == 0)
                        init_ring_grund(m, r1, r2, r3, g1, g2, g3);
                      progress_line(best, m, score);
                    }
                }
              local_best = best.score;         /* track the global best for the filter */
              local_best_idx = best.idx;
            }
        }
    }
  /* The remainder below tick_block, so the last worker to finish takes the line
     to 100% rather than leaving it short by up to 4095 items per thread. */
  if (ticking && (since_tick > 0))
    sweep_progress_tick(since_tick, best);
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
  int r2 = range.r_min[1] + (rr / rc[2]) * range.r_phase_step;
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
  /* ... and the right-wheel collapse by 13, the same way. init_walzen() above
     has already put the TRANSLATED rotor number in m.walzenlage, which is how
     notch_halfperiod[] is indexed. */
  if (g_r2_halve && notch_halfperiod[m.walzenlage[2]] && (r3 >= asize / 2))
    return false;

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

/* --- --confidence N: is the winner better than chance? ------------------------
   A raw score answers nothing on its own. A model's score has a distribution on
   text with no signal, and a search reports the MAXIMUM over the keys it
   analysed, which drifts upward as the keyspace grows -- so the same score can
   be a break at one keyspace size and noise at another.

   This samples N keys uniformly from the resolved key space, scores each exactly
   as the search scored them (climbing the plugboard too, when -c is on, because
   a climbed key is drawn from a different and higher distribution than a scanned
   one), and reports three things: how far the winner sits above that null in
   standard deviations, where the best of K draws is EXPECTED to sit by chance
   (mu + sigma*sqrt(2 ln K), the Gumbel location for a Gaussian null), and the
   margin between them. Only the margin means anything.

   MEASURED: on 12 signal-free ciphertexts swept over K = 17576 keys at L=200,
   the observed best-of-K matched that prediction to within 0.01 for quad
   (-7.2355 against -7.2432) and fused (-10.4368 against -10.4351). The index of
   coincidence does NOT follow it -- 6.1 sigma observed against 4.4 predicted --
   because its null is a quadratic form in the letter histogram rather than a sum
   over positions, and so is right-skewed. The p-value is therefore printed as
   Gaussian-tail and flagged as optimistic under -i.

   Sampling keys rather than random text is deliberate: the null a search actually
   draws from is "this ciphertext under a wrong key", and key_to_machine() already
   builds exactly that, in every machine mode, with no separate code path. */
static void calibrate_null(machine & m, size_t keys,
                           const std::vector<wheel_task> & tasks,
                           const search_range & range,
                           const int * rc, const int * gc, subst_table all,
                           size_t rg, size_t gsize, size_t rc12, size_t gc12,
                           size_t total_keys)
{
  /* Once per process. The null depends on the ciphertext, the model and
     whether the climb runs -- all fixed across a --crib-list's per-crib
     sweeps, so re-sampling for each of a hundred cribs would be a hundred
     times the cost for the same three numbers. */
  if (g_null_sd > 0.0)
    return;
  const bool save_report = m.report;
  const long save_scored = m.plugboards_scored;
  /* --dump-all's contract is "every converged climb OF THE SEARCH". A
     calibration climb is not one, and hillclimb_one() dumps unconditionally,
     so leaving this on put 16 extra rows in the diagnostic at --confidence 16
     -- silently changing what every harness that parses dumpall measures.
     Cleared for the sampling only; the workers have not started yet, so no
     other thread can observe it. */
  const bool save_dump = opt_dump_all;
  opt_dump_all = false;
  m.report = false;                 /* calibration must not echo progress lines */
  m.scoring = opt_scoring;

  uint64_t rng = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(opt_seed);
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];
  std::vector<double> xs;
  xs.reserve(static_cast<size_t>(opt_confidence));

  /* A live \r line, on a TTY only, so redirected logs and the tests stay clean --
     the same rule the -F tier-1 line follows. It earns its keep under -c, where a
     sample is a whole plugboard climb (~1.7 ms at L=200, so N=1024 is a couple of
     seconds of apparent hang before the first progress line of the search itself).
     Single-threaded -- the workers have not started -- so no atomic or mutex. */
  const bool show_progress = isatty(fileno(stderr)) != 0;
  const size_t want = static_cast<size_t>(opt_confidence);
  const size_t step = (want / 100) + 1;   /* every 1%, and every sample for small N */

  /* Draws are with replacement and skip keys the collapses removed; a run of
     misses cannot loop forever because total_keys is the INDEX space and at least
     one index in it always survives (the winner did).
       A REJECTED key is skipped too, and that one is load-bearing. Under --crib the
     unit returns unit_no_score for a key no hypothesis survives -- and a crib worth
     using rejects 99%+ of them, so nearly every sample came back as -1e300. The mean
     then sat at ~-1e300 and the variance OVERFLOWED to +inf, which made (s - mu)/sd
     exactly 0 for every board: every progress line printed the identical margin
     -z_k, and the summary printed a 300-digit null. Those keys are not part of the
     null the search draws from -- it never scores them at all -- so they must be
     dropped rather than counted. The guard is much larger than the plain path's
     because a rejected draw costs only the deduction (microseconds) while an
     accepted one costs a whole climb, so many attempts are affordable and the loop
     still terminates. */
  size_t guard = want * 256 + 4096;
  size_t rejected = 0;
  while ((xs.size() < want) && (guard-- > 0))
    {
      rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
      size_t idx = static_cast<size_t>((rng >> 11) % total_keys);
      if (! key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                           rc12, gc12, cur_wo, rg6))
        continue;
      const double s = opt_hillclimb ? climb_unit(m, idx, 0) : score_iter(m);
      if (s <= unit_no_score)
        {
          rejected++;
          continue;
        }
      xs.push_back(s);
      if (show_progress && (((xs.size() % step) == 0) || (xs.size() == want)))
        {
          fprintf(stderr, "\rConfidence: sampling the null %3zu%% (%zu / %zu keys)",
                  (xs.size() * 100) / want, xs.size(), want);
          fflush(stderr);
        }
    }
  /* Erase it rather than leaving the finished line: the settings echo already
     reported N, and the summary reports the result, so a permanent "100%" row
     would say nothing the run does not say twice already. */
  if (show_progress)
    fprintf(stderr, "\r%79s\r", "");

  m.report = save_report;
  opt_dump_all = save_dump;
  m.plugboards_scored = save_scored;   /* keep the diagnostic comparable */

  /* Naming the cause matters here: with a crib this is the EXPECTED outcome of a
     very selective one, not a malfunction, and the run is otherwise fine. */
  if (xs.size() < 8)
    {
      if (rejected > 0)
        fprintf(stderr,
                "Confidence: the crib rejected %zu of %zu sampled keys, leaving %zu "
                "to\n            calibrate against -- too few for a null. Reporting "
                "raw scores.\n", rejected, rejected + xs.size(), xs.size());
      else
        fprintf(stderr, "Confidence: too few sampled keys to calibrate\n");
      return;
    }
  double mu = 0.0;
  for (double x : xs)
    mu += x;
  mu /= static_cast<double>(xs.size());
  double var = 0.0;
  for (double x : xs)
    var += (x - mu) * (x - mu);
  var /= static_cast<double>(xs.size() - 1);
  const double sd = sqrt(var);

  /* A DEGENERATE null, tested relatively rather than against literal zero. The
     obvious `sd > 0.0` is not enough: with the rotor key fully specified the
     keyspace is ONE key, every sample climbs it to the same score, and sd comes
     out as float noise (~1e-15) rather than 0 -- so the guard passed and the
     margin became score/1e-15, i.e. ~1e13, which also blew the 8-wide first
     column out to 87 characters. Scores here are per-symbol log10 probabilities
     (order 1), so 1e-9 is nine orders below any real null (measured ~0.17) and
     six above the noise. Leaving g_null_sd at 0 makes showconfig fall back to
     raw scores, which is the honest display when there is nothing to calibrate
     against. */
  if (!(sd > 1e-9 * (fabs(mu) + 1.0)))
    {
      fprintf(stderr, "Confidence: all %zu sampled keys scored alike, so there is "
                      "no null to\n            measure against -- the key space "
                      "(%zu key%s) is too small to\n            hold one. "
                      "Reporting raw scores.\n",
              xs.size(), total_keys, (total_keys == 1) ? "" : "s");
      return;
    }
  g_null_mu = mu;
  g_null_sd = sd;
  /* Expected best of `keys` draws from a Gaussian null. The keys < 2 clamp only
     keeps log() defined; it is unreachable, because a key space that small
     cannot produce a spread of scores and the degenerate guard above has
     already returned. */
  g_null_keys = keys;
  g_null_zk = sqrt(2.0 * log(static_cast<double>(keys < 2 ? 2 : keys)));
  g_null_n = xs.size();
  /* Report the bar BEFORE the sweep, not only in the summary after it. The
     progress lines print a MARGIN, and a reader watching them has no way to
     convert that back to a raw sigma count -- which is the number every other
     account of a result is quoted in -- unless the offset is stated up front.
     The summary repeats it once the search is over; this is the same figure at
     the point where it is useful. */
  /* NOT prefixed "Confidence: null" -- that is the summary's anchor, and
     tests/run_tests.sh matches on it precisely because the bare "^Confidence"
     already collides with the settings echo. A third line sharing the anchor
     would break the -T-independence check the same way. */
  fprintf(stderr, "Confidence: margin 0 is z = %.1f, the best of %zu keys by "
                  "chance\n", g_null_zk, keys);
}

/* The summary line, printed after the search. The progress lines already
   carried the margin; this gives the pieces behind it -- the null itself, the
   raw distance above it, and a p-value -- so a log records what the margin was
   measured against
   rather than only the result. */
static void report_confidence(double best_score)
{
  if (!(g_null_sd > 0.0))
    return;
  const double z = (best_score - g_null_mu) / g_null_sd;
  /* Gaussian upper tail, family-wise over g_null_keys independent draws -- the
     same K the bar used, so the two halves of the line agree. erfc is exact
     enough far out; the 1-exp form avoids losing the small p to rounding. */
  const double tail = 0.5 * erfc(z / sqrt(2.0));
  const double pfam = -expm1(-static_cast<double>(g_null_keys) * tail);

  fprintf(stderr,
          "Confidence: null %.4f +/- %.4f over %zu sampled keys; best is %.1f sd\n"
          "            above it, chance best of %zu keys is %.1f sd -- margin "
          "%+.1f sd\n", g_null_mu, g_null_sd, g_null_n, z, g_null_keys, g_null_zk,
          z - g_null_zk);
  /* The Gaussian tail understates the false-positive rate near zero for EVERY
     model, not only IC. Measured on 2000 signal-free ciphertexts at L=200,
     K=17576: a margin of +0.54 came up 2.35% of the time against the 0.70% this
     p implies, because the real null's best-of-K sits +0.21 sd above a Gaussian
     of the same mu/sd and its upper tail is fatter (95th percentile +0.40
     measured against +0.11 predicted). The score is a sum over positions, so
     the CLT gives the centre quickly but the far tail at 4.4 sd -- exactly what
     a best-of-K statistic probes -- converges slowly. IC is worse again, its
     null being a quadratic form in the letter histogram rather than a sum. Far
     out none of this matters: a real break reads +15 to +17 sd, where a factor
     of three on 1e-98 changes nothing. */
  fprintf(stderr, "            p ~ %.1e (Gaussian tail, optimistic near zero%s)\n",
          pfam, (opt_scoring == SCORE_IC) ? " -- IC most of all" : "");
  /* Fires exactly when the number is in the range where the p-value misleads,
     and stays quiet on a real break. The threshold is the measured 99th
     percentile of pure noise rounded up, not a guess.
       NO line here may begin with something matching '^ *[+-][0-9]' -- that is
     the shape a progress line has, and the documented way to pull a run's
     margin out of stderr is to grep for it. This note used to wrap as
     "... a margin of\n            +0.5 sd came up ...", so the continuation
     WAS such a line and a summary sentence was read back as the run's result.
     Same bug class as the pre-flight lines guard against; found by it biting
     a day-key sweep whose extractor reported this sentence for all 33 keys. */
  if ((z - g_null_zk) < 2.0)
    fprintf(stderr,
            "            below +2 sd is not a find: on signal-free text a "
            "margin\n            of +0.5 sd came up in 2-5%% of runs "
            "(more often on a\n            bigger key space)\n");
}

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
  size_t scored_keys;           /* keys scored (< total_keys if collapsed) */
  bool r2_halved = false;       /* right-wheel collapse fired on some task */
  bool mid_collapsed = false;   /* §7.12 dropped a start1 on some task */
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
     except the final subst_array lookup diff26(g0, r0): wheel 0 has no notch
     check of its own (there is no wheel to its left to step), and its own
     stepping (driven entirely by wheel 1's notch) advances g0 by a pure
     additive constant untouched by r0 -- so shifting ring0 and start0 by the
     same delta leaves diff26(g0(i), r0) identical at every character position i,
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

  /* --tune-phase N: the middle and right wheels' phase (ring and start shifted
     together) is no longer enumerated -- tune_phase() scans it per key with the
     plugboard frozen -- so the sweep enumerates OFFSETS only. Pinning
     ring1/ring2 to a few starting phases and leaving start1/start2 over all 26
     makes start_i the offset directly, the same reparameterisation wheel 0
     already uses.
       N starting phases rather than one because the scan has a capture radius:
     the plug climb must start close enough to the true phase to recover a
     usable board. N=2 puts the worst case 6-7 away, inside the radius at
     L>=439. Requires ring and start wildcarded for both wheels, else the phases
     are the caller's and not ours to move. */
  ks.range.r_phase_step = 1;
  if (opt_tune_phase > 0)
    {
      const int step = asize / opt_tune_phase;
      for (int i = 1; i < wheels; i++)
        {
          ks.range.r_min[i] = 0;
          ks.range.r_max[i] = (opt_tune_phase - 1) * step;
          ks.rc[i] = opt_tune_phase;
        }
      ks.range.r_phase_step = static_cast<unsigned char>(step);
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
  /* Under --tune-phase ring2's values are the starting PHASES, spaced `step`
     apart, not a --ring-stride sample; validation makes the two exclusive. */
  const int r2_step = (opt_tune_phase > 0) ? (asize / opt_tune_phase)
                                           : opt_ring_stride;
  unsigned int r2_mask = 0;
  for (int v = ks.range.r_min[2]; v <= ks.range.r_max[2]; v += r2_step)
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
          seen[diff26(gp, gr)] = true;
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

  /* Right-wheel collapse by 13 (see g_r2_halve). rc[2] == 26 already implies
     ring2 was left fully wildcarded -- a pinned ring2 gives rc[2] == 1, and
     --ring-stride and --tune-phase both leave it short of 26 -- so this one
     test covers every precondition. --true-key opts out for the same reason
     §7.12 does: a collapsed key would simply be absent and never get a rank. */
  g_r2_halve = (ks.rc[2] == asize) && (ks.gc[2] == asize) && ! opt_true_key;

  ks.total_keys = ks.tasks.size() * ks.rsize * ks.gsize;

  /* Keys actually scored. The flat index space stays total_keys (the collapse skips
     during iteration rather than renumbering), so the diagnostic line would otherwise
     claim to have analysed keys it never touched. */
  ks.scored_keys = ks.total_keys;
  if ((g_mid_rep_mask != nullptr) || g_r2_halve)
    {
      ks.scored_keys = 0;
      for (const wheel_task & t : ks.tasks)
        {
          /* ring2 survivors for THIS wheel order: half of them when its right
             wheel has a period-13 notch set. notch_halfperiod[] is indexed by
             the TRANSLATED rotor number (as notch[] is), while wheel_task
             carries raw ones -- the distinction that once made the
             --ring-stride refinement search the wrong rotors under -n, and
             invisible in every other mode. */
          const int w2t = opt_norenigma ? norway_rotor_base + t.w[2] : t.w[2];
          size_t r2_surv = static_cast<size_t>(ks.rc[2]);
          if (g_r2_halve && notch_halfperiod[w2t])
            {
              r2_surv = asize / 2;
              ks.r2_halved = true;
            }
          const size_t rsurv =
            static_cast<size_t>(ks.rc[0]) * ks.rc[1] * r2_surv;

          if (g_mid_rep_mask == nullptr)
            {
              ks.scored_keys += rsurv * ks.gsize;
              continue;
            }
          const uint32_t * row = g_mid_rep_mask
            + (static_cast<size_t>(t.w[1]) * rotor_count + t.w[2]) * asize;
          size_t reps = 0;
          for (int s2 = ks.range.g_min[2]; s2 <= ks.range.g_max[2]; s2++)
            reps += static_cast<size_t>(__builtin_popcount(row[s2]));
          /* The mask can EXIST and drop nothing -- past L~676 every start1 is
             its own class -- so the echo must key on what was skipped, not on
             the mask being built, or it names a collapse that did no work. */
          if (reps < static_cast<size_t>(ks.gc[1]) * ks.gc[2])
            ks.mid_collapsed = true;
          ks.scored_keys += rsurv * static_cast<size_t>(ks.gc[0]) * reps;
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

/* --- what a crib will COST, measured before paying it (archived/cribs.md 4.2b) ---------------

   The unit is surviving HYPOTHESES per key, not rejection rate and not crib length.
   Under -c a surviving key is climbed once per surviving hypothesis, so that count is
   what decides whether a crib pays for itself: measured 1.0 per key where a crib
   rejects at all against 235 where it does not, which is the difference between a 126x
   speedup and a 66x slowdown.

   It CANNOT be predicted from the crib. NULLNULLNULL (12 letters) rejects 78% of rotor
   settings while XHOCKXHOCKX (11 letters) rejects 1% -- length and distinct-letter
   count do not track it, because the count depends on the crib AND the ciphertext
   together. So it is measured, on a sample.

   Nor can it be capped mid-run: an accumulated count across workers with an abort
   would make the result depend on thread timing, and the whole search holds to
   -T-determinism. So the decision is taken here, BEFORE the sweep, single-threaded,
   over a fixed stride of the key space -- deterministic by construction.

   Only wheel order 0's table is built (457 KB, one precompute), because the survival
   rate is a property of the crib and the ciphertext rather than of any particular
   rotor stack; the sample ranges over that order's whole ring x start space. */
struct crib_cost
{
  double hyps_per_key;   /* surviving hypotheses per sampled key */
  double pinned;         /* letters pinned per surviving hypothesis */
  size_t sampled;        /* keys actually sampled (after the 7.12 collapse) */
  /* Expected throughput gain: what a key costs WITHOUT this crib over what it costs
     WITH it, both measured on the same keys as plugboards scored -- so it already
     contains the two effects that pull against each other, the keys the crib rejects
     outright and the extra climbs it adds for every surviving hypothesis. No model:
     the seeded climb is actually run rather than priced by move-set arithmetic.
       > 1 means the crib is expected to save work, < 1 that it costs more than not
     using a crib at all. It measures THROUGHPUT ONLY and says nothing about recovery,
     which is the whole reason it must not be used to prune a library on its own
     (archived/cribs.md 12 step 6). Zero when there was nothing to measure. */
  double gain;
  /* Plugboards scored per key WITH this crib -- the deduction's surviving hypotheses,
     each climbed once. This is the quantity the default ordering sorts on: it is what
     the crib will actually cost per key when swept, measured rather than modelled. */
  double per_key;
  uint64_t boards;       /* plugboards the estimate itself scored, for the totals */
  bool gain_bounded;     /* the crib side hit its work budget: gain is "at most this" */
  bool gain_atleast;     /* the crib scored NOTHING on the gain keys: "at least this" */
};

static crib_cost crib_estimate(size_t nsample)
{
  crib_cost c = { 0.0, 0.0, 0, 0.0, 0.0, 0, false, false };
  key_space ks = build_key_space();
  size_t rg = ks.rsize * ks.gsize;
  if ((rg == 0) || ks.tasks.empty())
    return c;

  subst_table all = allocate_subst_tables(1);
  machine * m = new machine();
  m->plugboards_scored = 0;   /* differenced below; explicit so cppcheck can see it */
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];

  /* Build wheel order 0's table exactly as phase 1 does, then reuse it for every
     sampled key: key_to_machine points m.subst_array at `all + wo * asize`, and wo is
     0 for every index below rg. */
  init_walzen(*m, ks.tasks[0].u, ks.tasks[0].w[0], ks.tasks[0].w[1], ks.tasks[0].w[2]);
  m->greek = ks.tasks[0].greek;
  m->greek_offset = ks.tasks[0].greek_off;
  m->subst_array = all;
  set_effective_reflector(*m);
  precompute(*m);

  size_t stride = (rg + nsample - 1) / nsample;
  if (stride < 1)
    stride = 1;
  size_t hyps = 0, pins = 0;
  for (size_t idx = 0; idx < rg; idx += stride)
    {
      if (! key_to_machine(*m, idx, ks.tasks, ks.range, ks.rc, ks.gc, all,
                           rg, ks.gsize,
                           static_cast<size_t>(ks.rc[1]) * ks.rc[2],
                           static_cast<size_t>(ks.gc[1]) * ks.gc[2], cur_wo, rg6))
        continue;                       /* collapsed away by 7.12 */
      c.sampled++;
      crib_count_hypotheses(*m, hyps, pins);
    }

  if (c.sampled > 0)
    c.hyps_per_key = static_cast<double>(hyps) / static_cast<double>(c.sampled);
  if (hyps > 0)
    c.pinned = static_cast<double>(pins) / static_cast<double>(hyps);

  /* EXPECTED GAIN, measured rather than modelled: run both sides of the choice on the
     same keys and compare plugboards scored. crib_unit() is the real per-key work with
     this crib -- the deduction, then one seeded climb per surviving hypothesis -- and
     hillclimb_one() is the real per-key work without it. Their ratio therefore already
     contains both effects that pull against each other, the keys the crib rejects for
     free and the extra climbs it adds where it does not.
       Counting boards rather than timing keeps the number reproducible, which matters
     because it is printed. The one thing it leaves out is the deduction's own cost,
     which runs outside the score loop and so is not counted (the caveat CLAUDE.md
     records for score_iter generally) -- it flatters a crib that rejects nearly
     everything, where the true gain saturates at the deduction's own price.
       Only under -c: without a climb there is nothing to seed, and a crib is measured
     to lose against a plain scan anyway (archived/cribs.md 4.2b). */
  if (opt_hillclimb && (c.sampled > 0))
    {
      size_t gstride = stride * ((c.sampled + crib_gain_keys - 1) / crib_gain_keys);
      if (gstride < stride)
        gstride = stride;
      uint64_t with = 0, without = 0;
      size_t done = 0;
      for (size_t idx = 0; (idx < rg) && (done < crib_gain_keys); idx += gstride)
        {
          if (! key_to_machine(*m, idx, ks.tasks, ks.range, ks.rc, ks.gc, all,
                               rg, ks.gsize,
                               static_cast<size_t>(ks.rc[1]) * ks.rc[2],
                               static_cast<size_t>(ks.gc[1]) * ks.gc[2], cur_wo, rg6))
            continue;
          uint64_t b0 = m->plugboards_scored;
          crib_unit(*m, idx, 0);
          with += m->plugboards_scored - b0;
          /* Re-decode the key: the climb above left the machine holding its own board,
             and hillclimb_one must start from the same state crib_unit did. */
          key_to_machine(*m, idx, ks.tasks, ks.range, ks.rc, ks.gc, all, rg, ks.gsize,
                         static_cast<size_t>(ks.rc[1]) * ks.rc[2],
                         static_cast<size_t>(ks.gc[1]) * ks.gc[2], cur_wo, rg6);
          b0 = m->plugboards_scored;
          hillclimb_one(*m, idx, 0);
          without += m->plugboards_scored - b0;
          done++;
          /* A crib that rejects nothing runs a climb per surviving hypothesis, so a
             few keys of it can cost more than the whole rest of the estimate. Once it
             is that far behind, the exact figure does not matter -- stop and report
             the bound. Deterministic: fixed key order, fixed threshold. */
          if (with > crib_gain_budget * without)
            {
              c.gain_bounded = true;
              break;
            }
        }
      /* with == 0 means the crib rejected every one of the gain keys, so it cost no
         climbs at all -- the best possible result, and the one place a ratio has no
         denominator. Report the lower bound the sample supports (as if a single board
         had been scored) rather than nothing, which would read as "no information"
         for the strongest cribs in the list. */
      if (with > 0)
        c.gain = static_cast<double>(without) / static_cast<double>(with);
      else if (without > 0)
        {
          c.gain = static_cast<double>(without);
          c.gain_atleast = true;
        }
      /* The estimate runs real climbs, so its work belongs in the run's totals rather
         than vanishing with the machine it used -- ~150 ms per crib is small beside a
         sweep but should not be invisible. */
      c.boards = with + without;
      if (done > 0)
        c.per_key = static_cast<double>(with) / static_cast<double>(done);
    }

  delete m;
  delete[] all;
  return c;
}

/* One complete rotor sweep. Returns the best score, or score_min when nothing was
   scored at all -- which only happens when a crib rejected every key. That is fatal
   for a single run (nothing to print) but ORDINARY for --crib-list: a crib that does
   not match the message is expected to reject everything, and the caller moves on to
   the next one. Hence `allow_empty` rather than an unconditional fatal() here.
     Called once per crib under --crib-list, so anything it leaves behind must be
   per-sweep state. The two counters it sets (g_keys_analysed, g_plugboards_scored)
   are ASSIGNED, not accumulated, so the caller sums them across cribs. */
static double bruteforce(char * result, bool allow_empty)
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
  /* Two collapses can be live at once -- the middle wheel's (§7.12) and the
     right wheel's by 13 -- and they multiply, so the line NAMES the ones that
     fired and gives the combined reduction rather than attributing the whole of
     it to either. Splitting the total between them would mean apportioning a
     product, which is not a fact about any one of them. */
  if (scored_keys < total_keys)
    {
      const char * which =
        ks.mid_collapsed && ks.r2_halved ? "middle and right ring x start"
        : ks.r2_halved                   ? "right ring x start"
                                         : "middle ring x start";
      fprintf(stderr,
              "Collapse:   %s: %zu duplicate keys skipped "
              "(%.1fx);\n            reported ring/start may be an "
              "equivalent\n",
              which, total_keys - scored_keys,
              static_cast<double>(total_keys)
                / static_cast<double>(scored_keys));
    }

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

  /* --confidence N: calibrate BEFORE the sweep, because the progress lines
     report a margin against this null and the first of them can be printed
     within milliseconds of the search starting. The tables are built by now,
     which is all the sampling needs. Measured cost at L=200: free in scan mode
     (4096 samples sat inside the run's noise), ~0.75 ms per sample under -c
     where a sample is a full climb -- so +0.05 s at N=64, which is already well
     inside the error that matters (sigma/sqrt(N) ~ 0.13 sigma on mu, against
     margins measured in whole sigma). It is single-threaded, so its share grows
     with -T even as the search shrinks. */
  if (opt_confidence > 0)
    calibrate_null(*machines[0], scored_keys, tasks, range, rc, gc, all,
                   rsize * gsize, gsize,
                   static_cast<size_t>(rc[1]) * rc[2],
                   static_cast<size_t>(gc[1]) * gc[2], total_keys);

  /* phase 2: sweep the flat key space in adaptive chunks (~16 per thread: enough to
     balance the tail, few enough to amortise the atomic). The -F tiers are keyed over
     total_keys; the non-F sweep is over work_items (keys x restarts), so it gets its own
     chunk below. */
  best_result best;
  /* Carry the display high-water mark ACROSS sweeps. Under --crib-list each crib gets
     its own best_result, so without this a later crib would re-echo every board that
     beats its own (fresh) mark -- flooding stderr with lines scoring below the winner
     an earlier crib already found. Display state only: `shown` is never read by the
     merge logic, so which candidate wins stays -T-deterministic. */
  best.shown.store(g_shown_high, std::memory_order_relaxed);
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
      /* Arm the live progress line for THIS sweep only. --dump-all is excluded
         because its rows are the machine-readable form and print under their own
         mutex, so a \r line could interleave into them. */
      if ((isatty(fileno(stderr)) != 0) && ! opt_dump_all)
        {
          sweep_progress_arm(work_items,
                             (restarts_par > 0) ? restarts_par : 1);
        }
      run_parallel(nthreads, [&](int t)
        { search_worker(*machines[t], tasks, range, rc, gc, all,
                        rsize, gsize, next_key, schunk, restarts_par, best); });
      /* Disarm before anything else runs: the --ring-stride refinement reuses
         search_worker over its own key space and would push this past 100%. */
      sweep_progress_disarm();
      {
        std::lock_guard<std::mutex> lock(best.mutex);
        sweep_progress_clear();
      }
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
      /* work_key, not idx/restarts: restart is the OUTER dimension. */
      key_to_machine(m, work_key(best.idx, total_keys), tasks, range, rc, gc, all,
                     rg, gsize, rc12b, gc12b, cur_wo, rg6);
      /* --tune-phase: the winner sits on a phase key_to_machine cannot derive
         from best.idx (that is the whole point of tuning it), so overwrite the
         reconstructed ring/start with the recorded one. rg6 is corrected too,
         since it is what a finisher progress line would echo. */
      if (opt_tune_phase > 0)
        {
          for (int i = 0; i < 3; i++)
            {
              rg6[i] = best.ringstellung[i];
              rg6[3 + i] = best.grundstellung[i];
            }
          init_ring_grund(m, rg6[0], rg6[1], rg6[2], rg6[3], rg6[4], rg6[5]);
          setup_mapping(m, true);
          init_ring_grund(m, rg6[0], rg6[1], rg6[2], rg6[3], rg6[4], rg6[5]);
        }
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

          /* THE OFFSETS ARE DERIVED, NOT SEARCHED (archived/refinement.md).

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
          int coarse_off1 = diff26(m.grundstellung[1], m.ringstellung[1]);
          int coarse_off2 = diff26(m.grundstellung[2], m.ringstellung[2]);
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
             The band was built for archived/refinement.md §7.2, the one failure the derivation
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
          rrange.r_phase_step = 1;  /* candidates pin every position outright */
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
              int g2 = add26(v, coarse_off2);
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
              /* The refinement's own key space: rrsize == rgsize == 1, so its
                 key count is just the candidate task count. */
              key_to_machine(m, work_key(rbest.idx, rtasks.size()), rtasks, rrange,
                             rrc, rgc, m.subst_array, rrg, rgsize, rrc12, rgc12,
                             rcur_wo, rrg6);
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
        /* "after climb and polish": the per-restart call in hillclimb_one covers
           the climbs, this covers the one board the finisher touched. Reported
           on the finished board whether or not it beat the pre-polish best --
           the finisher's own output is what a reader wants checked, and a board
           that failed to improve is still a board worth a look if it carries a
           doubling. */
        report_doubling(m, s);
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

  /* --confidence N: the summary behind the margin the lines already carried. It
     reports the key count its own bar was built from (see g_null_keys), which
     under --ring-stride is the coarse sweep and excludes the refinement's few
     hundred extra keys; the "Analysed N rotor combinations" diagnostic below is
     where the inclusive total is reported. */
  if ((opt_confidence > 0) && best.found)
    report_confidence(best.score);

  /* diagnostics: every rotor combination is analysed (brute force has no early
     exit), and each worker counted the plugboards it scored -- sum them up */
  g_keys_analysed = scored_keys + extra_keys_analysed;
  g_plugboards_scored = 0;
  for (int t = 0; t < nthreads; t++)
    g_plugboards_scored += machines[t]->plugboards_scored;

  for (int t = 0; t < nthreads; t++)
    delete machines[t];
  delete[] all;

  double shown_now = best.shown.load(std::memory_order_relaxed);
  if (shown_now > g_shown_high)
    g_shown_high = shown_now;

  /* `best` is about to go out of scope, so the global must stop pointing at it. This
     was harmless while bruteforce() ran once per process, but --crib-list calls it
     once per crib and runs climbs BETWEEN the calls (the cost estimate), and those
     climbs read g_progress -- a dangling stack reference, caught by clang-analyzer.
     Clearing it also makes the estimate's climbs correctly silent: they are
     measurement, not search, and must not emit progress lines. */
  g_progress = nullptr;

  if (! best.found)
    {
      if (allow_empty)
        return score_min;      /* the crib rejected every key -- caller tries the next */
      fatal("No machine configuration produced a score");
    }

  memcpy(result, best.plaintext, textlength + 1);
  return best.score;
}

/* --- --crib-list: one rotor sweep per crib (archived/cribs.md 6.7, 12 step 6) -----------------

   Crib-outer, not rotor-outer. The sharing a rotor-outer loop would buy -- one
   setup_mapping and precompute across all the cribs at a given setting -- is 0.6% of
   the run, because the deduction dwarfs it ~180x. What crib-outer buys is EARLY EXIT,
   worth up to 50x, and it reuses the existing parallel sweep unchanged rather than
   threading a crib list into the hot path.

   Three things a single --crib treats as fatal are ordinary here, and skip the crib
   instead: it can be longer than the ciphertext, it can match the ciphertext at every
   alignment (so it cannot sit anywhere), and it can reject every key. A library is
   written against a network's vocabulary, not against one message, so most of its
   cribs do not fit any given message -- that is the normal case, not an error.

   The counters bruteforce() sets are per-sweep, so they are summed here and written
   back for the final diagnostic. */
/* One entry of the run plan: a crib, whether it is usable at all, and what it was
   measured to cost. Built for every crib BEFORE any sweep starts, because the order
   the sweeps run in is decided from the costs. */
struct crib_plan
{
  size_t index;          /* position in the file, 1-based -- printed, and the tie-break */
  int len;
  int aligns;
  crib_cost cost;
  const char * skip;     /* why this crib is not run, or null */
};

/* Cheapest first, ties by file order.

   This is the DEFAULT because the cost spread across crib lengths is enormous and
   runs the opposite way to intuition: measured on one message, a 20-letter crib swept
   in 0.15 s where a 10-letter one took 13.65 s -- 90x, because the long crib rejects
   almost every key by arithmetic and the short one rejects none, so every key is
   climbed once per surviving hypothesis. The whole long tail of a library therefore
   costs less than a single short crib, and running it first is very nearly free.

   It is worth being explicit that this REVERSES what archived/cribs.md 5 step 5 concluded.
   That measurement ordered by a MODELLED cost -- build_cribs.py prices a crib by its
   length on the assumption that sweep cost is roughly flat across lengths (4.1's
   table: 100-117 s for every row) -- and 4.2b showed the model has the wrong unit
   entirely. With a flat cost model, ordering by anything but hit probability can only
   delay the winner, which is exactly the 141 h against 6.7 h it reported. With
   measured costs the arithmetic changes, because the cost spread (~90x) is larger
   than the hit-rate spread (~26x: 93% of messages carry an 8-letter crib against 3%
   for a 20-letter one, 4.2).

   Ordering is a preference, not a filter: nothing is discarded, so the worst case is
   that the winner is found later rather than not at all. That is why this may default
   on where --crib-max-hyps, which does discard, must not. */
static bool crib_cheaper(const crib_plan & a, const crib_plan & b)
{
  if (a.cost.per_key != b.cost.per_key)
    return a.cost.per_key < b.cost.per_key;
  return a.index < b.index;
}

static void run_crib_list(char * result)
{
  char text[maxlen+1];
  double best_score = score_min;
  bool have = false;
  size_t tot_keys = 0, tried = 0, skipped = 0;
  unsigned long long tot_boards = 0;
  size_t n = g_crib_list.size();

  /* Pass 1: measure every crib. This has to finish before any sweep starts, since the
     order of the sweeps is decided from the results -- and it is cheap next to them
     (~150 ms per crib, independent of the key space, against sweeps of minutes). */
  std::vector<crib_plan> plan;
  plan.reserve(n);
  for (size_t i = 0; i < n; i++)
    {
      crib_plan p;
      p.index = i + 1;
      p.len = static_cast<int>(g_crib_list[i].size());
      p.aligns = 0;
      p.cost = { 0.0, 0.0, 0, 0.0, 0.0, 0, false, false };
      p.skip = nullptr;
      if (p.len > textlength)
        p.skip = "longer than the ciphertext";
      else
        {
          opt_crib_text = g_crib_list[i].c_str();
          init_crib();
          p.aligns = crib_alignment_count();
          /* Every alignment has the crib matching the ciphertext, which an Enigma
             never does -- so this crib cannot sit anywhere in this message. */
          if (crib_alignment_count() == 0)
            p.skip = "cannot sit anywhere";
          else
            {
              p.cost = crib_estimate(crib_sample_keys);
              tot_boards += p.cost.boards;
            }
        }
      plan.push_back(p);
    }

  /* Stable, so equal-cost cribs keep the file order the generator chose. */
  if (opt_crib_reorder)
    std::stable_sort(plan.begin(), plan.end(), crib_cheaper);

  /* Pass 2: the table, in the order the sweeps will actually run, so it doubles as
     the run plan. "gain" is what a key costs without the crib over what it costs with
     it, both measured -- the guide to why a crib is worth running or was skipped. */
  fprintf(stderr, "  %4s  %-24s %4s %4s %9s %8s  %s\n",
          "#", "crib", "len", "algn", "hyp/key", "gain", "note");
  for (const crib_plan & p : plan)
    {
      const std::string & crib = g_crib_list[p.index - 1];
      char hyp[16], gain[16];
      /* A strong crib leaves NO hypothesis alive anywhere in the sample, which is the
         best possible news and would read as an error printed as "0.0"; say what was
         actually measured, a bound from the sample size. */
      if (p.cost.sampled == 0)
        snprintf(hyp, sizeof hyp, "%s", "-");
      else if (p.cost.hyps_per_key == 0.0)
        snprintf(hyp, sizeof hyp, "<%.3f",
                 1.0 / static_cast<double>(p.cost.sampled));
      else
        snprintf(hyp, sizeof hyp, "%.1f", p.cost.hyps_per_key);
      if (p.cost.gain <= 0.0)
        snprintf(gain, sizeof gain, "%s", "-");
      else if (p.cost.gain >= 1000.0)
        snprintf(gain, sizeof gain, "%s", ">1000x");   /* beyond useful resolution */
      else
        snprintf(gain, sizeof gain, "%s%.2gx",
                 p.cost.gain_bounded ? "<" : (p.cost.gain_atleast ? ">" : ""),
                 p.cost.gain);
      char aln[8];
      if (p.len > textlength)
        snprintf(aln, sizeof aln, "%s", "-");   /* never got as far as a menu */
      else
        snprintf(aln, sizeof aln, "%d", p.aligns);
      fprintf(stderr, "  %4zu  %-24s %4d %4s %9s %8s%s%s\n",
              p.index, crib.c_str(), p.len, aln, hyp, gain,
              p.skip ? "  skipped: " : "", p.skip ? p.skip : "");
    }

  /* Pass 3: sweep, cheapest first unless the caller asked for file order. */
  for (const crib_plan & p : plan)
    {
      if (p.skip != nullptr)
        {
          skipped++;
          continue;
        }
      opt_crib_text = g_crib_list[p.index - 1].c_str();
      init_crib();          /* the menu globals belong to whichever crib ran last */
      tried++;
      double s = bruteforce(text, true);
      tot_keys += g_keys_analysed;
      tot_boards += g_plugboards_scored;
      if ((s > score_min) && (! have || (s > best_score)))
        {
          best_score = s;
          have = true;
          memcpy(result, text, textlength + 1);
        }
    }

  g_keys_analysed = tot_keys;
  g_plugboards_scored = tot_boards;
  fprintf(stderr, "Crib list:  %zu crib%s, %zu tried, %zu skipped\n",
          n, (n == 1) ? "" : "s", tried, skipped);
  if (! have)
    fatal("No crib in the list produced a scored configuration");
}

/* --- main ---------------------------------------------------------------- */

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
         OPT_DOUBLINGREPORT, OPT_DOUBLINGZ,
         OPT_DOUBLINGMM, OPT_NOPREFLIGHT, OPT_CRIBSEEDS, OPT_SCTANDEM };

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
      { "doubling-report", required_argument, nullptr, OPT_DOUBLINGREPORT },
      { "doubling-z",     required_argument, nullptr, OPT_DOUBLINGZ },
      { "doubling-mismatches", required_argument, nullptr, OPT_DOUBLINGMM },
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
        case OPT_TUNEPHASE:
          opt_tune_phase = atoi(optarg);
          break;
        case OPT_CONFIDENCE:
          opt_confidence = atoi(optarg);
          break;
        case OPT_DOUBLINGREPORT:
          opt_doubling_report = atoi(optarg);
          break;
        case OPT_DOUBLINGMM:
          opt_doubling_mismatches = atoi(optarg);
          opt_doubling_mismatches_set = 1;
          break;
        case OPT_DOUBLINGZ:
          {
            /* strtod with the end pointer checked, not atof: atof turns junk
               into 0.0 silently, and 0.0 is a legal (very loose) gate here, so
               a typo would look like a deliberate setting. */
            char * dz_end = nullptr;
            opt_doubling_z = strtod(optarg, & dz_end);
            if ((dz_end == optarg) || (*dz_end != 0))
              fatal("Illegal doubling gate (--doubling-z takes a number)");
            opt_doubling_z_set = 1;
          }
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
          opt_self_crib_seeds = atoi(optarg);
          break;
        case OPT_SCLEN:
          opt_self_crib_length = atoi(optarg);
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
          if (atoi(optarg) < 1)
            fatal("--crib-at is 1-based: the first position is 1, not 0");
          opt_crib_at = atoi(optarg) - 1;
          break;
        case OPT_CRIBSEEDS:
          opt_crib_seeds = atoi(optarg);
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
  if (opt_crib_rerank != nullptr)
    load_cribs(opt_crib_rerank);

  /* Same reason: read the crib library before stdin, so a missing or empty file is
     reported immediately rather than after the ciphertext has been consumed. */
  if (opt_crib_list != nullptr)
    load_crib_list(opt_crib_list);

  /* read ciphertext */

  ic_blend_init();
  readciphertext();

  /* Before show_settings(), which reports the hypothesis count -- it read 0 for a while
     because the list is built from the ciphertext and the echo ran first. */
  if (opt_self_crib_seeds > 0)
    init_self_crib();

  show_settings();

  if (textlength < 1)
    fatal("Ciphertext is empty (no A-Z letters on standard input)");

  /* After show_settings() so the resolved configuration is echoed first, and
     after the empty check so the statistics have something to describe. */
  report_preflight();

  for(int i=0; i< textlength; i++)
    num_ciphertext[i] = char2num(ciphertext[i]);

  init();

  init_plug_fixed(opt_steckerbrett, opt_no_plug);   /* -s pairs + --no-plug letters */

  /* --self-crib-seeds: like --crib's menu, the hypothesis list depends on the ciphertext
     (its length and which flanks could be a plaintext X), so it is built here. A message
     too short to hold even the shortest hypothesised signature yields none, which would
     make every key return "no seed" -- say so rather than sweeping and finding nothing. */
  if (opt_self_crib_seeds > 0)
    {
      if (g_selfcrib_nhyps == 0)
        fatal("--self-crib-seeds: no terminal signature of --self-crib-length "
              "letters or more fits this ciphertext (try a smaller value)");
    }

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
      if (crib_alignment_count() == 0)
        fatal((opt_crib_at >= 0)
              ? "--crib matches the ciphertext at that position: an Enigma never "
                "encrypts a letter to itself, so the crib cannot sit there"
              : "--crib cannot sit anywhere in this ciphertext: every alignment has "
                "the crib matching the ciphertext, which an Enigma never does");
    }

  /* try all combinations (bruteforce allocates one machine per worker thread) */

  char result[maxlen+1];
  if (opt_crib_list == nullptr)
    bruteforce(result, false);
  else
    run_crib_list(result);

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
  if (opt_crib_text || opt_crib_list)
    {
      /* With a list, crib_alignment_count() belongs to whichever crib ran last and would be a lie
         about the run as a whole, so the alignment count is reported only for a single
         crib. The rejection total IS meaningful across cribs: both counters accumulate
         over every sweep. */
      size_t rej = g_crib_rejected.load(std::memory_order_relaxed);
      if (opt_crib_list)
        fprintf(stderr, "Crib: rejected %zu of %zu key%s (%.1f%%) unscored, "
                "over every crib tried\n",
                rej, g_keys_analysed, (g_keys_analysed == 1) ? "" : "s",
                g_keys_analysed ? (100.0 * rej / g_keys_analysed) : 0.0);
      else
        fprintf(stderr,
                "Crib: %d alignment%s, rejected %zu of %zu key%s (%.1f%%) unscored\n",
                crib_alignment_count(), (crib_alignment_count() == 1) ? "" : "s",
                rej, g_keys_analysed, (g_keys_analysed == 1) ? "" : "s",
                g_keys_analysed ? (100.0 * rej / g_keys_analysed) : 0.0);
    }
  fprintf(stderr, "Finished in %.2f s using %d thread%s\n",
          secs, opt_threads, (opt_threads == 1) ? "" : "s");
  fprintf(stderr, "Precomputed %zu rotor table%s (%.1f MB); peak memory %.0f MB\n",
          g_table_count, (g_table_count == 1) ? "" : "s",
          g_table_bytes / (1024.0 * 1024.0), peak_mb);
}
