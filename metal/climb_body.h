/* metal/climb_body.h -- ONE plugboard climb, in plain integer C, shared by
   every target: the Metal kernel (climb.metal), the CUDA kernel (later) and
   the CPU reference backend (backend_cpu.cc), which runs this same body one
   lane at a time so the body can be tested against the repo's own climb on
   any machine, GPU or not.

   The contract it reproduces is src/plugboard.cc's hillclimb<false>() under
   --int (metal/DESIGN.md 2, 3a, 6): steepest ascent over the 325 toggles in
   a<b order with the switch-over-removal tie rule, the plug cap gating
   ADDs (and MOVEs under -M), try_repair() at every convergence, one such
   climb per --score stage in order, and every decision a comparison of the
   integer key I = A*isum + B*coin. There is no floating-point operation in
   this file at all, which is what makes the result identical on every
   target by construction rather than by measurement.

   WHAT THE WRAPPERS SUPPLY, and nothing else: the integer type (long on
   Metal, int64_t elsewhere), the address-space keywords (empty on the CPU),
   the thread index, the threadgroup copy of rows[] and the barrier. The
   body never names a global and never allocates. */

#ifndef ENIGMA_CLIMB_BODY_H
#define ENIGMA_CLIMB_BODY_H

#if defined(__METAL_VERSION__)
typedef long mc_i64;
#define MC_THR thread
#define MC_THR_CONST thread const
#define MC_TG_CONST threadgroup const
#define MC_DEV_CONST device const
#else
#include <stdint.h>
typedef int64_t mc_i64;
#define MC_THR
#define MC_THR_CONST const
#define MC_TG_CONST const
#define MC_DEV_CONST const
#endif

#define MC_ASIZE 26
/* Longest message the GPU path takes: rows[L][26] plus the ciphertext must
   fit threadgroup memory beside the lanes' state (DESIGN.md 5). Longer
   messages take the CPU. */
#define MC_MAXLEN 256
#define MC_MAX_STAGES 16
/* Lanes per threadgroup the host asks for; a backend may lower it to what
   its pipeline allows. */
#define MC_LANES 256
/* Climbs in one dispatch. A cap on DURATION, not on memory: macOS resets
   the GPU when a command buffer runs too long and takes the desktop with
   it, so this is what keeps a large sweep from freezing the machine.
   $ENIGMA_GPU_BATCH_ITEMS overrides it (host_common.cc). */
#define MC_ITEMS_PER_DISPATCH 4096

/* Scoring models, in the CPU's enum scoring order (src/common.h). The host
   asserts the correspondence at compile time. */
#define MC_IC 0
#define MC_MONO 1
#define MC_BI 2
#define MC_TRI 3
#define MC_QUAD 4
#define MC_ALL 5
#define MC_FUSED 6
#define MC_MONOIC 7

/* One --score stage: the model, its plug cap, and the two --int weights.
   Every member is 64-bit so the layout is the same on both sides of the
   upload without any packing pragma. */
typedef struct
{
  mc_i64 model;
  mc_i64 cap;
  mc_i64 A;
  mc_i64 B;
} mc_stage;

/* One dispatch's constants. items = nkeys * restarts, item i being key
   i / restarts at restart i % restarts, so a key's restarts are adjacent. */
typedef struct
{
  mc_i64 L;             /* ciphertext length */
  mc_i64 nkeys;         /* keys in this batch */
  mc_i64 restarts;      /* lanes per key (-R, or 1 for the un-kicked climb) */
  mc_i64 lanes_per_tg;  /* threadgroup size; ceil(restarts / this) per key */
  mc_i64 nstages;
  mc_i64 pf_mask;       /* bit x set: letter x is fixed (-s / --no-plug) */
  mc_i64 capmerge;      /* -M: at the cap, block count-preserving MOVEs too */
  mc_i64 no_repair;     /* --no-repair: skip try_repair at convergence */
  mc_stage stages[MC_MAX_STAGES];
} mc_params;

/* plugboard -> per-position rotor-stack row -> plugboard: decode_at()
   with rows[] flattened to L rows of 26 bytes. */
inline int mc_decode_at(MC_THR_CONST unsigned char * steck,
                        MC_TG_CONST unsigned char * rows,
                        MC_TG_CONST unsigned char * ct, int i)
{
  return steck[rows[i * MC_ASIZE + steck[ct[i]]]];
}

/* The two integer accumulators of score_components(): isum over the
   stage's table and coin = sum n(n-1) over the letter histogram. Which
   letters the histogram counts and when a short message yields zero follow
   the CPU decoders exactly (scoring.cc): the three histogram models count
   all L letters with tbl = mono8; the quad-shaped models need L >= 4 and
   -f counts all L letters into coin while -q/-a leave it 0. */
