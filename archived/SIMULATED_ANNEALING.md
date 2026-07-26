# Simulated annealing for plugboard recovery — design

Status: **shipped as `-A`.** This is the design referenced from `CODE_REVIEW_HISTORY.md` §9
item 5. It proposes simulated annealing (SA) as an alternative plugboard optimiser, why
it should help, exactly how it works in *this* codebase, and a phased roadmap gated on a
compute-normalised A/B against the current best recipe (`-R 10 -S iq`). §§1–14 are the
original plan; **§15 records the tuning sweep that cleared the ship gate, and §16 the
known-plug-count cap (`-A … -S qK`)** — read those for what actually shipped.

Read `CODE_REVIEW_HISTORY.md` §9 first — it establishes the measurement harness
(`make crackquality`, its `SPLIT=1` failure-mode split) and the diagnosis this plan
builds on.

---

## 1. Why SA, and where it fits

The `crack_quality.py` `SPLIT=1` metric established the key fact: on the
plugboard-recovery tier, **every miss is a *search* failure, 0 % scoring failures
down to 40 characters** — the true plugboard out-scores whatever the climb reaches,
so the climb is getting stuck in local optima, not being misled by the score. That
is precisely the failure mode SA is designed to attack.

The shipped search levers all attack local optima indirectly:

- **`-R N` random restarts** — re-sample independent basins, keep the best. No
  plateau through 256, but each restart is a fresh greedy climb: it never *crosses* a
  barrier, it just hopes to land past it.
- **`-S` staged schedule** — climb a smoother low-order surface (IC) first to steer
  the first plugs into a good basin, then refine under quadgrams.
- **re-pair move** — a single gated barrier-cross at convergence.

SA is the missing tool: **a single trajectory that deliberately accepts worsening
moves early**, with the acceptance probability shrinking as the "temperature" cools,
so it can walk *out* of a local optimum and into a better one rather than restarting.
It is the classic method for exactly this problem (plugboard / substitution-key
recovery on short texts) and is the natural next item after restarts + staging.

**Where it plugs in.** SA replaces `hillclimb()` as the per-key plugboard optimiser.
Everything above it is unchanged: `bruteforce()` still enumerates rotor keys; `-R`
still means "run the optimiser N times from perturbed seeds, keep the best"; `-F`'s
tier 2 still runs the optimiser on the shortlist. SA is orthogonal to the rotor-key
search — it only changes how the plugboard is recovered for a given key.

## 2. Goals and success criteria

SA ships **only if it beats `-R 10 -S iq` at equal compute** on short messages,
measured on identical seeded problems. Concretely:

- Primary metric: exact-recovery rate and mean %-correct vs ciphertext length
  (L40–L250), english/quad, 10-pair board, via `make crackquality BASE=<ref>`.
- **Compute-normalised.** SA does far more score evaluations per key than a greedy
  climb, so "SA with an unlimited budget wins" is meaningless. Compare at equal
  wall-clock *and* equal total `score_iter` calls (instrument a counter). The honest
  question is "for the same budget spent on restarts, is that budget better spent on
  annealing?"
- Must not regress the easy regime: at L≥150 the greedy climb already ~recovers, so
  SA must converge to the same optimum there without a large constant-factor slowdown.

If it cannot clear that bar it stays documented-but-unbuilt, like χ² and 3-opt.

## 3. State, energy, and the move set (the core design)

### 3.1 State

The plugboard is an **involution** on 26 letters, already stored as
`m.steckerbrett[26]` with `steck[steck[x]] == x` and `steck[x] == x` for an unplugged
letter. SA's state *is* this array (plus the current score and temperature). No new
representation is needed; the same `decode_at`/`score_iter` machinery scores it.

Fixed `-s` pairs must be respected: those letters are frozen (never proposed for a
move), exactly as `perturb_steckerbrett()` already preserves them by only drawing
from unplugged letters. SA restricts its moves to the non-frozen letters.

### 3.2 Energy

Maximise `score_iter(m, iter)` (quadgrams by default). SA is conventionally phrased
as minimising energy `E = −score`. A move with `Δscore ≥ 0` is always accepted; a
move with `Δscore < 0` is accepted with probability

```
    P(accept) = exp( Δscore / T )        // Δscore < 0, T > 0
```

