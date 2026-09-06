/* metal/probe_host.cc -- the driver for the probe microkernel (probe_body.h,
   DESIGN.md 17.6 (ii)).  $ENIGMA_GPU_PROBE=1 on either host binary runs
   this instead of the sweep: it takes the first key of the resolved key
   space, the target model, and a kicked start board, and times one toggle
   probe in today's shape against the same probe in the 17.4 shape.

   THE CHECKSUM IS THE POINT.  Every unit sums isum and coin over its
   probes; the driver replays the identical toggle sequence through the
   tool's own score_components() -- not through probe_body.h -- and every
   unit of every arm must reproduce it exactly.  A shape that is fast and
   wrong prints FAIL, not a rate.  The lane arm is additionally run on the
   CPU through probe_body.h before anything is dispatched, which separates
   a body bug from a wrapper bug the way backend_cpu.cc does for the climb.

   Sizing is by time, not by a guessed count: a pilot of a few threadgroups
   gives the rate, the run is then sized to ~0.4 s -- long enough to time,
   well under the watchdog -- and repeated, the min kept.  Rates are in
   PROBES per second, one probe being one board scored over L characters,
   so the two shapes are directly comparable whatever K is. */

#include "host_common.h"

#include "common.h"
#include "machine.h"
#include "options.h"
#include "plugboard.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

static const char * const arm_name[MC_PROBE_ARMS] =
  { "lane (today)", "lane packed regs", "climb pass (scan)",
    "group K=32 shuffle", "group K=32 packed", "group K=16 packed",
    "group K=8 packed" };
static const int arm_k[MC_PROBE_ARMS] = MC_PROBE_K_LIST;

static const char * model_name(int model)
{
  switch (model)
    {
    case MC_QUAD:
      return "quad";
    case MC_ALL:
      return "weighted";
    case MC_FUSED:
      return "fused";
    default:
      return "other";
    }
}

/* The oracle: the tool's own scorer over the replayed sequence. */
static void oracle(machine & m, const unsigned char * board0, int nprobes,
                   int64_t * ck_isum, int64_t * ck_coin)
{
  unsigned char st[asize];
  memcpy(st, board0, asize);
  int64_t cs = 0;
  int64_t cc = 0;
  for (int t = 0; t < nprobes; t++)
    {
      int a;
      int b;
      mc_probe_toggle(t, & a, & b);
      mc_probe_apply(st, a, b);
      memcpy(m.steckerbrett, st, asize);
      decode(m);
      long isum = 0;
      int coin = 0;
      score_components(m, & isum, & coin);
      cs += isum;
      cc += coin;
    }
  *ck_isum = cs;
  *ck_coin = cc;
}

/* Every unit's checksums against its board's oracle; the first mismatch
   of an arm is named, the rest only counted. */
static bool check(const std::vector<int64_t> & out, size_t units,
                  const int64_t * os, const int64_t * oc, size_t nboards,
                  const char * arm, bool quiet)
{
  for (size_t u = 0; u < units; u++)
    {
      const size_t bd = u % nboards;
      if ((out[u * 2] != os[bd]) || (out[u * 2 + 1] != oc[bd]))
        {
          if (! quiet)
            fprintf(stderr, "  %s: unit %zu (board %zu) checksum "
                    "%lld/%lld, oracle %lld/%lld\n", arm, u, bd,
                    static_cast<long long>(out[u * 2]),
                    static_cast<long long>(out[u * 2 + 1]),
                    static_cast<long long>(os[bd]),
                    static_cast<long long>(oc[bd]));
          return false;
        }
    }
  return true;
}

