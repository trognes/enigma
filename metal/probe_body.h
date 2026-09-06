/* metal/probe_body.h -- DESIGN.md 17.6 (ii): the probe microkernel.

   ONE toggle probe -- apply a plugboard toggle, score the board -- in two
   shapes, repeated thousands of times from the same start with the same
   deterministic toggle sequence, so the two arms do identical work and
   their rate ratio is the per-character step-latency ratio that section
   17.4 rests on and nothing else can measure.

     arm LANE   today's shape: one lane per probe, steck[26] and freq[26]
                in thread memory, L characters decoded serially.  This is
                mc_components() from climb_body.h, exactly, so it also
                runs on the CPU backend, where the driver checks it.
     arm GROUP  the 17.4 shape: K lanes per probe (K = 8, 16 or 32), the
                board replicated in every lane as five packed words
                (PACKED) or spread one entry per lane and read by shuffle
                (SHUF, K = 32 only), each lane decoding L/K characters,
                the per-lane isum and a 9-word packed histogram reduced
                across the K lanes by an xor butterfly, and the three
                quads that straddle a lane boundary scored from the
                previous lane's last three letters fetched by shuffle-up.
                No per-lane array anywhere: nothing a compiler could put
                in thread memory.

   Each unit -- a lane in arm LANE, a K-lane group in arm GROUP -- sums
   isum and coin over all its probes into two int64 checksums the driver
   compares with the tool's own score_components() replaying the same
   sequence.  A wrong arm therefore prints FAIL beside its number rather
   than a number, which is what lets the number be believed.

   Plain integer C, like climb_body.h.  The group body needs three
   cross-lane operations, which the wrapper supplies as macros:
   MC_SHFL(v, lane), MC_SHFL_UP(v, delta), MC_SHFL_XOR(v, mask) -- Metal's
   simd_shuffle / simd_shuffle_up / simd_shuffle_xor, CUDA's __shfl_sync
   family -- and is compiled only under MC_HAVE_SIMD.  The packed-board
   and histogram helpers are ordinary C and are unit-tested on the CPU. */

#ifndef ENIGMA_PROBE_BODY_H
#define ENIGMA_PROBE_BODY_H

#include "climb_body.h"

#define MC_PROBE_LANE 0     /* today's shape */
#define MC_PROBE_G1 1       /* today's decomposition, board and histogram
                               in registers: the group template at K = 1 */
#define MC_PROBE_PASS 2     /* one steepest-ascent pass per unit: the
                               climb's own scan loop, mc_pass() */
#define MC_PROBE_PASSPK 3   /* the same scan loop on the PACKED board:
                               mc_pass_pk(), the candidate climb */
#define MC_PROBE_G32S 4     /* group, K = 32, board by shuffle */
#define MC_PROBE_G32 5      /* group, K = 32, board packed */
#define MC_PROBE_G16 6      /* group, K = 16, board packed */
#define MC_PROBE_G8 7       /* group, K = 8, board packed */
#define MC_PROBE_ARMS 8
/* Lanes per unit, per arm; every wrapper and the driver index this list
   rather than carrying one of their own. */
#define MC_PROBE_K_LIST { 1, 1, 1, 1, 32, 32, 16, 8 }

/* THE TWO LANE-SHAPED ARMS BESIDE TODAY'S exist because of run 2: the
   scorer alone ran 66.7M probes/s where the fixed-pass climb, doing the
   same scoring 326 times a pass, ran 22.3M -- 3x, with the start-board
   confound excluded.  G1 asks whether the thread-memory board and
   histogram matter at all once the scan is gone (it is the group shape
   with no lanes to share, so no shuffles and no reductions).  PASS runs
   the real scan loop from climb_body.h -- MC_PROBE_PASSES(n) passes of
   MC_PROBE_PASS_PROBES scored boards each, n/4 so that the default 64
   gives 16 passes, exactly one fixed-pass climb's work -- and so prices
   the machinery around the scorer in the same harness as the scorer;
   its probes/s should reproduce the ablation table's, which is a check
   on both instruments.  Its checksum is body-against-body (mc_pass on
   the CPU), which verify_identity.py ties to the tool's climb. */
