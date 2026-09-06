/* metal/host_common.cc -- the GPU host's main(): everything except running
   the climbs, which a backend does (host_common.h).

   It links every object under src/ except main.o and REUSES the tool
   rather than re-implementing it (DESIGN.md 7): option parsing, the
   ciphertext reader,
   the settings echo, the pre-flight check, init(), the key space and its
   collapses, key_to_machine()/setup_mapping() for the rows, the table
   loader, restart_seed()/perturb_steckerbrett() for the kicks, and
   score_report() for every double a user reads. Per batch it builds the
   rows and the kicked start boards, hands them to the backend, then walks
   the returned boards through the CPU's own reporting: --dump-all rows,
   the merge with better_cand() on the CPU's work index (restart-major, so
   the tie-break is the CPU's), the progress line, and the plaintext.

   COMPONENT EXACTNESS IS CHECKED ON EVERY ITEM (DESIGN.md 8.1): the
   backend returns each board's (isum, coin) under the target model, and
   the host recomputes them with score_components() on the same board. A
   mismatch is a bug in the kernel body or its wrapper, never noise. */

#include "host_common.h"

#include "args.h"
#include "cli.h"
#include "common.h"
#include "keyspace.h"
#include "machine.h"
#include "options.h"
#include "plugboard.h"
#include "preflight.h"
#include "progress.h"
#include "result.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* isatty, for the batch progress line */

#include <chrono>
#include <mutex>
#include <vector>

/* The kernel body numbers the models by the CPU's enum. */
static_assert(MC_IC == SCORE_IC, "model numbering");
static_assert(MC_MONO == SCORE_MONO, "model numbering");
static_assert(MC_BI == SCORE_BI, "model numbering");
static_assert(MC_TRI == SCORE_TRI, "model numbering");
static_assert(MC_QUAD == SCORE_QUAD, "model numbering");
static_assert(MC_ALL == SCORE_ALL, "model numbering");
static_assert(MC_FUSED == SCORE_FUSED, "model numbering");
static_assert(MC_MONOIC == SCORE_MONOIC, "model numbering");
static_assert(MC_MAX_STAGES == max_stages, "stage count");
static_assert(MC_ASIZE == asize, "alphabet");

static double seconds_since(std::chrono::steady_clock::time_point t)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
    .count();
}

/* What this milestone does not do. Each refusal names the CPU path that
   does; nothing here is silently ignored. */
static void gpu_validate()
{
  if (! opt_hillclimb)
    fatal("the GPU path climbs the plugboard: give -c");
  if (! opt_intscore)
    fatal("the GPU path compares integer keys: give --int "
          "(metal/DESIGN.md 3a)");
  if (opt_anneal > 0)
    fatal("the GPU path has no simulated annealing (-A)");
  if (opt_firstimprove || opt_dynorder || opt_ic_order)
    fatal("the GPU path climbs by steepest ascent only in this milestone "
          "(no -J / -K; DESIGN.md 4)");
  if (opt_cascade || opt_cascade3)
    fatal("the GPU path has no gain cascade (--cascade)");
  if (opt_polish)
    fatal("the GPU path has no --polish yet (milestone 5)");
  if (opt_exhaust)
    fatal("the GPU path has no --exhaust");
  if (opt_crib_text || opt_crib_list || opt_crib_rerank)
    fatal("the GPU path has no crib modes");
  if (opt_self_crib_seeds > 0)
    fatal("the GPU path has no --self-crib-seeds");
  if (opt_tune_phase > 0)
    fatal("the GPU path has no --tune-phase");
  if (opt_ring_stride > 1)
    fatal("the GPU path has no --ring-stride refinement yet");
  if ((opt_prefilter > 0) || (opt_prefilter_frac > 0.0))
    fatal("the GPU path has no -F pre-filter");
  if (opt_seed_dedup)
    fatal("the GPU path has no --seed-dedup yet");
  if (opt_confidence > 0)
    fatal("the GPU path has no --confidence yet (milestone 6)");
  if (opt_doubling_report > 0)
    fatal("the GPU path has no --doubling-report yet (milestone 5)");
  if (opt_true_key)
    fatal("the GPU path has no --true-key");
  if (textlength > MC_MAXLEN)
    fatal("the GPU path takes messages of at most 256 letters; "
          "longer ones take the CPU (metal/DESIGN.md 5)");
}

