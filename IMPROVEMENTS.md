# IMPROVEMENTS.md — open work and pitfalls

What is still worth doing, and what to avoid doing again. The evidence behind
every claim here lives in `archived/` — chiefly `archived/PERFORMANCE.md`
(measurements), `archived/CODE_REVIEW.md` (the previous issue list) and
`archived/CRACKQUALITY_TESTS.md` (harness design). Those are history: read them
to check a number, not to find work.

Status in one line: the tool is correct, warning-free under `-std=c++17 -Wall
-Wextra -Wpedantic -Wcast-qual -Wshadow -Wold-style-cast` on g++ and clang,
clean under ASan/UBSan/TSan/valgrind/cppcheck/clang-tidy, multi-threaded, and
supports standard / Norway / M4 machines. Nothing below is a known bug.

Severity: 🔴 critical · 🟠 high · 🟡 medium · 🟢 low.

---

## 1. Where the remaining headroom appears to be

Two measurements shape everything below, and both point the same way: **there is
little apparent room left** — though neither is a proof, and one of them has
already been wrong once.

**Search looks compute-bound.** On the plugboard-recovery tier essentially every
miss is a *search* failure rather than a scoring one, and the exact-recovery
curve is still climbing at `-R 256` (~+15–25pp per 4× R). The truth is a rare
*deep* basin: 64 restarts give ~60 distinct exact boards but only ~15 distinct
correct-plug states, per-restart depth ~0.7/10, with the truth assembled only in
the *union* (~9/10). Every clever lever needs a truth-free way to pick that
union out of the noise, and none is known — per-plug consensus is ~1.1/10. So
raw `-R`, bought with `-T`, remains the dependable lever. That is an absence of
evidence for a shortcut, not evidence that none exists.

**Scoring looks near its ceiling.** The discrimination floor is ~1% at L40–60
and ~0 beyond, and an 8× sweep of the model's order weights moves the
search-failure rate by <1pp. But this conclusion has been drawn before and did
not hold: it was the stated position immediately before `-f` shipped, and `-f`
then gained **+3.0–4.4pp** over `-a`. Treat "no headroom" as a prior, not a
finding.

**The recommended model is `-f`** (fused weighted-all-order + IC), not `-a`. It
beats `-a` by +3.0–4.4pp on english, german *and* wehrmacht — the only scoring
change so far that does not depend on the writing style, which is expected
because IC is language-independent. Note that it is a better *climb* rather than
better discrimination: the gain decomposes into surface reshaping (+3.4pp) with
selection contributing −0.0pp, so it does not move the scoring floor. Recipe:

    -c -S m4f10 -J --polish -f -l <lang> -T <cores> -R <as high as -T affords>

## 2. Open items

Nothing above 🟢. Ordered by expected payoff within each group.

### Search

- 🟢 **Greedy plug-by-plug seed.** Pick the best single plug, fix it, repeat to
  a small budget, then hand over to the swap climb — a better start than the
  identity board. Cheap to try, and the last search idea not yet measured down.
  Temper expectations against the compute-bound picture above.
- 🟢 **ILS with incumbent-walk acceptance.** Nominally open but a long shot:
  converged boards are *scattered* rather than clustered near the truth, and
  clustering is the structure ILS would need to exploit.

### Scoring and new capability

- 🟢 **Crib-driven menu/closure deduction.** Hypothesise a plug, chain-deduce
  the forced plugs through the machine equation, reject contradictions
  (Turing/Welchman menu logic). This pins plugs *with certainty before* the
  climb runs rather than re-ranking after it, which is why it is worth more than
  the crib re-ranker that shipped and was measured down (`--crib-file`, −0.1pp
  at weight 0.5). Needs a new crib-planting harness tier and is invisible to
  `make crackquality` as written.
- 🟢 **The narrow L40 scoring re-opening.** The only observed scoring failures
  sit right at the identifiability floor — 5–10% at L40, robust across two
  seeds, 0 elsewhere. Length-sensitive scoring ideas could only help at L ≲ 40,
  where recovery is already near the information floor, so the payoff is small.
- 🟢 **The wheel-order scoring gate was never run.** The gate that parked
  scoring work tested *start*-discrimination only, with wheels and reflector
  pinned true. A wheel-order scoring failure would need the heavier unfiltered
  full-crack gate, so the "no scoring problem" claim is narrower than it reads.

### Throughput saved but not yet converted