inline void mc_components(MC_THR_CONST unsigned char * steck,
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
      int freq[MC_ASIZE];
      for (int j = 0; j < MC_ASIZE; j++)
        freq[j] = 0;
      for (int i = 0; i < L; i++)
        freq[mc_decode_at(steck, rows, ct, i)]++;
      for (int j = 0; j < MC_ASIZE; j++)
        {
          isum += (mc_i64) freq[j] * (mc_i64) tbl[j];
          coin += (mc_i64) freq[j] * (mc_i64) (freq[j] - 1);
        }
    }
  else if (model == MC_BI)
    {
      if (L >= 2)
        {
          int a = mc_decode_at(steck, rows, ct, 0);
          for (int i = 1; i < L; i++)
            {
              int b = mc_decode_at(steck, rows, ct, i);
              isum += (mc_i64) tbl[a * MC_ASIZE + b];
              a = b;
            }
        }
    }
  else if (model == MC_TRI)
    {
      if (L >= 3)
        {
          int a = mc_decode_at(steck, rows, ct, 0);
          int b = mc_decode_at(steck, rows, ct, 1);
          for (int i = 2; i < L; i++)
            {
              int c = mc_decode_at(steck, rows, ct, i);
              isum += (mc_i64) tbl[(a * MC_ASIZE + b) * MC_ASIZE + c];
              a = b;
              b = c;
            }
        }
    }
  else   /* MC_QUAD, MC_ALL, MC_FUSED: a quad-shaped table */
    {
      if (L >= 4)
        {
          int freq[MC_ASIZE];
          for (int j = 0; j < MC_ASIZE; j++)
            freq[j] = 0;
          int a = mc_decode_at(steck, rows, ct, 0);
          int b = mc_decode_at(steck, rows, ct, 1);
          int c = mc_decode_at(steck, rows, ct, 2);
          freq[a]++;
          freq[b]++;
          freq[c]++;
          for (int i = 3; i < L; i++)
            {
              int d = mc_decode_at(steck, rows, ct, i);
              freq[d]++;
              isum += (mc_i64) tbl[((a * MC_ASIZE + b) * MC_ASIZE + c)
                                   * MC_ASIZE + d];
              a = b;
              b = c;
              c = d;
            }
          if (model == MC_FUSED)
            {
              for (int j = 0; j < MC_ASIZE; j++)
                coin += (mc_i64) freq[j] * (mc_i64) (freq[j] - 1);
            }
        }
    }

  *isum_out = isum;
  *coin_out = coin;
}

/* The --int key: what every comparison in the climb is made on. */
inline mc_i64 mc_key(MC_THR_CONST unsigned char * steck,
                     MC_TG_CONST unsigned char * rows,
                     MC_TG_CONST unsigned char * ct,
                     int L, int model,
                     MC_DEV_CONST unsigned char * tbl,
                     mc_i64 A, mc_i64 B)
{
  mc_i64 isum = 0;
  mc_i64 coin = 0;
  mc_components(steck, rows, ct, L, model, tbl, & isum, & coin);
  return A * isum + B * coin;
}

inline int mc_plug_count(MC_THR_CONST unsigned char * steck)
{
  int n = 0;
  for (int j = 0; j < MC_ASIZE; j++)
    {
      if (steck[j] > j)
        n++;
    }
  return n;
}

/* try_repair(): every re-pairing of two existing plugs {a-x},{b-y} into
   {a-b,x-y} then {a-y,x-b}, plugs in ascending a, pairs i<j; the best is
   applied iff it strictly beats cur. Fixed letters' plugs are never
   rewired. Returns 1 if it applied one. */
inline int mc_try_repair(MC_THR unsigned char * steck,
                         MC_TG_CONST unsigned char * rows,
                         MC_TG_CONST unsigned char * ct,
                         int L, int model,
                         MC_DEV_CONST unsigned char * tbl,
                         mc_i64 A, mc_i64 B,
                         unsigned int pf, mc_i64 cur)
{
  int plo[MC_ASIZE / 2];
  int phi[MC_ASIZE / 2];
  int np = 0;
  for (int a = 0; a < MC_ASIZE; a++)
    {
      if ((steck[a] > a) && (((pf >> a) & 1u) == 0u))
        {
          plo[np] = a;
          phi[np] = steck[a];
          np++;
        }
    }

  mc_i64 best = cur;
  int found = 0;
  int rp[4];
  int rv[4];
  rp[0] = 0; rp[1] = 0; rp[2] = 0; rp[3] = 0;
  rv[0] = 0; rv[1] = 0; rv[2] = 0; rv[3] = 0;

  for (int i = 0; i < np; i++)
    {
      for (int j = i + 1; j < np; j++)
        {
          const int a = plo[i], x = phi[i], b = plo[j], y = phi[j];

          /* M1: {a-b, x-y} */
          steck[a] = (unsigned char) b; steck[b] = (unsigned char) a;
          steck[x] = (unsigned char) y; steck[y] = (unsigned char) x;
          const mc_i64 s1 = mc_key(steck, rows, ct, L, model, tbl, A, B);

          /* M2: {a-y, x-b} */
          steck[a] = (unsigned char) y; steck[y] = (unsigned char) a;
          steck[x] = (unsigned char) b; steck[b] = (unsigned char) x;
          const mc_i64 s2 = mc_key(steck, rows, ct, L, model, tbl, A, B);

          /* restore {a-x, b-y} */
          steck[a] = (unsigned char) x; steck[x] = (unsigned char) a;
          steck[b] = (unsigned char) y; steck[y] = (unsigned char) b;

          if (s1 > best)
            {
              best = s1; found = 1;
              rp[0] = a; rv[0] = b; rp[1] = b; rv[1] = a;
              rp[2] = x; rv[2] = y; rp[3] = y; rv[3] = x;
            }
          if (s2 > best)
            {
              best = s2; found = 1;
              rp[0] = a; rv[0] = y; rp[1] = y; rv[1] = a;
              rp[2] = x; rv[2] = b; rp[3] = b; rv[3] = x;
            }
        }
    }

  if (found)
    {
      for (int k = 0; k < 4; k++)
        steck[rp[k]] = (unsigned char) rv[k];
    }
  return found;
}

