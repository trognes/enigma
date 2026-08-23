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
filter  : one flat byte array, 64-byte aligned, K * 64 * lines_per_key
key i   : lines [i*lines_per_key, (i+1)*lines_per_key)
lookup  : h = hash64(board, opt_seed)
          block = key_base + 64 * ((h >> 32) % lines_per_key)
          pattern = k bits derived from (h & 0xffffffff)
          present = (block & pattern) == pattern            // ONE cache line
          insert  = block |= pattern
```

One 64-byte block per lookup means **one memory read**, which is the point of
blocking. The cost is a slightly worse false-positive rate than an unblocked
filter of the same size, because a block that happens to receive more items
than average is disproportionately bad — but at 512-bit blocks that penalty is
negligible, and it is what buys the single read:

| bits/item | k | FP unblocked | FP blocked (512-bit) |
|---:|---:|---:|---:|
| 5.12 | 4 | 8.64% | 8.78% |
| 8.00 | 6 | 2.16% | **2.33%** |
| 10.24 | 7 | 0.73% | 0.86% |

Smaller blocks make it worse fast — at 8 bits/item, 2.33% at 512 bits becomes
2.50% at 256, 2.84% at 128 and 3.47% at 64 — so the block stays a full line.

**A key's region must be a WHOLE NUMBER OF LINES.** An earlier draft of this
document said the opposite ("do not round the per-key region up to a whole
cache line ... only the *lookup* needs to hit one line"), and that is wrong:
with `bytes_per_key = 100` the regions are not 64-byte aligned, so an aligned
block is not *contained* in one — key 2 owns bytes [200, 300) and neither line
[192, 256) nor [256, 320) fits inside it. The two ways out both fail. An
unaligned 64-byte window straddles two cache lines 63/64 of the time, which
is precisely the property blocking exists to provide. And letting neighbouring
keys share a boundary line puts key *j*'s inserts into key *i*'s lookups
*within* a pass — a data race at every chunk boundary, and the end of the
determinism argument in §4. So the region is `64 * lines_per_key` bytes and
the **effective** bits per item is reported after rounding.

**Access is sequential, and that is a property worth protecting.** Restart is
the outer loop, so a pass walks key 0…K−1 in order and touches each key's
region exactly once. The filter therefore *streams*: no TLB thrash, no random
access, and total traffic is `K × bytes_per_key` per pass — under 2 MB/s
averaged over a week-long run. It also means the array can be `mmap`ed from a
file if RAM is short, with the page cache handling it efficiently. Do not
reorganise the loop so that this stops being true.

### Sizing

**Bits per item is the option, default 8**, and `k` follows from it
(`k = round(0.693 · bits_per_item)`, clamped to 1…16) rather than being set
separately. Memory is

```
lines_per_key = max(1, ceil(R * bits_per_item / 512))
total         = K * 64 * lines_per_key
```

Note the two roundings pull opposite ways and the tool must report both: at
`-R 100` a default of 8 bits/item asks for 100 bytes and gets **128**, while
at `-R 8` it asks for 8 and still gets 64. The feature only makes sense where
`R * bits_per_item ≥ 512` — below that the rounding dominates and duplication
is too rare to be worth the memory anyway (1.4% at `-R 8`).

**The filter holds only DISTINCT seeds, so sizing on `R` is conservative.** A
duplicate is detected and skipped, not re-inserted, so the load is
`R × (1 − duplicate_rate)`: 83 items at `-R 100`, not 100. That is worth a
whole step of the FP table.

At `K` = 79.6 M, one line per key = **4.74 GiB**, two = 9.49 GiB:

| `-R` | lines | total | items | bits/item | k | FP |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 1 | 4.74 GiB | 55.5 | 9.23 | 6 | 1.3% |
| 100 | 1 | **4.74 GiB** | 83.0 | 6.17 | 4 | **5.3%** |
| 100 | 2 | 9.49 GiB | 83.0 | 12.34 | 8 | 0.35% |
| 1000 | 16 | 75.91 GiB | 562.0 | 14.58 | 9 | 0.14% |

**Memory must grow with `-R` or the filter eats the coverage it saves.** Held
at one line per key, the net gain in distinct seeds at matched wall time peaks
and then goes negative:

| `-R` | distinct, off | distinct, on | FP | net |
|---:|---:|---:|---:|---:|
| 64 | 55.5 | 59.9 | 1.9% | **+8.1%** |
| 100 | 83.0 | 89.0 | 8.1% | +7.2% |
| 128 | 102.6 | 108.1 | 14.4% | +5.4% |
| 200 | 149.9 | 144.3 | 31.3% | **−3.7%** |

That is the argument for sizing by bits per item rather than by a memory
budget: bits per item is the quantity that has to stay fixed as `R` rises. A
`--seed-dedup-max BYTES` cap is still useful, but it should **refuse** when
the requested bits per item does not fit, naming what would fit, rather than
silently thinning the filter into the negative row above.

Two consequences worth stating plainly. At `K` = 79.6 M and a 7 GB ceiling,
`-R 100` can afford **one** line per key, i.e. 6.17 bits/item and ~5.3% FP —
the default of 8 does not fit. And at `-R 1000` on that keyspace nothing fits
at all (75.9 GiB), so this feature is for `R` of order 100 on a large
keyspace, or for high `R` on a small one.

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

- `--seed-dedup` — enable. Off by default; absent means the current behaviour,
  byte for byte.
- `--seed-dedup-bits N` — bits per item, **default 8**, range ~4…24. Below 4
  the FP exceeds ~15% and the filter costs more coverage than it saves, so
  that is a refusal rather than a warning.
- `--seed-dedup-max BYTES` — optional ceiling (`4G`, `512M`). It **refuses**
  when the requested bits per item does not fit, naming the largest that does;
  it never thins the filter silently.
- Echo in `show_settings()`: `lines_per_key`, total bytes, the **effective**
  bits per item after line rounding (which is not the requested figure — see
  §3), `k`, and the **expected false-positive rate**, because that is a
  coverage loss the user is choosing to accept and it must not be buried.
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
distinct seed. At `-R 100`, `K` = 79.6 M and one line per key (5.3% FP), that
is **+7.2%**; at two lines per key it would be ~+11%. Both are small, and on a
flat part of the restart curve they are worth nothing at all. The case for
building it rests on the claim that the curve is *not* flat for difficult
messages, which `CLAUDE.md`'s restart ladder supports (recovery still climbing
at `-R 5000` at L = 60–100) but which has not been measured at this keyspace
scale.

**So the honest prior is that this is a marginal feature**, and the §3 table
says the margin can be negative if the filter is under-sized. It is worth
building only because the same run that measures it also measures the restart
curve at a scale nothing here has reached.

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