#define MC_PROBE_PASS_PROBES 326   /* the base score and 325 toggles */
#define MC_PROBE_PASSES(n) ((((n) / 4) > 0) ? ((n) / 4) : 1)

/* One dispatch's constants; every member 64-bit, as mc_params. */
typedef struct
{
  mc_i64 L;
  mc_i64 model;         /* the target stage's model */
  mc_i64 nprobes;       /* toggles per unit; passes via MC_PROBE_PASSES */
  mc_i64 units;         /* lanes (arm LANE) or K-lane groups (arm GROUP) */
  mc_i64 lanes_per_tg;  /* a multiple of 32 for the group arms */
  mc_i64 nboards;       /* distinct start boards; unit u takes u % nboards */
  mc_i64 A;             /* the --int weights of the target stage, for */
  mc_i64 B;             /* the pass arm's mc_key */
} mc_probe_params;

/* WHY THERE ARE MANY START BOARDS.  The first run of this instrument gave
   every unit the SAME board, and under the same toggle sequence every
   lane of a simdgroup then read the same rows[] entry and gathered the
   same table cell at every character -- a broadcast and one cache line,
   where a climb's 32 lanes hold 32 boards and touch 32 lines.  The lane
   arm read 71M probes/s against 22M for the fixed-pass climb doing the
   same scoring: flattered, by an amount the run could not separate from
   the climb's own scan overhead.  The group arms gain nothing from it,
   their lanes reading different positions by design, so the ratio was
   biased against them.  With unit u on board u % nboards and nboards a
   multiple of 32, a simdgroup holds 32 boards as a climb does. */
#define MC_PROBE_BOARDS 64

/* --- the toggle sequence -------------------------------------------------
   Deterministic in t, so the driver can replay it.  The two indices
   collide at t = 12 mod 13 (7t+1 = 11t+5 mod 26), hence the fix-up. */
inline void mc_probe_toggle(int t, MC_THR int * a, MC_THR int * b)
{
  const int x = (t * 7 + 1) % MC_ASIZE;
  int y = (t * 11 + 5) % MC_ASIZE;
  if (x == y)
    y = (y + 1) % MC_ASIZE;
  *a = x;
  *b = y;
}

/* The climb's toggle operator on an array board: remove a-b if they are
   paired, else free both ends' partners and pair a-b.  What mc_hillclimb
   commits, so the boards the probe scores are the boards a climb scores. */
inline void mc_probe_apply(MC_THR unsigned char * steck, int a, int b)
{
  const int x = steck[a];
  const int y = steck[b];
  if (x == b)
    {
      steck[a] = (unsigned char) a;
      steck[b] = (unsigned char) b;
    }
  else
    {
      steck[x] = (unsigned char) x;
      steck[y] = (unsigned char) y;
      steck[a] = (unsigned char) b;
      steck[b] = (unsigned char) a;
    }
}

/* --- arm LANE ---------------------------------------------------------- */
inline void mc_probe_lane(MC_THR unsigned char * steck,
                          MC_TG_CONST unsigned char * rows,
                          MC_TG_CONST unsigned char * ct,
                          int L, int model,
                          MC_DEV_CONST unsigned char * tbl,
                          int nprobes,
                          MC_THR mc_i64 * ck_isum, MC_THR mc_i64 * ck_coin)
{
  mc_i64 cs = 0;
  mc_i64 cc = 0;
  for (int t = 0; t < nprobes; t++)
    {
      int a;
      int b;
      mc_probe_toggle(t, & a, & b);
      mc_probe_apply(steck, a, b);
      mc_i64 isum = 0;
      mc_i64 coin = 0;
      mc_components(steck, rows, ct, L, model, tbl, & isum, & coin);
      cs += isum;
      cc += coin;
    }
  *ck_isum = cs;
  *ck_coin = cc;
}

