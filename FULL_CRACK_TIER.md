# FULL_CRACK_TIER.md — Design: the full-crack `crackquality` tier

> **Scope and status.** This is the detailed design for the item PERFORMANCE.md
> §9 calls *"the one infrastructure item that gates everything: build the
> full-crack `crackquality` tier."* It is a **plan, not shipped code** — like
> PERFORMANCE.md itself, no code changes accompany it. It extends, and inherits
> the measurement discipline of, PERFORMANCE.md (compute-normalize on
> `score_iter`, the baseline is a higher `-R` at equal compute, `-T`
> byte-determinism, both compilers for hot-path touches). Every scope, budget,
> and threshold below is grounded in **measured** timings on a 4-core reference
> box (§3) or in **verified** identifiability facts from `enigma.cc` (§2); where
> a number is a single-shot measurement or box-specific it says so.

---

## 1. Motivation — what it gates

The existing `crack_quality.py` measures the **fixed-key plugboard-recovery
tier**: each ciphertext is handed back with the *true* rotor key
(`-u/-w/-r/-g`) pinned and only the plugboard hill-climbed. On that tier the
`SPLIT=1` oracle finds that **every miss down to L40 is a search failure** — the
true board out-scores the reached board, so the climb is simply stuck in a local
optimum. Two consequences follow, and they are the whole reason this tier is the
top infrastructure item:

- **All of §6 (scoring) is unfalsifiable there.** A purely *monotone* rescale
  cannot reorder one key's boards, and even surface-reshaping changes only help
  by letting the *same* climb reach the basin. With a single key there is no
  cross-key ranking to get wrong, so a genuine *scoring* failure cannot be
  observed.
- **§5.3 (cross-key plug marginalization) is literally inapplicable** — it
  pools plug evidence *across candidate rotor keys*, and there is only one.

The full-crack tier removes the fixed-key assumption: the harness brute-forces
the rotor key **and** climbs the plugboard — as a real attack does, *minus the
ring search, which we sidestep by construction* (§2). Its single job is to
become a **trustworthy arbiter** for three levers that are unmeasurable today:

- **§5.3 cross-key plug marginalization** — the highest-leverage non-crib lever,
  because the plugboard is identical for every rotor key, so pooling weak plug
  evidence over many candidate keys is the sample-size multiplier a single-key
  climb structurally lacks.
