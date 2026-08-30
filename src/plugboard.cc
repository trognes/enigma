#include "plugboard.h"

#include "common.h"
#include "machine.h"
#include "options.h"
#include "scoring.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <algorithm>

/* Still in enigma.cc; moves to the progress/diagnostics module in the next
   step. Called on ACCEPTED climb moves only -- never inside the 325-move
   scoring scans -- so it is nowhere near the hot loop despite being called
   from it. */
void report_climb_progress(machine & m, double score);

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

/* --soft-plug: lay the guessed pairs on the board init_steckerbrett() has just built.
   Deliberately does NOT touch plug_fixed[] -- that is the whole difference from -s. Called
   once per climb (per work item), never per scoring, so the loop is off the hot path; with
   the option unset the first test exits immediately.
     Walks the string two characters at a time rather than indexing by 2*i: an int
   multiplication used as a pointer offset is a clang-tidy error
   (bugprone-implicit-widening-of-multiplication-result), and this is the same pointer
   idiom --no-plug already uses. p[1] is always in range because validation has already
   rejected an odd number of letters. */
void apply_soft_plug(machine & m)
{
  for (const char * p = opt_soft_plug; *p != 0; p += 2)
    {
      int a = char2num(p[0]);
      int b = char2num(p[1]);
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
/* What those fixed letters are plugged TO: the -s partner, or the letter itself for a
   --no-plug letter. plug_fixed alone says a letter is known, not what it is known to be,
   and the crib deduction needs the value so it can reject a hypothesis that contradicts
   it. Set beside plug_fixed and read-only thereafter, like it. */
static unsigned char g_known_plug[asize];
/* Whether ANY letter is fixed at all, so the crib deduction can skip its known-plug
   prologue with a single predictable branch instead of scanning 26 letters. That scan
   cost a measured +50% on the crib benchmark: the sweep calls crib_try 26 times per key
   and most hypotheses die in the first few edges, so the per-hypothesis FIXED cost
   dominates and 26 extra iterations roughly doubled it. Set beside plug_fixed and
   read-only thereafter, like it. */
static bool g_have_known_plugs = false;
#if defined(__clang__)
static thread_local bool plug_fixed_ex[asize];   /* clang: thread_local scratch */
#define PLUG_FIXED_EX plug_fixed_ex
#else
#define PLUG_FIXED_EX m.plug_fixed_ex           /* g++: the machine member declared above */
#endif

void init_plug_fixed(const char * steckerbrett_string, const char * no_plug_string)
{
  for (int j = 0; j < asize; j++)
    {
      plug_fixed[j] = false;
      g_known_plug[j] = static_cast<unsigned char>(j);   /* self-steckered until told */
    }
  int plug_count = static_cast<int>(strlen(steckerbrett_string) / 2);
  for (int i = 0; i < plug_count; i++)
    {
      const int a = char2num(steckerbrett_string[2*i+0]);
      const int b = char2num(steckerbrett_string[2*i+1]);
      plug_fixed[a] = true;
      plug_fixed[b] = true;
      g_known_plug[a] = static_cast<unsigned char>(b);
      g_known_plug[b] = static_cast<unsigned char>(a);
    }
  /* --no-plug letters are fixed in exactly the same sense -- the climb may not rewire
     them -- they are simply fixed to nothing rather than to a partner. Since they stay
     self-steckered, every move set that skips a fixed letter skips them too. */
  for (const char * p = no_plug_string; *p != 0; p++)
    plug_fixed[char2num(*p)] = true;
  g_have_known_plugs = false;
  for (int j = 0; j < asize; j++)
    if (plug_fixed[j])
      g_have_known_plugs = true;
}

/* --- plugboard hill-climb (steckerbrett) -------------------------------- */

/* What a toggle of (a,b) does to the board, WITHOUT applying it: the letters
   whose partner changes and what it changes to. Four cases -- both ends free
   is an ADD (2 letters), already paired a REMOVE (2), one end plugged a MOVE
   of that plug's endpoint (3), both ends plugged to different partners a MERGE
   (4). Every named letter is distinct, which is what hist_probe() relies on.

   This exists for the histogram fast path: scoring by decoding needs the board
   mutated, scoring from the co-occurrence table needs only to be TOLD what
   would change. The letters are the same ones the mutate/restore code below
   computes anyway (x, y, sa, sb), so the plan is free where it is used. */
static inline int toggle_plan(const unsigned char * steck, int a, int b,
                              int * pos, int * val)
{
  const int x = steck[a], y = steck[b];
  pos[0] = a;
  pos[1] = b;
  if (x == b)                            /* already paired: REMOVE */
    {
      val[0] = a;
      val[1] = b;
      return 2;
    }
  val[0] = b;
  val[1] = a;
  if ((x == a) && (y == b))              /* both free: ADD */
    return 2;
  if (x == a)                            /* a free, b plugged: MOVE */
    {
      pos[2] = y;
      val[2] = y;
      return 3;
    }
  if (y == b)                            /* b free, a plugged: MOVE */
    {
      pos[2] = x;
      val[2] = x;
      return 3;
    }
  pos[2] = x;                            /* both plugged: MERGE */
  val[2] = x;
  pos[3] = y;
  val[3] = y;
  return 4;
}

/* Apply an accepted toggle: the histogram FIRST, then the board.

   THE ORDER IS NOT OPTIONAL, and is the whole reason this is one function.
   hist_apply() reads S[pos[k]] as the letter's OLD partner, so moving the
   board first makes it subtract a column that is no longer there -- and that
   drift does not misscore politely, it HANGS the steepest-ascent loop, which
   repeats while best_score > last_best.  Two adjacent statements in a caller
   would work today and rot the first time someone reordered them; here there
   is nothing to reorder.  ENIGMA_HIST=2 checks the invariant after every
   commit, so if it ever does come apart the run says so instead of
   spinning. */
static inline void commit_toggle(machine & m, const int * pos, const int * val,
                                 int cnt, bool hist_on)
{
  if (hist_on)
    hist_apply(m, pos, val, cnt);
  for (int k = 0; k < cnt; k++)
    m.steckerbrett[pos[k]] = static_cast<unsigned char>(val[k]);
  if (hist_on)
    hist_verify(m);
}

/* Last-resort "re-pair" move: take two existing plugs {a-x},{b-y} to the OTHER
   pairing of their four letters ({a-b,x-y} or {a-y,x-b}), keeping the plug count. A
   single switch cannot reach these (it would first drop to one plug, often a worse
   intermediate the greedy climb never takes), so this crosses a barrier two single
   moves cannot. It is run only once the cheap swap/remove moves have converged -- a
   handful of times per climb, not every pass -- so its O(plugs^2) cost is small.
   Applies and returns true iff the single best re-pair strictly beats cur_score. */
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

  /* The re-pair is a 4-position change with the plug count unchanged, so it
     has the same histogram form the toggle operator does (-S i/m/k). It fires
     at every convergence, and at ten plugs that is 2 x 45 scorings against a
     pass's 325, so it is worth the same fast path. The caller built the
     table. */
  const bool hist_on = hist_model(m.scoring);

  for (int i = 0; i < np; i++)
    for (int j = i + 1; j < np; j++)
      {
        int a = plo[i], x = phi[i], b = plo[j], y = phi[j];

        int p1[4], v1[4], p2[4], v2[4];
        p1[0] = a; v1[0] = b; p1[1] = b; v1[1] = a;
        p1[2] = x; v1[2] = y; p1[3] = y; v1[3] = x;
        p2[0] = a; v2[0] = y; p2[1] = y; v2[1] = a;
        p2[2] = x; v2[2] = b; p2[3] = b; v2[3] = x;

        double s1, s2;
        if (hist_on)
          {
            s1 = hist_probe(m, p1, v1, 4);
            s2 = hist_probe(m, p2, v2, 4);
          }
        else
          {
            /* M1: {a-b, x-y} */
            m.steckerbrett[a] = b; m.steckerbrett[b] = a;
            m.steckerbrett[x] = y; m.steckerbrett[y] = x;
            s1 = score_iter(m);

            /* M2: {a-y, x-b} */
            m.steckerbrett[a] = y; m.steckerbrett[y] = a;
            m.steckerbrett[x] = b; m.steckerbrett[b] = x;
            s2 = score_iter(m);

            /* restore {a-x, b-y} */
            m.steckerbrett[a] = x; m.steckerbrett[x] = a;
            m.steckerbrett[b] = y; m.steckerbrett[y] = b;
          }

        if (s1 > best)
          {
            best = s1; found = true;
            memcpy(rp_pos, p1, sizeof rp_pos);
            memcpy(rp_val, v1, sizeof rp_val);
          }
        if (s2 > best)
          {
            best = s2; found = true;
            memcpy(rp_pos, p2, sizeof rp_pos);
            memcpy(rp_val, v2, sizeof rp_val);
          }
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

template<bool EX> double hillclimb(machine & m, int max_pairs);   /* fwd: reclimb below */

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

  /* Histogram form for -S i/m/k (see hist_probe in scoring.cc). The caller
     built the table; this sweep only has to keep the histogram in step with
     the board, which it does on acceptance. */
  const bool hist_on = hist_model(m.scoring);
  if (hist_on)
    hist_resync(m);

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
    if (hist_on)
      {
        int pos[4], val[4];
        const int cnt = toggle_plan(steck, a, b, pos, val);
        return hist_probe(m, pos, val, cnt);
      }
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
  /* -K is that same climb with the scan ranked by the INDEX OF COINCIDENCE,
     computed from the co-occurrence table in O(26) per move, instead of by the
     target model at O(L) per move.

     ONLY WORTH ANYTHING WHEN hist_on IS FALSE.  With a low-order target
     probe_toggle already takes the histogram path, so the scan is O(26)
     AND exact; approximating it there would cost accuracy for nothing.  The
     case this addresses is the RECOMMENDED recipe -- a fused or quad target,
     where every one of the 325 probes is a full decode.  Measured at
     `-f -c -S f10 -R 64` with a 10-pair board, the scan is 20 800 of 91 451
     plugboards scored at L=100 (22.7%) and of 98 081 at L=167 (21.2%), and
     its share GROWS with length because it is linear in L where the
     histogram form is flat.

     IT IS A SEARCH CHANGE, NOT A SPEEDUP: ordering by IC is not ordering by
     the target, so the climb visits moves in a different order and can
     converge somewhere else.  Hence an option, defaulting to the existing
     behaviour, so the two can be A/B'd on recovery rather than assumed.

     MEASURED over 24 cells, 300 paired trials each, on authentic telegraphic
     German at L = 60..120 (eval/results-jorder.txt, per-cell data in
     eval/results-jorder-cells.tsv).  Judged on BREAK50.  IT SPLITS BY
     SCHEDULE, and the split is the obvious one:

       schedule  break50 target/ic  Stouffer Z  plugboards
       f10          629 / 766         -5.81      -25.1%   (8 cells, 2 seeds)
       m4f10        481 / 510         -2.32      -11.5%
       k4f10        498 / 496         +0.41      -11.0%
       i4f10        431 / 431         +0.10      -11.1%

     THE LAST COLUMN IS PLUGBOARDS SCORED, NOT COMPUTE.  The O(26) ranking
     below runs OUTSIDE the counted score loop, so score_iter prices the scans
     this removes and not the work it adds.  On wall time f10 is -13..-17%,
     and the ratio is trajectory-dependent -- k4f10 at L=100 read -3.3% on one
     fixture and -19.7% on another, the arms converging in different numbers
     of moves.

     The schedules that gain are exactly those whose pre-pass does not already
     feed IC into the climb.  Where it does (k4, i4) an IC order adds nothing;
     where it does not (none, mono) it carries information the climb lacks.

     ON A BARE FUSED TARGET IT IS BOTH CHEAPER AND BETTER, not a trade: eight
     of eight cells favour it, the effect grows monotonically with length in
     both seeds, and at L=120 the strongest cell breaks 182 of 300 against 140
     (z = -4.70; mean 52.3 -> 64.6 and exact 128 -> 165, as secondary) while
     scoring 25% fewer plugboards (~15% less wall time).  The saving splits the
     same way as the quality and for the same cause: probe_toggle is already
     O(26) for a low-order pre-pass stage, so only the fused target's scans
     are ever replaced.

     DEFAULT OFF pending prose, which is unmeasured and which CLAUDE.md
     records does not follow telegraphic results for scoring changes.  Nothing
     measured is worse. */
  const bool ic_order = dyn_order && ! hist_on && (opt_ic_order != 0);
  int visit[nmoves];
  if (dyn_order)
    {
      double sc[nmoves];
      if (ic_order)
        {
          /* T depends on (rows, ciphertext) and not on the board, so one
             build serves all 325 probes. Not built for this model otherwise
             -- hillclimb() only calls cooc_build when hist_on. */
          cooc_build(m);
          int n[asize];
          for (int y = 0; y < asize; y++)
            n[y] = 0;
          for (int c = 0; c < asize; c++)
            {
              const uint16_t * const col = cooc_col(c, steck[c]);
              for (int y = 0; y < asize; y++)
                n[y] += col[y];
            }
          for (int mv = 0; mv < nmoves; mv++)
            {
              const int a = P.a[mv], b = P.b[mv];
              visit[mv] = mv;
              if ((pf[a] || pf[b]) || cap_blocks(a, b))
                { sc[mv] = -1e300; continue; }
              int pos[4], val[4];
              const int cnt = toggle_plan(steck, a, b, pos, val);
              int nn[asize];
              for (int y = 0; y < asize; y++)
                nn[y] = n[y];
              for (int k = 0; k < cnt; k++)
                {
                  const int p0 = pos[k];
                  const uint16_t * __restrict rm = cooc_col(p0, steck[p0]);
                  const uint16_t * __restrict ad = cooc_col(p0, val[k]);
                  for (int y = 0; y < asize; y++)
                    nn[y] += ad[y] - rm[y];
                }
              long coin = 0;
              for (int y = 0; y < asize; y++)
                coin += static_cast<long>(nn[y]) * (nn[y] - 1);
              /* The IC divisor is a positive constant here, so ranking on the
                 raw coincidence count orders identically and avoids a divide
                 per move. Higher is better, as for the target-model score. */
              sc[mv] = static_cast<double>(coin);
            }
        }
      else
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

      if (hist_on)
        {
          /* Probe without touching the board, then apply only if it wins --
             the reverse of the mutate-then-maybe-restore below, and the same
             decision on the same score. */
          int pos[4], val[4];
          const int cnt = toggle_plan(steck, a, b, pos, val);
          const double s = hist_probe(m, pos, val, cnt);
          if (s > cur)
            {
              commit_toggle(m, pos, val, cnt, true);
              cur = s;
              improved = true;
            }
        }
      else if (steck[a] == b)                        /* REMOVE a-b */
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
double hillclimb(machine & m, int max_pairs)
{
  const bool * __restrict pf = EX ? PLUG_FIXED_EX : plug_fixed;
  /* -J: circular first-improvement instead of steepest ascent (off by default, so the
     baseline is byte-identical). */
  const bool firstimp = (opt_firstimprove != 0);

  /* Histogram form for the low-order stages (-S i/m/k): byte-identical, and
     O(26) per probe instead of O(L). The table depends on the KEY, so it is
     built here rather than per key -- --tune-phase re-runs setup_mapping
     between climbs, and a stale table would change results silently. */
  const bool hist_on = hist_model(m.scoring);
  if (hist_on)
    cooc_build(m);   /* per CALL, not per key -- a stale table hangs the
                        climb rather than merely misscoring it */

  bool progress;
  do
    {
      progress = false;

      /* Any move kept by the previous round (a re-pair, a cascade) moved the
         board out from under the histogram. */
      if (hist_on)
        hist_resync(m);

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

                    int new_kind = paired ? 1 : 0;
                    double score;

                    if (hist_on)
                      {
                        /* Histogram form: no mutate/restore at all, the plan
                           is enough. Byte-identical to the branch below. */
                        int pos[4], val[4];
                        const int cnt = toggle_plan(m.steckerbrett, a, b,
                                                    pos, val);
                        score = hist_probe(m, pos, val, cnt);
                      }
                    else
                      {
                        int x = 0, y = 0, xx = 0, yy = 0;
                        if (paired)
                          {
                            m.steckerbrett[a] = a;     /* REMOVE a-b */
                            m.steckerbrett[b] = b;
                          }
                        else
                          {
                            x = sa; y = sb;
                            xx = m.steckerbrett[x];
                            yy = m.steckerbrett[y];
                            /* force a-b: ADD / MOVE / MERGE */
                            m.steckerbrett[x] = x;
                            m.steckerbrett[y] = y;
                            m.steckerbrett[a] = b;
                            m.steckerbrett[b] = a;
                          }

                        score = score_iter(m);

                        /* Restore before the comparison rather than after it:
                           the comparison reads no board state, so this is a
                           pure reordering, and it keeps the mutate/restore
                           pair inside the branch that needs it. */
                        if (paired)
                          {
                            m.steckerbrett[a] = b;     /* restore REMOVE */
                            m.steckerbrett[b] = a;
                          }
                        else
                          {
                            m.steckerbrett[a] = sa;    /* restore force */
                            m.steckerbrett[b] = sb;
                            m.steckerbrett[x] = xx;
                            m.steckerbrett[y] = yy;
                          }
                      }

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
                  }

              if (move_score - best_score > 0)
                {
                  int a = move_a;
                  int b = move_b;

                  /* Every probe restored the board (or, on the histogram
                     path, never touched it), so the plan built here is
                     against the same board the winning probe was scored
                     against -- which is what lets the histogram be committed
                     from it.  toggle_plan reproduces the old remove/switch
                     mutation exactly: a REMOVE is {a,b} -> {a,b}, and the
                     force cases eject x and y to themselves before pairing
                     a with b. */
                  int pos[4], val[4];
                  const int cnt = toggle_plan(m.steckerbrett, a, b, pos, val);
                  commit_toggle(m, pos, val, cnt, hist_on);

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

/* Inject exactly k random plug pairs into the current plugboard, drawing only from
   letters that are still unplugged AND not fixed (so -s pairs are preserved, and so are
   --no-plug letters, which are self-steckered and would otherwise look free here). This is
   the per-restart perturbation: a kick of k random plugs (default_perturb, or an rN
   token) into a new basin, near the typical plug count so the staged climb need not
   tear down a near-saturated board (CODE_REVIEW §9). With k=0 it is a no-op (so r0
   makes restarts identical -- a useful control). */
/* ------------------------------------------------------ --biased-random --- */

/* Draw the restart kick's pairs with probability exp(z / T) over the 325
   single-plug INDEX-OF-COINCIDENCE z-scores, instead of uniformly.

   WHY THERE IS ANYTHING TO BIAS TOWARD.  A true plug is enriched in the top of
   that ranking -- 12.1% of true plugs land in the top 10 of 325 against 3.1%
   by chance at L = 100, and the mean rank of a true plug is 110 of 325 against
   163 (eval/proto_plugrank.cc).  So a plug's own IC says something about
   whether that cable is on the board, even though the JOINT four-plug IC
   argmax is a decoy that outscores the truth (eval/results-seed-landscape.txt).
   Sampling rather than maximising is what exploits the first without walking
   into the second, which is why this is a weighted kick and not a beam.

   WHERE IT PAYS, AND WHERE IT DOES NOT.  Measured worth ~+8% of breaks at
   R = 3..5 and ~+28% at R = 1, and NOTHING from about R = 6 up: the bias
   substitutes for the diversity that restarts would otherwise supply, so it
   fades exactly as restarts become plentiful.  Every number is at L = 100 on
   the plugboard tier with the rotor key given.  eval/results-weighted-kick.txt.

   THE SCORES COME FROM A CO-OCCURRENCE TABLE, NOT FROM DECODING.  decode_at is
   steck[rows[i][steck[ct[i]]]], and IC is a function of the letter-count
   MULTISET, which the exit plugboard only permutes -- so IC depends on the
   board through one 26-vector, n(q) = sum_c T[c][S[c]], where
   T[c][d][y] = #{i : ct_i == c, rows_i[d] == y}.  T is built once per key in
   L*26 increments and a single-plug board's histogram is then an O(26) delta
   from the empty one.  All 325 scores cost ~15 us at L = 100, against ~44 us
   of ordinary decoding, and the T form is FLAT in message length where
   decoding is linear (eval/results-tseed-proto.txt).

   THAT TABLE IS NOW SHARED with the low-order climb stages, which score off
   the same identity (cooc_build / cooc_col in scoring.cc).  It used to be
   private here; two copies would be 68 KB of TLS per worker for one table. */

static const int bias_npairs = asize * (asize - 1) / 2;   /* 325 */

/* PLAIN ARRAYS AND NO CONSTRUCTOR, deliberately.  A std::vector member gives
   the struct a throwing constructor, and clang-tidy rejects that at
   thread_local storage duration (bugprone-throwing-static-initialization) --
   an exception thrown there cannot be caught.  Trivial members are
   zero-initialised instead. */
struct biasscratch
{
  double cum[bias_npairs];            /* prefix sums of exp(z / T) */
  unsigned char pa[bias_npairs];
  unsigned char pb[bias_npairs];
};

static thread_local biasscratch bias_scratch;

/* Rebuild the table and the weights for the key the machine is currently set
   to. Called once per work item, before the kick. */
void biased_kick_prepare(machine & m)
{
  biasscratch & b = bias_scratch;
  cooc_build(m);

  double sc[bias_npairs];
  cooc_plug_scores(m, kick_rank_model(), sc);
  const int n = bias_npairs;
  {
    int i = 0;
    for (int a = 0; a < asize; a++)
      for (int bb = a + 1; bb < asize; bb++)
        {
          b.pa[i] = static_cast<unsigned char>(a);
          b.pb[i] = static_cast<unsigned char>(bb);
          i++;
        }
  }

  /* z-score, then softmax. Z-scoring is what makes one temperature mean the
     same thing for either ranking model: IC's absolute level wanders by key
     and k's units are not IC's at all, but the SPREAD is what carries the
     signal in both. */
  double mu = 0;
  for (int i = 0; i < n; i++)
    {
      mu += sc[i];
    }
  mu /= n;
  double var = 0;
  for (int i = 0; i < n; i++)
    {
      var += (sc[i] - mu) * (sc[i] - mu);
    }
  const double sd = sqrt(var / (n - 1));
  double run = 0;
  for (int i = 0; i < n; i++)
    {
      /* A degenerate spread (every pair scoring alike) falls back to uniform
         rather than dividing by ~0 and producing inf weights. */
      const double w = (sd > 0.0)
                         ? exp(((sc[i] - mu) / sd) / opt_biased_random) : 1.0;
      run += w;
      b.cum[i] = run;
    }
}

/* The biased counterpart of perturb_steckerbrett(): same contract -- k pairs
   among letters that are free (self-steckered and not plug_fixed), fewer if
   the board cannot supply them -- drawn from the weights instead of uniformly.

   REJECTION, NOT RENORMALISATION.  Redrawing on a conflict IS drawing from the
   legal set renormalised, so the two are the same distribution and the choice
   is pure cost; rejection is ~6 draws for 4 pairs at T >= 1 where an explicit
   renormalise is 4 x 325.  The exact pass is kept as a FALLBACK because
   rejection degrades as the board fills and as T falls -- and its failure mode
   is silent truncation, a kick that returns fewer pairs than asked, which
   would change what is being measured rather than how long it takes. */
void biased_perturb(machine & m, uint64_t * rng, int k)
{
  const biasscratch & b = bias_scratch;
  const double total = b.cum[bias_npairs - 1];
  if (!(total > 0.0))
    {
      perturb_steckerbrett(m, rng, k);      /* degenerate: keep the contract */
      return;
    }

  for (int got = 0; got < k; got++)
    {
      int pick = -1;
      for (int tries = 0; tries < 32; tries++)
        {
          const double x = (static_cast<double>(splitmix64(rng) >> 11)
                            / 9007199254740992.0) * total;
          const int i = static_cast<int>(
            std::lower_bound(b.cum, b.cum + bias_npairs, x) - b.cum);
          if (i >= bias_npairs)
            {
              continue;
            }
          const int aa = b.pa[i], bb = b.pb[i];
          if ((m.steckerbrett[aa] != aa) || (m.steckerbrett[bb] != bb)
              || plug_fixed[aa] || plug_fixed[bb])
            {
              continue;                     /* not free: reject and redraw */
            }
          pick = i;
          break;
        }
      if (pick < 0)
        {
          double tot = 0;
          for (int i = 0; i < bias_npairs; i++)
            {
              const int aa = b.pa[i], bb = b.pb[i];
              if ((m.steckerbrett[aa] == aa) && (m.steckerbrett[bb] == bb)
                  && (! plug_fixed[aa]) && (! plug_fixed[bb]))
                {
                  tot += (i > 0) ? (b.cum[i] - b.cum[i - 1]) : b.cum[0];
                }
            }
          if (!(tot > 0.0))
            {
              return;                       /* no free pair left */
            }
          double x = (static_cast<double>(splitmix64(rng) >> 11)
                      / 9007199254740992.0) * tot;
          for (int i = 0; i < bias_npairs; i++)
            {
              const int aa = b.pa[i], bb = b.pb[i];
              if ((m.steckerbrett[aa] != aa) || (m.steckerbrett[bb] != bb)
                  || plug_fixed[aa] || plug_fixed[bb])
                {
                  continue;
                }
              x -= (i > 0) ? (b.cum[i] - b.cum[i - 1]) : b.cum[0];
              if (x <= 0)
                {
                  pick = i;
                  break;
                }
            }
          if (pick < 0)
            {
              return;
            }
        }
      const int aa = b.pa[pick], bb = b.pb[pick];
      m.steckerbrett[aa] = static_cast<unsigned char>(bb);
      m.steckerbrett[bb] = static_cast<unsigned char>(aa);
    }
}

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

/* Staged plugboard climb: run each schedule stage in order, capping the plug pairs
   it may set. A lower-order model has a far smoother surface when only a plug or two
   are set, so an early stage steers the first plugs into a good basin that a
   single-model climb navigates poorly -- staging reshapes the *search* landscape
   (complementary to random restarts). The returned score and m.plaintext are in the
   target (last) model, so cross-key comparison is unaffected. With no -S this is a
   single uncapped climb in the -i/-m/.../-q model. */
template<bool EX>
double run_stages(machine & m)
{
  double s = 0.0;
  for (int i = 0; i < opt_nstages; i++)
    {
      m.scoring = opt_stages[i].model;
      s = hillclimb<EX>(m, opt_stages[i].cap);
    }
  return s;   /* opt_nstages >= 1, so s is the target-model score */
}


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
double optimize_once(machine & m, uint64_t * rng)
{
  if (opt_anneal > 0)
    return anneal_once(m, rng);
  return run_stages<false>(m);
}


/* The climb chain is a template on EX -- whether the fixed-plug set is the
   global -s/--no-plug mark or a per-worker copy carrying additionally forced
   pairs. Naming an instantiation here is what lets a caller in another unit
   reach it without the definitions leaving this file, which is what keeps
   plug_fixed and plug_fixed_ex a plain TU-local global and a plain
   static thread_local respectively.

   Only the two that ARE called from another unit are named: search.cc climbs
   through hillclimb<false>, and --exhaust / the crib hybrid / the self-crib
   seeder all enter through run_stages<true>. hillclimb<true> and
   run_stages<false> have no caller outside this file -- run_stages<true>
   calls the first, optimize_once the second -- so they instantiate implicitly
   and stay internal. Exporting them cost nothing and bought nothing; leaving
   them out lets the compiler treat them as the TU-local functions they are. */
template double hillclimb<false>(machine & m, int max_pairs);
template double run_stages<true>(machine & m);

/* The three sites that install extra pins -- --exhaust, the crib hybrid and
   the self-crib seeder -- live in other modules and must not touch the
   storage directly: under clang it is a static thread_local whose access
   model would weaken if it were declared extern, in the one loop where the
   recorded cost of getting this variable's storage wrong is ~18%. */
void plug_fixed_ex_reset(machine & m)
{
  memcpy(PLUG_FIXED_EX, plug_fixed, asize);
  (void) m;
}

void plug_fixed_ex_pin(machine & m, int letter)
{
  PLUG_FIXED_EX[letter] = true;
  (void) m;
}

/* A read-only view of the -s / --no-plug knowledge, for the crib deduction:
   it seeds its closure from whatever is already known, so a hypothesis that
   contradicts a pin dies immediately instead of being climbed. Read once per
   hypothesis and only when there IS knowledge -- the g_have_known_plugs gate
   is load-bearing, since an unconditional 26-letter prologue in crib_try cost
   +50% on a crib sweep. Not the climb's access path, which stays a plain
   global read inside this file. */
bool have_known_plugs()
{
  return g_have_known_plugs;
}

const bool * known_plug_mark()
{
  return plug_fixed;
}

const unsigned char * known_plug_partner()
{
  return g_known_plug;
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
/* Independent RNG seed for one restart, mixed from opt_seed, the flat key index and the
   restart index with a splitmix64 finaliser. Each restart draws from its OWN stream --
   not a single stream advanced sequentially through the restarts -- so restarts are
   order-independent and can run in any order / on any thread and still be reproducible
   (the precondition for parallelising them; see hillclimb_one). opt_seed==0 keeps the
   historical seedless-but-deterministic behaviour. */
uint64_t restart_seed(size_t key_index, int restart)
{
  uint64_t z = opt_seed + 0x0123456789abcdefULL
             + static_cast<uint64_t>(key_index) * 0x9E3779B97F4A7C15ULL
             + static_cast<uint64_t>(restart)   * 0xC2B2AE3D27D4EB4FULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
