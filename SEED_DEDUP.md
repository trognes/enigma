# Seed deduplication — design

**Status: BUILT** (`--seed-dedup`, `src/dedup.cc`). This began as the design
record, written before the code as `ENHANCEMENTS.md` §2a's experiments were,
and it is kept in that shape: the reasoning that led to each decision is worth
more than a description of the result. §10 records what implementation changed,
and the retracted designs stay in place rather than being tidied away.

**What it still owes is the measurement in §8**, which has not been run: the
feature is verified correct and free when off, and it is NOT yet shown to break
more messages at matched wall time.

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
  **provisioned** bits per item, `k`, and the **expected false-positive rate**,
  because that is a coverage loss the user is choosing to accept and it must
  not be buried.

  **Provisioned, not effective — the echo cannot know the effective figure.**
  §3 gives 10.02 bits per item at `-R 100`, but that divides by the 83
  *distinct* seeds, a realised quantity no run knows before it has run. At echo
  time only `832 / R = 8.32` is available, so that is what is printed, with the
  FP bound at that load (~2.8%) rather than the 1.65% actually paid. Printing
  the optimistic figure up front would be a claim about the run's outcome. The
  realised rate follows from the final skip line, and the manual should say how
  to get there rather than the echo pretending to know it.
### Reporting the skips — required, not optional

**The run must report how many full climbs were skipped, as a count and a
percentage.** Without it the feature is invisible: nothing else in the output
moves in a way that identifies it (`Analysed N rotor combinations` is
unchanged, since every key is still analysed, and `plugboards scored` falls
for reasons that could be anything). It sits with the existing final
diagnostics:

```
Skipped 13 542 118 full climbs on duplicate seeds of 7 960 000 000 (17.0%)
```

Three things about that line:

- **The unit is a SEED, not a key.** No key is ever skipped — a key is
  visited once per pass whatever the filter says. One seed is produced per
  `(key, restart)` work item, and it is that seed's *full climb* that is
  skipped, so the denominator is the seed count `K × R`, not `K`. Reporting
  skipped *keys* would misreport by a factor of `R`.
- **The count is an upper bound on true duplicates**, because a false positive
  is indistinguishable from a duplicate at run time — that is what a Bloom
  filter is. At the §3 operating point the FP share of the skips is small
  (~1.65% of distinct seeds against a 17% duplicate rate, so roughly 8% of the
  reported skips are not duplicates), and the echo already gives the expected
  FP rate, so the two can be read together. The word *duplicate* in the line is
  therefore approximate by construction, and the manual should say so rather
  than the line growing a caveat.
- **A skipped seed is not skipped work — it is a skipped CLIMB.** The pre-pass
  ran in full; that is what produced the seed to test. So the compute saved is
  the skip percentage times the target stage's share (~64% at `-S k4f10`),
  which is why the line names the climbs rather than claiming a saving — a
  "saved 17% of compute" line would be wrong by that factor.

**Mid-run visibility matters more than the final line here**, because the runs
this is for last days. The live sweep progress line already carries a pass
field under a restart-major sweep, and a duplicate percentage fits inside the
80-column budget:

```
Progress:  43% (pass 44/100, 3.4G / 7.9G keys) 11.0k/s, 4d2h left, 12% dup
```

That is 74 columns at the widths above; if a future field pushes it past 80
the duplicate figure is the one to drop, since the final line always carries
it.

## 6. Interactions — refuse rather than guess

Reject with a clear message (these either re-encode the work index, install
their own starting board, or have no staged seed):

`-F`, `--exhaust`, `--crib` / `--crib-list`, `--tune-phase`, `-A`, and any
single-stage `--score`.

**`--ring-stride` is ALLOWED, with the filter on the coarse pass only.** An
earlier draft refused it, on the grounds that the refinement reuses
`search_worker` over a private key space — true, but it only argues against
filtering the *refinement*, and the coarse pass is an ordinary restart-major
sweep that dedups like any other. So the filter is sized and indexed on the
**coarse** key count (`range.r2_vals` already holds the strided set, the same
quantity `--confidence` builds its bar from), and `refine_ring_stride()` runs
with the filter off.