/* --- arm PASS -------------------------------------------------------------
   Uncapped, no pins, no -M: the shape of the target stage's climb. */
inline void mc_probe_pass(MC_THR unsigned char * steck,
                          MC_TG_CONST unsigned char * rows,
                          MC_TG_CONST unsigned char * ct,
                          int L, int model,
                          MC_DEV_CONST unsigned char * tbl,
                          mc_i64 A, mc_i64 B, int npasses,
                          MC_THR mc_i64 * ck_isum, MC_THR mc_i64 * ck_coin)
{
  mc_i64 cs = 0;
  mc_i64 cc = 0;
  for (int t = 0; t < npasses; t++)
    {
      mc_i64 s = 0;
      mc_pass(steck, rows, ct, L, model, tbl, A, B, 0u, MC_ASIZE / 2, 0,
              & s);
      cs += s;
      cc += (mc_i64) mc_plug_count(steck);
    }
  *ck_isum = cs;
  *ck_coin = cc;
}

/* --- the packed board ----------------------------------------------------
   26 five-bit entries, six to a word, five words, held by NAME so that no
   dynamic index can send them to memory: every access is a select chain
   over the five, which is ALU and nothing else.  x / 6 as (x * 43) >> 8,
   exact for 0 <= x <= 25. */
typedef struct
{
  unsigned int w0;
  unsigned int w1;
  unsigned int w2;
  unsigned int w3;
  unsigned int w4;
} mc_pk;

inline unsigned int mc_pk_word(MC_THR_CONST mc_pk * p, int q)
{
  unsigned int w = p->w0;
  w = (q == 1) ? p->w1 : w;
  w = (q == 2) ? p->w2 : w;
  w = (q == 3) ? p->w3 : w;
  w = (q == 4) ? p->w4 : w;
  return w;
}

inline int mc_pk_get(MC_THR_CONST mc_pk * p, int x)
{
  const int q = (x * 43) >> 8;
  const int r = x - 6 * q;
  return (int) ((mc_pk_word(p, q) >> (5 * r)) & 31u);
}

inline void mc_pk_set(MC_THR mc_pk * p, int x, int v)
{
  const int q = (x * 43) >> 8;
  const int r = x - 6 * q;
  const unsigned int m = 31u << (5 * r);
  const unsigned int s = ((unsigned int) v) << (5 * r);
  p->w0 = (q == 0) ? ((p->w0 & ~m) | s) : p->w0;
  p->w1 = (q == 1) ? ((p->w1 & ~m) | s) : p->w1;
  p->w2 = (q == 2) ? ((p->w2 & ~m) | s) : p->w2;
  p->w3 = (q == 3) ? ((p->w3 & ~m) | s) : p->w3;
  p->w4 = (q == 4) ? ((p->w4 & ~m) | s) : p->w4;
}

inline void mc_pk_pack(MC_THR mc_pk * p, MC_DEV_CONST unsigned char * steck)
{
  p->w0 = 0u;
  p->w1 = 0u;
  p->w2 = 0u;
  p->w3 = 0u;
  p->w4 = 0u;
  for (int x = 0; x < MC_ASIZE; x++)
    mc_pk_set(p, x, steck[x]);
}

/* mc_probe_apply on the packed board.  The test is uniform across a
   group, since every lane holds the same board. */
inline void mc_pk_apply(MC_THR mc_pk * p, int a, int b)
{
  const int x = mc_pk_get(p, a);
  const int y = mc_pk_get(p, b);
  if (x == b)
    {
      mc_pk_set(p, a, a);
      mc_pk_set(p, b, b);
    }
  else
    {
      mc_pk_set(p, x, x);
      mc_pk_set(p, y, y);
      mc_pk_set(p, a, b);
      mc_pk_set(p, b, a);
    }
}