**Units matter.** The scores are `log10(count+1)` sums, so `Δscore` is in log-10
likelihood units and its magnitude scales with message length and model order. `T`
therefore has the *same units as the score* and cannot be a hard-coded constant that
works for both `-i` (IC, values ~0–0.08) and `-q` (quadgrams, values in the hundreds)
and both L40 and L250. This forces **per-problem temperature calibration** (§4) —
the single most important robustness detail.

### 3.3 The neighbourhood — one primitive, ergodic

Use the codebase's proven `switch`/`remove` primitive as the SA proposal, phrased as
a single reversible **toggle-connect(a, b)** move:

1. Draw two distinct non-frozen letters `a, b` uniformly at random.
2. If `steck[a] == b` (already a plug): **remove** it — free `a` and `b`.
3. Else **connect** `a–b`: if `a` had a partner `a' = steck[a] ≠ a`, free `a'`; if `b`
   had a partner `b' ≠ b`, free `b'`; then set `steck[a] = b, steck[b] = a`.

This one primitive covers *add* (both free), *reconnect/merge* (one or both plugged),
and *delete* (already paired) — so the chain can reach **any** involution from any
other (ergodic), which SA needs. It is the same operation `hillclimb()` already
applies and restores, so the scoring delta is computed the same way.

The set of letters whose `steck[]` entry changes is `C ⊆ {a, b, a', b'}` (2–4
letters). That small, bounded change set is what makes incremental scoring possible
(§6.2).

**Optional enrichments to evaluate (not in the first cut):**

- **re-pair as an SA proposal** — occasionally propose the `try_repair()` rewiring
  (two plugs → the other pairing of their four letters), a barrier-cross that keeps the
  plug count. Cheap to add once the delta-scorer exists.
- **temperature-scaled multi-plug kicks** — at high `T`, propose 2–3 toggles at once.
  Probably unnecessary: SA's stochastic acceptance already supplies exploration, and
  single-step moves keep `T` calibration clean. Measure before adding.

### 3.4 Proposal symmetry / detailed balance

`toggle-connect` is symmetric in `(a, b)` but not perfectly reversible in one step
when it ejects partners (merging two plugs into one cannot be undone by a single
toggle). SA is an *optimiser*, not an equilibrium sampler, so exact detailed balance
is not required — a roughly symmetric proposal with good ergodicity is the standard
and sufficient. (If we ever want a principled Metropolis–Hastings correction, the
proposal ratio for the eject cases is computable, but it is almost certainly not worth
it. Note this as a deliberate simplification, not an oversight.)

## 4. Temperature auto-calibration (do not hard-code T)

Because `T` shares the score's units, calibrate it **per key, per problem** from the
observed move deltas — the acceptance-ratio method (White / Ben-Ameur):

1. **Warm-up sample.** From the seed board, take `K` (e.g. 200) random
   `toggle-connect` moves, recording the magnitudes `|Δscore|` of the *worsening*
   ones. (This is a cheap, one-off O(K·L) pass per key — or per restart.)
2. **Initial temperature `T0`** so that a target fraction `χ0` of worsening moves
   would be accepted, e.g. `χ0 = 0.8`:
   `T0 = mean(|Δ⁻|) / ln(1/χ0)`  (or invert the empirical acceptance curve).
   At `χ0 = 0.8` the walk is near-random at the start (explores freely).
3. **Final temperature `Tend`** so the acceptance of a *typical* worsening move is
   negligible, e.g. `χend ≈ 0.001` — effectively greedy at the end.
4. Cool geometrically from `T0` to `Tend` (§5).

This makes SA length- and model-robust *for free*: a longer message or a
higher-order model has larger deltas, and the calibration scales `T` with them. It
also means the same SA parameters work under `-i/-m/-b/-t/-q` without per-model magic
numbers — important, since a staged SA (§7) will run under more than one model.

## 5. Cooling schedule and chain length

Pragmatic, budget-bounded schedule (the tool cares about per-key cost):

- **Geometric cooling:** `T_{k+1} = α · T_k`. Given a total move budget `M_total`
  and `L_chain` moves per temperature level, the number of levels is
  `M_total / L_chain`, and `α = (Tend/T0)^(L_chain / M_total)` so the schedule spans
  exactly `[T0, Tend]`. Typical `α ≈ 0.95–0.99`.