That costs nothing: the refinement is one pass over 25 ring2 values on a single
pinned wheel order — hundreds of keys against the coarse pass's tens of
millions — so there is no duplication there worth catching, and leaving it
unfiltered also keeps it free of the pass-barrier restructuring below. The
refusal mattered in practice, because `--ring-stride 3` is the recommended
setting for exactly the week-long large-keyspace run this feature is for; a
version that refused it would have been unusable on its own motivating case.

`-R 0` is inert (one climb, nothing to repeat). `--polish` is unaffected: it
runs once on the best board after all restarts.

**A skipped item must not reach the merge.** It scores nothing, so it cannot
win; and because a duplicate would have produced an identical board and score,
and `better_cand` breaks ties on the *lowest* work index, the surviving
original already wins any tie the duplicate could have entered. Progress
accounting must still count the item, or the percentage and ETA go wrong.

## 7. Verification

*Status of each is in §10.*

1. **Off is byte-identical.** With the flag absent, output and `score_iter`
   match the current binary exactly on a matrix of invocations.
2. **A disabled filter is byte-identical.** Run at `-R 1`, where no duplicate
   can exist: the result must equal the ordinary staged climb. This is what
   makes the second copy of the stage loop safe, and it is cheaper and less
   invasive than the forced-"not present" build first proposed.
3. **Skipping does not change the answer.** Under `--random 0` every restart
   climbs from the same seed, so the skipped climbs would have reproduced the
   kept one exactly: an equivalence, not an approximation. A monotonicity check
   stands in for the exact-set arm — a larger `bits_per_item` can only skip
   FEWER seeds, since extra bits remove only false positives.
4. **`-T` independence.** Same output at `-T 1/2/4/8` with dedup on, **and the
   same reported skip count** — the counter is part of the contract, not a
   by-product, so a `-T`-dependent total means the barrier is not doing its
   job even if the winning board happens to agree.
5. **Skip count is exact under `--random 0`.** That is the cheap test fixture,
   and it is the one to write first: with no kick every restart climbs from the
   same unperturbed seed, so at `-R 4` exactly **3 of 4** are duplicates and the
   line must read 75.0% — an exact expected value on a 26-key keyspace, in a
   fraction of a second. The natural fixture (real kicks) duplicates only 1.4%
   at `-R 8` and would need a large sweep to assert anything, which is precisely
   the kind of check `CLAUDE.md` warns costs 10× under the sanitizers for no
   extra coverage. It also pins the denominator directly: `-R 1` must read
   0.0%, and `-R 2` / `-R 4` must read 50.0% / 75.0%, so a divide-by-`K` error
   is invisible in the first and unmistakable across the rest.
6. **Skip rate matches prediction on real kicks.** ~17% at `-R 100`, ~44% at
   `-R 1000`, from `eval/results-experiment-f.txt` §7. Against the exact-set arm
   of check 3 the Bloom count must exceed the exact one by the expected
   false-positive share and no more. This one is a bench-scale measurement, not
   a suite check.
7. **TSan and valgrind cases, both new.** The suite's TSan job is a handful of
   hand-picked invocations, and a real race in the `--ring-stride` refinement
   once sat there unreported because none of them passed `--ring-stride`; a
   path that spawns threads and touches shared state needs its own case. The
   **valgrind** job is the more important of the two here, because it is the
   only gate for *uninitialised* reads and this adds a multi-gigabyte heap
   array whose zeroing is load-bearing — a filter read before it is zeroed
   would skip climbs at random and be invisible in every other check. Use
   `--error-exitcode=66`, since the program's own fatal exit is 1.
8. **Bench** with the flag off, to confirm the added branch costs nothing on
   the default path.

### Build order

The filter is the small half. Turning one `run_parallel` over `keys × restarts`
into one per pass touches the chunking, `g_sweep_total` and the progress line's
pass field, and must keep the **global** flat work index intact so
`better_cand`'s lowest-index tie-break is unchanged.

