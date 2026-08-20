/* See refine.h for what this module does, why the offsets are derived rather
   than searched, and the four bugs whose fixes the code below is shaped by. */

#include "refine.h"

#include "common.h"
#include "keyspace.h"
#include "machine.h"
#include "options.h"
#include "parallel.h"
#include "plugboard.h"
#include "progress.h"
#include "result.h"
#include "search.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <cstdlib>
#include <vector>

size_t refine_ring_stride(std::vector<machine *> & machines,
                          const std::vector<wheel_task> & tasks,
                          size_t cur_wo,
                          const search_range & range,
                          const int * rc, const int * gc,
                          size_t restarts_par,
                          best_result & best)
{
  /* machines[0] carries the coarse winner: the caller reconstructed it from
     best.idx once, so that --polish can share the same reconstruction. */
  machine & m = *machines[0];
  size_t extra_keys_analysed = 0;
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
    if (*bp != 0)   /* empty means unset, as elsewhere */
      refine_band = parse_opt_int(bp, "$ENIGMA_REFINE_BAND");
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
    if (*wp != 0)   /* empty means unset, as elsewhere */
    {
      int wk = parse_opt_int(wp, "$ENIGMA_REFINE_WINDOW");
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
  /* Hoist the table pointer OUT of the lambda. search_worker() writes
     m.subst_array on a task boundary, and m is *machines[0] -- so reading
     m.subst_array inside the lambda has thread 0 writing the very field the
     other threads read to build their own argument list. Harmless by value
     (rtasks holds one task, so every write stores the pointer that was already
     there) but a real data race, and TSan reported it as one. */
  subst_table rall = m.subst_array;
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
        { search_worker(*machines[t], rtasks, rrange, rrc, rgc, rall,
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
         key count is just the candidate task count. rall, not m.subst_array:
         the workers above have been writing that field, and this module's own
         rule is not to re-read from m what the candidate list already knows. */
      key_to_machine(m, work_key(rbest.idx, rtasks.size()), rtasks, rrange,
                     rrc, rgc, rall, rrg, rgsize, rrc12, rgc12,
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
  return extra_keys_analysed;
}
