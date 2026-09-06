/* metal/backend_cpu.cc -- the reference backend: the kernel body from
   climb_body.h run one lane at a time on the CPU, over opt_threads
   threads. It exists so the body can be tested against the repo's own
   --int climb on a machine with no GPU (metal/verify_identity.py), and so
   a Metal or CUDA result can be compared against the SAME body on the CPU
   when the two disagree -- which separates a wrapper bug from a body bug. */

#include "host_common.h"

#include "common.h"
#include "options.h"

#include <string.h>

#include <chrono>
#include <thread>
#include <vector>

const char * backend_name()
{
  return "CPU reference backend (the kernel body run per lane on the CPU)";
}

void backend_init(const char *)
{
}

static const uint8_t * stage_table(const mc_batch & b, int model)
{
  switch (model)
    {
    case MC_BI:
      return b.bi8;
    case MC_TRI:
      return b.tri8;
    case MC_QUAD:
      return b.quad8;
    case MC_ALL:
    case MC_FUSED:
      return b.all8;
    default:
      return b.mono8;
    }
}

static void run_keys(const mc_batch & b, size_t k0, size_t k1)
{
  const mc_params & p = *b.params;
  const int L = static_cast<int>(p.L);
  const int restarts = static_cast<int>(p.restarts);
  const unsigned int pf = static_cast<unsigned int>(p.pf_mask);
  const int nstages = static_cast<int>(p.nstages);
  const size_t row_bytes = static_cast<size_t>(L) * MC_ASIZE;

  for (size_t k = k0; k < k1; k++)
    {
      const uint8_t * rows = b.rows + k * row_bytes;
      for (int r = 0; r < restarts; r++)
        {
          const size_t item = k * static_cast<size_t>(restarts)
                              + static_cast<size_t>(r);
          unsigned char steck[MC_ASIZE];
          memcpy(steck, b.boards_in + item * MC_ASIZE, MC_ASIZE);

          int model = MC_IC;
          const uint8_t * tbl = b.mono8;
          int passes = 0;
          for (int s = 0; s < nstages; s++)
            {
              model = static_cast<int>(p.stages[s].model);
              tbl = stage_table(b, model);
              passes += mc_hillclimb(steck, rows, b.ct, L, model, tbl,
                                     p.stages[s].A, p.stages[s].B, pf,
                                     static_cast<int>(p.stages[s].cap),
                                     static_cast<int>(p.capmerge),
                                     static_cast<int>(p.no_repair));
            }
          mc_i64 isum = 0;
          mc_i64 coin = 0;
          mc_components(steck, rows, b.ct, L, model, tbl, & isum, & coin);
          memcpy(b.boards_out + item * MC_ASIZE, steck, MC_ASIZE);
#if MC_PASSES
          /* The same substitution climb.metal makes, so the probe
             machinery -- MC_PASSES and MC_FIXED_PASSES both -- can be
             verified on a machine with no GPU. The default build does not
             reach this: MC_PASSES is 0 unless a probe asked for it. */
          b.comps[item * 2] = static_cast<mc_i64>(passes);
          b.comps[item * 2 + 1] = static_cast<mc_i64>(r % 32);
#else
          (void) passes;
          b.comps[item * 2] = isum;
          b.comps[item * 2 + 1] = coin;
#endif
        }
    }
}

void backend_run(const mc_batch & b)
{
  int nthreads = opt_threads;
  if (nthreads < 1)
    nthreads = 1;
  if (static_cast<size_t>(nthreads) > b.nkeys)
    nthreads = static_cast<int>(b.nkeys);
  if (nthreads <= 1)
    {
      run_keys(b, 0, b.nkeys);
      return;
    }
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(nthreads));
  for (int t = 0; t < nthreads; t++)
    {
      const size_t k0 = b.nkeys * static_cast<size_t>(t)
                        / static_cast<size_t>(nthreads);
      const size_t k1 = b.nkeys * static_cast<size_t>(t + 1)
                        / static_cast<size_t>(nthreads);
      pool.emplace_back([&b, k0, k1]() { run_keys(b, k0, k1); });
    }
  for (std::thread & th : pool)
    th.join();
}

/* The probe's lane arm on the CPU: the driver is testable on a machine
   with no GPU, and its checksum check has something to check.  The group
   arms need a simdgroup and are not here. */
bool backend_probe(const mc_probe_batch & b, double * secs, int * cap)
{
  if (b.arm != MC_PROBE_LANE)
    return false;
  const mc_probe_params & p = *b.params;
  const int L = static_cast<int>(p.L);
  const size_t units = static_cast<size_t>(p.units);
  const auto t0 = std::chrono::steady_clock::now();
  const size_t nboards = static_cast<size_t>(p.nboards);
  for (size_t u = 0; u < units; u++)
    {
      unsigned char steck[MC_ASIZE];
      memcpy(steck, b.board0 + (u % nboards) * MC_ASIZE, MC_ASIZE);
      mc_i64 cs = 0;
      mc_i64 cc = 0;
      mc_probe_lane(steck, b.rows, b.ct, L, static_cast<int>(p.model),
                    b.tbl, static_cast<int>(p.nprobes), & cs, & cc);
      b.out[u * 2] = cs;
      b.out[u * 2 + 1] = cc;
    }
  *secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
  *cap = 0;
  return true;
}
