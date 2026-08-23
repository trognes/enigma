#include "search.h"
#include "exhaust.h"
#include "confidence.h"
#include "schedule.h"

#include "common.h"
#include "crib.h"
#include "dedup.h"
#include "keyspace.h"
#include "machine.h"
#include "options.h"
#include "parallel.h"
#include "plugboard.h"
#include "progress.h"
#include "refine.h"
#include "result.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
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
#include <vector>
#include <unistd.h>
#include <sys/resource.h>

/* Alternation cap for tune_phase(); it converges well before this. */
static const int tune_phase_rounds = 4;
/* Plug-pair cap for the tier-1 IC filter climb. Capping the climb both speeds tier 1
   up (fewer passes per key) and improves rotor-key discrimination: an uncapped climb
   lets wrong keys overfit IC with surplus plugs and bury the true key, so a cap near
   the true plug count ranks it better. ~5 is the measured optimum (both-axes win vs
   uncapped; harmless on easy keyspaces) -- see archived/CODE_REVIEW_HISTORY.md §9 item 2. */
static const int filter_climb_cap = 5;
int g_tk_u, g_tk_w[3], g_tk_r[3], g_tk_g[3];   /* parsed --true-key (numeric) */
static std::vector<float> g_tk_scores;                /* tier-1 IC score per flat key idx */
static std::atomic<size_t> g_tk_idx{static_cast<size_t>(-1)};   /* flat idx of the true key */

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
/* The staged climb with the seed-dedup gate between stage 0 and the rest.

   Spelled out here rather than hooked into run_stages() so that plugboard.cc
   keeps knowing nothing about the filter -- the same reason its own module
   boundary is drawn where it is. The duplication is five lines and is checked
   by construction: with the filter forced to answer "not present" this must be
   byte-identical to the run_stages() path, which is verification check 2.

   Only run_stages<false> is reproduced. The EX=true path belongs to --crib,
   --exhaust and the self-crib seeder, all of which install their own starting
   board and are refused by --seed-dedup for exactly that reason. */