/* --- the packed histogram ------------------------------------------------
   27 nine-bit fields, three to a word, nine words: bin d is field d % 3 of
   word d / 3 (as (d * 11) >> 5, exact for 0 <= d <= 25; field 26 stays
   zero).  A lane's fields hold at most L/K <= 32; after the K-lane
   reduction at most L <= 256, under the 511 a field holds, with no carry
   into a neighbour.  coin unpacks by word, never by bin, so nothing is
   dynamically indexed here either. */
typedef struct
{
  unsigned int h0;
  unsigned int h1;
  unsigned int h2;
  unsigned int h3;
  unsigned int h4;
  unsigned int h5;
  unsigned int h6;
  unsigned int h7;
  unsigned int h8;
} mc_h9;

inline void mc_h9_zero(MC_THR mc_h9 * h)
{
  h->h0 = 0u;
  h->h1 = 0u;
  h->h2 = 0u;
  h->h3 = 0u;
  h->h4 = 0u;
  h->h5 = 0u;
  h->h6 = 0u;
  h->h7 = 0u;
  h->h8 = 0u;
}

inline void mc_h9_inc(MC_THR mc_h9 * h, int d)
{
  const int q = (d * 11) >> 5;
  const unsigned int inc = 1u << (9 * (d - 3 * q));
  h->h0 += (q == 0) ? inc : 0u;
  h->h1 += (q == 1) ? inc : 0u;
  h->h2 += (q == 2) ? inc : 0u;
  h->h3 += (q == 3) ? inc : 0u;
  h->h4 += (q == 4) ? inc : 0u;
  h->h5 += (q == 5) ? inc : 0u;
  h->h6 += (q == 6) ? inc : 0u;
  h->h7 += (q == 7) ? inc : 0u;
  h->h8 += (q == 8) ? inc : 0u;
}

/* sum n(n-1) over the three fields of one word */
inline int mc_h9_word_coin(unsigned int w)
{
  const int n0 = (int) (w & 511u);
  const int n1 = (int) ((w >> 9) & 511u);
  const int n2 = (int) ((w >> 18) & 511u);
  return n0 * (n0 - 1) + n1 * (n1 - 1) + n2 * (n2 - 1);
}

inline int mc_h9_coin(MC_THR_CONST mc_h9 * h)
{
  return mc_h9_word_coin(h->h0) + mc_h9_word_coin(h->h1)
    + mc_h9_word_coin(h->h2) + mc_h9_word_coin(h->h3)
    + mc_h9_word_coin(h->h4) + mc_h9_word_coin(h->h5)
    + mc_h9_word_coin(h->h6) + mc_h9_word_coin(h->h7)
    + mc_h9_word_coin(h->h8);
}

/* --- arm PASSPK: the climb's scan loop on the packed board ---------------
   Probe run 4 left the scan at 0.50x the scorer alone with the double
   scoring gone: 1.33x of it is the cap (384 against 512) and the rest the
   mutate and restore writes to a thread-memory board, eight per toggle,
   each ordered before the scorer's reads.  On the packed board those are
   select chains, and the board is five registers.  This is mc_pass()
   transcribed onto mc_pk, one line for one line, with the scorer being
   the K = 1 group decode without the loop -- so if it wins here it is
   the climb's next body, and its checksum against mc_pass() on the
   array board is the proof the two representations agree. */

inline int mc_pk_plug_count(MC_THR_CONST mc_pk * pk)
{
  int n = 0;
  for (int x = 0; x < MC_ASIZE; x++)
    n += (mc_pk_get(pk, x) != x) ? 1 : 0;
  return n / 2;
}

/* mc_components() on the packed board.  The histogram models count into
   mc_h9 and unpack; the quad-shaped ones run the window with the
   histogram beside it for -f.  BI and TRI unpack to an array and take the
   ordinary scorer: correct, slow, and not on the climb's recipe. */
