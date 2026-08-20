#include "confidence.h"
#include "search.h"

#include "common.h"
#include "crib.h"
#include "keyspace.h"
#include "machine.h"
#include "options.h"
#include "plugboard.h"
#include "progress.h"
#include "result.h"
#include "scoring.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/resource.h>


/* --- --confidence N: is the winner better than chance? ------------------------
   A raw score answers nothing on its own. A model's score has a distribution on
   text with no signal, and a search reports the MAXIMUM over the keys it
   analysed, which drifts upward as the keyspace grows -- so the same score can
   be a break at one keyspace size and noise at another.

   This samples N keys uniformly from the resolved key space, scores each exactly
   as the search scored them (climbing the plugboard too, when -c is on, because
   a climbed key is drawn from a different and higher distribution than a scanned
   one), and reports three things: how far the winner sits above that null in
   standard deviations, where the best of K draws is EXPECTED to sit by chance
   (mu + sigma*sqrt(2 ln K), the Gumbel location for a Gaussian null), and the
   margin between them. Only the margin means anything.

   MEASURED: on 12 signal-free ciphertexts swept over K = 17576 keys at L=200,
   the observed best-of-K matched that prediction to within 0.01 for quad
   (-7.2355 against -7.2432) and fused (-10.4368 against -10.4351). The index of
   coincidence does NOT follow it -- 6.1 sigma observed against 4.4 predicted --
   because its null is a quadratic form in the letter histogram rather than a sum
   over positions, and so is right-skewed. The p-value is therefore printed as
   Gaussian-tail and flagged as optimistic under -i.

   Sampling keys rather than random text is deliberate: the null a search actually
   draws from is "this ciphertext under a wrong key", and key_to_machine() already
   builds exactly that, in every machine mode, with no separate code path. */
