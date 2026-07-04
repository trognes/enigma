# CRACKQUALITY_TESTS.md — Planned crackquality test additions

> **Scope and status.** Three focused, cheap additions to the `make crackquality`
> harness, grounded in **measured** 4-core timings and **verified** `enigma.cc`
> identifiability facts. It inherits PERFORMANCE.md's measurement discipline
> (compute-normalize on `score_iter`, the baseline is a higher `-R` at equal
> compute, `-T` byte-determinism, both compilers for hot-path touches,
> `ENIGMA_SEED=0`).
>
> **Build status: all three are implemented.** §1's harness plumbing (the opt-in
> `WILDCARD`/`FILTER`/`RESTARTS`/`FULLCRACK` knobs, `last_key`/`key_ok`, the `key%`
> column and the generalized SPLIT). §2's `--true-key` binary hook + the
> `FILTERRECALL` recall@N mode and `SCOREITER` column. §3's `--dump-restarts`
> binary hook + the `DIVERSITY` basin-collapse mode. All three binary hooks are
> **off by default** (default paths byte-identical and bench-neutral, verified),
> and `WILDCARD=""` / no-mode runs are byte-identical to the original harness. The
> commands below are runnable now. **Numbers collected:** the §1 scoring-failure
> gate (H0 — fires only at L40, ~7.5%; search-bound at L ≥ 50; see §1) and §2 Test A
> (`-F` recall on the 6-order proxy; see §2). The full 60-order recall, Test B, the
> `FULLCRACK` wheel-order gate, and §3 diversity sweeps are not yet run.
>
> **This supersedes the earlier "full-crack tier" design.** Its cross-key plug
> marginalization goal (PERFORMANCE.md §5.3) is **dropped** — see the note at the
> end — leaving one cheap survivor of that idea (a one-time scoring-failure gate,
> §1) plus two new test workstreams the maintainer asked for: `-F` prefilter
> validation (§2) and restart-diversity diagnostics (§3).

The three, and why each is cheap and worth running:

1. **A one-time scoring-failure gate** — does the *objective* ever misrank once
   the rotor key is unknown? One run, ~15 min. Re-opens §6 only if it fires.
2. **`-F` prefilter validation** — the prefilter's *throughput* is documented;
   its *recall* (does tier-1 ever drop the true key?) is not. Tier-1-only, cheap.
3. **Restart-diversity diagnostics** — measure how often restarts collapse into
   the same local optimum, and which *existing* knobs best avoid it. Fixed-key,
   cheap. (Instrumentation + comparison only — no new search algorithm here.)

---

## 1. A one-time scoring-failure gate (unknown key)

**Why.** On the current fixed-key plugboard tier the `SPLIT=1` oracle finds
**every miss down to L40 is a *search* failure** — the true board out-scores the
reached board. With a single key there is no cross-key ranking to get wrong, so a
genuine **scoring** failure (the objective preferring a *wrong* config) is
structurally unobservable, and all of §6 (scoring) is unfalsifiable. This gate
asks that question exactly once: **with an unknown rotor setting, does any wrong
(key, board) out-score the true one under quad?** If no, §6 stays parked below
§3/§7 and effort returns to search; if yes, §6 re-opens. It is a diagnostic, not
a recurring suite.

**Scope — `START`, unfiltered.** Fix reflector and wheels to the true value, pin
**ring `AAA`**, wildcard only the **start** (`-g ...`) → 26³ = **17,576** keys.
Ring is pinned because the wiring makes it nearly free to do so: turnover is
checked on the *absolute* stepped position with no ring term (`notch[w1][g1]` /
`notch[w2][g2]`, `enigma.cc:668–677`), while the substitution uses the
`(start − ring)` offset (`enigma.cc:680`). So the **left** rotor's ring and start
are mutually unidentifiable (only `g0 − r0` matters — wildcarding both is a pure
×26 double-count), and searching all rings costs ×17,576 for little identifiable
gain. Trials are therefore **generated with true ring `AAA`** so fixed-ring
recovery is never spuriously denied. With ring pinned, the **start is fully
identifiable**, so this scope genuinely tests start-vs-plugboard discrimination.

