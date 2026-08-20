#include "keyspace.h"

#include "common.h"
#include "crib.h"
#include "machine.h"
#include "plugboard.h"
#include "options.h"
#include "text.h"
#include "wiring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <set>
#include <vector>
#include <stdint.h>

/* Fill a search_range's ring-2 value list from a 26-bit mask (bit v = test ring2 v).
   A mask is how callers naturally express the set -- a stride, a wrapped window, a
   window minus its centre -- and expanding it once here keeps every decode site a
   simple indexed load. Ascending order makes the enumeration deterministic. */
void set_ring2(search_range & r, unsigned int mask)
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
const uint32_t * g_mid_rep_mask = nullptr;

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

bool g_r2_halve = false;
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
size_t work_key(size_t idx, size_t keys)
{
  return (keys > 0) ? (idx % keys) : idx;
}
/* Decode a flat key index and configure the machine for it: switch to the wheel
   order's table only when it changes from cur_wo, set ring/start, reset the
   plugboard and build mapping[]. Fills rg6 = {r1,r2,r3,g1,g2,g3} for showconfig. */
/* Decode a flat key index into `m`. Returns false if the key is collapsed away by the
   middle-wheel reduction (§7.12) -- it decodes byte-identically to a representative that
   IS searched -- in which case `m` is left untouched and no setup_mapping is done. The
   reconstruction callers (--polish, the --ring-stride refinement) always pass an index
   that survived the search, so they never see false. */
bool key_to_machine(machine & m, size_t idx,
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
key_space build_key_space()
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
subst_table allocate_subst_tables(size_t nwo)
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