- ✅ **Does `--ring-stride`'s saving convert into recovery? Measured: no, and
  no loss either.** At matched wall time on authentic telegraphic German
  (L=100, 10-pair board hidden, `-f -l wehrmacht`, 40 paired trials/cell) the
  stride plus the extra `-R` its saving buys is indistinguishable from an
  exhaustive ring2 sweep: K=3 `+0.2pp` mean %-correct, K=2 `−4.2pp` with a 95%
  CI of [−13.3, +4.9]. Full writeup:
  `eval/results-ring-stride-vs-restarts.txt`.

  Two things worth carrying forward. **The trade only exists on a
  wildcarded-ring keyspace** — the refinement's fixed `25 × rc[1] × gc[1] × 26`
  keys exceed the coarse saving under the default `-r AA.`, where the stride
  costs *more* than it saves. And **n=40 resolves only ±9pp here**; a ~2pp
  effect would need ~20× the trials at ~75 s per arm per trial, which is not
  worth spending on a flag that is not recommended by default.
- 🟢 **Does the middle-wheel collapse's saving convert?** The same question for
  the §7.12 keyspace reduction (3–5× at short lengths): the compute is saved,
  but whether spending it on `-R` raises recovery is untested.

### Maintainability and packaging

- 🟢 **`-Wconversion` (~52) deliberately deferred.** 43 are `int → unsigned
  char` narrowings in the hottest loops. That many casts clutter the hot path
  for a low-value nit on deliberately C-style code. A future ratchet, not a bug.
- 🟢 **No `install` target**, and the n-gram files are not declared as build/run
  dependencies. Fine for development; add if the tool is packaged.
- 🟢 **Single-file distribution.** Embedding the tables was declined once, but
  the shipped uint8 tables are ~4× smaller than the float tables that analysis
  assumed, so a blob is much cheaper now. Keep `-d` / `$ENIGMA_DATA` as the
  override if this is ever built.
- 🟢 **`Scoring:` line can exceed 79 columns** when the `-d` path is long. Path
  length is unbounded and cannot be shortened without hiding it; every other
  status line is guaranteed to fit.

## 3. How to measure

Getting a trustworthy number here is harder than it looks — most of the traps in
§5 are measurement traps.

- **Normalise on compute, never on `-R`.** For anything whose point is to buy
  more restarts, the honest baseline is *a higher `-R` at equal compute*. `-R`
  never plateaus through 256, so a matched-`-R` comparison flatters any new
  lever. Use total `score_iter` (echoed in the final diagnostic) or wall time.
- **Prefer wall time when work happens outside the score loop.** `score_iter`
  counts only the fused n-gram loop. The gain scan in `--cascade`/`--polish`
  does ≈`n·26·4` uncounted lookups, so `score_iter` can undercount real cost
  several-fold, and the two axes can disagree outright.
- **Judge cracking changes on mean %-correct, not exact-recovery rate.** The
  mean is graded and lower-variance; the exact rate is coarse and noise-
  dominated at short lengths. Use `make crackquality`, with `SPLIT=1` for the
  scoring-versus-search split.
- **When evaluating a *search* change, drop the scoring failures first.** They
  are an information floor no search can cross, so leaving them in adds noise
  that nothing under test can move.
- **Establish the noise floor before believing any A/B.** Run `make bench
  BASE=<the same ref>`; every non-zero number is then pure jitter. The floors
  are per-tier *and* per-compiler — measured `search` ±0.5% but `hillclimb`
  ±4.5% under g++, while clang's own `search` control swung −2.2%…+0.4%.
- **A/B the hot path under both g++ and clang.** They have disagreed by 20
  points on the same change.
- **`-T`-determinism is a contract.** Results must not depend on thread count.
  Keep new randomness on the per-key RNG stream; pin `ENIGMA_SEED=0` for A/Bs.
- **For a claim of *exactness*, test equivalence rather than recovery rate.** If
  a reduction is meant to be lossless, the reduced and full searches must return
  byte-identical output on every input, including those where both fail.

## 4. Measured down — do not re-attempt

Each of these was built or prototyped, measured, and lost. Re-attempt only in a
materially different regime, and read the evidence in `archived/` first.

| idea | verdict |
|---|---|
| Tabu / visited sets | restarts already almost never revisit a basin |
| Genetic algorithms | crossover material exists (~9/10) but is unselectable |
| Cross-restart consensus / plug fixation | beaten by simply raising `-R` |
| `max(greedy, SA)` portfolio | complementary, but the split cancels it |
| Truth-targeted kick, basin-repelling | need the same absent selection signal |
| 3-opt / 3-plug re-pair | cost exceeds gain |
| Don't-look bits | exact only for separable objectives; here a wash |
| Static (fixed) informed move ordering | collapses restart diversity |
| Surrogate-ranked ascent | ~1.5× slower at ~50 chars; only wins at ≥150 |
| Incremental quad delta-scoring | ~2× slower |
| χ² as the scoring or `-F` model | gameable by the plugboard permutation |
| Linear (Jelinek-Mercer) interpolation | loses; log-linear shipped |
| Back-off / graded-floor smoothing | neutral to harmful |
| `-march=native`, SIMD gathers, GPU | the scorer is gather-latency-bound |
| 5-grams, 4-bit scores | too sparse / too coarse |
| Rotor-stepping reuse across starts | measured down |
| Top-M coarse refinement (`--ring-stride`) | wheels already right 96/96 |
| Plugboard→score cache (`--score-tt`) | only ~7–13% cacheable; net loss |

