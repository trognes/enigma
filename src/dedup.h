/* Seed deduplication: skip the expensive target climb when this restart's
   cheap-stage seed has already been climbed for this key.

   THE SEED DETERMINES THE CLIMB. Under a staged schedule the board after
   stage 0 is a deterministic function of (key, restart) -- the kick comes from
   restart_seed() and the capped climb from it is deterministic -- so two
   restarts reaching the same seed produce byte-identical results. At high
   restart counts that is most of the work: measured 17% duplicate seeds at
   -R 100 and 73% at -R 10 000 (eval/results-experiment-f.txt 7).

   WHY THIS IS NOT A HASH TABLE. One exact set per key over a keyspace of tens
   of millions is far past any memory budget; a Bloom filter trades a small,
   quantified loss of coverage for a fixed cost per key. A false positive skips
   a seed that was NOT a duplicate, so the filter costs distinct seeds and the
   sizing is chosen to keep that well under what the skipping buys back.

   WHY THE BLOCK IS EIGHT BYTES. A lookup reads one uint64: one load, one AND,
   one compare. 8 divides 64, so an aligned word never straddles a cache line
   and the single-read property comes for free rather than being engineered --
   and the per-key region rounds up to 8 bytes rather than to 64, so at most 7
   bytes per key are wasted instead of up to 63. That continuous sizing is what
   makes the payoff grow with -R instead of peaking and reversing as the filter
   saturates. SEED_DEDUP.md has the measurements.

   NO LOCKS AND NO ATOMICS ON THE FILTER, because the sweep runs one restart
   pass at a time (search.cc) and a key appears exactly once per pass. Within a
   pass no two threads touch one key's region; across passes the run_parallel
   join separates them. That is also what keeps the skip decision -- and so the
   whole run -- independent of -T. */

#ifndef ENIGMA_DEDUP_H
#define ENIGMA_DEDUP_H

#include <stddef.h>
#include <stdint.h>

/* Allocate the filter for `nkeys` keys x `restarts` seeds each. Call once,
   after the key space is resolved and before the sweep. Returns false with a
   message on stderr if the budget cannot be met or the allocation fails; the
   caller then has a fatal error, not a silent downgrade. Inert (returns true,
   allocates nothing) when the option is off. */
bool seed_dedup_init(size_t nkeys, size_t restarts);

/* Query-and-insert. True means "this seed has been seen for this key" and the
   caller should skip the target climb; the board is inserted when it is new.
   `key` indexes the per-key region, `board` is the 26-byte involution.
   Off => always false, so the caller needs no second test. */
bool seed_dedup_seen(size_t key, const unsigned char * board);

/* Whether a filter is allocated -- for the settings echo and the guard that
   keeps the query out of the default path. */
bool seed_dedup_on();

/* Turn the filter off for a nested search that reuses search_worker over its
   OWN key space -- the --ring-stride refinement.  Its key indices start again
   at 0 and so ALIAS the coarse pass's per-key regions: without this the
   refinement both consumes coarse regions and gets its own climbs skipped by
   seeds it never produced.  Suspending is the whole of what "the refinement
   runs unfiltered" means, and it is not optional.

   Set from the single thread that drives the refinement, before it fans out
   and after it joins, so no synchronisation is needed. */
void seed_dedup_suspend(bool off);

/* Reporting. `skipped` is the number of full climbs not run, reported against
   the SEED count below -- never against the key count, which would be wrong by
   a factor of R. `describe` fills a caller-owned buffer with the geometry line
   for show_settings(). */
uint64_t seed_dedup_skipped();
/* Seeds actually produced -- the denominator the skip line reports against.
   Counted here rather than as keys x restarts, because a key the middle-wheel
   collapse (or a crib) rejects never reaches stage 0 and so never makes a
   seed. */
uint64_t seed_dedup_seeds();
void seed_dedup_describe(char * buf, size_t buflen);

/* Release the filter. */
void seed_dedup_free();

#endif