/* The merged best, kept in the CPU's own shape so progress_line() and the
   tie-break are the CPU's. */
static best_result g_best;

/* $ENIGMA_GPU_LANES: the threadgroup width. DESIGN.md 16 asks for this to
   be measured rather than guessed, and 17.6 step 1 measures it: at 256
   lanes ONE threadgroup fits a core, since two would need 512 threads
   against the kernel's 384-thread register cap, while at 128 three fit --
   +50% occupancy for a constant. Resolved once; the settings echo and the
   dispatch must not disagree. */
static int gpu_lanes_cap()
{
  static int cached = -1;
  if (cached > 0)
    return cached;
  cached = MC_LANES;
  const char * v = getenv("ENIGMA_GPU_LANES");
  if ((v != nullptr) && (*v != 0))
    {
      const uint64_t n = parse_opt_u64(v, "$ENIGMA_GPU_LANES");
      if ((n < 1) || (n > MC_LANES))
        fatal("$ENIGMA_GPU_LANES must be between 1 and " MC_LANES_STR);
      cached = static_cast<int>(n);
    }
  return cached;
}

static const char * gpu_lanes_echo()
{
  static char buf[32];
  const int n = gpu_lanes_cap();
  snprintf(buf, sizeof buf, (n == MC_LANES) ? "%d" : "%d (overridden)", n);
  return buf;
}