static double staged_with_dedup(machine & m, size_t key_index, bool & skipped)
{
  m.scoring = opt_stages[0].model;
  double s = hillclimb<false>(m, opt_stages[0].cap);

  /* The seed. Everything downstream of it is a deterministic function of this
     board, so if it has been climbed for this key the result would be
     byte-identical and the target climb is pure waste. */
  if (seed_dedup_seen(key_index, m.steckerbrett))
    {
      skipped = true;
      return unit_no_score;
    }

  for (int i = 1; i < opt_nstages; i++)
    {
      m.scoring = opt_stages[i].model;
      s = hillclimb<false>(m, opt_stages[i].cap);
    }
  return s;
}
static double hillclimb_one(machine & m, size_t key_index, int restart)
{
  init_steckerbrett(m, opt_steckerbrett);
  apply_soft_plug(m);            /* before the kick -- see the opt_soft_plug note */
  uint64_t rng = restart_seed(key_index, restart);
  if (opt_restarts >= 1)
    perturb_steckerbrett(m, & rng, opt_perturb);
  double score;
  if (seed_dedup_on())
    {
      bool skipped = false;
      score = staged_with_dedup(m, key_index, skipped);
      /* A skipped item produced no candidate at all, so it returns the same
         sentinel a rejected crib key does and must reach none of the
         per-climb reporting below: dump_all would print a row scoring
         -1e300, and the doubling report would decode a board that was never
         finished. It cannot win the merge either -- unit_no_score is below
         score_min -- and it does not need to: the earlier restart that
         reached this same seed is still in the sweep with a lower work
         index, which is the index better_cand's tie-break already prefers. */
      if (skipped)
        return score;
    }
  else
    score = optimize_once(m, & rng);
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
double climb_unit(machine & m, size_t key_index, int restart)
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
/* Hill-climb the plugboard with optional random restarts. --restarts 0 runs a single climb
   from the configured seed (identity or -s pairs), no kick -- fully deterministic. --restarts
   N runs N climbs, each from the seed plus a fresh --random kick (opt_perturb plug pairs, a
   moderate kick near the typical plug count), keeping the best; the un-kicked seed climb is not
   additionally run (REDESIGN Option A). The rotor-stack mapping[] depends only on the key (not
   the plugboard), so it is reused across restarts; only the steckerbrett is reset each time.
   The RNG is seeded from the flat key index, so the result is independent of -T. Each start
   runs the staged climb. */
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
/* Accounting for the final diagnostic (set by bruteforce). */
size_t g_table_count = 0;
size_t g_table_bytes = 0;
size_t g_keys_analysed = 0;       /* rotor combinations examined */
uint64_t g_plugboards_scored = 0; /* total score_iter calls across workers */
/* Phase 1: fill the table for each wheel-order task pulled off the counter.
   all + i*asize is task i's table (asize rows of [asize][asize][asize]). */
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
static void precompute_worker(machine & m,
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
void search_worker(machine & m,
                   const std::vector<wheel_task> & tasks,
                   const search_range & range,
                   const int * rc, const int * gc,
                   subst_table all,
                   size_t rsize, size_t gsize,
                   std::atomic<size_t> & next_key,
                   size_t chunk_max,
                   size_t idx_end,
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
  const size_t total = idx_end;
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

  /* Hand-outs are sized by DURATION, not by a fixed fraction of the space. A
     chunk of total/(threads*16) is minutes of work on a large sweep -- fine as
     an amortisation of the atomic, ruinous as a tail: the main sweep now joins
     once per restart pass (the pass barrier), so every over-long chunk is idle
     time paid R times over. At the ~1600 climbs/s/thread of a big run the old
     divisor gave ~6-minute chunks, i.e. ~5% of the wall clock in barrier tails.

     Adapting inside the worker rather than precomputing a size handles every
     regime with no calibration pass: a scanned item is ~0.3 us and a climbed
     one ~1 ms, and the same code lands on ~10 s either way. Growth is capped at
     4x per hand-out so one fast chunk cannot overshoot, and chunk_max keeps
     enough chunks in a pass for the threads to balance at the end of it.
     Chunking affects only WHICH thread sees an item, never that item's result,
     so none of this is visible in the output. */
  const double chunk_target_s = 10.0;
  size_t chunk = opt_hillclimb ? 8 : 512;   /* bootstrap: ~10 ms either way */
  if (chunk > chunk_max)
    chunk = chunk_max;

  size_t start;
  while ((start = next_key.fetch_add(chunk)) < total)
    {
      const size_t took = chunk;   /* the stride this hand-out used */
      size_t end = start + took;
      if (end > total)
        end = total;
      const std::chrono::steady_clock::time_point chunk_t0 =
        std::chrono::steady_clock::now();

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

      /* Re-aim at chunk_target_s from what this hand-out actually cost. `end`
         may have been clipped by `total`, so measure the items really done. */
      const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now()
                                      - chunk_t0).count();
      const size_t did = end - start;
      if ((secs > 0.0) && (did > 0))
        {
          double want = static_cast<double>(did) * (chunk_target_s / secs);
          if (want > static_cast<double>(took) * 4.0)
            want = static_cast<double>(took) * 4.0;   /* cap the growth rate */
          if (want < 1.0)
            want = 1.0;
          chunk = static_cast<size_t>(want);
          if (chunk > chunk_max)
            chunk = chunk_max;
        }
    }
  /* The remainder below tick_block, so the last worker to finish takes the line
     to 100% rather than leaving it short by up to 4095 items per thread. */
  if (ticking && (since_tick > 0))
    sweep_progress_tick(since_tick, best);
}
/* Tier 1: rank a slice of the flat key space by a cheap IC climb; keep the
   thread-local top-N, then merge into the shared candidate list. When show_progress
   is set (stderr is a terminal) it also updates a live "\r" progress line: the shared
   'progress' counter tracks keys ranked, and because each atomic add owns a disjoint
   range of that counter, exactly one thread crosses each 1%-of-total boundary and
   prints it -- so the line advances once per percent with no races or duplicates. */
/* --- key pre-filter (-F) ---------------------------------------------------

   With -c, the full plugboard climb (-R restarts x -S stages) is paid on *every*
   key. The pre-filter instead ranks all keys by a single cheap index-of-coincidence
   climb -- which, unlike a plugboard-free IC scan, partially recovers the stecker
   and so discriminates the true rotor key even under a full 10-pair board -- and
   then runs the expensive climb only on the top -F keys. */
static void filter_worker(machine & m,
                          const std::vector<wheel_task> & tasks,
                          const search_range & range,
                          const int * rc, const int * gc,
                          subst_table all, size_t rsize, size_t gsize,
                          std::atomic<size_t> & next_key, size_t chunk,
                          size_t topn,
                          std::mutex & cand_mutex,
                          std::vector<scored_key> & cand,
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
static void finish_worker(machine & m,
                          const std::vector<wheel_task> & tasks,
                          const search_range & range,
                          const int * rc, const int * gc,
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
double bruteforce(char * result, bool allow_empty)
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
    opt_exhaust ? exhaust_unit_count() : climbs_per_key;
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

  /* The filter is sized on the keys THIS sweep will visit and the restarts it
     will run, both known only now. Under --ring-stride that is the COARSE key
     count, which is what total_keys already holds -- the refinement runs with
     the filter off, since it is hundreds of keys against the coarse pass's
     millions and has no duplication worth catching. */
  if (! seed_dedup_init(total_keys, restarts_par))
    fatal("--seed-dedup could not be configured");
  if (seed_dedup_on())
    {
      /* The geometry belongs with the rest of the settings echo, but it cannot
         be computed there: it depends on the resolved keyspace and restart
         count, which build_key_space() decides. Printed here for the same
         reason the middle-wheel collapse line is -- so the figure cannot drift
         from the thing that was actually allocated. */
      char geo[256];
      seed_dedup_describe(geo, sizeof(geo));
      fprintf(stderr, "Seed dedup: %s\n", geo);
    }

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
      /* A ceiling, not the chunk: search_worker grows its hand-outs toward a
         target duration and clamps to this, which keeps at least ~4 chunks per
         thread in a pass so the barrier's tail stays short. */
      size_t schunk = total_keys / (static_cast<size_t>(nthreads) * 4);
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
      /* ONE run_parallel PER RESTART PASS, and its join is the pass barrier.
         Restart is the outer dimension, so pass p is exactly the work range
         [p*total_keys, (p+1)*total_keys) and every key appears in it once.
         Splitting the sweep this way costs nothing on its own -- the items are
         independent and the work index stays global, so which pass a thread is
         in changes nothing about the result -- and it is what lets a per-key
         structure be read and written across passes without a lock: no two
         threads are ever in different passes on the same key.

         Barrier cost is a tail of at most one chunk per thread per pass, which
         the duration-sized hand-outs hold near 10 s against passes measured in
         hours. */
      for (size_t pass = 0; pass < restarts_par; pass++)
        {
          next_key.store(pass * total_keys, std::memory_order_relaxed);
          const size_t pass_end = (pass + 1) * total_keys;
          run_parallel(nthreads, [&](int t)
            { search_worker(*machines[t], tasks, range, rc, gc, all,
                            rsize, gsize, next_key, schunk, pass_end, best); });
        }
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
      /* --ring-stride's refinement (src/refine.h): re-check the ring2 values
         the coarse pass skipped, under the winner reconstructed just above.
         Kept a module of its own because it is the one part of the sweep with
         a separable contract -- and the one with a design document behind it. */
      if (opt_ring_stride > 1)
        {
          /* THE REFINEMENT RUNS UNFILTERED, and saying so is not enough -- it
             has to be enforced. It reuses search_worker over its OWN key
             space, whose indices start again at 0 and therefore ALIAS the
             coarse pass's per-key filter regions: unsuspended it would both
             consume those regions and have its own climbs skipped by seeds it
             never produced. There is nothing to catch there anyway (hundreds
             of keys against the coarse pass's millions). Set and cleared from
             this thread, outside the refinement's own fan-out and join. */
          seed_dedup_suspend(true);
          extra_keys_analysed = refine_ring_stride(machines, tasks, cur_wo, range,
                                                   rc, gc, restarts_par, best);
          seed_dedup_suspend(false);
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
void run_crib_list(char * result)
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