**The gate must be *unfiltered*.** This is the subtle correctness point. Under
`-F`, tier-1 IC-climbs every key and can drop the true key *before* tier-2 — a
**filter-recall** failure, a third mode distinct from search and scoring. The
SPLIT oracle (`o = oracle_score(true key + true board)` vs `h =` the climb best;
`o > h → search else scoring`) cannot see tier-1 recall, so a filtered run
mislabels dropped-true-key misses as "search" and confounds the gate. Running
**with no `-F`** (every key gets the full climb) makes the split a clean
search-vs-scoring dichotomy. Unfiltered is expensive (every key is a full
`-R`-restart climb, ~20 s/trial at L50 measured, ~25–50 s across L60–120), but the
gate is one-time and the `START` keyspace is small.

**The run (G1 — the gate):**

```
make crackquality WILDCARD=g RESTARTS=8 SPLIT=1 PAIRS=6 LENGTHS='60 90 120' TRIALS=8
```

≈ **~15 min, once.** 6 plugs and L ≥ 60 deliberately: the timing cells showed
0/5 exact recovery at L50/10-plug, so a 10-plug/short gate would measure the split
on a near-all-miss population with no power. An **optional** heavier confirmation
that adds wheel-order discrimination — wildcard wheels too, still unfiltered
(`FULLCRACK=1 FILTER=0`, ~220 s/trial at L90, ~30 min for 8 trials) — is noted for
completeness but **not required**; the `START` gate answers the question.

**Harness plumbing (minimal, opt-in, backward-compatible).** In
`tests/crack_quality.py`:

- New env knobs in the `env()` block (`:90–98`): `WILDCARD` (subset of `"uwrg"`;
  `""` keeps today's fixed-key tier **byte-identical**), `FILTER` (`-F` budget;
  `""`/`"0"` = off), `RESTARTS` (`-R`); plus a `FULLCRACK=1` sugar that sets
  `WILDCARD="wg"` and defaults `FILTER`/`RESTARTS` only when unset.
- `gen_trials()` (`:166–180`): when `WILDCARD` is truthy, force the true ring to
  `"AAA"` (still *draw* it from the RNG for stream stability, then override), so
  fixed-ring recovery is identifiable.
- `climb()` (`:142–148`): substitute wildcard dot-strings for the wildcarded
  dimensions, append `-F`/`-R` when set, and return a **3-tuple**
  `(plaintext, last_score, last_key)`.
- `last_key(stderr)`: sibling of `last_score()`, returns `(W, R, G)` from the last
  progress line (the columns are always present, even with an empty board);
  `None` on the old `"W: B241"` format.
- `key_ok(recovered, true)`: compares only the **identifiable** columns — with
  ring pinned `AAA`, that is the **W *and* G** columns (reflector+wheels *and* the
  three start letters). Ring is not compared.
- `oracle_score()` (`:151–156`): **unchanged** — it already scores the
  fully-specified true key + true board with no `-c`, which *is* the generalized
  SPLIT oracle.
- `main()`: unpack the 3-tuple, accumulate `key_ok`, print `key%` and, under
  `SPLIT=1`, the `search-fail% / scoring-fail%` columns.

**Metric:** plaintext **mean %-correct** stays primary; `exact%` and `key%`
secondary. Recovery is measured on the plaintext, never by raw ring/start
equality. **Expect lower absolute recovery than the fixed-key tier** — an unknown
key is strictly harder than the ~47-bit plugboard-only problem, and per
identifiability a plug's evidence lives only where its two letters appear, so
rare-letter plugs in 50 chars are near-unidentifiable regardless of effort
(PERFORMANCE.md §2). Report graded mean %-correct; do not gate on exact recovery.

**Determinism:** the acceptance check is **stdout** (recovered plaintext) and the
parsed `last_score`/`last_key` *values* byte-identical across `T=1/3/8` — **not**
full-stderr, since which progress *lines appear* is thread-timing dependent by
design (`best_result.shown`).

### Measured — the gate (H0), first run

Config: `START` scope unfiltered (`WILDCARD=g`, reflector+wheels fixed true, ring
`AAA`, start wildcarded ⇒ 17,576 keys), English quad, **6 plugs**, `-R 8`,
`SEED=1`, 12 trials/length (L40 extended to **80** — two seeds × 40 — to test the
short-end signal). `search-fail%` / `scoring-fail%` are over *all* trials (exact
recoveries fill the remainder).