int main(int argc, char * * argv)
{
  const auto t_start = std::chrono::steady_clock::now();

  kick_rank_init();
  parse_args(argc, argv);
  ic_blend_init();
  hist_init();
  readciphertext();
  intscore_init();
  gpu_validate();

  backend_init(argv[0]);

  show_settings();
  /* $ENIGMA_GPU_ABLATE: this binary's kernel is one of DESIGN.md 17.6
     step 1's cost probes, so most variants return a WRONG board and the
     component check would fire on every item. Say so on every run --
     loudly, because a probe binary that looks like the tool is exactly
     how a measurement ends up quoted as a result -- and skip the check.
     The kernel is chosen by which metallib is loaded, never at run time
     (see MC_ABLATE in climb_body.h), so the host cannot name the variant
     and reports only that one is active. */
  const char * aenv = getenv("ENIGMA_GPU_ABLATE");
  const bool ablating = (aenv != nullptr) && (*aenv != 0) && (*aenv != '0');
  if (ablating)
    fprintf(stderr,
            "*** ABLATION PROBE: this kernel is instrumented, the "
            "plaintext is NOT a result\n*** (DESIGN.md 17.6 step 1); "
            "the component check is off.\n");
  fprintf(stderr, "GPU: %s\n", backend_name());
  fprintf(stderr, "     steepest-ascent climb per lane, %s lanes per "
          "threadgroup at most\n", gpu_lanes_echo());

  if (textlength < 1)
    fatal("Ciphertext is empty (no A-Z letters on standard input)");
  report_preflight();

  for (int i = 0; i < textlength; i++)
    num_ciphertext[i] = static_cast<unsigned char>(char2num(ciphertext[i]));

  init();
  init_plug_fixed(opt_steckerbrett, opt_no_plug);

  /* The key space and every wheel order's rotor table, as bruteforce()
     builds them (single-threaded here: the tables are a few ms each). */
  key_space ks = build_key_space();
  const size_t nwo = ks.tasks.size();
  subst_table all = allocate_subst_tables(nwo);
  machine * mp = new machine();
  machine & m = *mp;
  for (size_t i = 0; i < nwo; i++)
    {
      const wheel_task & t = ks.tasks[i];
      init_walzen(m, t.u, t.w[0], t.w[1], t.w[2]);
      m.greek = t.greek;
      m.greek_offset = t.greek_off;
      set_effective_reflector(m);
      m.subst_array = all + i * asize;
      precompute(m);
    }
  const size_t rg = ks.rsize * ks.gsize;
  const size_t rc12 = static_cast<size_t>(ks.rc[1]) * ks.rc[2];
  const size_t gc12 = static_cast<size_t>(ks.gc[1]) * ks.gc[2];
  const size_t total_keys = ks.total_keys;

  /* The dispatch constants. */
  const int L = textlength;
  const int restarts = (opt_restarts >= 1) ? opt_restarts : 1;
  mc_params p;
  memset(& p, 0, sizeof p);
  p.L = L;
  p.restarts = restarts;
  const int lanes_cap = gpu_lanes_cap();
  p.lanes_per_tg = (restarts < lanes_cap) ? restarts : lanes_cap;
  p.nstages = opt_nstages;
  for (int s = 0; s < opt_nstages; s++)
    {
      int64_t a = 0, b = 0;
      intscore_weights(opt_stages[s].model, & a, & b);
      p.stages[s].model = opt_stages[s].model;
      p.stages[s].cap = opt_stages[s].cap;
      p.stages[s].A = a;
      p.stages[s].B = b;
    }
  const bool * pf = known_plug_mark();
  for (int j = 0; j < asize; j++)
    {
      if (pf[j])
        p.pf_mask |= (static_cast<int64_t>(1) << j);
    }
  p.capmerge = opt_capmerge ? 1 : 0;
  p.no_repair = opt_no_repair ? 1 : 0;

  /* Keys per batch: bound the upload at ~64 MB of rows and boards. */
  const size_t row_bytes = static_cast<size_t>(L) * asize;
  const size_t per_key = row_bytes
    + static_cast<size_t>(restarts) * (2 * asize + 2 * sizeof(int64_t));
  size_t keys_per_batch = (64u << 20) / per_key;
  if (keys_per_batch < 1)
    keys_per_batch = 1;
  if (keys_per_batch > 65536)
    keys_per_batch = 65536;

  /* AND bound it by WORK, which is the constraint that actually bites.
     macOS resets the GPU when one command buffer runs too long, and the
     reset takes the window server with it -- observed on an M1 as the
     machine freezing, then a run that appeared to hang. A memory bound
     does not cap duration: at -R 1 a batch is 41 226 climbs of ONE thread
     each (lanes_per_tg = restarts), which is both the slowest shape the
     kernel has and the longest single dispatch. The two sweeps that
     completed on that machine dispatched 17 576 and 8 788 items, so the
     default here sits below the smaller of the two rather than beside the
     largest that survived. Raising it is a throughput knob for milestone
     9's numbers, not something to reach for casually: the failure mode is
     the user's desktop, not an error message. */
  size_t items_per_batch = MC_ITEMS_PER_DISPATCH;
  const char * ienv = getenv("ENIGMA_GPU_BATCH_ITEMS");
  if ((ienv != nullptr) && (*ienv != 0))
    {
      const uint64_t v = parse_opt_u64(ienv, "$ENIGMA_GPU_BATCH_ITEMS");
      if (v < 1)
        fatal("$ENIGMA_GPU_BATCH_ITEMS must be at least 1");
      items_per_batch = static_cast<size_t>(v);
    }
  const size_t by_items = items_per_batch / static_cast<size_t>(restarts);
  if (keys_per_batch > by_items)
    keys_per_batch = (by_items < 1) ? 1 : by_items;

  std::vector<uint8_t> rows;
  std::vector<uint8_t> boards_in;
  std::vector<uint8_t> boards_out;
  std::vector<int64_t> comps;
  std::vector<size_t> batch_keys;   /* the work-key index of each batch key */
  rows.reserve(keys_per_batch * row_bytes);
  boards_in.reserve(keys_per_batch * restarts * asize);

  m.scoring = opt_scoring;
  m.report = false;
  g_progress = & g_best;

  size_t keys_done = 0;
  size_t items_done = 0;
  size_t comps_bad = 0;
  std::vector<int64_t> all_comps;   /* ablation only; empty otherwise */
  double device_secs = 0.0;
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];
  /* --dump-all is excluded for the tool's own reason: its rows are the
     machine-readable form and print under their own mutex, so a \r line
     could interleave into them. */
  if ((isatty(fileno(stderr)) != 0) && ! opt_dump_all)
    sweep_progress_arm(total_keys, 1);

  auto run_batch = [&]()
    {
      const size_t nk = batch_keys.size();
      if (nk == 0)
        return;
      const size_t nitems = nk * static_cast<size_t>(restarts);
      boards_out.assign(nitems * asize, 0);
      comps.assign(nitems * 2, 0);
      p.nkeys = static_cast<int64_t>(nk);

      mc_batch b;
      b.params = & p;
      b.rows = rows.data();
      b.ct = num_ciphertext;
      b.mono8 = ngram_table(SCORE_MONO);
      b.bi8 = ngram_table(SCORE_BI);
      b.tri8 = ngram_table(SCORE_TRI);
      b.quad8 = ngram_table(SCORE_QUAD);
      b.all8 = ngram_table(SCORE_ALL);
      b.boards_in = boards_in.data();
      b.boards_out = boards_out.data();
      b.comps = comps.data();
      b.nkeys = nk;
      b.nitems = nitems;

      const auto t_dev = std::chrono::steady_clock::now();
      backend_run(b);
      device_secs += seconds_since(t_dev);

      /* Walk the results through the CPU's reporting, key by key. */
      for (size_t k = 0; k < nk; k++)
        {
          const size_t keyidx = batch_keys[k];
          if (! key_to_machine(m, keyidx, ks.tasks, ks.range, ks.rc, ks.gc,
                               all, rg, ks.gsize, rc12, gc12, cur_wo, rg6))
            fatal("internal: a batched key no longer decodes");
          m.scoring = opt_scoring;
          for (int r = 0; r < restarts; r++)
            {
              const size_t item = k * static_cast<size_t>(restarts)
                                  + static_cast<size_t>(r);
              memcpy(m.steckerbrett, & boards_out[item * asize], asize);
              decode(m);
              const double score = score_report(m);

              long isum = 0;
              int coin = 0;
              score_components(m, & isum, & coin);
              if (ablating)
                all_comps.push_back(comps[item * 2]);
              if ((! ablating)
                  && ((static_cast<int64_t>(isum) != comps[item * 2])
                      || (static_cast<int64_t>(coin) != comps[item * 2 + 1])))
                {
                  comps_bad++;
                  if (comps_bad <= 10)
                    {
                      char w[8], rr[8], g[8], s[3 * 13];
                      format_key(m, w, rr, g);
                      format_plugboard(m, s);
                      fprintf(stderr, "COMPONENT MISMATCH %s %s %s restart %d "
                              "%s: cpu %ld/%d device %lld/%lld\n",
                              w, rr, g, r, s, isum, coin,
                              static_cast<long long>(comps[item * 2]),
                              static_cast<long long>(comps[item * 2 + 1]));
                    }
                }

              if (opt_dump_all)
                dump_all(m, score);

              /* The CPU's work index: restart-major over the FULL key
                 space, so the tie-break matches a -T run exactly. */
              const size_t idx = static_cast<size_t>(r) * total_keys + keyidx;
              std::lock_guard<std::mutex> lock(g_best.mutex);
              if (better_cand(score, idx, g_best.score, g_best.idx))
                {
                  g_best.score = score;
                  g_best.idx = idx;
                  g_best.found = true;
                  memcpy(g_best.plaintext, m.plaintext,
                         static_cast<size_t>(textlength) + 1);
                  memcpy(g_best.steckerbrett, m.steckerbrett, asize);
                  for (int i = 0; i < 3; i++)
                    {
                      g_best.ringstellung[i] = m.ringstellung[i];
                      g_best.grundstellung[i] = m.grundstellung[i];
                    }
                  if (score > g_best.shown.load(std::memory_order_relaxed))
                    {
                      g_best.shown.store(score, std::memory_order_relaxed);
                      progress_line(g_best, m, score);
                    }
                }
            }
        }
      keys_done += nk;
      items_done += nitems;
      batch_keys.clear();
      rows.clear();
      boards_in.clear();

      /* Liveness, through the tool's OWN sweep line rather than one of this
         host's: a batch is many climbs with nothing printed between them,
         and the failure above -- the GPU reset -- looks exactly like a slow
         run until the desktop freezes.

         Reusing it is not merely tidier. progress_line() opens with
         sweep_progress_clear(), so a score line erases the \r line before
         printing over it; a second, private line has no such contract, and
         a bespoke one drew `GPU: 4096 keys, 4096 climbs, 23% -6.6455 B123
         AAK ...` with the next batch's score line smeared onto its row. The
         clock, the width-exact erase and the TTY gate come with it.

         Armed in KEYS, with restarts = 1, because the tool's pass field
         describes a restart-MAJOR sweep and this host is key-major: a key's
         restarts are adjacent (item i is key i/restarts), so there are no
         passes to report and keys are the honest unit. */
      sweep_progress_tick(nk, g_best);
    };

  /* Enumerate the key space exactly as search_worker() does -- the
     collapses are inside key_to_machine() -- building each key's rows and
     its kicked start boards. */
  for (size_t keyidx = 0; keyidx < total_keys; keyidx++)
    {
      if (! key_to_machine(m, keyidx, ks.tasks, ks.range, ks.rc, ks.gc, all,
                           rg, ks.gsize, rc12, gc12, cur_wo, rg6))
        continue;   /* collapsed away: decodes identically to one kept */

      for (int i = 0; i < L; i++)
        rows.insert(rows.end(), m.rows[i], m.rows[i] + asize);

      /* hillclimb_one()'s start board, per restart: the seed, the soft
         plugs, then the kick from the (key, restart) stream. */
      if ((opt_restarts >= 1) && (opt_biased_random > 0.0))
        biased_kick_prepare(m);
      for (int r = 0; r < restarts; r++)
        {
          init_steckerbrett(m, opt_steckerbrett);
          apply_soft_plug(m);
          uint64_t rng = restart_seed(keyidx, r);
          if (opt_restarts >= 1)
            {
              if (opt_biased_random > 0.0)
                biased_perturb(m, & rng, opt_perturb);
              else
                perturb_steckerbrett(m, & rng, opt_perturb);
            }
          boards_in.insert(boards_in.end(), m.steckerbrett,
                           m.steckerbrett + asize);
        }
      batch_keys.push_back(keyidx);

      if (batch_keys.size() >= keys_per_batch)
        run_batch();
    }
  run_batch();
  sweep_progress_clear();
  sweep_progress_disarm();

  if (! g_best.found)
    fatal("No machine configuration produced a score");

  fprintf(stderr, "\n");
  printf("%s\n", g_best.plaintext);

  if (opt_plaintext)
    readplaintext(opt_plaintext, g_best.plaintext);

  const double secs = seconds_since(t_start);
  fprintf(stderr, "Analysed %zu rotor combination%s, climbed %zu restart%s "
          "on the device\n",
          keys_done, (keys_done == 1) ? "" : "s",
          items_done, (items_done == 1) ? "" : "s");
  if (ablating)
    {
      /* Statistics on the FIRST returned component, per aligned group of
         32 items. A threadgroup's lanes are consecutive restarts and a
         simdgroup is 32 lanes, so a group is a simdgroup. Under an
         MC_PASSES build that component is the lane's climb-pass count:
         the MEAN is what normalises climbs/s into passes/s, the unit in
         which variants doing different amounts of work are comparable,
         and max/mean is the divergence factor of 17.2(c) -- the work a
         simdgroup runs against the work it needed. Under an ordinary
         build it is `isum` and neither figure means anything. The host
         cannot tell which metallib was loaded, so it reports both and
         names the component rather than the interpretation: the first
         table printed here labelled max/mean "divergence" on every row,
         which was true of one of them. */
      double ratio_sum = 0.0;
      double mean_sum = 0.0;
      size_t groups = 0;
      for (size_t g = 0; g + 32 <= all_comps.size(); g += 32)
        {
          int64_t mx = all_comps[g];
          double sum = 0.0;
          for (size_t k = 0; k < 32; k++)
            {
              if (all_comps[g + k] > mx)
                mx = all_comps[g + k];
              sum += static_cast<double>(all_comps[g + k]);
            }
          if (sum > 0.0)
            {
              ratio_sum += static_cast<double>(mx) / (sum / 32.0);
              mean_sum += sum / 32.0;
              groups++;
            }
        }
      if (groups > 0)
        fprintf(stderr, "Ablation: first component per 32-item group, "
                "mean = %.3f, max/mean = %.3f over %zu groups\n",
                mean_sum / static_cast<double>(groups),
                ratio_sum / static_cast<double>(groups), groups);
    }
  fprintf(stderr, "Components: %zu of %zu boards exact\n",
          items_done - comps_bad, items_done);
  fprintf(stderr, "Device time %.2f s (%.0f climbs/s); finished in %.2f s "
          "(%.0f climbs/s end to end)\n",
          device_secs,
          (device_secs > 0.0) ? static_cast<double>(items_done) / device_secs
                              : 0.0,
          secs,
          (secs > 0.0) ? static_cast<double>(items_done) / secs : 0.0);
  /* A probe's components are wrong by construction, so it must not
     look like a failed run to a harness that only reads exit codes. */
  return (ablating || (comps_bad == 0)) ? 0 : 2;
}