inline void mc_components_pk(MC_THR_CONST mc_pk * pk,
                             MC_TG_CONST unsigned char * rows,
                             MC_TG_CONST unsigned char * ct,
                             int L, int model,
                             MC_DEV_CONST unsigned char * tbl,
                             MC_THR mc_i64 * isum_out,
                             MC_THR mc_i64 * coin_out)
{
  mc_i64 isum = 0;
  mc_i64 coin = 0;
  if ((model == MC_IC) || (model == MC_MONO) || (model == MC_MONOIC))
    {
      mc_h9 h;
      mc_h9_zero(& h);
      for (int i = 0; i < L; i++)
        {
          const int s1 = mc_pk_get(pk, ct[i]);
          const int d = mc_pk_get(pk, rows[i * MC_ASIZE + s1]);
          mc_h9_inc(& h, d);
        }
      /* bin d = field d % 3 of word d / 3 */
      const unsigned int w[9] = { h.h0, h.h1, h.h2, h.h3, h.h4, h.h5,
                                  h.h6, h.h7, h.h8 };
      for (int q = 0; q < 9; q++)
        {
          for (int r = 0; r < 3; r++)
            {
              const int d = 3 * q + r;
              if (d < MC_ASIZE)
                {
                  const int n = (int) ((w[q] >> (9 * r)) & 511u);
                  isum += (mc_i64) n * (mc_i64) tbl[d];
                  coin += (mc_i64) n * (mc_i64) (n - 1);
                }
            }
        }
    }
  else if ((model == MC_BI) || (model == MC_TRI))
    {
      unsigned char st[MC_ASIZE];
      for (int x = 0; x < MC_ASIZE; x++)
        st[x] = (unsigned char) mc_pk_get(pk, x);
      mc_components(st, rows, ct, L, model, tbl, & isum, & coin);
    }
  else
    {
      if (L >= 4)
        {
          const bool fused = (model == MC_FUSED);
          mc_h9 h;
          mc_h9_zero(& h);
          int wa = 0;
          int wb = 0;
          int wc = 0;
          int is = 0;
          for (int i = 0; i < L; i++)
            {
              const int s1 = mc_pk_get(pk, ct[i]);
              const int d = mc_pk_get(pk, rows[i * MC_ASIZE + s1]);
              if (fused)
                mc_h9_inc(& h, d);
              if (i >= 3)
                is += (int) tbl[((wa * MC_ASIZE + wb) * MC_ASIZE + wc)
                                * MC_ASIZE + d];
              wa = wb;
              wb = wc;
              wc = d;
            }
          isum = (mc_i64) is;
          if (fused)
            coin = (mc_i64) mc_h9_coin(& h);
        }
    }
  *isum_out = isum;
  *coin_out = coin;
}

inline mc_i64 mc_key_pk(MC_THR_CONST mc_pk * pk,
                        MC_TG_CONST unsigned char * rows,
                        MC_TG_CONST unsigned char * ct,
                        int L, int model,
                        MC_DEV_CONST unsigned char * tbl,
                        mc_i64 A, mc_i64 B)
{
  mc_i64 isum = 0;
  mc_i64 coin = 0;
  mc_components_pk(pk, rows, ct, L, model, tbl, & isum, & coin);
  return A * isum + B * coin;
}