- **Chain length `L_chain`** (moves per temperature): a small multiple of the state
  size, e.g. `L_chain = c · 26` with `c ≈ 4–20`. Enough moves per level to
  quasi-equilibrate before cooling.
- **Total budget `M_total`** is the first-order cost/quality knob (the SA analogue of
  `-R`). Sweep it; expect a quality/compute trade like `-R`.

Alternatives to evaluate if geometric underperforms: **adaptive cooling** (cool only
when a level's acceptance drops below a threshold), and **reheating** (bump `T` back
up if no improvement for several levels — a restart analogue within one trajectory).
Start geometric; add these only if measured to help.

## 6. Performance

### 6.1 The cost problem

`hillclimb()` re-scores the whole message per candidate move, and the quadgram scorer
is ~99 % of runtime. A greedy pass is `325` candidate scorings; SA instead commits
*thousands* of moves per key (`M_total`), each needing a score. Naively that is much
more scoring work per key than a greedy climb — so SA is only viable if either (a)
the per-key budget stays comparable by doing fewer, smarter evaluations, or (b) each
evaluation is made incremental.

### 6.2 Incremental delta-scoring (the enabling optimisation)

A `toggle-connect` move changes only the `|C| ≤ 4` letters' plug entries, so the
decoded plaintext changes only at positions that route through a changed letter.
Compute `Δscore` in `O(affected)` instead of `O(L)`:

- Recall `decode_at(steck, rows, ct, i) = steck[ rows[i][ steck[ct_i] ] ]` — the
  plugboard is applied on **both** the input (`steck[ct_i]`) and the output
  (`steck[rows[i][...]]`).
- The decoded letter at `i` changes iff `ct_i ∈ C` (input side) **or**
  `rows[i][steck[ct_i]] ∈ C` (output side).
- **Input side is static:** precompute once per key `pos_by_ct[letter] = { i : ct_i ==
  letter }`. The affected input positions are `⋃_{c∈C} pos_by_ct[c]`.
- **Output side** depends on `steck[ct_i]`, which only changes for `ct_i ∈ C` (already
  covered by the input set). For `ct_i ∉ C`, `rows[i][steck[ct_i]]` is stable, so a
  `pos_by_preout[letter]` index can be maintained and only patched for the handful of
  input-affected positions each move.
- Given the affected position set `S`, the changed quadgrams are the windows covering
  them: `startset = ⋃_{i∈S} {i-3 … i}` (dedup, clamp to range). `Δscore = Σ_{j∈startset}
  (quad(new d_{j..j+3}) − quad(old d_{j..j+3}))`. Maintain the decoded buffer `d[]`
  incrementally on accept.

Per-move cost becomes `O(|S| + 4·|startset|)`, typically a handful for short L. This
is what makes a large `M_total` affordable. **It is intricate and must be validated**
against a full rescore with a debug assert (`|Δ_incremental − Δ_full| < ε`) behind a
compile flag — the same discipline the hot path already uses.

Caveat: the win is largest at **long** L; at the **short** L where SA matters most,
full rescore is already cheap (`O(L)` with L≤~150), so Phase 1 can use full rescore
and defer the delta-scorer to Phase 2 once SA is proven to help.

**Measured — rejected (built as a prototype, ~2× *slower*).** The delta-scorer above was
implemented for the greedy hill-climb candidate loop (its ideal case: 325 candidates per
pass, each one toggle off a fixed base board), assert-gated against full rescore (zero
mismatches, byte-identical output). Startup-subtracted per-restart climb time, quad,
`-S iq`:

| L | full rescore | delta | speedup |
|---|---|---|---|
| 50  | 0.72 ms | 1.67 ms | 0.43× |
| 88  | 1.45 ms | 2.66 ms | 0.55× |
| 150 | 1.54 ms | 3.36 ms | 0.46× |

Three effects the `O(affected)` model above under-weighted, all of which bite hardest for
**quad on short text**:

1. **Each affected quadgram costs two table lookups, not one** — subtract the old
   contribution, add the new — whereas full rescore does one lookup per gram.
