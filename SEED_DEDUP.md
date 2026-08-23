# Seed deduplication — design

**Status: planned, not built.** This is the design record; nothing in `src/`
implements it yet. Written before the code, as `ENHANCEMENTS.md` §2a's
experiments were.

## 1. What it does, and the one number that justifies it

Under a staged schedule (`-S k4f10`) each restart runs a **cheap capped
pre-pass** and then an **expensive target climb**. The pre-pass output — call
it the *seed* — is a deterministic function of `(key, restart)`, because the
kick comes from `restart_seed(key_index, restart)` and the climb from that kick
is deterministic. So if two restarts of one key produce the same seed, the
target climb from it produces a **byte-identical** result (verified directly,
6/6, and by construction under `-R 0`).

Re-running it is therefore pure waste, and at operational restart counts there
is a lot of it (`eval/results-experiment-f.txt` §7, 100 keys at L = 100,
default 10-plug kick):

| `-R` | duplicate seeds | `f10` share | compute wasted |
|---:|---:|---:|---:|
| 8 | 1.4% | 64% | 0.9% |
| 32 | 7.7% | 64% | 4.9% |
| 100 | 17% | 64% | **11%** |
| 1000 | 43.8% | 64% | 28% |
| 10000 | 73.1% | 64% | 47% |

**Skip the target climb when the seed has been seen before for that key.**
The pre-pass still runs every restart — it is what produces the seed — so the
ceiling on the saving is `duplicate_fraction × f10_share`.

**Scope.** This targets a large keyspace at a restart count where restarts
still buy something on hard messages. It is deliberately *not* aimed at the
pinned-key case: one key at `-R 10 000` is 78 KB of state and six seconds of
work, so there is nothing to optimise there.

## 2. What is hashed

The **seed board** after the first `--score` stage: `machine.steckerbrett`,
26 bytes, where `steck[i]` is `i`'s partner and `steck[i] == i` means
unplugged. That is already a canonical form — no normalisation needed.

- With a **single-stage** schedule there is no cheap prefix, so the feature is
  inert and says so.
- With **more than two stages** the seed is the board after stage 0. Later
  stages are part of what gets skipped.

## 3. The filter

**Per key, blocked, one cache line per lookup.**

```
filter  : one flat byte array, K * bytes_per_key
key i   : bytes [i*bytes_per_key, (i+1)*bytes_per_key)
lookup  : h = hash64(board, opt_seed)
          block = key_base + (h >> 32) % blocks_per_key     // 64-byte block
          pattern = k bits derived from (h & 0xffffffff)
          present = (block & pattern) == pattern            // ONE cache line
          insert  = block |= pattern
```

One 64-byte block per lookup means **one memory read**, which is the point of
blocking. The cost is a slightly worse false-positive rate than an unblocked
filter of the same size, because a block that happens to receive more items
than average is disproportionately bad — but at 512-bit blocks that penalty is
negligible:

| bits/item | k | FP unblocked | FP blocked (512-bit) |
|---:|---:|---:|---:|
| 5.12 | 4 | 8.64% | 8.78% |
| 8.00 | 6 | 2.16% | **2.33%** |
| 10.24 | 7 | 0.73% | 0.86% |

**Access is sequential, and that is a property worth protecting.** Restart is
the outer loop, so a pass walks key 0…K−1 in order and touches each key's
region exactly once. The filter therefore *streams*: no TLB thrash, no random
access, and total traffic is `K × bytes_per_key` per pass — under 2 MB/s
averaged over a week-long run. It also means the array can be `mmap`ed from a
file if RAM is short, with the page cache handling it efficiently. Do not
reorganise the loop so that this stops being true.

### Sizing

The natural target is **8 bits per item**, giving 2.33% FP. Memory is then
`K × R` bytes, since one item is inserted per restart:

| K | `-R` | bytes/key | bits/item | FP | total |
|---:|---:|---:|---:|---:|---:|
| 79.6 M | 64 | 64 (1 line) | 8.00 | 2.33% | **4.74 GiB** |
| 79.6 M | 100 | 100 | 8.00 | 2.33% | 7.41 GiB |
| 79.6 M | 100 | 64 (1 line) | 5.12 | 8.78% | 4.74 GiB |
| 79.6 M | 128 | 128 (2 lines) | 8.00 | 2.33% | 9.49 GiB |

**Do not round the per-key region up to a whole cache line.** At `-R 100` that
turns 100 bytes into 128 and costs 2 GiB for nothing. A key's region is
`bytes_per_key` bytes at byte granularity; blocks within it are 64-byte
aligned relative to the array base, and the final partial block is simply
smaller. Only the *lookup* needs to hit one line, not the whole region.

**Sizing is budget-driven, not hard-coded.** The user gives a memory budget;
the tool derives bits per item and reports what that implies:

```
bits_per_item = budget_bytes * 8 / (K * R)
```

and refuses (rather than silently degrading) below ~4 bits/item, where FP
exceeds ~15% and the filter costs more coverage than it saves.

### Hash

64-bit, seeded from `opt_seed` so a run reproduces and `-e` controls it, in
keeping with `restart_seed`'s existing convention. Requirements: well-mixed in
both halves (the high half picks the block, the low half the pattern), and
fast — a few ns against a 246 µs pre-pass, so almost anything works. Derive
the k-bit pattern from a small precomputed table indexed by bytes of the low
half, which is the standard cache-line-Bloom trick and avoids k separate
modulo operations.

## 4. Concurrency and determinism

