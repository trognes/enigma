/* The search's shared best candidate, and the rule that orders two candidates.

   In its own header because both sides need it and neither owns it: the search
   workers write it, the progress display reads it. better_cand() is what makes
   the winner independent of the thread count -- parallel restarts of one key
   often converge to the same score, so the tie-break on the lower work index
   is the -T-independence contract rather than "whichever thread got there
   first". */

#ifndef ENIGMA_RESULT_H
#define ENIGMA_RESULT_H

#include "common.h"
#include "machine.h"

#include <atomic>
#include <mutex>

/* Best result so far, shared across worker threads. It is updated (and the live
   progress line printed) under the mutex only when a worker beats the current
   global best; improvements are rare, so contention is negligible. */
struct best_result
{
  std::mutex mutex;
  double score = score_min;
  size_t idx = static_cast<size_t>(-1);   /* work index of the best (for the tie-break) */
  bool found = false;
  char plaintext[maxlen+1];
  /* Winning plugboard, recorded at the merge so the post-search --polish pass can
     reconstruct the machine (via the key from `idx`) and finish the single best board. */
  unsigned char steckerbrett[asize];
  /* Winning ring/start, recorded the same way. --tune-phase moves ring1/start1
     and ring2/start2 away from the key the work index encodes, so `idx` alone
     no longer identifies the winner and the --polish reconstruction would
     restore the phase the climb STARTED from. Recorded unconditionally (a
     6-byte copy under the merge mutex, off the hot path), read only when
     --tune-phase is on. */
  unsigned char ringstellung[3];
  unsigned char grundstellung[3];
  /* Highest score already ECHOED as a progress line -- display state only, never read
     by the merge logic, so it cannot affect which candidate wins (the -T-determinism
     contract is untouched). It can run ahead of `score`: an intermediate plugboard
     inside a still-running climb echoes as soon as it beats every line shown so far,
     while `score` only advances when a finished work item merges. Atomic so climbing
     workers can pre-check it cheaply (and race-free) before taking the mutex. */
  std::atomic<double> shown{score_min};
  /* Column header emitted before the first progress line (display state, only
     touched under the mutex). */
  bool header_shown = false;
};

/* The live search's shared best. Set by bruteforce() before the workers start,
   null outside a search. */
extern best_result * g_progress;

/* Deterministic ordering: higher score wins, ties go to the lower work index. */
inline bool better_cand(double s1, size_t i1, double s2, size_t i2)
{
  return (s1 > s2) || ((s1 == s2) && (i1 < i2));
}

#endif
