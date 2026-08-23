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

**Per key, blocked, one 64-bit word per lookup.**

```
filter  : one flat uint64 array, K * blocks_per_key words
key i   : words [i*blocks_per_key, (i+1)*blocks_per_key)
lookup  : h = hash64(board, opt_seed)
          w = key_base[(h >> 32) % blocks_per_key]    // ONE 8-byte load
          pattern = k bits derived from (h & 0xffffffff)
          present = (w & pattern) == pattern
          insert  = w |= pattern
```

**The block is 8 bytes, and the per-key region is rounded up to a multiple of
8.** At `-R 100` and 8 bits per item that is 800 bits = 100 bytes, rounded to
**104 bytes = 13 blocks = 832 bits**.

Three things follow, and together they are why the block is a word and not a
cache line:

- **The lookup is one load, and it is still one cache line.** 8 divides 64, so
  an 8-byte aligned word never straddles a line — the single-read property is
  kept for free rather than engineered. The operation is then literally one
  `uint64` load, one AND and one compare, where a 512-bit block means testing
  `k` bits scattered across eight words of the line.
- **Waste is at most 7 bytes per key** instead of up to 63. That is what lets
  memory track `-R` continuously; the cache-line version jumped in 64-byte
  steps, which at `-R 100` meant either 36% too little or 23% too much.
- **The alignment problem disappears.** An earlier draft of this document sized
  regions at byte granularity and was wrong — with 100-byte regions an aligned
  64-byte block is not *contained* in one (key 2 owns bytes [200, 300) and
  neither line [192, 256) nor [256, 320) fits inside it), so the choice was
  between straddling two lines 63/64 of the time and sharing a boundary line
  between neighbouring keys, which would put key *j*'s inserts into key *i*'s
  lookups within a pass — a race at every chunk boundary and the end of §4's
  determinism argument. At 8-byte granularity neither arises: 8 | 64, and
  8 divides every region size.

The cost is a worse false-positive rate than a large block, because a block
that happens to receive more items than average is disproportionately bad and
a 64-bit block sees far more of that scatter:

| bits/item | unblocked | 64-bit block | 512-bit block |
|---:|---:|---:|---:|
| 6.17 | 5.18% | 6.16% (k=4) | 5.31% (k=4) |
| 8.00 | 2.16% | **3.19%** (k=4) | 2.30% (k=5) |
| 10.02 | 0.81% | **1.65%** (k=5) | 0.94% (k=7) |
| 12.00 | 0.31% | 0.95% (k=6) | 0.41% (k=8) |

Roughly a factor of two in FP, against a factor of nine in rounding waste and
a simpler inner loop. The two middle rows are the operating point: 8 bits per
item *requested* becomes 10.02 *effective* at `-R 100` for the reason below,
so the FP actually paid there is 1.65%, not 3.19%.

**Access is sequential, so caching takes care of itself.** Restart is the outer
loop, so a pass walks key 0…K−1 in order and touches each key's region exactly
once. The filter *streams*: no TLB thrash, no random access, and total traffic
is `K × bytes_per_key` per pass — under 2 MB/s averaged over a week-long run.
It also means the array can be `mmap`ed from a file if RAM is short, with the
page cache handling it efficiently. Do not reorganise the loop so that this
stops being true; the block size is chosen on rounding waste, not on locality,
and it is the sequential sweep that makes that safe.

### Sizing

**Bits per item is the option, default 8**, and `k` follows from it rather
than being set separately — chosen to minimise the *blocked* FP at the
effective load, which is not the textbook `0.693 · bits_per_item` (small
blocks favour a slightly lower `k`: 5 rather than 7 at 10 bits per item).
Memory is

```
blocks_per_key = max(1, ceil(R * bits_per_item / 64))
total          = K * 8 * blocks_per_key
```

**The filter holds only DISTINCT seeds, so sizing on `R` is conservative.** A
duplicate is detected and skipped, not re-inserted, so the load is
`R × (1 − duplicate_rate)`: 83 items at `-R 100`, not 100. Together with the
round-up that turns a *requested* 8 bits per item into an *effective* 10.02 at
`-R 100` — which is why the FP paid is 1.65% rather than the 3.19% the table
above lists for 8. Both numbers must be echoed, or the setting and the
behaviour disagree.

At `K` = 79.6 M, with 8 bits per item requested:

| `-R` | B/key | blocks | total | items | eff. bits | k | FP |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 64 | 8 | 4.74 GiB | 55.5 | 9.23 | 5 | 2.10% |
| **100** | **104** | **13** | **7.71 GiB** | 83.0 | 10.02 | 5 | **1.65%** |
| 200 | 200 | 25 | 14.83 GiB | 149.9 | 10.68 | 5 | 1.36% |
| 1000 | 1000 | 125 | 74.13 GiB | 562.0 | 14.23 | 6 | 0.54% |