/* One stage's climb to convergence: hillclimb<false>() under --int with the
   default (steepest-ascent) rule. The scan order a outer, b inner, both
   ascending, is part of the contract: ties between two switch moves keep
   the FIRST found, so any other order reproduces every score and still
   diverges on ties (DESIGN.md 3a). */
inline void mc_hillclimb(MC_THR unsigned char * steck,
                         MC_TG_CONST unsigned char * rows,
                         MC_TG_CONST unsigned char * ct,
                         int L, int model,
                         MC_DEV_CONST unsigned char * tbl,
                         mc_i64 A, mc_i64 B,
                         unsigned int pf, int max_pairs,
                         int capmerge, int no_repair)
{
  int progress;
  do
    {
      progress = 0;

      mc_i64 best_score;
      mc_i64 last_best;
      do
        {
          best_score = mc_key(steck, rows, ct, L, model, tbl, A, B);
          last_best = best_score;

          const int pairs = mc_plug_count(steck);

          mc_i64 move_score = best_score;
          int move_kind = 0;   /* 0 = switch, 1 = remove */
          int move_a = 0;
          int move_b = 0;

          for (int a = 0; a < MC_ASIZE; a++)
            {
              for (int b = a + 1; b < MC_ASIZE; b++)
                {
                  if ((((pf >> a) & 1u) != 0u) || (((pf >> b) & 1u) != 0u))
                    continue;

                  const int sa = steck[a];
                  const int sb = steck[b];
                  const int a_free = (sa == a);
                  const int b_free = (sb == b);
                  const int paired = (sa == b);

                  if ((pairs >= max_pairs) && ! paired)
                    {
                      if (a_free && b_free)
                        continue;                    /* block ADD (+1) */
                      if (capmerge && (a_free || b_free))
                        continue;                    /* -M: block MOVE (0) */
                    }

                  const int new_kind = paired ? 1 : 0;
                  mc_i64 score;

                  if (paired)
                    {
                      steck[a] = (unsigned char) a;   /* REMOVE a-b */
                      steck[b] = (unsigned char) b;
                      score = mc_key(steck, rows, ct, L, model, tbl, A, B);
                      steck[a] = (unsigned char) b;   /* restore */
                      steck[b] = (unsigned char) a;
                    }
                  else
                    {
                      const int x = sa;
                      const int y = sb;
                      const int xx = steck[x];
                      const int yy = steck[y];
                      steck[x] = (unsigned char) x;   /* force a-b */
                      steck[y] = (unsigned char) y;
                      steck[a] = (unsigned char) b;
                      steck[b] = (unsigned char) a;
                      score = mc_key(steck, rows, ct, L, model, tbl, A, B);
                      steck[a] = (unsigned char) sa;  /* restore */
                      steck[b] = (unsigned char) sb;
                      steck[x] = (unsigned char) xx;
                      steck[y] = (unsigned char) yy;
                    }

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
              /* commit: the same mutation the winning probe made */
              const int a = move_a;
              const int b = move_b;
              if (move_kind == 1)
                {
                  steck[a] = (unsigned char) a;
                  steck[b] = (unsigned char) b;
                }
              else
                {
                  const int x = steck[a];
                  const int y = steck[b];
                  steck[x] = (unsigned char) x;
                  steck[y] = (unsigned char) y;
                  steck[a] = (unsigned char) b;
                  steck[b] = (unsigned char) a;
                }
              best_score = move_score;
            }
        }
      while (best_score > last_best);

      if ((! no_repair) &&
          mc_try_repair(steck, rows, ct, L, model, tbl, A, B, pf, best_score))
        progress = 1;
    }
  while (progress);
}

#endif
