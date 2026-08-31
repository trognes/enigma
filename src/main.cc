#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/resource.h>

#include <stdint.h>

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
#include <cmath>
#include <limits>
#include <vector>

#include "common.h"
#include "options.h"
#include "machine.h"
#include "scoring.h"
#include "plugboard.h"
#include "crib.h"
#include "dedup.h"
#include "confidence.h"
#include "exhaust.h"
#include "keyspace.h"
#include "schedule.h"
#include "search.h"
#include "progress.h"
#include "result.h"
#include "args.h"
#include "cli.h"
#include "preflight.h"
#include "ngrams.h"
#include "text.h"
#include "wiring.h"

/* The run, once args.cc has resolved the command line: read the ciphertext,
   build what depends on it, sweep, and report. */
int main(int argc, char * * argv)
{
  auto t_start = std::chrono::steady_clock::now();

  /* Reads only the environment, and parse_args needs the answer: ranking
     the kick by k means the monogram table has to be loaded. */
  kick_rank_init();

  parse_args(argc, argv);

  ic_blend_init();
  hist_init();
  xstruct_init();
  readciphertext();

  /* Before show_settings(), which reports the hypothesis count -- it read 0 for a while
     because the list is built from the ciphertext and the echo ran first. */
  if (opt_self_crib_seeds > 0)
    init_self_crib();

  show_settings();

  if (textlength < 1)
    fatal("Ciphertext is empty (no A-Z letters on standard input)");

  /* After show_settings() so the resolved configuration is echoed first, and
     after the empty check so the statistics have something to describe. */
  report_preflight();

  for(int i=0; i< textlength; i++)
    num_ciphertext[i] = char2num(ciphertext[i]);

  init();

  init_plug_fixed(opt_steckerbrett, opt_no_plug);   /* -s pairs + --no-plug letters */

  /* --self-crib-seeds: like --crib's menu, the hypothesis list depends on the ciphertext
     (its length and which flanks could be a plaintext X), so it is built here. A message
     too short to hold even the shortest hypothesised signature yields none, which would
     make every key return "no seed" -- say so rather than sweeping and finding nothing. */
  if (opt_self_crib_seeds > 0)
    {
      if (g_selfcrib_nhyps == 0)
        fatal("--self-crib-seeds: no terminal signature of --self-crib-length "
              "letters or more fits this ciphertext (try a smaller value)");
    }

  /* --crib: the menu depends on the ciphertext, so it is built here rather than during
     option validation. The checks that need the ciphertext live here too. */
  if (opt_crib_text)
    {
      int n = static_cast<int>(strlen(opt_crib_text));
      if (n > textlength)
        fatal("--crib is longer than the ciphertext");
      if ((opt_crib_at >= 0) && (opt_crib_at + n > textlength))
        fatal("--crib runs past the end of the ciphertext (check --crib-at)");
      init_crib();
      /* An Enigma never encrypts a letter to itself, so an alignment where the crib
         matches the ciphertext is impossible. With --crib-at that kills the one alignment
         asked for, and every key would be rejected: say so rather than silently finding
         nothing. Sweeping, it is just the filter doing its job -- unless it removes
         everything, which means the crib cannot sit anywhere in this message. */
      if (crib_alignment_count() == 0)
        fatal((opt_crib_at >= 0)
              ? "--crib matches the ciphertext at that position: an Enigma never "
                "encrypts a letter to itself, so the crib cannot sit there"
              : "--crib cannot sit anywhere in this ciphertext: every alignment has "
                "the crib matching the ciphertext, which an Enigma never does");
    }

  /* try all combinations (bruteforce allocates one machine per worker thread) */

  char result[maxlen+1];
  if (opt_crib_list == nullptr)
    bruteforce(result, false);
  else
    run_crib_list(result);

  /* write plaintext */

  fprintf(stderr, "\n");
  printf("%s\n", result);

  /* read plaintext to compare to, if given */

  if (opt_plaintext)
    readplaintext(opt_plaintext, result);

  /* final diagnostic: wall-clock time and memory use */
  double secs = std::chrono::duration<double>
    (std::chrono::steady_clock::now() - t_start).count();
  struct rusage ru;
  double peak_mb = 0.0;
  if (getrusage(RUSAGE_SELF, & ru) == 0)
    {
      /* ru_maxrss is kilobytes on Linux but bytes on macOS/BSD */
#ifdef __APPLE__
      peak_mb = ru.ru_maxrss / (1024.0 * 1024.0);
#else
      peak_mb = ru.ru_maxrss / 1024.0;
#endif
    }
  fprintf(stderr,
          "Analysed %zu rotor combination%s, scored %llu plugboard%s\n",
          g_keys_analysed, (g_keys_analysed == 1) ? "" : "s",
          static_cast<unsigned long long>(g_plugboards_scored),
          (g_plugboards_scored == 1) ? "" : "s");
  if (opt_crib_text || opt_crib_list)
    {
      /* With a list, crib_alignment_count() belongs to whichever crib ran last and would be a lie
         about the run as a whole, so the alignment count is reported only for a single
         crib. The rejection total IS meaningful across cribs: both counters accumulate
         over every sweep. */
      size_t rej = g_crib_rejected.load(std::memory_order_relaxed);
      if (opt_crib_list)
        fprintf(stderr, "Crib: rejected %zu of %zu key%s (%.1f%%) unscored, "
                "over every crib tried\n",
                rej, g_keys_analysed, (g_keys_analysed == 1) ? "" : "s",
                g_keys_analysed ? (100.0 * rej / g_keys_analysed) : 0.0);
      else
        fprintf(stderr,
                "Crib: %d alignment%s, rejected %zu of %zu key%s (%.1f%%) unscored\n",
                crib_alignment_count(), (crib_alignment_count() == 1) ? "" : "s",
                rej, g_keys_analysed, (g_keys_analysed == 1) ? "" : "s",
                g_keys_analysed ? (100.0 * rej / g_keys_analysed) : 0.0);
    }
  /* --seed-dedup: without this line the feature is invisible. Every key is
     still analysed, so the count above is unchanged, and plugboards scored
     falls for reasons that could be anything.

     The unit is a SEED, not a key: one seed is produced per (key, restart)
     work item and it is that seed's full climb that is skipped, so the
     denominator is the seed count. Dividing by the key count instead would
     misreport by a factor of R.

     "duplicate" is approximate by construction -- a Bloom false positive is
     indistinguishable from a duplicate at run time -- and the settings echo
     gives the expected rate so the two can be read together. The line reports
     CLIMBS, not a compute saving: the cheap stage ran on every seed, since
     that is what produced the seed being tested. */
  if (seed_dedup_on())
    {
      const unsigned long long sk =
        static_cast<unsigned long long>(seed_dedup_skipped());
      const unsigned long long seeds =
        static_cast<unsigned long long>(seed_dedup_seeds());
      fprintf(stderr,
              "Skipped %llu full climb%s on duplicate seeds of %llu (%.1f%%)\n",
              sk, (sk == 1) ? "" : "s", seeds,
              seeds ? (100.0 * static_cast<double>(sk)
                       / static_cast<double>(seeds)) : 0.0);
    }
  fprintf(stderr, "Finished in %.2f s using %d thread%s\n",
          secs, opt_threads, (opt_threads == 1) ? "" : "s");
  fprintf(stderr, "Precomputed %zu rotor table%s (%.1f MB); peak memory %.0f MB\n",
          g_table_count, (g_table_count == 1) ? "" : "s",
          g_table_bytes / (1024.0 * 1024.0), peak_mb);
}