**Because memory tracks `-R` continuously, the payoff grows with the restart
budget** rather than peaking and reversing. Net distinct seeds climbed at
matched wall time (`f10` = 64% of a restart):

| `-R` | distinct, off | distinct, on | FP | net |
|---:|---:|---:|---:|---:|
| 64 | 55.5 | 59.9 | 2.0% | +8.0% |
| **100** | **83.0** | **91.8** | 1.5% | **+10.6%** |
| 200 | 149.9 | 175.2 | 1.2% | +16.9% |
| 1000 | 562.0 | 744.8 | 0.4% | +32.5% |

That trend is the property to protect, and it is what an earlier cache-line
draft of this section did not have: held at a fixed region size the same
curve peaked at `-R 64` and went **negative by `-R 200`**, because the filter
saturated faster than the duplicate rate rose. Sizing by bits per item rather
than by a memory budget is what keeps it monotone. A `--seed-dedup-max BYTES`
cap is still useful, but it must **refuse** when the requested bits per item
does not fit, naming what would fit, rather than silently thinning the filter.

What each setting costs at `-R 100`, `K` = 79.6 M, so a budget can be met by
choosing bits rather than by degrading silently:

| bits asked | B/key | eff. bits | k | FP | total |
|---:|---:|---:|---:|---:|---:|
| 5 | 64 | 6.17 | 4 | 6.16% | 4.74 GiB |
| 6 | 80 | 7.71 | 4 | 3.51% | 5.93 GiB |
| **7** | 88 | 8.48 | 5 | 2.70% | **6.52 GiB** |
| 8 | 104 | 10.02 | 5 | 1.65% | 7.71 GiB |
| 10 | 128 | 12.34 | 6 | 0.87% | 9.49 GiB |

So a 7 GB ceiling at `-R 100` buys 7 bits per item, not the default 8. At
`-R 1000` on this keyspace nothing fits at all (74 GiB), so the feature is for
`R` of order 100 on a large keyspace, or for high `R` on a small one.

### Hash

64-bit, seeded from `opt_seed` so a run reproduces and `-e` controls it, in
keeping with `restart_seed`'s existing convention. Requirements: well-mixed in
both halves (the high half picks the block, the low half the pattern), and
fast — a few ns against a 246 µs pre-pass, so almost anything works.

**A 64-bit block makes the pattern cheap.** Each of the `k` bits needs 6 bits
of hash to name a position in the word, so `k ≤ 5` fits in the low 32 bits
with nothing left over to reuse, and the pattern is `k` shifts and ORs. At
`k = 6` (the `-R 1000` row) take the extra 6 bits from the high half *below*
the bits used for the block index. The alternative — a precomputed table of
byte → sparse-word masks — buys nothing here, because the shifts are already
a handful of instructions and the table would be a second memory touch.

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

- `--seed-dedup` — enable. Off by default; absent means the current behaviour,
  byte for byte.
- `--seed-dedup-bits N` — bits per item, **default 8**, range ~4…24. Below 4
  the FP exceeds ~15% and the filter costs more coverage than it saves, so
  that is a refusal rather than a warning.
- `--seed-dedup-max BYTES` — optional ceiling (`4G`, `512M`). It **refuses**
  when the requested bits per item does not fit, naming the largest that does;
  it never thins the filter silently.
- Echo in `show_settings()`: `blocks_per_key`, bytes per key, total bytes, the
  **effective** bits per item (which is not the requested figure — the round-up
  to 8 bytes and the distinct-only load both move it; see §3), `k`, and the
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

**Expected magnitude, stated up front so it can be wrong.** The right currency
is **distinct seeds climbed at matched wall time**, not compute saved — a
skipped duplicate is worth nothing on its own, and a false positive costs a
distinct seed. At `-R 100` and `K` = 79.6 M that is **+10.6%** (§3), rising to
+16.9% at `-R 200` and +32.5% at `-R 1000` where the memory allows it. On a
flat part of the restart curve even the largest of those is worth nothing; the
case for building it rests on the claim that the curve is *not* flat for
difficult messages, which `CLAUDE.md`'s restart ladder supports (recovery still
climbing at `-R 5000` at L = 60–100) but which has not been measured at this
keyspace scale.

**+10.6% at the operating point is a modest effect**, of the order of a
restart-count change, and it should be judged as one — the reason to build it
is that the effect *grows* with `-R` while the alternatives do not, and that
the run measuring it also measures the restart curve at a scale nothing here
has reached.

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