| L | mean% | exact% | search-fail% | **scoring-fail%** | trials |
|--:|--:|--:|--:|--:|--:|
| 40 | ~16 | ~9 | ~84 | **7.5** (6/80) | 80 |
| 50 | 20.2 | 8.3 | 91.7 | **0.0** | 12 |
| 60 | 53.6 | 50.0 | 50.0 | **0.0** | 12 |
| 90 | 45.4 | 41.7 | 58.3 | **0.0** | 12 |
| 120 | 100 | 100 | 0.0 | **0.0** | 12 |

**H0 fires, but narrowly.** From **L50 up the tier is cleanly search-bound** —
`scoring-fail% = 0`, every miss is the plugboard climb stuck in a local optimum,
exactly as on the fixed-key tier. **At L40 only**, ~7.5% of messages have a wrong
(key, board) out-score the truth under quad — **robust across two independent
seeds** (5% and 10%, so not the 1/12 trial-noise the first 12-trial run
suggested). These are the **first scoring failures this project has observed**:
right at the identifiability floor, a noisy quad score over ~37 quadgrams
occasionally mis-ranks a decoy start with an overfit plugboard above the truth.

**What it means.** §6 (scoring) **stays parked for the regime that matters** —
there is no scoring problem at L ≥ 50. The narrow L40 re-opening is a concrete but
small target for the two length-sensitive scoring ideas (§6.1 trigram-at-short-end,
§6.2 back-off smoothing): they could only help at L ≲ 40, where recovery is already
near the information floor (`exact% ≈ 9`), so the practical payoff is limited.

**Caveats.** `START` scope tests **start-discrimination only** (wheels/reflector
fixed true); a wheel-order scoring failure would need the heavier `FULLCRACK`
unfiltered gate, not run. One machine; L50–120 are 12 trials each (percentages
coarse — e.g. L60 > L90 mean is trial noise, not a real dip). The load-bearing
result is the `scoring-fail%` column, and it is 0 everywhere except the L40 floor.

---

## 2. `-F` prefilter validation

**Why.** The prefilter's *throughput* win (~8–20×) and its tier-1 design (a capped
IC *climb*, not a plugboard-free scan; `cap≈5`) are documented. What is **not**
measured is **recall**: how often does the cheap tier-1 IC climb rank the *true*
key outside the top-`N` and silently discard it? A dropped true key is an
unrecoverable miss no amount of tier-2 `-R` can fix, and it is invisible in normal
output. These two tests close that gap; both are cheap (tier-1 dominates and is
~length-independent, ~10k keys/s at L50 measured).

**Required instrumentation (a small, off-by-default binary hook).** A
`--true-key <U><W><R><G>` flag that, during the `-F` tier-1 ranking, tracks that
key's position and prints one line to stderr: `true-key tier1 rank R of N`. Gated
behind the flag so normal output stays byte-identical; the harness passes the
known true key and parses the rank.

**Test A — recall@N (tier-1 only, cheap).** For a wildcarded keyspace (`START`
17.6k and `FULLCRACK` 105k), report the fraction of trials whose true-key tier-1
rank ≤ N, for **N ∈ {50, 100, 200, 500}**, across **L ∈ {50, 70, 100, 140}** and
**PAIRS ∈ {6, 10}**. Run at `-R 0` (no tier-2 needed to read the rank), so cost is
essentially the tier-1 sweep (~2 s/trial `START`, ~10 s `FULLCRACK`). New harness
mode `FILTERRECALL=1` that emits a `recall@N` table instead of recovery columns.
**The question:** at the shortlist sizes the tier actually uses, does the true key
survive — and how does recall fall with shorter messages and more plugs (where the
IC signal is weakest)?

**Test B — matched-compute recovery A/B.** The honest end-to-end test: does the
throughput win *net* more recoveries, or does recall loss cancel it? Compare
`-F N -R big` against **no-`F` `-R small`** at **equal total `score_iter`** (parse
the "scored M plugboards" diagnostic; scale `-R` so both match), on identical
SEED-matched trials. Metric: mean %-correct and `exact%` vs length. New harness
support: a `SCOREITER=1` mode that reads and reports the per-run `score_iter` so
the two arms can be balanced.

```
# A: recall curve, FULLCRACK keyspace, tier-1 only
make crackquality FULLCRACK=1 FILTERRECALL=1 RESTARTS=0 LENGTHS='50 70 100 140' PAIRS=10 TRIALS=20
make crackquality FULLCRACK=1 FILTERRECALL=1 RESTARTS=0 LENGTHS='50 70 100 140' PAIRS=6  TRIALS=20
# B: matched-score_iter recovery, filtered vs unfiltered (balance -R via SCOREITER)
make crackquality WILDCARD=g FILTER=100 RESTARTS=64 SCOREITER=1 LENGTHS='60 90 120' TRIALS=20
make crackquality WILDCARD=g               RESTARTS=<matched> SCOREITER=1 LENGTHS='60 90 120' TRIALS=20
```