## 5. Pitfalls

Traps that have already cost real time here. The first group produces wrong
*code*, the second wrong *numbers*, the third wrong *conclusions*.

### Correctness traps

- **A shared `if` is a shared enable.** `--polish`'s finisher shared an
  enclosing `if` with the `--ring-stride` refinement because both need the same
  expensive prerequisite, and it never re-checked its own flag — so strided runs
  silently climbed the plugboard *with no `-c` given*, corrupting decrypts and
  invalidating every accuracy measurement of that flag. Where two features share
  a guard for cost reasons, each still needs its own.
- **`wheel_task` holds RAW wheel and reflector numbers.** `init_walzen()`
  translates on the way into a `machine` (Norway adds an offset), so rebuilding
  a `wheel_task` from `m.walzenlage[]` translates twice. This is **invisible in
  standard and M4 mode**, where raw equals translated, so anything touching
  `wheel_task` needs a Norway test or the whole suite passes over a broken
  feature.
- **Reported ring/start may be a class representative.** Wheel 0's ring is
  always reported `A`, and wheel 1's ring/start may be any member of its
  equivalence class. Raw values are therefore *not comparable across runs* —
  compare the offset `(start − ring) mod 26`. Banding raw `ring1` instead of the
  offset lost keys, and looked like a failure of the idea rather than of the
  axis.
- **`search_worker()` leaves ring/start stale on the plain-scan path** — a
  deliberate lazy restore; only the hillclimb path restores per key. Snapshot
  anything a later sub-search will pin *before* the first search runs, and never
  re-read it from the machine in between.
- **Keyspace reductions fail silently.** A wrong equivalence class drops real
  keys with no error anywhere. Derive class membership by simulating the
  stepping, never from a formula: `⌈L/26⌉+1` is wrong for two-notch rotors and
  for double stepping. Cover both cases explicitly in tests.
- **Hot-path layout is load-bearing.** Heap-separate `subst_array`; hoist base
  pointers into `__restrict` locals; hold rotor positions in locals across
  `setup_mapping`; keep `plug_fixed` a plain global. Struct size matters too —
  an `int[26]` in `search_range` cost ~5% on `search`.

### Measurement traps

- **`make bench` times whole invocations**, so anything that changes *startup*
  reads as a hot-path win. Halving n-gram load time showed as `search −3.4%` and
  `hillclimb −10.1%` with no hot-path code touched. The tell was that both
  deltas were the same ~60 ms in absolute terms — the shape of a constant, not
  of a per-iteration gain. The base-vs-base noise control cannot catch this,
  because both sides have identical startup.
- **`awk 'length'` counts bytes unless the locale is UTF-8**, and `LANG` is
  unset in this environment. These docs use `—`, `×`, `≈` and `§` constantly —
  three bytes, one column each — so a byte-based width check over-reports.
- **A regression test that passes against the buggy binary is decoration.**
  Always build the pre-fix binary and confirm the test fails against it. Two
  tests written here passed on both, because the case chosen was too easy to
  discriminate.
- **Cheap proxies drift from the thing they proxy.** `score_iter` for wall time,
  proximity for exact recovery, a 240-sample rate for a bound — each has
  produced a confident wrong answer here.

### Reasoning traps

- **Prefer examples over aggregate rates when a rate is about to drive a
  decision.** Twice in one session a self-consistent aggregate pointed the wrong
  way, and dumping individual failing cases broke it open — once revealing a
  case with every key component correct yet a wrong plaintext, which is
  impossible and was the thread to pull.
- **A ratio is not a cost.** A refinement that "outweighs the coarse pass"
  sounds serious; it was 988 keys against 676, i.e. microseconds. Check the
  absolute number before designing around a proportion.
- **A baseline that skips the buggy path hides the bug *and* doubles as its
  control.** When an option's measured cost is large and suspiciously uniform,
  check whether it switches on anything besides the thing being measured.
- **Match the scoring language to the text.** `-l` is not cosmetic: quadgrams
  are highly language-specific, and `wehrmacht` is a *writing style*
  (telegraphic military German) — right for real traffic, wrong for prose by
  −10.2pp.
- **Keep tests fast.** The sanitizer job runs the whole suite at roughly a 10×
  slowdown. Size each check's keyspace to the property under test, and watch for
  options that disable a reduction — `--true-key` turns off the middle-wheel
  collapse, so a check using it pays full price where its neighbours do not.