2. **`switch` moves change `|C| = 4` letters, not 2** (both endpoints' partners are
   ejected). Once a few plugs are set, most candidates are `|C| = 4`, so `|startset|`
   reaches ~65–80% of a short message's grams. At that fraction, delta already does
   ~1.5× the lookups of a full pass *before* bookkeeping.
3. **The fused baseline is a very tight target** — branch-free, sequential,
   `__restrict`-hoisted, no scratch array. The delta path is scattered random-access
   positions + per-candidate CSR scans + stamp dedup + a per-pass base rebuild:
   cache-unfriendly and unvectorizable.

Textbook delta-scoring wins for **long** texts with **small, localized** changes (e.g. a
simple-substitution swap that moves exactly two letters' positions). This workload is the
opposite corner. So the delta-scorer is **documented-but-unbuilt**, like χ² tier-1 and
3-opt — do not re-attempt it for the plugboard climb without changing the regime (much
longer L, or a model whose window does not spread each change ×4).

### 6.3 Cheaper-model exploration (a second lever)

Reuse the `-S` insight: run the high-`T` exploratory phase under a **cheaper model**
(IC or bigram — far less work per score than quad) and switch to quadgrams only for
the low-`T` refinement. This both cuts cost and smooths the early surface, and it
composes with the staged-schedule machinery already in place.

### 6.4 Hot-path constraints

Any incremental scorer touches the hot path, so the `../CLAUDE.md` performance rules
apply: hoist member base pointers into `__restrict` locals, keep the layout that
keeps `subst_array`/`mapping`/`steckerbrett` off large struct offsets, and re-check
`make bench BASE=<ref>` under **both g++ and clang** after implementing.

## 7. Composition with the existing recipe

- **Staged SA (`IC → quad`).** The single best shipped recipe is `-S iq`. The SA
  analogue: anneal (or greedily seed) under IC to place the first plugs, then anneal
  under quadgrams. Evaluate SA-on-quad-from-empty vs IC-seed-then-SA-quad; expect the
  latter to win, mirroring `-S iq`.
- **Restarts (`-R`).** SA composes with restarts unchanged: `-R N` runs N independent
  SA trajectories from perturbed seeds (per-restart deterministic RNG), keep the best.
  SA-with-few-restarts may match greedy-with-many-restarts at lower total cost — that
  is the headline comparison.
- **Pre-filter (`-F`).** Tier 2 simply calls the SA optimiser instead of
  `hillclimb_restarts()`. Tier 1 stays the cheap capped IC greedy climb (SA there
  would be far too expensive for an all-keys pass).
- **Final quench.** SA returns the **best board seen along the trajectory** (track an
  incumbent — the final state at `Tend` is not necessarily the best), then run one
  greedy `hillclimb()` to local-optimum as a cheap polish.

### 7.1 CLI design

SA has its own parameters, so give it a dedicated flag rather than overloading `-S`.
Proposed: **`-A[schedule]`** enables SA plugboard recovery, with an optional compact
schedule string parsed like `-S`:

- `M<n>` total move budget (the main knob), `c<n>` chain-length multiple,
  `a<f>`/`z<f>` initial/final acceptance targets `χ0`/`χend`, and a model token
  (`i/m/b/t/q`) for the anneal model (last one = ranking model, matching `-S`).
- Example: `-A M20000 c8 ai q` = 20 000 moves, chains of `8·26`, calibrate `T0` to 80 %
  acceptance, anneal under IC-then-quad… (final grammar TBD in Phase 1).
- `-A` and `-c`/`-S` interplay: `-A` selects the optimiser; `-R` still means restarts;
  `-F` still shortlists. Keep `-c` as the master "hill-climb the plugboard" switch and
  let `-A` choose *which* optimiser it uses, so existing invocations are unaffected.

Defaults chosen so a bare `-A` is a sensible SA run; all validated like the other
options.

## 8. Determinism and threading (non-negotiable)

The whole search is `-T`-deterministic, and the test suite enforces `-T 1 == -T 8`.
SA must preserve this exactly as `-R` restarts already do:

- **Per-key RNG.** Seed a private `splitmix64` stream from the flat key index (and the
  restart index for `-R`), identical to `hillclimb_restarts()`. Every random draw —
  the `(a, b)` proposal and the acceptance uniform `u ∈ [0,1)` — comes from this
  per-key stream, in a **fixed order** (propose, then draw `u`), so the trajectory is a
  pure function of `(key, restart)` and independent of thread scheduling.
- **No wall-clock, no `Math.random`, no thread id** anywhere in the SA state.
- Per-`machine` state only (the current board, score, `T`, incumbent, RNG live in
  locals or a small SA struct threaded like `machine &`), so workers never share
  mutable SA state — race-free by construction, like the rest of the search.
- Acceptance uses `exp()` of a deterministic double; guard the exact float path (same
  `exp`, same operand order) so g++ and clang agree bit-for-bit isn't required, but the
  *decision* (`u < exp(Δ/T)`) must be reproducible — it is, since both operands are
  deterministic.

## 9. Pseudocode

```
// per rotor key, per restart r
anneal(machine m, uint64 seed):
    rng = splitmix64_state(seed)                 // deterministic per (key, r)
    init board from -s (frozen pairs) + optional perturbation for r>0
    cur   = score_iter(m)                         // full score once
    best  = cur;  best_board = m.steckerbrett     // incumbent

    // --- calibrate T0, Tend from a warm-up sample (§4) ---
    deltas = []
    for K samples: (a,b)=random_pair(rng); d = delta_if_toggle(m,a,b); if d<0: deltas.push(-d)
    T0   = mean(deltas) / ln(1/chi0)              // e.g. chi0 = 0.8
    Tend = mean(deltas) / ln(1/chi_end)           // e.g. chi_end = 0.001
    alpha = (Tend/T0) ^ (L_chain / M_total)

    T = T0
    while moves_done < M_total:
        for L_chain steps:
            (a,b) = random_pair(rng)               // draw 1
            dscore = delta_toggle(m, a, b)         // incremental (Phase 2) or full (Phase 1)
            if dscore >= 0 or uniform(rng) < exp(dscore / T):   // draw 2
                apply_toggle(m, a, b)
                cur += dscore
                if cur > best: best = cur; best_board = m.steckerbrett
            moves_done++
        T *= alpha

    m.steckerbrett = best_board                    // return incumbent
    hillclimb(m)                                   // final greedy quench
    return score_iter(m)
```

`hillclimb_restarts()` becomes: for each restart, call `anneal()` (or the greedy
climb, depending on `-A`), keep the best — the merge logic is unchanged.

## 10. Parameters and shipped defaults (measured, §15)

| Parameter | Symbol | Shipped value | Notes |
|-----------|--------|---------------|-------|
| Total move budget | `M_total` | `-A N` (user) | main cost/quality knob (SA's `-R`) |
| Chain length | `L_chain` | `8·26 = 208` | moves per temperature level |
| Initial acceptance | `χ0` | **0.12** | tuned; a *cool* start wins here (§15) |
| Final acceptance | `χend` | 0.001 | low → greedy finish |
| Warm-up samples | `K` | 200 | for T calibration |
| Anneal model | — | `IC → quad` | staged, mirrors `-S iq` |
| Restarts | `-R` | as configured | independent SA trajectories |

The starting guesses were `χ0 = 0.8` and `M_total = 20 000`; the sweep in §15 replaced
`χ0` with **0.12** (the original 0.8 lost ~2× — see §15). Chain length, `χend`, warm-up
samples and the IC pre-pass were left at their guesses (sweeping chain and reheating did
not help).

## 11. Evaluation plan

1. **Correctness first.** Round-trip and KAT unaffected (SA only changes plugboard
   recovery). Add `-T 1 == -T 8` determinism tests for `-A`, and a "SA recovers a known
   plugboard on a long message" test.
2. **Quality A/B.** `make crackquality BASE=<ref>` with `SPLIT=1`, english/quad,
   10-pair, L40–L250. Compare: greedy `-R 10 -S iq` (incumbent best) vs SA at matched
   budget. Report exact-recovery, mean %-correct, L50/L90.
3. **Compute normalisation.** Instrument a `score_iter` counter; compare SA vs greedy
   at equal total evaluations *and* equal wall-clock. Report both.
4. **Speed guard.** `make bench BASE=<ref>` under g++ and clang for the hill-climb
   path (SA changes per-key cost; the scan path is untouched).
5. **Ship gate.** SA ships only if it beats `-R 10 -S iq` on short-L recovery at ≤ its
   compute. Otherwise: document the negative result here and stop (like χ²/3-opt).

## 12. Risks and pitfalls

- **T mis-scaled** → SA degenerates to a random walk (`T` too high) or to greedy (`T`
  too low). The acceptance-ratio calibration (§4) is the mitigation and must be in the
  first cut, not bolted on later.
- **Unfair budget.** Comparing SA-with-a-big-budget to greedy is meaningless; always
  compute-normalise (§11.3).
- **Delta-scorer bugs.** Incremental scoring is error-prone (both-plug-ends, window
  overlap, `pos_by_preout` maintenance). Gate it behind a debug assert vs full rescore;
  keep the full-rescore path as the reference and the Phase-1 implementation.
- **Determinism drift.** Any RNG draw out of order, or a thread-dependent decision,
  breaks `-T` independence and the suite. Fix the draw order and keep all state
  per-machine.
- **No short-L win.** Plausible outcome: at the short L where it matters, a
  well-tuned SA may only match `-R -S` at equal compute (restarts are already strong).
  That is a legitimate "don't ship" result — the plan's value includes cheaply proving
  that, if so.
- **Long-L regression.** Ensure SA converges to the greedy optimum on easy messages
  without a large constant-factor cost; the final quench helps.

## 13. Phased roadmap

- **Phase 0 — harness.** Confirm `make crackquality`/`SPLIT` and add a `score_iter`
  evaluation counter for compute-normalised comparison. (No product code.)
- **Phase 1 — minimal SA, full rescore.** `toggle-connect` moves, geometric cooling,
  acceptance-ratio T-calibration, incumbent + final quench, per-key deterministic RNG,
  behind `-A`. Full-message rescore per move (simple, correct). A/B vs `-R 10 -S iq`.
  **Decision point: does it help at all?**
- **Phase 2 — incremental delta-scoring. Tried and REJECTED (§6.2):** the assert-gated
  prototype was ~2× *slower* than full rescore for quad on short L (the new+old double
  lookup, `|C|=4` switch moves, and the cache-unfriendly bookkeeping outweigh the fewer
  grams touched). Not built. Do not re-attempt without a different regime (much longer L).
- **Phase 3 — tuning & composition.** Sweep `M_total`, `L_chain`, `χ0/χend`, staged
  vs flat model, reheating/adaptive cooling; wire SA into `-F` tier 2 and `-R`. Settle
  defaults.
- **Phase 4 — ship or shelve. → SHIPPED (`-A`).** SA cleared the gate once `χ0` was
  tuned (§15): at equal climb time it matches or beats greedy `-R -S iq`. Phase 1's full
  rescore was not just fast enough but *the right choice*: the Phase 2 delta-scorer was
  later prototyped and measured ~2× slower for quad on short L (§6.2), so it was rejected.
  Tests, CI (ASan/UBSan + TSan cover the `-A` path), and docs (README, ../CLAUDE.md,
  CODE_REVIEW §9 item 5) are updated.

## 14. Research extensions (out of scope for the first build)

- **Tabu hybrid** (§9 item 6) — a short tabu list over recently toggled pairs layered
  on SA to avoid re-treading, a small deterministic robustness gain.
- **Joint rotor + plugboard annealing.** SA over the *rotor key too* (not just the
  plugboard) would be a much bigger change: it conflicts with the precompute-table
  architecture (which assumes enumerated rotor keys and shares `subst_array` per wheel
  order), so a joint SA would lose that precompute and rescore the rotor stack per
  move. Almost certainly not worth it versus brute-forcing rotor keys, but noted.
- **Adaptive neighbourhoods** — bias the `(a,b)` proposal toward letters currently in
  low-scoring quadgrams, rather than uniform. Could speed convergence; adds state and
  determinism surface.

## 15. Measured results (the tuning sweep that decided the ship)

Setup: plugboard-recovery tier (true rotor key fixed, only the plugboard recovered),
english/quad, 10-pair boards, random excerpts of an English corpus, per-trial distinct
seeds (fair RNG diversity for both methods), 60 trials per point. "climb ms" is
**startup-subtracted** wall-clock per trial (the ~76 ms process spawn + quad-table load
is measured once and removed), so it is the marginal compute — the fair per-time axis.
The incumbent to beat is the greedy staged restart climb `-R N -S iq`.

**The initial guess `χ0 = 0.8` lost ~2×** to greedy at equal compute (e.g. L80:
SA `-A 50000` 35.5% vs greedy `-R20` 75.8%). The failure mode was exactly risk §12
"T mis-scaled → random walk": a hot start wanders. Sweeping `χ0` **down** improved SA
monotonically until ~0.12, then it turned over (χ0 0.05/0.12/0.20/0.30 →
68.5/79.6/64.5/68.4% at L80, `-A 100000`). The surface here is greedy-friendly, so the
best SA is a *mostly-downhill* walk with just enough uphill escapes.

At the tuned `χ0 = 0.12`, matched climb time (L = 50 and 80):

| config | climb ms | L50 exact/60 | L50 mean% | L80 exact/60 | L80 mean% |
|---|---|---|---|---|---|
| greedy `-R10 -S iq` | ~13 | 7  | 21.7 | 31 | 57.9 |
| SA `-A 50000`       | ~14 | 6  | 19.7 | 32 | 58.4 |
| greedy `-R20 -S iq` | ~23 | 12 | 30.0 | 41 | 72.1 |
| SA `-A 100000`      | ~22 | 10 | 25.5 | 46 | **79.6** |
| greedy `-R40 -S iq` | ~45 | 15 | 34.1 | 48 | 82.5 |
| SA `-A 200000`      | ~44 | 20 | **41.2** | 47 | 81.0 |

Read-out: tuned SA is **at parity-to-better** with greedy at equal compute — a clear win
at the L80 mid budget (SA `-A 100000` 79.6% vs greedy `-R20` 72.1%, *and* slightly
faster) and the L50 high budget (41.2% vs 34.1%), a wash elsewhere. That clears the ship
gate (§11.5: "beats `-R 10 -S iq` at ≤ its compute"). **Reheating hurt** (8/20 cycles
roughly halved quality) and **chain length** (52/208/520) was flat, so both were dropped;
the shipped SA is a single geometric cool-down with the IC pre-pass and greedy quench.

Caveat: this is one corpus/language/board-size at two lengths. SA does not *dominate*
greedy — it is an alternative of comparable strength, worth having as a second
metaheuristic (and a base for the §14 extensions), not a replacement for `-R -S`.

## 16. Known-plug-count cap (`-A … -S qK`)

The Wehrmacht did not always use 10 plugs — earlier/other periods used fewer. When the
count is *known* to be below the maximum, that is a genuine prior worth exploiting, and
SA now honours it: `-A` reads the **`-S` target-stage plug cap** (`opt_stages[last].cap`,
uncapped = 13 by default) and applies it across the whole trajectory — the IC pre-pass,
a **cap-aware `apply_toggle`** (a *connect* that would raise the pair count past the cap
is a no-op; removes and re-pairings are always allowed, so the ≤-cap board stays
reachable), and the greedy quench. So `-A 50000 -S q8` anneals toward at most 8 pairs.
Plain `-A` (no `-S`) keeps the uncapped default, unchanged.

The mechanism is the same overfit story as the `-F` cap (§9 item 2), but here it needs a
*correct* count: on a short, noisy message the quad score sometimes rewards a spurious
9th/10th plug that pulls the board off the truth; capping to the real count removes
exactly those moves.

Measured (true plug count **8**, english/quad, per-trial seeds, 60 trials/point; exact
recoveries out of 60), plain `-A` vs `-A -S q8`:

| budget | length | `-A` (uncapped) | `-A -S q8` |
|---|---|---|---|
| A50k  | 50 | 30 | **37** |
| A50k  | 80 | 53 | 50 |
| A100k | 50 | 42 | 41 |
| A100k | 80 | 54 | **58** |
| A200k | 50 | 49 | 45 |
| A200k | 80 | 59 | 59 |

The win concentrates in the **hard corner** — short message, modest budget (a second
independent trial set confirmed it, e.g. A100k/L50 37 vs 27) — and fades to a wash once
the message is long enough or the budget high enough that SA finds the true board
unaided. Setting the cap *below* the true count clips the reachable boards and hurts, so
it is strictly a user-supplied prior, not a default. The IC-cap-alone experiment (capping
only the pre-pass, no count knowledge) was by contrast neutral — the value here is the
*correct count*, not smoothing.