**The filter is per key, and each key appears exactly once per pass** — the
flat work index is `restart × keys + key`. So within a pass no two work items
ever touch the same key's region. All that is needed is to stop threads from
being in *different* passes on the same key simultaneously.

**Run one `run_parallel` per pass.** The existing join at the end of
`run_parallel` is the barrier; no new primitive is required. Then when key `i`
is queried at pass `r`, its filter contains exactly the seeds of passes
`0…r−1`, which makes the skip decision a deterministic function of
`(key, restart, opt_seed)` — **`-T`-independence is preserved**, there is no
data race, and the filter needs no atomics.

**Chunk by target duration, not by a fixed divisor.** Today the chunk is
`work_items/(16·threads)`, which spans about `R/16` passes. Per pass the chunk
should be sized so that a chunk is ~10 s of work: that gives ~0.1% barrier
tail idle, and one atomic `fetch_add` per few thousand climbs, which is
nothing. Calibrate from the observed rate of pass 0 and hold it for the rest;
clamp to at least one key and at most `keys/threads`.

Cost of the barrier, at `T = 8`, `K = 79.6 M`, `-R 100`:

| chunk target | chunks/thread/pass | tail idle | atomics over the run |
|---|---:|---:|---:|
| 60 s | 99 | 0.5% | ~79 k |
| **10 s** | **595** | **0.08%** | ~476 k |
| 2 s | 2 980 | 0.02% | ~2.4 M |

## 5. CLI

- `--seed-dedup BYTES` — enable, with a memory budget (`4G`, `512M`). Off by
  default; absent means the current behaviour, byte for byte.
- Echo in `show_settings()`: budget, `bytes_per_key`, bits/item, `k`, and the
  **expected false-positive rate**, because that is a coverage loss the user is
  choosing to accept and it must not be buried.
- Final diagnostic: climbs skipped, as a count and a percentage, plus the
  observed duplicate rate. Without this the feature is invisible and its
  benefit unmeasurable.

## 6. Interactions — refuse rather than guess

Reject with a clear message (these either re-encode the work index, install
their own starting board, or have no staged seed):

`-F`, `--exhaust`, `--crib` / `--crib-list`, `--tune-phase`, `-A`,
`--ring-stride` (its refinement reuses `search_worker` over a private key
space and would need a filter of its own), and any single-stage `--score`.

`-R 0` is inert (one climb, nothing to repeat). `--polish` is unaffected: it
runs once on the best board after all restarts.

**A skipped item must not reach the merge.** It scores nothing, so it cannot
win; and because a duplicate would have produced an identical board and score,
and `better_cand` breaks ties on the *lowest* work index, the surviving
original already wins any tie the duplicate could have entered. Progress
accounting must still count the item, or the percentage and ETA go wrong.

## 7. Verification

1. **Off is byte-identical.** With the flag absent, output and `score_iter`
   match the current binary exactly on a matrix of invocations.
2. **A disabled filter is byte-identical.** With the filter forced to always
   answer "not present", output matches dedup-off — proving the skip path is
   the only difference.
3. **Exact-set equivalence.** On a small keyspace, run with the Bloom filter
   replaced by an exact `unordered_set`. The skipped set must be a superset
   with the difference explained entirely by false positives, and at a large
   `bits_per_item` the two must agree exactly.
4. **`-T` independence.** Same output at `-T 1/2/4/8` with dedup on. This is
   the check the whole barrier design exists for.
5. **Skip count matches prediction.** ~17% at `-R 100`, ~44% at `-R 1000`,
   from `eval/results-experiment-f.txt` §7.
6. **TSan** with dedup on and `-T 4`. The suite's TSan job is a handful of
   hand-picked invocations; a new one is required here, because a path that
   spawns threads and touches shared state is exactly what it is for.
7. **Bench** with the flag off, to confirm the added branch costs nothing on
   the default path.

## 8. Falsification, in advance

The saving is compute, not recovery. It converts into breaks only where
restarts still pay, so the pre-registered test is an **end-to-end, matched
wall time** comparison on hard cases — short messages at a restart count where
the ladder is still climbing — not a `score_iter` or wall-clock ratio.

If at matched wall time dedup-on does not break more messages than dedup-off,
the feature is recorded as measured-down and removed, however good the compute
saving looks. That is the same rule experiments D, E and F ran under, and the
`score_iter` traps in `results-monoic-endtoend.txt` and
`results-experiment-f.txt` are why the test has to be end to end.

**Expected magnitude, stated up front so it can be wrong:** ~11% of compute at
`-R 100`, minus ~2.3% of distinct seeds lost to false positives, minus ~0.1%
barrier idle — call it 8–9% effective. On a flat part of the restart curve
that is worth almost nothing; the case for building it rests on the claim that
the curve is *not* flat for difficult messages, which `CLAUDE.md`'s restart
ladder supports (recovery still climbing at `-R 5000` at L = 60–100) but which
has not been measured at this keyspace scale.

## 9. Deliberately not done

- **No dedup of converged boards.** The removed `--restart-tt` hashed those,
  by which point the expensive climb is already paid for. Duplication is
  higher downstream (81.5% against 73.1% at `-R 10 000`), but catching it
  saves nothing.
- **No cross-key filter.** One shared filter would remove the per-key rounding
  waste, but key `i`'s inserts would then affect key `j`'s queries *within* a
  pass, and the determinism argument in §4 collapses.
- **No adaptive resizing.** A Bloom filter cannot grow; the budget is fixed at
  startup from `K` and `R`, both known then.
