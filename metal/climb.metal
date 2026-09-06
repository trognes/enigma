/* metal/climb.metal -- the Metal wrapper around climb_body.h: one
   threadgroup per rotor key (per slice of its restarts), one lane per
   restart running the whole staged climb (DESIGN.md 4).

   Built with -fno-fast-math although the body holds no floating-point
   operation at all: defence in depth, and free. */

#include <metal_stdlib>
using namespace metal;

#include "climb_body.h"

/* The table a stage reads: mono8 for the histogram models, all8 for -a
   and -f, the single-order table otherwise. Mirrors ngram_table(). */
static inline device const unsigned char *
mc_stage_table(int model,
               device const unsigned char * mono8,
               device const unsigned char * bi8,
               device const unsigned char * tri8,
               device const unsigned char * quad8,
               device const unsigned char * all8)
{
  switch (model)
    {
    case MC_BI:
      return bi8;
    case MC_TRI:
      return tri8;
    case MC_QUAD:
      return quad8;
    case MC_ALL:
    case MC_FUSED:
      return all8;
    default:
      return mono8;   /* MC_IC, MC_MONO, MC_MONOIC */
    }
}

kernel void enigma_climb(device const mc_params & p [[buffer(0)]],
                         device const unsigned char * rows_all [[buffer(1)]],
                         device const unsigned char * ct_dev [[buffer(2)]],
                         device const unsigned char * mono8 [[buffer(3)]],
                         device const unsigned char * bi8 [[buffer(4)]],
                         device const unsigned char * tri8 [[buffer(5)]],
                         device const unsigned char * quad8 [[buffer(6)]],
                         device const unsigned char * all8 [[buffer(7)]],
                         device const unsigned char * boards_in [[buffer(8)]],
                         device unsigned char * boards_out [[buffer(9)]],
                         device long * comps_out [[buffer(10)]],
                         uint tg_id [[threadgroup_position_in_grid]],
                         uint lane [[thread_index_in_threadgroup]],
                         uint tg_size [[threads_per_threadgroup]])
{
  threadgroup unsigned char rows_tg[MC_MAXLEN * MC_ASIZE];
  threadgroup unsigned char ct_tg[MC_MAXLEN];

  const int L = int(p.L);
  const uint restarts = uint(p.restarts);
  const uint lanes = uint(p.lanes_per_tg);
  const uint tgs_per_key = (restarts + lanes - 1) / lanes;
  const uint key = tg_id / tgs_per_key;
  const uint slice = tg_id % tgs_per_key;

  /* The key's rows[] and the ciphertext into threadgroup memory,
     cooperatively; every lane of this group climbs the same key. */
  const uint nrow = uint(L) * MC_ASIZE;
  for (uint i = lane; i < nrow; i += tg_size)
    rows_tg[i] = rows_all[key * nrow + i];
  for (uint i = lane; i < uint(L); i += tg_size)
    ct_tg[i] = ct_dev[i];
  threadgroup_barrier(mem_flags::mem_threadgroup);

  const uint restart = slice * lanes + lane;
  if (restart >= restarts)
    return;
  const uint item = key * restarts + restart;

  thread unsigned char steck[MC_ASIZE];
  for (int j = 0; j < MC_ASIZE; j++)
    steck[j] = boards_in[item * MC_ASIZE + j];

  const unsigned int pf = (unsigned int) p.pf_mask;
  const int capmerge = int(p.capmerge);
  const int no_repair = int(p.no_repair);
  const int nstages = int(p.nstages);

  int model = MC_IC;
  device const unsigned char * tbl = mono8;
  mc_i64 A = 0;
  mc_i64 B = 0;
  int passes = 0;
  for (int s = 0; s < nstages; s++)
    {
      model = int(p.stages[s].model);
      tbl = mc_stage_table(model, mono8, bi8, tri8, quad8, all8);
      A = p.stages[s].A;
      B = p.stages[s].B;
      passes += mc_hillclimb(steck, rows_tg, ct_tg, L, model, tbl, A, B, pf,
                             int(p.stages[s].cap), capmerge, no_repair);
    }

  /* The converged board and its integer components under the target
     model, for the host's exactness check and its double reconstruction. */
  mc_i64 isum = 0;
  mc_i64 coin = 0;
  mc_components(steck, rows_tg, ct_tg, L, model, tbl, & isum, & coin);
  for (int j = 0; j < MC_ASIZE; j++)
    boards_out[item * MC_ASIZE + j] = steck[j];
#if MC_ABLATE == 5
  /* The divergence probe (17.2(c)): this lane's pass count and its lane
     index within the simdgroup, in place of the components, which the
     host is told to ignore by $ENIGMA_GPU_ABLATE. */
  comps_out[item * 2] = (mc_i64) passes;
  comps_out[item * 2 + 1] = (mc_i64) (lane % 32u);
#else
  comps_out[item * 2] = isum;
  comps_out[item * 2 + 1] = coin;
#endif
}