**Land that restructuring first, on its own, with the output byte-identical**
(check 1 against the current binary, plus check 4's `-T` matrix). Otherwise a
determinism regression from the barrier arrives in the same commit as the
filter and gets attributed to it — and of the two, the barrier is where such a
bug is more likely and harder to see.

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

## 10. What implementation changed

The design above is as written before the code, with the corrections marked in
place. This section is what the build itself taught.

**Verification, as run.** 1 and 2 pass on a matrix of invocations (the barrier
that precedes the filter is separately byte-identical over 14 invocations at
`-T` 1/2/4). 3 passes as re-specified. 4 passes: the skip count is identical at
`-T` 1/2/4/8, and so is the result. 5 is the `--random 0` fixture and is exact
— 0.0 / 50.0 / 75.0% at `-R` 1/2/4 on 26 keys, with the denominator asserted at
936 under `--ring-stride` so the refinement's keys are provably in neither
counter. 6 is measured: 5.8% duplicates at `-R 64` on real kicks. 7 is two new
CI cases, both injection-verified. 8 is flat: `make bench LONG=1` against the
pre-change base reads +0.6 / −1.2 / +0.4 / −1.1% on the four tiers with the
flag off.

**The `--ring-stride` refinement had to be SUSPENDED, not merely left alone.**
§6 says it runs unfiltered; saying so is not enough. It reuses `search_worker`
over its own key space, whose indices start again at 0 and therefore **alias**
the coarse pass's per-key regions — so it both consumed those regions and had
its own climbs skipped against seeds they never produced. Plugboards scored
went 2 275 780 → 2 495 481 once suspended, i.e. the refinement had been losing
about 9% of its work to a filter that knew nothing about it. Caught by the test
asserting the exact skip percentage, which read 75.8% instead of 75.0%.

**The TSan case could not fail as first written, and the reason generalises.**
Injecting the bug — the pass loop replaced by a single `run_parallel` — left it
silent. Two separate corrections were needed:

- **The fixture must be degenerate.** Without the barrier a race needs two
  threads on the *same* key at once, but key `k`'s items sit `R` apart in the
  work index while threads hand out small consecutive chunks. On a 676-key
  sweep the threads are always on different keys, so there is nothing to
  observe. The key count has to be at most the thread count; the shipped case
  uses **one** key.
- **`--random 0` is exactly wrong here**, which is the opposite of what it is
  right for in the functional tests. With every seed identical there is one
  write and `R−1` reads, and read/read is not a race. Real kicks make every
  restart a write.

With `-r AAA -g AAA -c -R 64 --random 10 -T 8` it exits 66 on the broken build
and 0 on the good one.

**A false lead, recorded so it is not re-derived.** The first explanation for
TSan's silence was that the relaxed atomic counters were masking the race —
TSan does not model relaxed ordering precisely, so an atomic RMW before each
filter access could plausibly build a happens-before chain through it. The
counters were rewritten thread-local with a per-pass flush on that basis. It is
**false**: TSan reports the race with the atomics in place. The real cause was a
**stale object file** — the `Makefile` does not track `EXTRA_CXXFLAGS`, so a
sanitizer build over existing `-O2` objects leaves part of the program
uninstrumented. `make clean` first. The thread-local counters were reverted,
since their only justification had evaporated.

**The echoed false-positive rate is at FULL LOAD and is conservative by about
`k+1`.** Measured across `bits_per_item` on 43 264 real seeds at `-R 64`:

| bits/item | echoed FP | skips | excess over true duplicates |
|---:|---:|---:|---:|
| 24 | 0.09% | 5.8% | — (taken as the baseline) |
| 12 | 0.95% | 6.0% | 102 |
| 8 | 3.19% | 6.4% | 303 |
| 6 | 6.60% | 7.3% | 669 |
| 4 | 15.22% | 10.1% | 1 878 |

At 8 bits the full-load figure implies ~1 301 false positives and 303 were
observed — 4.3× fewer, against the 5× that `1/(k+1)` predicts for `k` = 4,
because a `k`-bit test grows as `load^k` and most queries hit a part-filled
filter. Quoting the full-load rate overstates the coverage being given up
rather than understating it, which is the right direction, and the echo now
says "at full load" so the two figures cannot be confused.

**The settings echo cannot report the effective bits per item**, and this was
found before coding rather than after: the effective figure divides by the
realised distinct-seed count, which no run knows in advance. The echo prints
the provisioned figure; the realised rate follows from the final skip line.

**Two things the design did not anticipate needing.** A skipped item must
return before `dump_all` and the doubling report — the first would print a row
scoring `-1e300` and the second would decode a board that was never finished.
And the reporting had to distinguish `--seed-dedup-bits` unset from set, so
that a sub-option given without the flag it configures is fatal rather than
silently inert.
