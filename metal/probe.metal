/* metal/probe.metal -- the Metal wrapper around probe_body.h: five kernels,
   one per arm, in ONE library.  The register cap the host reports is per
   pipeline, and each kernel here is its own pipeline, so unlike the
   ablation variants these need no separate metallibs.

   Built with -fno-fast-math for the same reason climb.metal is. */

#include <metal_stdlib>
using namespace metal;

#define MC_HAVE_SIMD 1
#define MC_SHFL(v, l) simd_shuffle((v), (ushort) (l))
#define MC_SHFL_UP(v, d) simd_shuffle_up((v), (ushort) (d))
#define MC_SHFL_XOR(v, m) simd_shuffle_xor((v), (ushort) (m))

#include "probe_body.h"

/* Every kernel stages the key's rows[] and the ciphertext into threadgroup
   memory first, as enigma_climb does; the probes then read nothing from
   device memory but the table. */
#define MC_PROBE_STAGE()                                                    \
  threadgroup unsigned char rows_tg[MC_MAXLEN * MC_ASIZE];                 \
  threadgroup unsigned char ct_tg[MC_MAXLEN];                              \
  const int L = int(p.L);                                                  \
  const uint nrow = uint(L) * MC_ASIZE;                                    \
  for (uint i = lane; i < nrow; i += tg_size)                              \
    rows_tg[i] = rows_dev[i];                                              \
  for (uint i = lane; i < uint(L); i += tg_size)                           \
    ct_tg[i] = ct_dev[i];                                                  \
  threadgroup_barrier(mem_flags::mem_threadgroup)

#define MC_PROBE_ARGS                                                       \
  device const mc_probe_params & p [[buffer(0)]],                          \
  device const unsigned char * rows_dev [[buffer(1)]],                     \
  device const unsigned char * ct_dev [[buffer(2)]],                       \
  device const unsigned char * tbl [[buffer(3)]],                          \
  device const unsigned char * board0 [[buffer(4)]],                       \
  device long * out [[buffer(5)]],                                         \
  uint tg_id [[threadgroup_position_in_grid]],                             \
  uint lane [[thread_index_in_threadgroup]],                               \
  uint tg_size [[threads_per_threadgroup]]

kernel void enigma_probe_lane(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  const uint unit = tg_id * tg_size + lane;
  if (unit >= uint(p.units))
    return;
  device const unsigned char * b0 =
    board0 + (unit % uint(p.nboards)) * MC_ASIZE;
  thread unsigned char steck[MC_ASIZE];
  for (int j = 0; j < MC_ASIZE; j++)
    steck[j] = b0[j];
  mc_i64 cs = 0;
  mc_i64 cc = 0;
  mc_probe_lane(steck, rows_tg, ct_tg, L, int(p.model), tbl,
                int(p.nprobes), & cs, & cc);
  out[unit * 2] = cs;
  out[unit * 2 + 1] = cc;
}

/* The host rounds units up to whole threadgroups, so no lane returns
   early and every shuffle sees a live simdgroup. */
template <int K, bool SHUF>
static inline void probe_group_kernel(device const mc_probe_params & p,
                                      threadgroup const unsigned char * rows,
                                      threadgroup const unsigned char * ct,
                                      device const unsigned char * tbl,
                                      device const unsigned char * board0,
                                      device long * out,
                                      uint tg_id, uint lane, uint tg_size)
{
  const uint groups_per_tg = tg_size / uint(K);
  const uint unit = tg_id * groups_per_tg + lane / uint(K);
  device const unsigned char * b0 =
    board0 + (unit % uint(p.nboards)) * MC_ASIZE;
  mc_i64 cs = 0;
  mc_i64 cc = 0;
  mc_probe_group<K, SHUF>(lane % 32u, b0, rows, ct, int(p.L),
                          int(p.model), tbl, int(p.nprobes), & cs, & cc);
  if ((lane % uint(K)) == 0u)
    {
      out[unit * 2] = cs;
      out[unit * 2 + 1] = cc;
    }
}

kernel void enigma_probe_pass(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  const uint unit = tg_id * tg_size + lane;
  if (unit >= uint(p.units))
    return;
  device const unsigned char * b0 =
    board0 + (unit % uint(p.nboards)) * MC_ASIZE;
  thread unsigned char steck[MC_ASIZE];
  for (int j = 0; j < MC_ASIZE; j++)
    steck[j] = b0[j];
  mc_i64 cs = 0;
  mc_i64 cc = 0;
  mc_probe_pass(steck, rows_tg, ct_tg, L, int(p.model), tbl, p.A, p.B,
                MC_PROBE_PASSES(int(p.nprobes)), & cs, & cc);
  out[unit * 2] = cs;
  out[unit * 2 + 1] = cc;
}

kernel void enigma_probe_passpk(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  const uint unit = tg_id * tg_size + lane;
  if (unit >= uint(p.units))
    return;
  device const unsigned char * b0 =
    board0 + (unit % uint(p.nboards)) * MC_ASIZE;
  mc_i64 cs = 0;
  mc_i64 cc = 0;
  mc_probe_pass_pk(b0, rows_tg, ct_tg, L, int(p.model), tbl, p.A, p.B,
                   MC_PROBE_PASSES(int(p.nprobes)), & cs, & cc);
  out[unit * 2] = cs;
  out[unit * 2 + 1] = cc;
}

kernel void enigma_probe_g1(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  probe_group_kernel<1, false>(p, rows_tg, ct_tg, tbl, board0, out,
                               tg_id, lane, tg_size);
}

kernel void enigma_probe_g32s(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  probe_group_kernel<32, true>(p, rows_tg, ct_tg, tbl, board0, out,
                               tg_id, lane, tg_size);
}

kernel void enigma_probe_g32(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  probe_group_kernel<32, false>(p, rows_tg, ct_tg, tbl, board0, out,
                                tg_id, lane, tg_size);
}

kernel void enigma_probe_g16(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  probe_group_kernel<16, false>(p, rows_tg, ct_tg, tbl, board0, out,
                                tg_id, lane, tg_size);
}

kernel void enigma_probe_g8(MC_PROBE_ARGS)
{
  MC_PROBE_STAGE();
  probe_group_kernel<8, false>(p, rows_tg, ct_tg, tbl, board0, out,
                               tg_id, lane, tg_size);
}