void calibrate_null(machine & m, size_t keys,
                           const std::vector<wheel_task> & tasks,
                           const search_range & range,
                           const int * rc, const int * gc, subst_table all,
                           size_t rg, size_t gsize, size_t rc12, size_t gc12,
                           size_t total_keys)
{
  /* Once per process. The null depends on the ciphertext, the model and
     whether the climb runs -- all fixed across a --crib-list's per-crib
     sweeps, so re-sampling for each of a hundred cribs would be a hundred
     times the cost for the same three numbers. */
  if (g_null_sd > 0.0)
    return;
  const bool save_report = m.report;
  const long save_scored = m.plugboards_scored;
  /* --dump-all's contract is "every converged climb OF THE SEARCH". A
     calibration climb is not one, and hillclimb_one() dumps unconditionally,
     so leaving this on put 16 extra rows in the diagnostic at --confidence 16
     -- silently changing what every harness that parses dumpall measures.
     Cleared for the sampling only; the workers have not started yet, so no
     other thread can observe it. */
  const bool save_dump = opt_dump_all;
  opt_dump_all = false;
  m.report = false;                 /* calibration must not echo progress lines */
  m.scoring = opt_scoring;

  uint64_t rng = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(opt_seed);
  size_t cur_wo = static_cast<size_t>(-1);
  int rg6[6];
  std::vector<double> xs;
  xs.reserve(static_cast<size_t>(opt_confidence));

  /* A live \r line, on a TTY only, so redirected logs and the tests stay clean --
     the same rule the -F tier-1 line follows. It earns its keep under -c, where a
     sample is a whole plugboard climb (~1.7 ms at L=200, so N=1024 is a couple of
     seconds of apparent hang before the first progress line of the search itself).
     Single-threaded -- the workers have not started -- so no atomic or mutex. */
  const bool show_progress = isatty(fileno(stderr)) != 0;
  const size_t want = static_cast<size_t>(opt_confidence);
  const size_t step = (want / 100) + 1;   /* every 1%, and every sample for small N */

  /* Draws are with replacement and skip keys the collapses removed; a run of
     misses cannot loop forever because total_keys is the INDEX space and at least
     one index in it always survives (the winner did).
       A REJECTED key is skipped too, and that one is load-bearing. Under --crib the
     unit returns unit_no_score for a key no hypothesis survives -- and a crib worth
     using rejects 99%+ of them, so nearly every sample came back as -1e300. The mean
     then sat at ~-1e300 and the variance OVERFLOWED to +inf, which made (s - mu)/sd
     exactly 0 for every board: every progress line printed the identical margin
     -z_k, and the summary printed a 300-digit null. Those keys are not part of the
     null the search draws from -- it never scores them at all -- so they must be
     dropped rather than counted. The guard is much larger than the plain path's
     because a rejected draw costs only the deduction (microseconds) while an
     accepted one costs a whole climb, so many attempts are affordable and the loop
     still terminates. */
  size_t guard = want * 256 + 4096;
  size_t rejected = 0;
  while ((xs.size() < want) && (guard-- > 0))
    {
      rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
      size_t idx = static_cast<size_t>((rng >> 11) % total_keys);
      if (! key_to_machine(m, idx, tasks, range, rc, gc, all, rg, gsize,
                           rc12, gc12, cur_wo, rg6))
        continue;
      const double s = opt_hillclimb ? climb_unit(m, idx, 0) : score_iter(m);
      if (s <= unit_no_score)
        {
          rejected++;
          continue;
        }
      xs.push_back(s);
      if (show_progress && (((xs.size() % step) == 0) || (xs.size() == want)))
        {
          fprintf(stderr, "\rConfidence: sampling the null %3zu%% (%zu / %zu keys)",
                  (xs.size() * 100) / want, xs.size(), want);
          fflush(stderr);
        }
    }
  /* Erase it rather than leaving the finished line: the settings echo already
     reported N, and the summary reports the result, so a permanent "100%" row
     would say nothing the run does not say twice already. */
  if (show_progress)
    fprintf(stderr, "\r%79s\r", "");

  m.report = save_report;
  opt_dump_all = save_dump;
  m.plugboards_scored = save_scored;   /* keep the diagnostic comparable */

  /* Naming the cause matters here: with a crib this is the EXPECTED outcome of a
     very selective one, not a malfunction, and the run is otherwise fine. */
  if (xs.size() < 8)
    {
      if (rejected > 0)
        fprintf(stderr,
                "Confidence: the crib rejected %zu of %zu sampled keys, leaving %zu "
                "to\n            calibrate against -- too few for a null. Reporting "
                "raw scores.\n", rejected, rejected + xs.size(), xs.size());
      else
        fprintf(stderr, "Confidence: too few sampled keys to calibrate\n");
      return;
    }
  double mu = 0.0;
  for (double x : xs)
    mu += x;
  mu /= static_cast<double>(xs.size());
  double var = 0.0;
  for (double x : xs)
    var += (x - mu) * (x - mu);
  var /= static_cast<double>(xs.size() - 1);
  const double sd = sqrt(var);

  /* A DEGENERATE null, tested relatively rather than against literal zero. The
     obvious `sd > 0.0` is not enough: with the rotor key fully specified the
     keyspace is ONE key, every sample climbs it to the same score, and sd comes
     out as float noise (~1e-15) rather than 0 -- so the guard passed and the
     margin became score/1e-15, i.e. ~1e13, which also blew the 8-wide first
     column out to 87 characters. Scores here are per-symbol log10 probabilities
     (order 1), so 1e-9 is nine orders below any real null (measured ~0.17) and
     six above the noise. Leaving g_null_sd at 0 makes showconfig fall back to
     raw scores, which is the honest display when there is nothing to calibrate
     against. */
  if (!(sd > 1e-9 * (fabs(mu) + 1.0)))
    {
      fprintf(stderr, "Confidence: all %zu sampled keys scored alike, so there is "
                      "no null to\n            measure against -- the key space "
                      "(%zu key%s) is too small to\n            hold one. "
                      "Reporting raw scores.\n",
              xs.size(), total_keys, (total_keys == 1) ? "" : "s");
      return;
    }
  g_null_mu = mu;
  g_null_sd = sd;
  /* Expected best of `keys` draws from a Gaussian null. The keys < 2 clamp only
     keeps log() defined; it is unreachable, because a key space that small
     cannot produce a spread of scores and the degenerate guard above has
     already returned. */
  g_null_keys = keys;
  g_null_zk = sqrt(2.0 * log(static_cast<double>(keys < 2 ? 2 : keys)));
  g_null_n = xs.size();
  /* Report the bar BEFORE the sweep, not only in the summary after it. The
     progress lines print a MARGIN, and a reader watching them has no way to
     convert that back to a raw sigma count -- which is the number every other
     account of a result is quoted in -- unless the offset is stated up front.
     The summary repeats it once the search is over; this is the same figure at
     the point where it is useful. */
  /* NOT prefixed "Confidence: null" -- that is the summary's anchor, and
     tests/run_tests.sh matches on it precisely because the bare "^Confidence"
     already collides with the settings echo. A third line sharing the anchor
     would break the -T-independence check the same way. */
  fprintf(stderr, "Confidence: margin 0 is z = %.1f, the best of %zu keys by "
                  "chance\n", g_null_zk, keys);
}
/* The summary line, printed after the search. The progress lines already
   carried the margin; this gives the pieces behind it -- the null itself, the
   raw distance above it, and a p-value -- so a log records what the margin was
   measured against
   rather than only the result. */