int probe_run(machine & m, int lanes_cap)
{
  const int L = textlength;
  const int model = opt_scoring;
  if ((model != MC_QUAD) && (model != MC_ALL) && (model != MC_FUSED))
    fatal("the probe is for a quad-shaped target (-q, -a or -f); the "
          "group arms have no histogram-model form");

  int nprobes = 64;
  const char * nenv = getenv("ENIGMA_GPU_PROBE_N");
  if ((nenv != nullptr) && (*nenv != 0))
    {
      const uint64_t v = parse_opt_u64(nenv, "$ENIGMA_GPU_PROBE_N");
      if ((v < 1) || (v > 65536))
        fatal("$ENIGMA_GPU_PROBE_N must be between 1 and 65536");
      nprobes = static_cast<int>(v);
    }

  int lanes = lanes_cap;
  lanes -= lanes % 32;
  if (lanes < 32)
    lanes = 32;

  /* One key's rows, the ciphertext, the table, and hillclimb_one()'s
     kicked start boards for (key 0, restart b), b < MC_PROBE_BOARDS: see
     probe_body.h for why one board was not enough. */
  std::vector<uint8_t> rows;
  rows.reserve(static_cast<size_t>(L) * asize);
  for (int i = 0; i < L; i++)
    rows.insert(rows.end(), m.rows[i], m.rows[i] + asize);
  const uint8_t * tbl = ngram_table(model);
  const size_t nboards = MC_PROBE_BOARDS;
  unsigned char boards[MC_PROBE_BOARDS * asize];
  int64_t os[MC_PROBE_BOARDS];
  int64_t oc[MC_PROBE_BOARDS];
  int64_t ps[MC_PROBE_BOARDS];   /* the pass arm's, body-against-body */
  int64_t pc[MC_PROBE_BOARDS];
  int64_t A = 0;
  int64_t B = 0;
  intscore_weights(model, & A, & B);
  const int npasses = MC_PROBE_PASSES(nprobes);
  m.scoring = model;
  for (size_t bd = 0; bd < nboards; bd++)
    {
      init_steckerbrett(m, opt_steckerbrett);
      uint64_t rng = restart_seed(0, static_cast<int>(bd));
      perturb_steckerbrett(m, & rng, opt_perturb);
      unsigned char * b0 = boards + bd * asize;
      memcpy(b0, m.steckerbrett, asize);
      oracle(m, b0, nprobes, & os[bd], & oc[bd]);

      /* The body itself on the CPU first: a mismatch here is in
         probe_body.h, not in any backend. */
      unsigned char st[asize];
      memcpy(st, b0, asize);
      mc_i64 bs = 0;
      mc_i64 bc = 0;
      mc_probe_lane(st, rows.data(), num_ciphertext, L, model, tbl,
                    nprobes, & bs, & bc);
      if ((bs != os[bd]) || (bc != oc[bd]))
        {
          fprintf(stderr, "board %zu: probe body %lld/%lld, "
                  "score_components %lld/%lld\n", bd,
                  static_cast<long long>(bs), static_cast<long long>(bc),
                  static_cast<long long>(os[bd]),
                  static_cast<long long>(oc[bd]));
          fatal("probe_body.h disagrees with the tool's scorer on the "
                "CPU");
        }

      /* The pass arm's oracle: mc_pass() itself on the CPU.  Its tie to
         the tool's climb is verify_identity.py, not this. */
      memcpy(st, b0, asize);
      mc_probe_pass(st, rows.data(), num_ciphertext, L, model, tbl, A, B,
                    npasses, & ps[bd], & pc[bd]);
    }

  fprintf(stderr, "Probe: DESIGN.md 17.6 (ii), one toggle probe in two "
          "shapes\n  L=%d, model %s, %d probes per unit (%d passes of %d "
          "for the pass arm),\n  %zu start boards, %d lanes per "
          "threadgroup, checksums against\n  score_components over the "
          "same toggles\n", L, model_name(model), nprobes, npasses,
          MC_PROBE_PASS_PROBES, nboards, lanes);
  fprintf(stderr, "  %-20s %9s %12s %9s %5s  %s\n", "arm", "units",
          "probes/s", "vs lane", "cap", "check");

  mc_probe_params p;
  memset(& p, 0, sizeof p);
  p.L = L;
  p.model = model;
  p.nprobes = nprobes;
  p.lanes_per_tg = lanes;
  p.nboards = static_cast<int64_t>(nboards);
  p.A = A;
  p.B = B;

  std::vector<int64_t> out;
  double lane_rate = 0.0;
  int failed = 0;
  for (int arm = 0; arm < MC_PROBE_ARMS; arm++)
    {
      const int K = arm_k[arm];
      if ((K > 1) && (L < 3 * K))
        {
          fprintf(stderr, "  %-20s %9s  n/a: L < 3K, under three "
                  "characters per lane\n", arm_name[arm], "");
          continue;
        }
      const size_t per_tg = static_cast<size_t>(lanes / K);
      const bool pass_arm = (arm == MC_PROBE_PASS);
      const int ppu = pass_arm ? npasses * MC_PROBE_PASS_PROBES : nprobes;
      const int64_t * ar_s = pass_arm ? ps : os;
      const int64_t * ar_c = pass_arm ? pc : oc;

      /* Pilot: 64 threadgroups. */
      size_t units = 64 * per_tg;
      out.assign(units * 2, 0);
      p.units = static_cast<int64_t>(units);
      mc_probe_batch b;
      b.params = & p;
      b.rows = rows.data();
      b.ct = num_ciphertext;
      b.tbl = tbl;
      b.board0 = boards;
      b.out = out.data();
      b.arm = arm;
      double secs = 0.0;
      int cap = 0;
      if (! backend_probe(b, & secs, & cap))
        {
          fprintf(stderr, "  %-20s %9s  n/a: not in this backend\n",
                  arm_name[arm], "");
          continue;
        }
      bool ok = check(out, units, ar_s, ar_c, nboards, arm_name[arm],
                      false);
      double rate = (secs > 0.0)
        ? static_cast<double>(units) * ppu / secs : 0.0;

      /* Size to ~0.4 s, in whole threadgroups, capped at 1M units. */
      if (rate > 0.0)
        {
          size_t want = static_cast<size_t>(rate * 0.4 / ppu);
          want = ((want + per_tg - 1) / per_tg) * per_tg;
          if (want < per_tg)
            want = per_tg;
          if (want > (static_cast<size_t>(1) << 20))
            want = (static_cast<size_t>(1) << 20);
          units = want;
        }
      out.assign(units * 2, 0);
      p.units = static_cast<int64_t>(units);
      b.out = out.data();
      double best = 0.0;
      for (int rep = 0; rep < 3; rep++)
        {
          if (! backend_probe(b, & secs, & cap))
            fatal("internal: an arm that ran refuses to run again");
          ok = check(out, units, ar_s, ar_c, nboards, arm_name[arm], ! ok)
            && ok;
          const double r = (secs > 0.0)
            ? static_cast<double>(units) * ppu / secs : 0.0;
          if (r > best)
            best = r;
        }
      if (arm == MC_PROBE_LANE)
        lane_rate = best;
      if (! ok)
        failed++;
      fprintf(stderr, "  %-20s %9zu %12.0f %8.2fx %5d  %s\n", arm_name[arm],
              units, best,
              (lane_rate > 0.0) ? best / lane_rate : 0.0, cap,
              ok ? "ok" : "FAIL");
    }

  fprintf(stderr, "\n  vs lane: the group rows are the step-latency "
          "ratio 17.4 rests on, occupancy\n  included; the pass row is "
          "the scan machinery's cost, the scorer alone being\n  1.00x; "
          "cap is the pipeline's register-derived thread limit, 1024 for "
          "a lean\n  kernel.  A FAIL row's rate means nothing.\n");
  return (failed == 0) ? 0 : 2;
}