**Expected read:** if recall@{100,200} stays high across L and PAIRS, the
prefilter is vindicated; if it sags at the short/high-plug corners, that
quantifies exactly where `-F` costs recoveries — and Test B says whether the
throughput still wins on net. Either way it is the thorough measurement `-F` has
lacked.

### Measured — Test A, first proxy run

Config: 6-order **proxy** keyspace (`WILDCARD=wg XMAX=3` — reflector fixed to the
true value, wheels over I–III = 6 orders, ring `AAA`, start wildcarded ⇒ **105,456
keys**), English quad, **10 plugs**, `SEED=1`, **16 trials/length**. Command:

```
WILDCARD=wg XMAX=3 FILTERRECALL=1 CLANG=english PAIRS=10 LENGTHS='120 200 300' TRIALS=16 CRACKOPTS='-T 4'
```

| L | rec@50 | rec@100 | rec@200 | rec@500 | median rank |
|--:|--:|--:|--:|--:|--:|
| 120 | 6.2% | 6.2% | 6.2% | 6.2% | **12416** |
| 200 | 56.2% | 56.2% | 62.5% | 75.0% | **27** |
| 300 | 93.8% | 93.8% | 93.8% | 93.8% | **1** |

**The prefilter has a sharp length threshold, right where the literature puts it.**

- **L120 (short/hard) — `-F` is effectively broken.** Median rank ~12,400 of 105k:
  the true key sits mid-pack, so any practical shortlist drops it. On short 10-plug
  traffic the capped-5 IC climb cannot discriminate the true rotor key.
- **L200 (the knee) — usable but lossy, and *bimodal*.** Median rank 27, yet
  recall@50 is only 56%: outcomes are either "rank ≲ 50" or "rank in the
  thousands," almost nothing between (note recall@50 = recall@100). So a bigger
  shortlist barely helps at the knee — message length is the only real lever.
- **L300 (realistic operational length) — `-F` works cleanly.** Median rank 1, 94%
  recall at any shortlist ≥ 50.

**Caveats (do not over-read):**

- **This is the *optimistic* proxy** — 6 wheel orders, not the real 60. ~10× fewer
  decoys, so the full Wehrmacht keyspace would rank the true key **worse** (most
  where the IC signal is weak, i.e. the L200 knee); the real "works reliably" point
  is likely **L250–300+**, not L200. The full 60-order confirm was not run.
- One realization (`SEED=1`, 16 trials); the buckets top out at N=500, which for a
  105k (let alone 1.05M) keyspace is a tight shortlist — the **median-rank** column,
  not the recall buckets, is the load-bearing number.
- **Verdict:** the prefilter is sound for realistic-length traffic (~L300) and
  genuinely unreliable on the short/hard messages the tool is otherwise aimed at.
  A concrete, honest map of where `-F` earns its ~8–20× and where it silently costs
  the crack — directly bearing out the standing `-F` skepticism.

Test B (matched-compute recovery) was not run.

---

## 3. Restart-diversity diagnostics (fixed-key; no new algorithm)

**Why.** The repo has already learned — twice — that **diversity across restarts
is load-bearing**: *static* (fixed-across-restarts) informed move-ordering was
**rejected** for collapsing it, while shipped `-J` wins precisely because its
ordering is **rebuilt per restart**. Yet the phenomenon — how often independent
restarts converge into the *same* local (non-global) optimum — has never been
measured directly. These diagnostics make it observable and rank the *existing*
knobs by how well they avoid the collapse. **No new search algorithm is proposed
here** (tabu / repulsion / guided-kick are deferred); this is instrumentation plus
a comparison of shipped options, on the cheap fixed-key tier.

**Diagnostic instrumentation (a small, off-by-default binary hook).** A
`--dump-restarts` flag that, for the fixed key, emits each of the `-R` restarts'
**converged `(score, board)`** to stderr (gated off by default). The harness
aggregates per key.

**Diagnostic — basin collapse.** Per key, report:
- **distinct converged optima** (unique boards) among the `-R` restarts;
- whether the **global-best** board (best over all restarts) was reached, and by
  **how many** restarts (a low count = a narrow basin the search rarely finds; a
  high count with few *distinct* optima = restarts collapsing together);