void report_confidence(double best_score)
{
  if (!(g_null_sd > 0.0))
    return;
  const double z = (best_score - g_null_mu) / g_null_sd;
  /* Gaussian upper tail, family-wise over g_null_keys independent draws -- the
     same K the bar used, so the two halves of the line agree. erfc is exact
     enough far out; the 1-exp form avoids losing the small p to rounding. */
  const double tail = 0.5 * erfc(z / sqrt(2.0));
  const double pfam = -expm1(-static_cast<double>(g_null_keys) * tail);

  fprintf(stderr,
          "Confidence: null %.4f +/- %.4f over %zu sampled keys; best is %.1f sd\n"
          "            above it, chance best of %zu keys is %.1f sd -- margin "
          "%+.1f sd\n", g_null_mu, g_null_sd, g_null_n, z, g_null_keys, g_null_zk,
          z - g_null_zk);
  /* The Gaussian tail understates the false-positive rate near zero for EVERY
     model, not only IC. Measured on 2000 signal-free ciphertexts at L=200,
     K=17576: a margin of +0.54 came up 2.35% of the time against the 0.70% this
     p implies, because the real null's best-of-K sits +0.21 sd above a Gaussian
     of the same mu/sd and its upper tail is fatter (95th percentile +0.40
     measured against +0.11 predicted). The score is a sum over positions, so
     the CLT gives the centre quickly but the far tail at 4.4 sd -- exactly what
     a best-of-K statistic probes -- converges slowly. IC is worse again, its
     null being a quadratic form in the letter histogram rather than a sum. Far
     out none of this matters: a real break reads +15 to +17 sd, where a factor
     of three on 1e-98 changes nothing. */
  fprintf(stderr, "            p ~ %.1e (Gaussian tail, optimistic near zero%s)\n",
          pfam, (opt_scoring == SCORE_IC) ? " -- IC most of all" : "");
  /* Fires exactly when the number is in the range where the p-value misleads,
     and stays quiet on a real break. The threshold is the measured 99th
     percentile of pure noise rounded up, not a guess.
       NO line here may begin with something matching '^ *[+-][0-9]' -- that is
     the shape a progress line has, and the documented way to pull a run's
     margin out of stderr is to grep for it. This note used to wrap as
     "... a margin of\n            +0.5 sd came up ...", so the continuation
     WAS such a line and a summary sentence was read back as the run's result.
     Same bug class as the pre-flight lines guard against; found by it biting
     a day-key sweep whose extractor reported this sentence for all 33 keys. */
  if ((z - g_null_zk) < 2.0)
    fprintf(stderr,
            "            below +2 sd is not a find: on signal-free text a "
            "margin\n            of +0.5 sd came up in 2-5%% of runs "
            "(more often on a\n            bigger key space)\n");
}