- **§6.3 MDL plug-count prior** and **§6.7 Bayesian LLR / score-margin
  calibration** — both are ranking-inert on the fixed-key tier (a per-length
  constant cannot reorder one key's boards); they only bite *across* keys.

**The gate question the tier must answer first, before crediting any of those:
is the scoring-failure share non-zero once the key is unknown?** If yes, §6
re-opens. If no, §6 stays parked below §3/§7 and effort returns to search. As
§5 explains, answering this cleanly requires care — a naive `-F` run confounds
the answer with a *third* failure mode.

---

## 2. What "full crack" means here + identifiability & scope

"Full crack" here means **unknown wheel order and start position, plugboard
climbed** — not a literal wildcard of all five dimensions, which is either
unaffordable or unidentifiable. The scope follows directly from how the machine
is wired (verified against `enigma.cc`):

- **Stepping (turnover) is checked on the *absolute* stepped position with no
  ring term:** `notch[w1][g1]` (middle, including the double-step) and
  `notch[w2][g2]` (right), `setup_mapping()` (`enigma.cc:668–677`).
- **The substitution row depends only on the `(start − ring)` offset per rotor:**
  `sa[mod26(g0−r0)][mod26(g1−r1)][mod26(g2−r2)]` (`enigma.cc:680`), tables
  precomputed at ring 0.

From those two facts:

- **Wildcard start (`-g ...`), always.** Turnover depends on the absolute start,
  so sweeping start exercises real rotor discrimination and start is genuinely
  identifiable.
- **Wildcard wheel order (`-w ...`), capped.** Ordered arrangements of distinct
  wheels (`enigma.cc:2619–2623`) — the discrimination that matters most, since
  each order is a physically different rotor stack. Capped for tractability (§3).
- **Fix ring at `AAA`, and *generate trials with true ring `AAA`*.** This is the
  subtle one. Ring is *not* globally redundant: because `g1`/`g2` enter both the
  offset *and* the absolute-position turnover, the **middle and right** ring and
  start are each individually identifiable (via their effect on the stepping
  schedule) whenever those rotors reach a turnover in the window. The genuine
  degeneracy is narrower: the **left** rotor never appears in a notch check, so
  its position enters *only* through the offset `g0 − r0` — left ring and left
  start are mutually unidentifiable, and wildcarding **both** left `-r` and left
  `-g` is a pure ×26 double-count. (The middle/right rings also collapse to the
  offset degeneracy on messages too short for those rotors to step.) Rather than
  pay ×17,576 to search rings for a mostly-unidentifiable gain, the tier **pins
  ring `AAA` and constructs trials with true ring `AAA`**, so fixed-ring recovery
  is never spuriously denied. Honest floor, documented, not hidden: a message
  whose true middle/right ring is non-A is *outside* this tier's search and would
  not be recovered — a real full crack must also search (or accept a degenerate
  offset for) the ring.
- **Reflector: fixed to the true value** in the canonical tier; wildcarded
  (`-u .`, ×3) only in the heavier documented variant.
- **M4 and the full 60-order wheel wildcard: out of the automated tier.** A full
  M4 wildcard is ~15 GiB of rotor tables (guarded at 16 GiB); a 60-order wheel
  wildcard is ~99 s/trial (§3). Manual showcase only.

**Recovery is measured as plaintext %-correct, never raw key equality** — some
settings are structurally unidentifiable (the left ring, the M4 Greek offset),
and a correct crack can legitimately report a degenerate ring. A separate
`key%` metric (§5) compares only the **identifiable** columns.

---

## 3. The per-trial invocation & why it is tractable

The novel cost versus the fixed-key tier is that **`-F` tier-1 IC-climbs *every*
key in the resolved keyspace** — a cost neither `-F` nor `-R` can buy down (they
bound only the handful of tier-2 survivors). So per-trial wall time is set almost
entirely by keyspace size. Measured tier-1 throughput on the 4-core reference box
is a stable **~10,000 keys/s at L50** and **~7,900 keys/s at L100**, corroborated
across three keyspace sizes (the capped-5 IC climb makes length nearly
irrelevant):

| keyspace | keys | L50 wall | keys/s |
|---|---|--:|--:|
| 1 order, start only | 17,576 | 1.75 s | 10,043 |
| 6 orders, start | 105,456 | 10.07 s | 10,472 |
| 60 orders, start | 1,054,560 | 98.87 s | 10,666 |

**Budget rule: wall ≈ keys / 10,000 at L50** (÷7,900 at L100). These are
single-shot, 4-core numbers (±10–20% shared-box noise); the keys/s figure is the
reliable planning constant — **re-measure and rescale on the target box** (tier-1
scales ~linearly with threads).

Two named scopes result:

**`FULLCRACK` (canonical):** reflector fixed to true, wheels wildcarded **capped
to 6 orders** (`-w ... -x 3`, trials sampling wheels 1–3), ring `AAA`, start
wildcarded, filter + restarts on:

```
./enigma -q -l english -u <trueU> -w ... -x 3 -r AAA -g ... -c -F 200 -R 8 -T <nproc> -e 0
```

**105,456 keys → ~10 s (L50) / ~13 s (L100)** measured. The smallest scope that
actually cracks the wheel *order*; `-F 200` bounds tier-2 to ~200 survivors and
`-R 8` adds < 0.3 s (restarts only multiply the few survivors).

**`START` (17,576 keys — wheels *and* reflector fixed to true, ring `AAA`, start
wildcarded).** Used two ways, and the difference is load-bearing:

```
# filtered — fast dev/smoke (~1.8 s L50 / ~2.2 s L100, measured):
./enigma -q -l english -u <trueU> -w <trueW> -r AAA -g ... -c -F 100 -R 8 -T <nproc> -e 0
# unfiltered — the clean gate (§5); every key gets a full climb, ~25–50 s/trial:
./enigma -q -l english -u <trueU> -w <trueW> -r AAA -g ... -c       -R 8 -T <nproc> -e 0
```

Filtered START gives the best statistics for a dev sweep; **unfiltered START is
the primary gate** (§5), because a filter can silently drop the true key. Both
measure start + plugboard recovery, **not** wheel-order recovery — stated plainly
wherever used. The cost gap is the whole story of §3: filtered tier-1 IC-climbs
every key at ~100 µs/key, while an unfiltered full quad climb costs ~146 µs ×
`(L/50)` **per restart per key** — so `-R 8` over 17,576 keys is ~20 s at L50,
not ~2 s.

`-F` must be **absolute** (`-F 200`), never `-F N%`: tier-1 time is identical
either way, but `-F 5%` of a 1.05M keyspace floods tier-2 with ~52k survivors.
**Do not default to `-x 5`** — that is P(5,3)=**60 orders**, 1.05M keys,
~99 s/trial; showcase only (this is the miscalculation the design review caught:
`-x 5` is 60 orders, not 6).

---

## 4. Harness changes to `crack_quality.py` (concrete)

**New env knobs** (added to the `env()` block, `crack_quality.py:90–98`,
style-matched to `MODEL`/`PAIRS`/`LENGTHS`/`SPLIT`):

```python
WILDCARD  = env("WILDCARD", "")      # subset of "uwrg"; "" keeps today's fixed-key tier
XMAX      = env("XMAX", "3")         # -x value when wheels wildcarded; also caps trial wheels
FILTER    = env("FILTER", "")        # -F budget (absolute); "" or "0" => no filter (full climb every key)
RESTARTS  = env("RESTARTS", "")      # -R budget; "" => tool default (-R 0)
FULLCRACK = env("FULLCRACK", "0")    # "1" sugar (each applied only when the field is unset/""):
                                     #   WILDCARD->"wg", FILTER->"200", RESTARTS->"8"
```

The sugar defaults each field **only when it is empty**, so an *explicit*
`FILTER=0` survives it — that is how the gate turns the filter off under
`FULLCRACK=1` (`env("FILTER","")` maps `""`→`""`, and `climb()`'s guard is
`if FILTER and FILTER != "0"`, so both `""` and `"0"` mean no `-F`):

`WILDCARD=""` preserves the current fixed-key tier **byte-for-byte** — the whole
change is opt-in and backward-compatible. `XMAX` is **load-bearing**:
`gen_trials()` must sample the *true* wheels from `range(1, int(XMAX)+1)` when
wheels are wildcarded, or the true order lies outside the searched space and
recovery is structurally impossible. Default `XMAX=3` matches the 6-order
canonical scope.

**`gen_trials()` (`crack_quality.py:166–180`)** — two guarded changes, only when
`WILDCARD` is truthy, both preserving the deterministic RNG stream *within* the
tier (draw every field, then override, so the stream does not shift):

- sample wheels from `range(1, int(XMAX)+1)` when `"w" in WILDCARD` (was
  `range(1,9)`);
- force the true ring to `"AAA"` when `"r" not in WILDCARD` (still *draw* `r`
  from the RNG for stream stability, then discard it), so fixed-ring recovery is
  identifiable (§2).

**`climb()` (`crack_quality.py:142–148`)** — the core edit. Note the fix the
design review flagged: `-F`/`-R` are wired **here**, not left to `CRACKOPTS`
(the canonical `FULLCRACK` runs must actually apply the filter and restarts):

```python
def climb(binary, key, ct):
    u, w, r, g, _ = key
    if "u" in WILDCARD: u = "."
    if "w" in WILDCARD: w = "..."
    if "r" in WILDCARD: r = "..."
    if "g" in WILDCARD: g = "..."
    args = ["-" + MODEL, "-l", CLANG, "-u", u, "-w", w, "-r", r, "-g", g, "-c"]
    if "w" in WILDCARD:              args += ["-x", XMAX]
    if FILTER and FILTER != "0":     args += ["-F", FILTER]
    if RESTARTS:                     args += ["-R", RESTARTS]
    args += CRACKOPTS
    out, err = run(binary, args, ct)
    return out.strip(), last_score(err), last_key(err)   # now a 3-tuple
```

**`last_key(stderr)`** — new sibling of `last_score()`. It reuses the exact last
progress line `last_score` already identifies (the last stderr line whose
`split()[0]` contains `"."`) and returns `(W, R, G) = (fields[1], fields[2],
fields[3])`, or `None`. Safe with an empty plugboard (the W/R/G columns are
always present). Returns `None` on the old `"W: B241 R: ..."` format, so `key%`
is **head-only** in a `BASE` A/B against a pre-progress-format ref.

> **Caveat (documented, not fixed in code):** the progress line is emitted by the
> *display* path (`best_result.shown`), while the returned plaintext (stdout)
> comes from the deterministic *merge* winner (lowest-work-index tie-break in
> `better_cand`). Under an exact score *tie across two keys* the last-printed
> line's key can differ from the merge winner and is thread-timing dependent. So
> `key%` (parsed from stderr) can, in principle, disagree with `pct_correct`
> (from stdout) on a measure-zero set of float ties. This is accepted as
> negligible; if it ever matters, emit a deterministic `winner:` line from the
> merge and parse *that*.

**`key_ok(recovered, true_key)`** — new; compares only **identifiable** columns.
Because ring is pinned `AAA` (so start is fully identifiable, §2), this is the
**W column *and* the G column**: reflector letter + 3 wheel digits (e.g.
`"B"+"241"`) *and* the 3 start letters must match. Ring is not compared (pinned,
and the left ring is unidentifiable). (The design review's fix: an earlier draft
excluded start and would have counted "right wheels, wrong start" as a recovery.)
In the `START` scope W is fixed-true, so `key_ok` reduces to start recovery.

**`oracle_score()` (`crack_quality.py:151–156`) — unchanged.** It already scores
the fully-specified true key + true plugboard with no `-c`; that *is* the
generalized SPLIT oracle (§5).

**`main()`** — unpack the 3-tuple (`rec, hscore, hkey = climb(...)`), accumulate
`key_ok(hkey, key)` into a per-length list, and print the new columns (§5). The
header comment (`crack_quality.py:18–60`) and the Makefile recipe gain the new
knobs, noting `WILDCARD=""` keeps the cheap fixed-key tier.

---

## 5. Metrics & the generalized SPLIT oracle

**Primary metric: plaintext mean %-correct** (`pct_correct` — graded,
low-variance, moves smoothly near the difficulty cliff), per CLAUDE.md's
tuning-signal guidance. **Secondary:** `exact%`, `key%` (via `key_ok`), and the
`L50`/`L90` headlines.

**Column set** (emitted only when `WILDCARD` is truthy):

| mode | columns |
|---|---|
| plain full-crack | `len  mean%  exact%  key%` |
| `SPLIT=1` | `len  mean%  exact%  key%  search-fail%  scoring-fail%  filter-fail%` |
| `BASE=<ref>` A/B | `len  head mean% exact% key%   base mean% exact%` (base `key%` omitted) |

**The generalized SPLIT oracle — and the third failure mode.** With
`o = oracle_score(true key + true board, no -c)` and `h =` the climb's best
score, the fixed-key test is `o > h + 0.01 → search else scoring`. Generalizing
to an unknown key *looks* like a no-op, but there is a trap the design review
caught and it changes how the **gate** must be run:

- **search failure** — the true full config out-scores the tool's best; the
  search did not *reach* the truth (now including "did not find the true key").
- **scoring failure** — a wrong key/board out-scored the truth; the objective is
  misranking. **This is the phenomenon §9 wants surfaced**, structurally
  impossible to observe on the fixed-key tier.
- **filter-recall failure — a *third* mode that only exists under `-F`.** The
  tier-1 IC climb ranks *every* key and can drop the true key before tier-2 ever
  climbs it. Then the tool never scored the true key under quad at all, `h` comes
  from some wrong key, and `o > h` gets mislabeled **search** — when the real fix
  is a better *filter*, not a better search or objective. The harness cannot see
  tier-1 recall from normal output, so under `-F` the search/scoring split is
  **confounded**.

**Consequence for the gate (H0).** The one question the tier exists to answer —
*is scoring-fail% > 0 under an unknown key?* — **must be run with the filter
off** (`FILTER=0`), so every key gets the full quad climb and the split is a
clean search-vs-scoring dichotomy with no recall contamination. Unfiltered runs
are expensive (every key is a full `-R`-restart climb), so the **primary gate
runs at the `START` scope** (17,576 start-keys, wheels fixed, no filter, `-R 8` ≈
~20 s/trial at L50, ~25–50 s across L60–120): there is no wheel-order recall to
lose, and the true start must still out-rank 17,575 wrong starts under quad — a
genuine cross-key scoring test. A **secondary, nightly gate** runs `FULLCRACK`
with the filter off (~120 s/trial at L50, ~220 s at L90) to add wheel-order
discrimination at fewer trials. The routine (filtered) tiers still print
`filter-fail%` as a coarse `100 − key%`-among-misses proxy, but the
**authoritative gate is the unfiltered run**.

`key%` is **not** true-key tier-1 rank / `-F` recall. Producing that needs
minimal, **optional** binary instrumentation — a flag-gated, `--true-key`-fed
`true-key tier1 rank R of N` stderr line, off by default so normal output stays
byte-identical. It is **not required to ship** the tier, and it is the only clean
way to measure `-F` recall directly (as opposed to inferring it by running the
gate unfiltered). Build it only if §5.3 needs shortlist-recall reporting.

Expect *more* scoring-labelled misses at short lengths than the fixed-key tier —
that is the point, not a bug.

---

## 6. The test matrix (explicit)

All runs: `ENIGMA_SEED=0`, `-e 0`, `SEED=1`, `MODEL=q`, `-T <nproc>`. `TRIALS`
is **always explicit** (it defaults to 40, which would silently multiply the
intended runtime of the small-trial rows). The wall-time column uses the measured
**cost model** (4-core box; rescale by your `keys/s`):

- *filtered* trial ≈ `keys × 100 µs` (tier-1, ~length-independent) `+ F × R × climb(L)` (tier-2, tiny);
- *unfiltered* trial ≈ `keys × R × climb(L)`, with `climb(L) ≈ 146 µs × (L/50)` (a full quad plugboard climb, ~linear in L).

`~wall` below is `Σ_L per-trial(L) × TRIALS`, summed over the row's lengths.

| # | command | scope / keys | LENGTHS | TRIALS | PAIRS | ~wall | purpose |
|---|---|---|---|---:|---:|--:|---|
| **G1** | `make crackquality WILDCARD=g RESTARTS=8 SPLIT=1 PAIRS=6 LENGTHS='60 90 120' TRIALS=8` | start, **no `-F`** / 17.6k | 60–120 | 8 | 6 | ~15 min | **primary gate (H0)**: clean search-vs-scoring at 6 plugs |
| **G2** | `make crackquality FULLCRACK=1 FILTER=0 SPLIT=1 PAIRS=6 LENGTHS='90' TRIALS=8` | `wg` ×6ord, **no `-F`** / 105k | 90 | 8 | 6 | ~30 min | **secondary gate**: adds wheel-order discrimination, unfiltered |
| F1 | `make crackquality FULLCRACK=1 LENGTHS='50 100' TRIALS=12` | `wg` ×6ord / 105k, `-F 200` | 50, 100 | 12 | 10 | ~5 min | canonical full-crack baseline curve |
| F3 | `make crackquality WILDCARD=g FILTER=100 RESTARTS=8 LENGTHS='40 50 60 70 90 120 190' TRIALS=40` | start / 17.6k, `-F 100` | 40–190 | 40 | 10 | ~10 min | fast dev/smoke, best statistics |
| F4 | `make crackquality FULLCRACK=1 PAIRS=6 LENGTHS='90 120' TRIALS=12` | `wg` ×6ord / 105k, `-F 200` | 90, 120 | 12 | 6 | ~6 min | known-few-plug regime (where §6.3/§6.7 pay off) |
| F5 | `make crackquality WILDCARD=uwg XMAX=3 FILTER=200 RESTARTS=8 TRIALS=8 LENGTHS='100 140'` | `uwg` 6ord ×3refl / 316k | 100, 140 | 8 | 6 | ~11 min | reflector+wheel+start, heavier nightly |
| F6 | `make crackquality FULLCRACK=1 PAIRS=6 LENGTHS='100' TRIALS=8 CLANG=german` (+ danish, french) | `wg` ×6ord / 105k, `-F 200` | 100 | 8 | 6 | ~2 min ea | cross-language sanity of the split |
| F8 | *(showcase, not CI)* `WILDCARD=uwg XMAX=5 FILTER=200 RESTARTS=40 PAIRS=6 LENGTHS='120' TRIALS=1` | `uwg` 60ord ×3 / 3.16M | 120 | 1 | 6 | ~7 min | "does it ever fully crack an unknown key" demo |

Notes on the corrected budgets: **F5 is `XMAX=3`** (6 wheel orders × 3 reflectors
× 26³ = **316,368 keys** ≈ ~40 s/trial at L100) — the earlier `XMAX=5` label was a
10× miscalculation (`-x 5` is P(5,3)=**60 orders** → 3.16M keys → ~400 s/trial,
which is F8 territory). The **unfiltered gates are the expensive rows** (every key
is a full `-R 8` climb, ~25–50 s/trial for G1, ~220 s for G2), which is why they
carry few trials and short length lists; the filtered rows are cheap because
tier-1 IC-climbs at ~100 µs/key. Plug counts are **10** (default, hard/realistic)
and **6** (known-few, the §6 regime). The gates (G1/G2) deliberately lead with
**6 plugs and L ≥ 90**: the timing cells showed **0/5 exact recovery at
L50/10-plug**, so a 10-plug/short gate would measure the split on a near-all-miss
population with no power. Lengths span the L40–120 barrier plus L190 (F3) for a
feasible positive signal (literature frame: Gillogly/Williams used 500–650
letters, Ostwald–Weierud targeted 250 and call short messages "rarely
successful").

---

## 7. Determinism & A/B

`os.environ.setdefault("ENIGMA_SEED","0")` (`crack_quality.py:75`) plus `-e 0`
pin the per-key splitmix64 restart stream; results are `-T`-independent. **The
acceptance test is stdout byte-identity, not stderr.** CLAUDE.md is explicit that
which progress *lines appear* is thread-timing dependent (`best_result.shown` is
display-only); only which candidate *wins* is `-T`-deterministic. So the guard
is: the **recovered plaintext (stdout)** and the parsed `last_score`/`last_key`
*values* must be identical across `T=1/3/8` — never full-stderr byte-identity,
which would spuriously fail.

Trials are `random.Random(SEED*1000003+length)` — reproducible cross-machine.
`make crackquality FULLCRACK=1 BASE=<ref>` builds the ref in a throwaway git
worktree and solves the identical SEED-matched trials; `key%` is head-only when
the ref predates the progress-line format. Every tier experiment is
**compute-normalized on total `score_iter`** (the per-machine counter, echoed in
the final diagnostic) against a **higher `-R` at equal compute — never matched
`-R`** (the §3.1/§3.2 discipline, verbatim). Any hot-path touch near
`hillclimb`/`score_iter` gets `make bench BASE=<ref>` under **both g++ and
clang** (20–60% clang/ARM layout swings). Exact-speedup ideas must produce
byte-identical `crackquality` output.

---

## 8. Downstream unblocks & the hypotheses tested

- **§5.3 cross-key plug marginalization** (highest leverage): after the `-F`
  tier, marginalize plug appearances over the top-N survivors' cheap-IC boards
  into a key-agnostic prior, seed each finalist's full climb from it, optionally
  EM-iterate. Any cross-key accumulator must be a **deterministic pure function
  of per-key state** (no shared mutable races) to preserve `-T` byte-determinism.
  **H1:** at equal `score_iter`, marginalized seeding beats independent-per-key
  climbing at a higher `-R` (mean %-correct and scoring-fail% on G2/F4 — at
  L ≥ 90, where the split has statistical power; extend downward if a positive
  signal emerges).
- **§6.3 MDL plug-count prior** (`score − λ·plug_count`, IC untouched):
  overfitting manifests *as* a search failure on the fixed-key tier; here it is
  directly judgeable. **H2:** under unknown key with small/unknown true plug
  counts (F4), the prior lifts mean %-correct without raising the search-failure
  share.
- **§6.7 LLR / z-score calibration:** a monotone rescale cannot change per-key
  ranking — its value is a **cross-key-comparable confidence** enabling an `-F`
  **score-margin cutoff** (margin instead of top-N) and a full-crack
  accept/early-stop threshold. **H3:** a calibrated margin holds shortlist recall
  at a smaller mean shortlist than a fixed `-F N`.
- **The gate, H0:** if G1/G2 still show every miss as a search failure, §6 stays
  parked and effort returns to §3/§7. This is the first experiment to run.

---

## 9. Risks & expectation-setting

- **Runtime is set by wildcard scope, not `-F`/`-R`** — tier-1 IC-climbs every
  key. Keep the default at 6 orders; gate wider scopes behind explicit env. Use
  **absolute `-F 200`**, never `-F 5%`, on large scopes.
- **Timings are 4-core-specific and single-shot (±10–20%).** The keys/s figure
  (~10k @ L50, ~7.9k @ L100, corroborated ×3) is the planning constant —
  re-measure and rescale on the target box; tier-1 scales ~linearly with threads.
- **Lower absolute recovery than the fixed-key tier, by construction.** Unknown
  key is strictly harder than the ~47-bit plugboard-only problem, and
  identifiability leads over any bit-budget: a plug's evidence lives only where
  its two letters appear, so rare-letter plugs in 50 chars are near-unidentifiable
  *regardless of search effort* — a hard floor (§2 of PERFORMANCE.md), not a
  defect. Recovery was **0/5 at L50/L100 with 10 plugs** in the timing cells.
  **Report graded mean %-correct and partial recovery at L40–60; do not gate on
  exact recovery.**
- **Never string-compare ring/start to truth blindly.** `key_ok` checks only the
  identifiable W+G columns (ring pinned `AAA`); `pct_correct` on the plaintext is
  the recovery oracle.
- **The `-F` filter-recall confound (§5)** is the subtle correctness risk: run
  the gate **unfiltered**, and treat filtered-tier `filter-fail%` as a proxy only.
- **A residual of structurally unrecoverable settings** (left ring, M4 Greek
  offset, non-A middle/right ring, rare-letter plugs) is inherent. Do not
  wildcard `-r` by default; never wildcard both left `-r` and left `-g`.

---

## 10. Phased build / rollout order

1. **Harness plumbing (no measurement claims yet).** Add
   `WILDCARD`/`XMAX`/`FILTER`/`RESTARTS`/`FULLCRACK`; edit `gen_trials()` (wheel
   range + ring-`AAA` force), `climb()` (wildcards + `-F`/`-R` wiring + 3-tuple),
   `last_key()`, `key_ok()` (W **and** G), `main()` columns; update the header
   comment + Makefile. **Verify `WILDCARD=""` is byte-identical to today** (the
   regression guard) and the wildcarded path is stdout-byte-identical at
   `T=1/3/8`.
2. **Establish the baseline curve.** Run F1/F3 and record mean%/exact%/key% at
   each length — the reference the downstream ideas must beat at equal
   `score_iter`.
3. **Answer H0 (the gate).** Run **G1** (unfiltered `START`, the clean primary
   gate) and **G2** (unfiltered `FULLCRACK`, nightly). Publish the
   search-vs-scoring split. If scoring-fail% ≈ 0 everywhere, stop — §6 stays
   parked. If non-zero, §6 re-opens.
4. **§5.3 cross-key marginalization** vs an independent-per-key baseline at a
   higher `-R`, matched `score_iter` (H1) — the highest-leverage lever, judged on
   the split shift.
5. **§6.3 MDL prior** (F4, H2), then **§6.7 calibration / `-F` score-margin**
   (H3), each judged by whether the scoring/search split shifts *without* raising
   search failures.
6. **Optional binary instrumentation** (flag-gated true-key tier-1 rank / `-F`
   recall) only if §5.3 needs shortlist-recall reporting — never a blocker for
   steps 1–5.

---

## References

Repo-internal: PERFORMANCE.md §1–§2 (framing, identifiability), §5.3 (cross-key
marginalization), §6.3 / §6.7 (MDL prior, calibration), §9 (the shortlist and
this infrastructure item); CLAUDE.md (progress-line display semantics, `-T`
determinism, `make crackquality`/`make bench` discipline);
`archived/CODE_REVIEW_HISTORY.md` §9 (nested hill-climb / `-F` rationale);
`archived/SIMULATED_ANNEALING.md` (SA design). External framing: Gillogly
(Cryptologia 19(4), 1995), Williams (24(1), 2000), Ostwald & Weierud (41(5),
2017), Lasry et al. (nested hill-climb + SA). Code anchors verified against the
current source: `setup_mapping` stepping/turnover `enigma.cc:668–680`,
`build_key_space` `enigma.cc:2504–2633`, `-F` activation `enigma.cc:2738`,
`filter_climb_cap` `enigma.cc:201`; `crack_quality.py` `gen_trials` :166,
`climb` :142, `oracle_score` :151, `last_score` :119.