- these vs **`-R` ∈ {8, 32, 128}** and kick **`--random K` ∈ {4, 8, 13}**.

**Comparison — which *shipped* knob buys the most distinct-basin coverage per unit
compute.** At **matched `score_iter`**, contrast the already-shipped levers — kick
size `K`, and steepest vs `-I` vs `-J` — on **both** axes: distinct-optima
coverage **and** recovery (mean %-correct). Hypothesis to test, not assume: a
larger kick and per-restart `-J` ordering increase basin coverage, and coverage
tracks recovery. New harness mode `DIVERSITY=1` printing, per length, mean
distinct-optima, global-best-hit rate, and mean %-correct.

```
make crackquality DIVERSITY=1 CRACKOPTS='--dump-restarts' LENGTHS='50 70 100' PAIRS=10 TRIALS=20 RESTARTS=32
# sweep the shipped knobs at matched score_iter (balance -R via SCOREITER):
#   --random {4,8,13}; then {steepest, -I, -J}
```

Fixed-key tier → cheap (~0.5 s/key-climb-set at L50 with modest `-R`).
Deterministic on the per-key splitmix stream (`ENIGMA_SEED=0`, `-e 0`). This
section is **measurement only** — it tells us whether the shipped knobs already
avoid restart collapse, and quantifies the gap a future anti-convergence algorithm
would need to close, without committing to one now.

---

## Shared measurement discipline

- **Compute-normalize on total `score_iter`** (the per-machine counter, echoed in
  the final diagnostic), or matched wall-clock — never matched `-R` — for anything
  whose point is to buy more restarts; the baseline is a **higher `-R` at equal
  compute** (`-R` never plateaus through 256).
- **`-T` byte-determinism on *stdout* + parsed values** (not full stderr), at
  `T=1/3/8`; keep all randomness on the per-key splitmix64 stream; pin
  `ENIGMA_SEED=0` / `-e 0` for A/B.
- **`make bench` under both g++ and clang** for any hot-path touch — but note the
  three binary hooks here (`--true-key`, `--dump-restarts`, and the
  `FILTERRECALL`/`SCOREITER`/`DIVERSITY` harness modes) are all **off-by-default
  diagnostics**, so the default paths must stay byte-identical and bench-neutral.
- New harness env knobs must be **opt-in** and leave `WILDCARD=""` /
  no-diagnostic-flag runs identical to today.

---

## Note — §5.3 cross-key plug marginalization: dropped

The earlier plan's headline goal was PERFORMANCE.md §5.3 — pool plug evidence
across many candidate rotor keys, on the premise that "a plug that helps many keys
is likely true." It is **dropped** (maintainer decision, and well-founded): under
a *wrong* rotor key the decrypt is garbage, so its best-fitting plugboard is
**noise**, uncorrelated with the truth. Marginalizing over the top-N (mostly
wrong) keys therefore aggregates mostly noise — the same **correlated-wrong-basin**
failure that already sank **§3.1 cross-restart consensus** (built, measured,
rejected). §5.3 is the cross-*key* twin of that cross-*restart* idea and inherits
the result, so the expensive cross-key experiments are not worth running. The one
cheap survivor — checking whether the *objective itself* ever misranks under an
unknown key — is retained as the §1 gate.

---

## References

Repo-internal: PERFORMANCE.md §2 (identifiability floor), §3.1 (rejected
cross-restart consensus — the precedent for dropping §5.3), §5.3 (now
de-prioritized), §6 (scoring, gated by §1), §7.2 (`-I`/`-J`, static-ordering
rejection — the restart-diversity precedent), §7.3 / archived §9 (`-F` design);
CLAUDE.md (`-F` two-tier, progress-line display semantics, `-T` determinism).
External framing: Gillogly (Cryptologia 19(4), 1995) and Williams (24(1), 2000)
used 500–650 letters; Ostwald & Weierud (41(5), 2017) targeted 250 and call short
messages "rarely successful" — the expectation floor for §1. Code anchors verified
against the current source: stepping/turnover `enigma.cc:668–680`, `-F` activation
`enigma.cc:2738`, `filter_climb_cap` `enigma.cc:201`; `crack_quality.py`
`gen_trials` :166, `climb` :142, `oracle_score` :151, `last_score` :119.