/* mc_pass() on the packed board, line for line. */
inline int mc_pass_pk(MC_THR mc_pk * pk,
                      MC_TG_CONST unsigned char * rows,
                      MC_TG_CONST unsigned char * ct,
                      int L, int model,
                      MC_DEV_CONST unsigned char * tbl,
                      mc_i64 A, mc_i64 B,
                      unsigned int pf, int max_pairs, int capmerge,
                      MC_THR mc_i64 * score_out)
{
  mc_i64 best_score = mc_key_pk(pk, rows, ct, L, model, tbl, A, B);

  const int pairs = mc_pk_plug_count(pk);

  mc_i64 move_score = best_score;
  int move_kind = 0;
  int move_a = 0;
  int move_b = 0;

  for (int a = 0; a < MC_ASIZE; a++)
    {
      for (int b = a + 1; b < MC_ASIZE; b++)
        {
          if ((((pf >> a) & 1u) != 0u) || (((pf >> b) & 1u) != 0u))
            continue;

          const int sa = mc_pk_get(pk, a);
          const int sb = mc_pk_get(pk, b);
          const int a_free = (sa == a);
          const int b_free = (sb == b);
          const int paired = (sa == b);

          if ((pairs >= max_pairs) && ! paired)
            {
              if (a_free && b_free)
                continue;
              if (capmerge && (a_free || b_free))
                continue;
            }

          const int new_kind = paired ? 1 : 0;
          const int x = sa;
          const int y = sb;
          const int xx = mc_pk_get(pk, x);
          const int yy = mc_pk_get(pk, y);
          const int na = paired ? a : b;
          const int nb = paired ? b : a;
          mc_pk_set(pk, x, x);
          mc_pk_set(pk, y, y);
          mc_pk_set(pk, a, na);
          mc_pk_set(pk, b, nb);
          const mc_i64 score = mc_key_pk(pk, rows, ct, L, model, tbl, A, B);
          mc_pk_set(pk, a, sa);
          mc_pk_set(pk, b, sb);
          mc_pk_set(pk, x, xx);
          mc_pk_set(pk, y, yy);

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
    }

  if (move_score > best_score)
    {
      const int a = move_a;
      const int b = move_b;
      if (move_kind == 1)
        {
          mc_pk_set(pk, a, a);
          mc_pk_set(pk, b, b);
        }
      else
        {
          const int x = mc_pk_get(pk, a);
          const int y = mc_pk_get(pk, b);
          mc_pk_set(pk, x, x);
          mc_pk_set(pk, y, y);
          mc_pk_set(pk, a, b);
          mc_pk_set(pk, b, a);
        }
      *score_out = move_score;
      return 1;
    }
  *score_out = best_score;
  return 0;
}

inline void mc_probe_pass_pk(MC_DEV_CONST unsigned char * board0,
                             MC_TG_CONST unsigned char * rows,
                             MC_TG_CONST unsigned char * ct,
                             int L, int model,
                             MC_DEV_CONST unsigned char * tbl,
                             mc_i64 A, mc_i64 B, int npasses,
                             MC_THR mc_i64 * ck_isum,
                             MC_THR mc_i64 * ck_coin)
{
  mc_pk pk;
  mc_pk_pack(& pk, board0);
  mc_i64 cs = 0;
  mc_i64 cc = 0;
  for (int t = 0; t < npasses; t++)
    {
      mc_i64 s = 0;
      mc_pass_pk(& pk, rows, ct, L, model, tbl, A, B, 0u, MC_ASIZE / 2, 0,
                 & s);
      cs += s;
      cc += (mc_i64) mc_pk_plug_count(& pk);
    }
  *ck_isum = cs;
  *ck_coin = cc;
}

/* --- arm GROUP ---------------------------------------------------------- */
#ifdef MC_HAVE_SIMD

/* lane: this thread's index within its 32-wide simdgroup.  A group is K
   consecutive lanes aligned to K, so K divides 32 and the xor butterfly
   over masks < K never leaves it.  Requires L >= 3K, i.e. at least three
   characters per lane, so that a lane's last three letters are its own;
   the host refuses anything shorter.  Lanes past the message (the last
   group member when K does not divide L) run the loop with `live` off so
   every shuffle is executed by every lane. */
template <int K, bool SHUF>
inline void mc_probe_group(unsigned int lane,
                           MC_DEV_CONST unsigned char * board0,
                           MC_TG_CONST unsigned char * rows,
                           MC_TG_CONST unsigned char * ct,
                           int L, int model,
                           MC_DEV_CONST unsigned char * tbl,
                           int nprobes,
                           MC_THR mc_i64 * ck_isum,
                           MC_THR mc_i64 * ck_coin)
{
  const int g = (int) (lane % (unsigned int) K);
  const int C = (L + K - 1) / K;
  const int start = g * C;
  const bool fused = (model == MC_FUSED);

  mc_pk pk;
  mc_pk_pack(& pk, board0);
  int mine = (lane < (unsigned int) MC_ASIZE) ? (int) board0[lane] : 0;

  mc_i64 cs = 0;
  mc_i64 cc = 0;
  for (int t = 0; t < nprobes; t++)
    {
      int a;
      int b;
      mc_probe_toggle(t, & a, & b);
      if (SHUF)
        {
          const int x = MC_SHFL(mine, a);
          const int y = MC_SHFL(mine, b);
          const int me = (int) lane;
          if (x == b)
            {
              mine = ((me == a) || (me == b)) ? me : mine;
            }
          else
            {
              mine = (me == x) ? x : mine;
              mine = (me == y) ? y : mine;
              mine = (me == a) ? b : mine;
              mine = (me == b) ? a : mine;
            }
        }
      else
        {
          mc_pk_apply(& pk, a, b);
        }

      int isum = 0;
      mc_h9 h;
      mc_h9_zero(& h);
      int wa = 0;
      int wb = 0;
      int wc = 0;
      int f0 = 0;
      int f1 = 0;
      int f2 = 0;
      for (int k = 0; k < C; k++)
        {
          const int i = start + k;
          const bool live = (i < L);
          const int ii = live ? i : 0;
          const int y = ct[ii];
          const int s1 = SHUF ? MC_SHFL(mine, y) : mc_pk_get(& pk, y);
          const int z = rows[ii * MC_ASIZE + s1];
          const int d = SHUF ? MC_SHFL(mine, z) : mc_pk_get(& pk, z);
          if (live && fused)
            mc_h9_inc(& h, d);
          f0 = (k == 0) ? d : f0;
          f1 = (k == 1) ? d : f1;
          f2 = (k == 2) ? d : f2;
          if ((k >= 3) && live)
            isum += (int) tbl[((wa * MC_ASIZE + wb) * MC_ASIZE + wc)
                              * MC_ASIZE + d];
          wa = wb;
          wb = wc;
          wc = d;
        }

      /* The three quads straddling the boundary: the previous lane's last
         three letters and this lane's first three.  Lane 0 of a group
         starts the message, where the CPU scores nothing before i = 3. */
      const int p0 = MC_SHFL_UP(wa, 1);
      const int p1 = MC_SHFL_UP(wb, 1);
      const int p2 = MC_SHFL_UP(wc, 1);
      if (g > 0)
        {
          if (start < L)
            isum += (int) tbl[((p0 * MC_ASIZE + p1) * MC_ASIZE + p2)
                              * MC_ASIZE + f0];
          if (start + 1 < L)
            isum += (int) tbl[((p1 * MC_ASIZE + p2) * MC_ASIZE + f0)
                              * MC_ASIZE + f1];
          if (start + 2 < L)
            isum += (int) tbl[((p2 * MC_ASIZE + f0) * MC_ASIZE + f1)
                              * MC_ASIZE + f2];
        }

      /* Reduce over the group: an xor butterfly, every lane ending with
         the total. */
      for (int m = 1; m < K; m <<= 1)
        {
          isum += MC_SHFL_XOR(isum, m);
          if (fused)
            {
              h.h0 += MC_SHFL_XOR(h.h0, m);
              h.h1 += MC_SHFL_XOR(h.h1, m);
              h.h2 += MC_SHFL_XOR(h.h2, m);
              h.h3 += MC_SHFL_XOR(h.h3, m);
              h.h4 += MC_SHFL_XOR(h.h4, m);
              h.h5 += MC_SHFL_XOR(h.h5, m);
              h.h6 += MC_SHFL_XOR(h.h6, m);
              h.h7 += MC_SHFL_XOR(h.h7, m);
              h.h8 += MC_SHFL_XOR(h.h8, m);
            }
        }
      const int coin = fused ? mc_h9_coin(& h) : 0;
      cs += (mc_i64) isum;
      cc += (mc_i64) coin;
    }
  *ck_isum = cs;
  *ck_coin = cc;
}

#endif /* MC_HAVE_SIMD */

#endif
