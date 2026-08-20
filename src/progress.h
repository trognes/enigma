/* Everything the search writes to stderr while it runs: the fixed-width
   progress lines, the live sweep counter, the --confidence null the margin
   column is measured against, and the --doubling-report marker.

   TWO RULES GOVERN THIS MODULE, and both have cost a real bug.

   The columns are budgeted to land on EXACTLY 80, per machine mode and with or
   without the crib column. A change that widens one field has to take the
   width out of the text preview, which is what absorbs the difference.

   No line may LOOK like a progress line. The documented way to read a run's
   margin off stderr is `grep '^ *[+-][0-9]'`, so a wrapped continuation
   beginning with a signed number is read back as the result -- silently, since
   it is a plausible value. That has happened twice: once in the --confidence
   summary and once in the pre-flight block, the second time reporting a caveat
   as the margin for 33 consecutive day keys. tests/run_tests.sh asserts it.

   Display state (best_result::shown) is deliberately never read by the merge,
   so WHICH candidate wins stays -T-deterministic even though which lines
   appear is thread-timing dependent. */

#ifndef ENIGMA_PROGRESS_H
#define ENIGMA_PROGRESS_H

#include "common.h"
#include "machine.h"
#include "result.h"

#include <stddef.h>
#include <mutex>

/* The calibrated null (--confidence). Set once per process before the sweep;
   a zero sd means "not calibrated" and every consumer falls back to printing
   raw scores. */
extern double g_null_mu;
extern double g_null_sd;
extern double g_null_zk;      /* sqrt(2 ln K): the best of K keys by chance */
extern size_t g_null_keys;    /* the K that g_null_zk was computed from */
extern size_t g_null_n;       /* samples behind mu/sd, for the summary */

/* Longest doubling --doubling-report will look for. Also the bound its option
   validation checks, so a too-large L cannot silently search nothing. */
extern const int doubling_maxlen;

/* Highest score echoed by ANY sweep so far. --crib-list runs one sweep per
   crib and the progress lines are one stream to the reader, so the mark that
   suppresses a repeat outlives a single best_result. */
extern double g_shown_high;

/* Which alignment the current key's crib survived at, for the progress line's
   A column. Per worker; display-only. */
void set_crib_stop_shown(int alignment);
int crib_stop_shown(void);

/* One-time column header, then one line per new best. */
void showconfig_header(void);
void showconfig(machine & m, double score);

/* One progress line for a new best. The caller holds best_result::mutex. */
void progress_line(best_result & b, machine & m, double score);

/* Format one machine's key and board into the progress line's columns. Shared
   with the --dump-all and --crib-dump diagnostics so every line agrees. */
void format_key(machine & m, char (&w)[8], char (&r)[8], char (&g)[8]);
void format_plugboard(machine & m, char (&s)[3 * 13]);

/* Serialises the display-only dump diagnostics, so their multiset is
   -T-invariant and only line ORDER is thread-timing dependent. */
std::mutex & dump_mutex();

/* Echo an improvement found mid-climb. Called on ACCEPTED moves only -- never
   inside the 325-move scoring scans -- so it is off the hot path despite being
   called from it. */
void report_climb_progress(machine & m, double score);

/* --dump-all: one line per converged (rotor key, restart) climb, under its own
   mutex so the dumped multiset is -T-invariant. */
void dump_all(machine & m, double score);

/* --doubling-report: flag a converged climb whose decrypt carries a doubled
   word around an X. Self-gating on the z threshold, which is what keeps it
   free -- only ~0.56% of keys reach the scan. */
void report_doubling(machine & m, double score);

/* The live "Progress: NN%" line. arm() before the main sweep and disarm()
   after, so the --ring-stride refinement does not push it past 100%. */
void sweep_progress_arm(size_t total_items, size_t restarts);
void sweep_progress_disarm(void);
/* Read ONCE per worker, outside the key loop, to decide whether to count at
   all -- which is what keeps the line free when it is off. */
bool sweep_progress_armed(void);
void sweep_progress_tick(size_t n, best_result & best);
void sweep_progress_clear(void);

#endif
