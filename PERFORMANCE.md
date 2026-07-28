# PERFORMANCE.md — Improving short-message plugboard recovery

> **Scope and relationship to the other docs.** Despite the file name, ~80% of
> this document is about **cracking quality** (recovering the plugboard on short
> ciphertext), not raw throughput — in this repo `make bench` is the "performance"
> harness and `make crackquality` is the recovery harness, and most ideas below are
> measured by the latter. Read this as the forward-looking roadmap that **extends
> `CODE_REVIEW.md` §1–§2** (the live "cracking quality — search / scoring" items)
> and the archived **`archived/CODE_REVIEW_HISTORY.md` §9** (the shipped-feature rationale and
> the measured-and-rejected list). The simulated-annealing design and its tuning
> evidence live in **`archived/SIMULATED_ANNEALING.md`**; where an idea here touches SA it
> cites that file. Nothing here is a decision to build — it is the option space with
> honest priors, so the maintainer can pick the next experiment. **No code changes
> accompany this document.**

## 1. Framing and current state

The hard problem this tool has left to solve is **cracking quality on short
ciphertext** (~50 letters), specifically **recovering the plugboard
(steckerbrett)** once the rotor key is known. This is the regime the
`make crackquality` harness measures on its plugboard-recovery tier (true rotor
key fixed, plugboard hill-climbed from scratch).

The decisive diagnostic — and the premise of everything below — is the harness's
`SPLIT=1` classification: **down to 40 characters, every miss is a *search*
failure, not a *scoring* failure.** The true plugboard out-scores whatever board
the climb reaches; the climb is simply stuck in a local optimum. So on this tier
the lever is the **search**, not the shape of the score. Scoring changes are only
justified where they *reshape the search landscape* (smoother surface → fewer
local optima) or where they prepare the ground for a *not-yet-built full-crack
tier* (rotor key also unknown), where rotor-key discrimination may expose genuine
scoring failures.

> **Measurement rule for search work: exclude the scoring-failure cases.** Because
> the goal now is improving **search** (not scoring), evaluate every search lever
> (restarts, `-F`, SA, the `--cascade`/`--polish` finishers, and any future tabu/GA) on the
> **search-failure + exact** population only — drop the trials where the true board
> is *not* the top-scoring one, since no search can ever reach them. Operationally a
> trial is a scoring failure when it is **non-exact and `recovered_score ≥ true_score`**
> (the converged board already scores at least as high as the truth — the information
> floor); `SPLIT=1`, or the oracle `recovered_score`/`true_score` columns in the
> `eval/results-*.tsv` baselines, classify them. Leaving them in only injects noise a
> search change cannot move. A scoring change is the mirror case: it *is* judged on the
> full population, because shrinking that floor is its entire job.

**Already shipped** (do not re-propose; tune only): steepest-ascent hill-climb
(switch / remove / gated re-pair moves); random restarts `-R`/`--restarts` with a
`--random`-sized kick (default `k=10` pairs); staged schedule `--score` with the general
recipe `--score iq` (IC pre-pass then quadgrams) and per-stage plug caps — on realistic
~10-plug short texts the **tuned recipe is `--score i4q10`** (IC pre-pass capped at 4, quad
capped at 10, default kick): measured **+3–10pp mean %-correct at L40–80 over `--score iq`
at matched compute**, both seeds, because capping stops the noisy short-message score adding
spurious plugs *and* makes each climb cheaper so the same budget buys ~30% more restarts. The
win is the **quad cap of 10**; the IC pre-pass cap is a **flat plateau** — `i3`/`i4`/`i5`/`i6`
tie within noise (~0.7pp, both seeds, matched `-R 26`), so `i4` is just a central
representative, not a sharp optimum. A *small* kick (`--random 3/4`) hurts, so the kick stays
the default; key pre-filter `-F` (cheap capped-IC
climb shortlists keys, ~8–20× throughput); simulated annealing `-A`
(toggle-connect, acceptance-ratio-calibrated temperature, tuned `χ0=0.12`, IC
pre-pass + greedy quench, honoring a known-plug-count cap) — a **peer** of the
restart climb at equal compute, not a strict win; `-s` fixed known plugs held
through the climb; uint8 fixed-point score tables with a hapax floor; per-symbol
cross-entropy normalization.

**Already measured and rejected** (cited below as evidence, never re-proposed):
incremental/delta scoring of only plugboard-affected quadgrams (~2× *slower* for
quad on short text); chi-squared as scoring / `-F` tier-1 model (gameable by the
plugboard permutation, far worse recall); 3-opt / 3-plug re-pair (cost exceeded
gain); SIMD / `-march=native` (the score loop is gather-**latency**-bound, not
throughput-bound); GPU (scan-only, still gather-bound, loses the portable
single-TU design); rotor-stepping reuse across start positions (byte-identical
but slower); SA reheating and chain-length sweeps (no help); `χ0=0.8` (lost ~2×);
plugboard-free IC scan as pre-filter (~0% recall); 5-grams / 4-bit scores (too
sparse / too coarse); uncapped tier-1 IC climb (overfits, worse recall *and*
slower); back-off / interpolated quadgram smoothing (`--backoff`, §6.2 — conditional
form −20pp, joint-floor form neutral; the harsh flat floor is a discrimination
*feature*); trigram-target-at-short-end (§6.1 — with the tuned `-S i4qK` recipe quad
beats trigram at the realistic 10-plug count, +~9pp L90 two seeds; the bare-model
"trigram wins" was a weak-baseline artifact).

**Constraints every proposal must respect:** `-T` thread determinism (all
randomness rides the per-key splitmix64 stream seeded from the flat key index —
byte-identical results at any thread count); a portable single translation unit
(no platform intrinsics); and measurability with the existing harnesses
(`make crackquality` for recovery vs length, `SPLIT=1` for the scoring/search
split, `BASE=<ref>` for same-machine A/B; `make bench` for throughput, under
**both g++ and clang** for anything near the hot path). Compute-normalize
trajectory-changing ideas on **total `score_iter` calls** (the per-machine
counter already exists) or on matched wall-clock — *not* matched `-R` — because
the point of a speedup is to buy more restarts. And note the sharpest baseline:
`-R` **never plateaus through 256**, so every metaheuristic in §3 competes with
*simply raising `-R`* at equal compute — that comparison, not "beats a single
climb," is the bar.

---

## 2. Information-theoretic reality check

Set expectations before building anything. The plaintext-recovery tier fixes the
rotor key, so the only unknown is the ~10-pair involution: about
`log2(1.5×10^14) ≈ 47` bits.

**Lead with identifiability, not a bit budget.** A given plug's evidence lives
*only* at the message positions where its two letters appear. In 50 characters
several of the 26 letters occur 0–2 times, so a subset of the ten plugs — those on
rare letters — is **near-unidentifiable from the sample, regardless of search
effort.** This is a hard floor: no amount of restarts recovers a plug whose two
letters barely occur, because there is almost no data that distinguishes it from
the identity. This occurrence-count argument is the sound one and the premise for
everything below.

A cruder **bit-budget heuristic** points the same way but must not be over-read:
English redundancy is ~3.2 bits/char, so 50 letters carry on the order of ~150
bits of language constraint — nominally enough to pin 47 bits, but that conflates
*total* language redundancy with the *mutual information the plugboard specifically
receives*, and the budget is spread unevenly across letters exactly as the
identifiability argument says. Treat "~150 bits ≥ 47 bits" as a loose upper bound
on what is *ever* possible, not a promise that search can reach it.

Corollaries that discipline the rest of this document:

- The realistic target at 40–50 chars is **higher mean %-correct and partial
  recovery** (the harness's graded metric), not exact recovery. The literature
  agrees: Gillogly and Williams used 500–650 letters; Ostwald & Weierud targeted
  250 and call short messages "rarely successful."
- The **identifiable** plugs (those on letters with real support) are what any
  method can hope to recover. Techniques that *separate identifiable from
  unidentifiable* (consensus across restarts, evidence-restricted moves) are
  attractive precisely because they localize the achievable signal.
- The only way to genuinely add information at 50 chars is a **crib** (known
  plaintext) — bombe-style deduction — or **pooling across candidate rotor keys**
  in a full-crack tier. Everything else trades compute for basin coverage.

---

## 3. Search / metaheuristics

These attack the documented failure mode directly. The honest prior from both
the repo (SA is only a *peer* of `-R -S iq`) and the survey literature
(GA ≈ SA ≈ tabu ≈ hill-climbing for substitution/transposition cryptanalysis) is
that **no metaheuristic is a demonstrated strict winner**; expect "convert some
short-text search failures / reduce variance at equal budget," not a step change.
Rank within this section is by evidence and payoff-per-effort. **The baseline each
one must beat is a higher `-R` at equal compute**, not a single climb.

### 3.1 Cross-restart consensus / plug fixation — ❌ BUILT, MEASURED, REJECTED

*This was the flagship "if you do three things" pick. It was implemented in full and
measured against its own stated bar (beat a higher `-R` at equal compute). It does
not. Kept here as a documented rejection so it is not re-attempted; the top-of-shortlist
slot is reassigned in §9.*

**The idea.** `hillclimb_restarts()` (`:1478`) runs `-R` independent climbs and keeps
only the single best board, discarding the rest. Consensus instead mines the discarded
boards: keep the top-E by score (an elite set), vote each plug across them, freeze the
plugs that appear in ≥ N% of the elite via a per-key freeze mask (the generalization of
`-s`'s `plug_fixed[]`), and run one more climb over only the residual (free) letters.
The premise (per §2): a truly-set plug survives across the *good* basins, so voting is a
variance-reduction estimator that pulls the identifiable plugs out of the noise and
shrinks the problem the final climb must solve. Fully deterministic (a pure function of
the deterministic restart boards), so `-T`-independent.

**What was built.** A per-machine `plug_frozen[26]` mask (routed through the four climb
freeze-checks — switch/remove/re-pair/SA-toggle — so consensus can freeze plugs per key
without racing on the shared global); elite top-E voting; the residual climb via
`optimize_once` (so it composes with `-A`); best-of-{restarts, residual} kept so it can
only ever help. Correct, `-Werror`-clean on g++/clang, all tests pass, `-T`-deterministic
(identical output at T=1/3/8 on a real multi-key search).

**Why it was rejected — the measurement.** The residual climb costs only ~+8%
`score_iter` (about half a restart), so the honest baseline for `-C` at `-R N` is roughly
`-R N.5`. Measured on `make crackquality` (exact-recovery %, identical problems):

| regime | plain `-R N` | `-R N` + consensus | equal-compute `-R N+1` |
|---|---|---|---|
| R8, PAIRS 10, L90, seed 0 | 52.2 | 52.5 | **54.8** |
| R8, PAIRS 10, L90, seed 1 | 53.0 | 53.8 | **56.0** |
| R8, PAIRS 6, L70, seed 0 | 67.3 | 68.0 | **70.0** |
| R20, PAIRS 10, L90 | 73.0 | 73.0 | **73.3** |

Consensus consistently edges out *plain* `-R N` by a fraction but **loses to (or at best
ties) the same compute spent on one more restart** — across thresholds {50,60,70}, elite
sizes {top-2, top-5, R/2, all-boards}, `R` {8,16,20}, PAIRS {6,10}, lengths {50,70,90},
and two seeds.

**And — counter-intuitively — raising `R` makes it *worse*, not better.** At `R20` it is
a near-total no-op: identical to plain `R20` at *every* threshold and elite size (even
top-2 agreement). Mechanism: best-of-`R` saturates. By `R=20` the best of 20 independent
climbs is already excellent (73% exact at L90), so the consensus residual climb almost
never finds a board better than one already in the top-20. The marginal climb is always
better spent on a *fresh* restart (a new basin) than on re-climbing a consensus-frozen
seed. The stated risk — restarts sharing a *correlated* wrong basin, so consensus
reinforces the error — is exactly what dominates, and it does not diminish with more
votes because the votes agree on the same wrong (or already-found) answer.

**Verdict.** Compute-neutral-to-negative; strictly weaker than `-A` (a genuine *peer*).
Not shipped. Do not re-attempt without a materially different mechanism (the untested
levers — threshold, elite size, `R` — were all swept and none crosses the equal-compute
`-R` line).

### 3.2 Portfolio: per-key `max(greedy, SA)` — ❌ MEASURED, REJECTED

**Form.** For each rotor key run greedy at half budget and one SA trajectory at half
budget, keep the better board. The pitch was near-free upside: SA is a *peer* of greedy,
so (it was argued) they fail on *different* keys and `max` captures the union.

**Measured (crackquality, PAIRS=10, L70/80/90, 100 trials × 2 seeds, matched `score_iter`:
greedy `-R20 -S iq` ≈ SA `-A6000 -R12 -S iq` ≈ 132k).** The portfolio at matched compute is
**neutral-to-negative** — Δ vs the best single solver: −2/+0/−1 (seed 1) and −5/−7/+2 (seed 2),
averaging ~−3pp, never a clean win. Rejected. Two-part reason, and the second half is the
non-obvious one:

- **The complementarity is real** — so the doc's *falsifier* (nested solved-sets) did **not**
  hold. At *double* budget (each solver at full B, "either solves") the union beats the best
  single by **+10–17pp**: greedy and SA genuinely fail on different keys (SA's stochastic
  acceptance escapes different local optima than greedy's restarts). The overlap check
  confirmed large `greedy-only` **and** `SA-only` sets.
- **But the budget split cancels it.** Halving each solver to fund both costs ~11–14pp per
  solver (recovery has not saturated at these budgets), and that loss almost exactly cancels
  the union gain, so `max(halves) ≈ best single @B`. **And when one solver dominates at a
  given compute (SA out-recovered greedy in both seeds), the portfolio is strictly worse** —
  it spends half the budget on the weaker solver. A portfolio only helps when neither solver
  dominates *and* the split is cheap; here one usually dominates and the split is expensive.

**Where the original reasoning failed.** "SA is a peer ⇒ the solved-sets differ" is an
invalid inference (equal *average* recovery is consistent with identical *or* disjoint
solved-sets); the sets happened to differ anyway, but the argument never priced in the cost
of running each solver at half budget, which is what actually sinks it.

**Takeaway.** Don't split — identify and run the single best solver at full budget. The real
complementarity (~+15pp at 2B) is worth capturing, but *only* without the halving penalty,
which a post-hoc `max` of two independent runs cannot do — a single trajectory that blends
exploration and exploitation can (→ ILS, §3.3).

**Follow-up: greedy vs SA at matched compute, both properly tuned.** An earlier note here
claimed "SA consistently out-recovers greedy" — that was an artifact of comparing SA against a
*weak* greedy (`-R --score iq`). Re-run with each solver at its best (greedy
`-J --score i4q10 --random 10`; SA `-A12000 --score q10` — SA needs *deep* anneals, `-A6000`
starves it), matched `score_iter`, 10-plug messages, 100 trials × 2 seeds, they are **genuine
peers with a length-dependent crossover** (mean%/exact%, at ~2× the `-R20` budget):

| L | greedy `-J --score i4q10 --random 10 -R80` | SA `-A12000 --score q10 -R12` |
|---|---|---|
| 40 | 24 / 8  | **27 / 12** |
| 50 | **45 / 33** | 40 / 32 |
| 60 | 60 / 50 | **63 / 57** |
| 70 | **78 / 73** | 70 / 64 |

SA owns the mid-short / hardest end (L40, L60: +4–7pp exact); greedy owns the longer short
end (L70: +9pp), with L50 ~tied. The result held at half budget too. So the repo's original
"SA is a *peer*, not a win" framing is correct — and *both* under-tuning greedy and
under-tuning SA flip the apparent winner, which is why a matched-compute comparison must have
**both** at their best. (Practical recipes for each are in the README / `-h`.)

### 3.3 Iterated Local Search with incumbent-walk acceptance (HIGH priority)

**Form in this codebase.** Today `hillclimb_restarts()` (`:1497`) resets to the
**fixed seed board** (`init_steckerbrett(m, opt_steckerbrett)`) and *then*
perturbs it each restart — so every restart is an independent kick off the same
seed (true random-restart), never a walk off the incumbent. True ILS instead keeps
a **walk incumbent** and accepts the post-climb result via a criterion (accept if
`score ≥ incumbent − δ`), so the search can drift through a chain of nearby optima
instead of always relaunching from the seed. Add an acceptance mode: perturb *from
the current incumbent*, local-search, accept within `δ` (else revert to the
incumbent); keep the global best separately for the returned answer. ~40 lines;
deterministic; expose the kick `k` via the existing `rN` token.

**Why it helps at 50 chars.** The true board is often a few swaps from a strong
decoy optimum. Fixed-seed restart re-explores the same seed neighborhood every
time; an accepting walk strings small perturbations together to tunnel across a
plateau the greedy climb cannot cross, carrying partial progress forward instead
of discarding it.

**Honest payoff.** Medium, and the cheapest plausible improvement over `-R`
because it reuses every existing piece (perturb + climb). Skeptical note: with a
*large* kick (`k=8`), ILS and fixed-seed restart converge in behavior — the win
likely needs a *smaller* perturbation (`k≈2–4`) paired with δ-acceptance. That is
exactly the small-`k` regime the shipped sweep found to be a *footgun for
restarts*, but it is the natural regime for ILS (a small kick off a *good*
incumbent is very different from a small kick off a *fixed seed*). Co-tune `k` and
`δ`.

**Cost/risk.** Low. One new knob (`δ`); `δ=0, k=8` must reproduce today's `-R`
numbers exactly (a sanity control).

**Experiment.** `make crackquality` at L40–120 sweeping `k∈{2,3,4,8}`,
`δ∈{0, small, ∞}`, at fixed total `optimize_once` calls; must beat plain `-R` at
that budget.

### 3.4 Parallel tempering / replica exchange (MEDIUM priority)

**Form in this codebase.** Run M *logical* SA replicas (not OS threads) of one
key's plugboard at a geometric temperature ladder (e.g. 4–8 rungs from χ≈0.4 down
to the current 0.12), each doing the shipped `apply_toggle` walk. Every
`anneal_chain` moves, propose Metropolis swaps of adjacent-temperature replicas
with `min(1, exp((1/Ti − 1/Tj)(Ei − Ej)))`. The replica loop and swap RNG draws
sit on the per-key stream inside `optimize_once`, so `-T` determinism holds and
the hot `subst_array` path is untouched (scoring unchanged). ~150 lines layered
over `anneal_once()` (`:1378`).

**Why it helps at 50 chars.** Rugged-landscape local-optima problems are where PT
is documented to beat single-chain SA: a hot explorer keeps discovering new
basins and hands them to the cold exploiter, instead of `-R`'s throwaway
independent climbs that share no information.

**Honest payoff.** Medium. PT is "a better SA," and SA here is only a *peer* of
`-R -S iq`, so gains inherit that ceiling. The literature's "PT beats SA" edge is
strongest as **variance reduction** (fewer catastrophic misses at fixed budget) —
which maps to converting some L50–L80 mid-budget search failures — rather than a
new capability. Worth building because it is the one metaheuristic with the
clearest "outperforms plain SA" evidence and reuses the existing move/scorer
wholesale.

**Cost/risk.** Medium. Compute-normalize honestly: M replicas cost M chains, so
compare A-budget/M per replica against `-R` and single-chain `-A` **at equal total
`score_iter`** — and against a higher `-R`.

**Experiment.** `make crackquality BASE=<ref> SPLIT=1` at L40–120, equal total
`score_iter` vs `-A` and `-R 10 -S iq`; report exact-recovery, mean %-correct,
**and variance across seeds** (PT's claimed edge). `make bench` to confirm no
regression (new code is off the default path).

### 3.5 Tabu search over the steepest-ascent climb (MEDIUM–LOW priority)

*Merged from three researchers. Listed as future work in `CODE_REVIEW.md` §1
(history §9 item 6).*

**Form in this codebase.** Add a short tabu list (tenure ≈ 6–10) of recently
toggled letter-pairs to `hillclimb()` (`:975`): at a local optimum, take the best
*non-tabu* switch/remove/re-pair move even if it worsens the score, with an
aspiration override when a tabu move would beat the global best. Fully
deterministic (a plus for `-T`); moves and scorer unchanged. ~50 lines.

**Why it helps at 50 chars.** Deterministic ridge-walking across the plateaus
that the single gated re-pair barrier cannot cross, without accepting the random
downhill noise SA does.

**Honest payoff.** Low–medium. The survey consensus is that tabu is *comparable*
to SA and hill-climbing, not superior — and SA is already a peer of `-R` here.
Value is a deterministic alternative and composability, with a moderate chance it
beats SA on the shortest texts precisely because it never accepts random noise.
New knob (tenure).

**Experiment.** `make crackquality SPLIT=1` at L40–120 vs `-A` and `-R -S iq` at
equal `score_iter` budget; sweep tenure ∈ {4,8,12}. Report **worst-case (min over
seeds)** recovery, where tabu's determinism should show best.

### 3.6 Partial plugboard exhaustion — forced first pair (❌ implemented as `--exhaust 1`, measured, DOMINATED)

**Form.** The core diversification device in Ostwald & Weierud (2017): instead of a
random kick, systematically **force each candidate first pair** (`C(26,2)=325`), pin it
(as `-s` pins plugs), run the staged IC→quad climb, keep the best. Deterministic. Built
as the `--exhaust E` option (`--exhaust 1 --score i4q10`), where `E` is the number of
**extra forced** pairs among the free letters, on top of any `-s` pairs (so `-s ABCD
--exhaust 1` forces one more pair beyond the two fixed). `E > 1` explodes combinatorially
(`free!/(2^E E! (free−2E)!)`: ~45k for 2, ~3.5M for 3) so it is an **exploration knob
only**; the measured result below is for `E=1`. It composes with the `--random` kick and
`-R` restarts (each forced combo runs the restart loop), and is **parallel** over the first
forced pair (≤325 units per key, spread across threads like restarts — REDESIGN Part D;
`-T`-independent, TSan-clean, per-worker pin state). (Historically this was the `-S aN` schedule token, where
`N` was the *total* pinned pairs; Part B renamed it `--exhaust E` and switched to *forced*-pair
counting, so the old `-S a1` and `-s ABCD -S a3` become `--exhaust 1` and `-s ABCD --exhaust 1`.)

**It works, and it is dominated.** The basin guarantee is real — on a message where a
single plain climb (`-R 0`) sticks, `--exhaust 1 --score i4q10` recovers exactly. But the
honest test is **matched `score_iter`**, and there it loses badly to spending the same budget
on the tuned greedy restart climb (10-plug messages, 80 trials/cell). Two matchings, both decisive:

| | L50 | L60 | L70 |
|---|---|---|---|
| `--exhaust 1 --score i4q10` (325 climbs, ~1.05M) | 51 / 35 | 58 / 39 | 76 / 65 |
| greedy `-J --score i4q10 --random 10 -R436` (~1.05M) | **69 / 64** | **82 / 79** | **96 / 94** |
| `--exhaust 1 --score i4q10 -J` (325 climbs, ~483k) | 48 / 32 | 61 / 46 | 78 / 65 |
| greedy `-J --score i4q10 --random 10 -R205` (~483k) | **58 / 50** | **77 / 74** | **88 / 84** |
*(mean% / exact%)*

**Why it loses.** Only ~10 of the 325 forced pairs are true plugs, so ~315 climbs launch
from a wrong basin and are wasted; and forcing 1 of 10 plugs barely constrains the climb —
the other 9 must still be found in a *single* forced-seed climb. Meanwhile the greedy arm
spends the same budget on hundreds of full random-restart trajectories, and restarts "never
plateau through 256." The second matching is apples-to-apples (**both arms `-J`**, and
exhaustion even gets *more* climbs — 325 vs 205, because its 1-plug seeds are cheaper than
greedy's 10-plug kick) and greedy still wins by −10 to −28pp exact, so the loss is not a
config artifact. Fails its own bar ("must beat spending that same 325× as raw `-R`") by a
wide margin. Kept as an experimental opt-in (`--exhaust 1`), not recommended.

**Untested variant.** A ranked ~150-pair shortlist (ciphertext × plaintext-language letter
frequency, so the shortlist concentrates on likely-true pairs — see §4.5 for the concrete
influence weight `w(X,Y)=(c_X+p_X)+(c_Y+p_Y)`) would halve the cost, but each forced pair still
gets one weak single climb and only true-plug pairs help — unlikely to close a 20–40pp gap. Not
pursued.

### 3.7 Multi-seed IC basin-hopping (LOW–MEDIUM priority)

**Form in this codebase.** `-S iq` runs *one* IC pre-pass to seed the quad climb.
Instead retain the top-M distinct IC local optima (M short IC climbs from
different tiny seeds, or an M-best beam) and launch a quad climb from each,
keeping the best. Bridges the two shipped ideas (IC pre-pass + restarts) by
starting restarts from IC-good, structurally-diverse boards.

**Honest payoff.** Low–medium. Cheaper than full first-pair exhaustion (§3.6) and
reuses IC infrastructure, but M is small so coverage is far below 325 seeds;
little gain if IC's top optima are near-duplicates. A compute-efficient middle
option, not the top performer.

**Experiment.** Sweep M ∈ {1,2,4,8} in `make crackquality` at fixed compute vs
plain `-S iq -R`; L50/L90 and mean %-correct.

### 3.8 Cross-entropy / EDA plug-pair marginals (LOW priority; overlaps §3.1)

One-liner: maintain a score-weighted 26×26 pair-frequency matrix over elite
restart boards and *sample* each new restart's seed from it (a CE/EDA hybrid over
`optimize_once`). This is the sampling generalization of consensus/fixation
(§3.1) and shares its signal; **build §3.1 first** and only escalate to EDA
sampling if §3.1 wins but leaves headroom. EDAs are competitive-not-dominant and
can prematurely converge, and the per-key elite set is small, so P is likely too
noisy to help — hence the demotion. If tried: smoothing floor on P plus some
uniform-kick restarts as a control; draw from the per-key RNG.

### 3.9 Adaptive restart budget with early-stop (LOW priority; tuning)

**Form in this codebase.** `-R` never plateaus through 256 but spends a fixed
budget on every key, including easy ones. Keep restarting a key only while its
consensus board (§3.1) is still *changing*; once the top plugs stabilize, stop
early and reallocate to keys still churning (natural under `-F`'s shortlist). The
stop decision must be a pure function of per-key state (not wall-clock / thread
interleaving) to preserve `-T` determinism.

**Honest payoff.** Low–medium — a throughput/allocation win, not a new capability:
same recovery for less compute, or more recovery for the same total budget on the
hard tail. Bounded by how uneven per-key difficulty is.

**Experiment.** `make crackquality` at fixed *total* `score_iter` budget across
the keyspace: adaptive vs uniform `-R`.

### 3.10 Not recommended: GA / GRASP / basin-hopping / PSO / ACO

- **Memetic GA with involution-safe crossover** (union of two parents' disjoint
  pairs, conflict-resolved, then `optimize_once`). The one defensible mechanism is
  crossover *assembling complementary partial boards*. But multiple studies find
  GA no better than SA/tabu for substitution/transposition cryptanalysis, the
  repo's own audit ranks GA lowest, and it is the highest-complexity build here.
  The bullet's own gate — *only pursue if an oracle probe confirms two elite boards
  jointly cover the true pairs while neither does alone* — has now been **run
  (❌ MEASURED, REJECTED)**, and it splits the precondition into a part that holds
  and a part that kills the idea. Probe: `--dump-restarts` over 30 × L40 english,
  R=60–100, true rotor key fixed, 10 true plugs; each converged restart board scored
  by exact-pair overlap with the truth (chance = 0.30 true pairs in a random 10-pair
  board).
  - **Coverage precondition — MET.** The correct plugs *are* distributed across the
    population: the **union across restarts reaches ~8/10** true pairs while the best
    single board holds only ~3.7 and no board is complete. So complementary partial
    boards genuinely exist — crossover has raw material.
  - **Selectability — FAILS, and this is decisive.** A GA selects on fitness, and no
    available fitness isolates the true plugs. The per-board correct count barely beats
    chance (mean **0.47 vs 0.30**); `corr(board score, #correct plugs) = +0.20`; the
    single best-**scoring** board yields only **2.5/10** true pairs and the top-10 boards
    by score union to just ~4/10 (vs the 8/10 the full population holds). So board-fitness
    selection captures **half** the available signal at best.
  - **Consensus is *worse*, not the cheaper alternative this bullet assumed.** A per-plug
    vote (frequency across the population, §3.1's idea) gives only **1.1/10** true in its
    top-10 disjoint pick — below even the single best-scoring board — because the climb has
    systematic **decoy attractors** (true plugs appear in ~5.5/100 boards, false ones in
    ~3.6/100, only **1.5× separation**), so frequency amplifies popular *wrong* plugs. This
    is the plug-level echo of the `--restart-tt` finding that the heaviest basins are not the
    best (§6.14).

  So the raw material for crossover is present but **unreachable**: neither board-fitness
  nor consensus can pick the correct plugs out of the noise at short lengths. The bottleneck
  is the **scoring model's information floor**, not the search or recombination mechanism —
  the same wall the finishers (§4.11) and the score-cache (§7.9) hit. GA is squeezed exactly
  like them: where it is needed (short messages) selection is blind; where selection works
  (long messages, quad score sharp) plain restarts already solve it. The only thing that
  would revive GA is a **per-plug truth signal that beats both board-score and vote-frequency**
  — i.e. a sharper scoring/evidence model, which is the scoring frontier, not a GA build.
- **GRASP greedy-randomized construction** and **basin-hopping (SA-quench ILS)**
  are largely subsumed by informed seeding (§4.2) + `-S iq` and by ILS (§3.3) +
  `-A` respectively. List as compositions to try only after the primitives land.
- **PSO / ACO:** do not build. No natural encoding for a 26-letter involution;
  the survey literature does not show them beating SA/tabu/GA; any ACO
  pair-pheromone form collapses into the CE/EDA entry (§3.8), which is cleaner.
- **Learned/offline plug predictor** (train a small model to propose plugs from the
  rotor-only decode). Deliberately omitted, not overlooked: it needs an offline
  training pipeline and a shipped model blob, which does not fit a portable
  single-TU CPU tool with no build-time ML dependency. The tractable, in-TU
  approximations of "learn which plugs to propose" are the assignment seed (§8) and
  the EDA marginals (§3.8); prefer those.

### 3.11 Greedy vs SA on **telegraphic** traffic — ✅ MEASURED: greedy wins outright, no crossover (`eval/eval_sa_vs_greedy.py`)

**The prose "peers" result does not transfer to `-l wehrmacht`.** §3.2 and
`archived/SIMULATED_ANNEALING.md` §15 establish SA as a *peer* of the greedy restart
climb at equal compute, with the README adding a length-dependent crossover ("SA tends
to win the very shortest/hardest lengths"). Both were measured on **English prose**.
Re-measured on **authentic telegraphic plaintext**, greedy wins at every length tested.

**Setup.** Plugboard-recovery tier (true rotor key fixed, plugboard hidden and
recovered), 10-plug boards, matched at 200k `score_iter`, both arms at their shipped
recipes *and* both given `--polish` (it is not blocked with `-A`). Plaintext is pooled
from the 69 authentic decrypts in `eval/` (~6,470 letters) and excerpted at random —
`make crackquality` samples *prose*, which is the wrong substrate for a register.
300 paired trials per length per seed family, two independent families, **3000 paired
trials**; per-trial values retained so the paired difference carries a CI.

| L | mean SA − greedy | 95% CI | SA per-trial win rate |
|---:|---:|---|---:|
| 50 | **−5.7 pp** | [−7.9, −3.4] | 36.5% |
| 60 | **−8.5 pp** | [−11.2, −5.9] | 32.2% |
| 70 | **−8.4 pp** | [−11.6, −5.2] | 34.0% |
| 80 | **−14.2 pp** | [−17.7, −10.7] | 26.8% |
| 90 | **−15.4 pp** | [−19.0, −11.8] | 22.0% |
| **all** | **−10.4 pp** | [−11.8, −9.1] | |

**No crossover.** The prose result's *direction* survives — the gap shrinks steadily
toward the short end and SA's win rate climbs 22% → 36.5% — but SA never reaches
parity in L50–90, let alone overtakes. All ten cells (5 lengths × 2 families) favour
greedy with the CI excluding zero.

**Not compute, not the finisher.** Matching held within ~4% per cell (greedy 197–205k
vs SA 194–200k); greedy's largest edge is +3.5%, and ~75% more restarts buys only
~1–2pp, so that is worth ~0.1pp against 5–15pp gaps. The `--polish` control costs SA
only ~1pp, so the finisher is not the mechanism either.

**Part of the gap is structural, not algorithmic.** `-A` consumes only the *last*
`--score` stage's plug cap and seeds itself with a **hardcoded IC pre-pass**, so it
cannot use greedy's mono pre-pass — measured at +2.7/+3.9pp over an IC pre-pass and
~+16pp over none. Comparing greedy's `i4a10` (IC-seeded) against SA narrows the gap
from ~14pp to −12.2/−8.1pp. Greedy still wins, by less. **This was the actionable
lever, and it has since been built and measured — see below.**

**Follow-up — giving SA the mono pre-pass: ✅ a real +2.3pp win, but it does not
change the verdict (`ENIGMA_SA_STAGES`).** Confirmed first that the schedule really is
inert under `-A`: `--score a10`, `m4a10` and `i4a10` produce **identical output and
identical `score_iter`**. The probe makes `anneal_once` run the leading `--score`
stages at their own caps instead of the built-in IC pre-pass (env-gated; default path
byte-identical under g++ and clang). Re-run over the *same* paired instances as above
(3000 trials; the greedy column reproduced exactly, 0/3000 mismatches, which validates
the pairing):

| L | SA + IC (shipped) | SA + mono | delta | 95% CI |
|---:|---:|---:|---:|---|
| 50 | 12.2 | 14.2 | **+2.1 pp** | [+0.2, +4.0] |
| 60 | 17.1 | 19.0 | +1.9 pp | [−0.6, +4.3] |
| 70 | 23.3 | 23.9 | +0.6 pp | [−2.2, +3.5] |
| 80 | 31.1 | 32.1 | +1.0 pp | [−2.2, +4.2] |
| 90 | 36.7 | 42.6 | **+5.8 pp** | [+2.1, +9.6] |
| **all** | | | **+2.3 pp** | **[+1.0, +3.6]** |

Pooled it is a genuine win (CI excludes zero) and every length is directionally
positive, though only L50 and L90 reach significance individually. The size matches
the ~3–4pp predicted from greedy's own IC-vs-mono comparison, which is a useful
consistency check on the structural diagnosis.

**But it does not overturn the result.** The gap to greedy narrows at every length —
L50 −5.7→−3.6, L60 −8.5→−6.6, L70 −8.4→−7.8, L80 −14.2→−13.2, L90 −15.4→−9.6 — and
**every one still excludes zero**. So roughly a fifth of SA's ~10.4pp average deficit
was the missing pre-pass; the remaining ~8pp is the search itself. Greedy still wins
every length in L50–90.

**Prose control — ❌ DO NOT PROMOTE TO DEFAULT. The lever is register-dependent.**
The wehrmacht win alone was not enough to ship it: the IC pre-pass it replaces was
tuned on prose, so prose is where a regression would appear. Re-run on the corpora
`tests/crack_quality.py` samples (L50/70/90, 300 paired trials × 2 families each):

| substrate | mono − IC pre-pass | n | verdict |
|---|---:|---:|---|
| wehrmacht (telegraphic) | **+2.3 pp** [+1.0, +3.6] | 3000 | helps |
| english prose | **−2.6 pp** [−4.4, −0.7] | 1800 | **hurts** |
| german prose | +0.1 pp [−1.5, +1.8] | 1800 | neutral |

English is negative at all three lengths (−2.5/−2.9/−2.3) and the pooled CI excludes
zero, so it is a real regression, not noise. The shape mirrors the tables themselves
(§6.6: +20.9pp on real traffic, −10.2pp on prose) — what suits telegraphic orthography
does not suit prose. So `ENIGMA_SA_STAGES` **stays an off-by-default probe**; making it
the default would trade a 2.3pp telegraphic gain for a 2.6pp English loss.

**And it has no operational value even where it wins.** The +2.3pp lifts SA from
−10.4pp to roughly −8pp against greedy — it improves the *losing* arm without making it
win at any length. A user on telegraphic traffic should run greedy, not SA with a mono
pre-pass. The finding's worth is diagnostic: it confirms that the structural pre-pass
handicap explains about a fifth of SA's deficit, and that the rest is genuinely the
search. No CLI flag is warranted (cf. the option-surface cleanup in 2.0.0 — a knob that
never produces the best answer is exactly the kind that was removed).

*Prose caveat.* The prose corpora are ~477 letters against wehrmacht's 6473, so at L90
only ~390 distinct offsets exist and 300 trials overlap heavily. Both arms see identical
excerpts, so the paired comparison holds, but the prose CIs are optimistic relative to
the wehrmacht ones. English prose at L90 is also near ceiling (93.1%), which compresses
any effect there.

**Tuning was a no-op — both shipped defaults are already right for this register.**
A per-arm sweep (stage 1) found greedy's `m4a10`/kick-10 in the top group and SA's
best split reproducing the shipped `-A 12000 -R 12` (`A=11439 R=12`). Two negative
results worth recording: greedy's top three configs **reorder between seed families**
(~2pp spread), and SA's depth/restart split is **flat across R=2–24** (best split moved
R=12 → R=6 between families) — only an un-restarted `R=1` anneal is consistently worst.
Picking a winner from one family would be fitting noise; an 8-trial pilot did exactly
that and crowned two configs that inverted at n=300.

**Two calibration traps, for anyone re-running this.** (1) Cost per SA restart is
`A + ~4900` — the pre-pass and quench are paid regardless of depth — so cost is **not**
linear in `A`. A naive scaler chasing a budget drives `A → 1`, which is a greedy climb
wearing an `-A` flag, and it then "wins". Enforce a floor and report over-restarted
splits as *infeasible*. (2) An analytic two-point cost fit fails the other way: SA does
not always consume its full move budget, which flattens the slope and inflates the
intercept, and the bad seed rejects genuinely affordable splits (it rejected `R=12`,
the shipped one). Let measurement decide feasibility.

**Limits.** One budget (200k) at one plug count (10); the flat-split finding makes a
budget-dependent reversal unlikely but it is untested. Per-cell *magnitudes* are
draw-dependent (family 1 read −19.1pp at L80 where family 2 read −9.4pp), so the pooled
per-length rows — not any single cell — are the result.

---

## 4. Move set & informed seeding

### 4.1 Guided (iterated-local-search) perturbation instead of a blind kick (MEDIUM priority)

**Form in this codebase.** The restart kick is a fixed random `k=8` pairs
(`perturb_steckerbrett`, `:1166`). Replace/augment with an *informed* kick: at
convergence, score each set plug's marginal contribution (Δ if removed) and each
unplugged letter's best-partner marginal (Δ if added) — quantities the
steepest-ascent pass already computes — then perturb by removing the
**lowest-contribution** (most likely spurious) plugs and adding high-marginal
candidates, rather than random pairs. Deterministic on the per-key RNG.

**Why it helps at 50 chars.** Several true plugs are only weakly supported by
sparse statistics; a uniform random kick as often destroys a correct plug as a
wrong one. Targeting the weakest plugs preserves the correct partial board across
restarts, so restarts *accumulate* progress instead of resampling.

**Honest payoff.** Medium — a low-risk refinement of an already-tuned mechanism
(the `k=8` kick was swept and won), so gains are incremental, most valuable at
high `-R` where the uniform kick wastes restarts re-breaking correct plugs.

**Experiment.** `make crackquality` at high `-R` (64/128/256) comparing guided vs
random kick on the shortest lengths; hypothesis is guided wins more at large `-R`
and never loses.

### 4.2 Informed single-plug seeding (greedy / GRASP; refinement of history §9 item 4)

**Form in this codebase.** Before the climb, score all 325 single plugs once
against the cheap model (IC, already used by `-S iq`) from the identity board.
Two uses: (1) greedily accept the top non-conflicting single-plug deltas up to ~6
pairs as the start board (the never-built "greedy plug-by-plug seed",
`CODE_REVIEW.md` §1 / history §9 item 4); (2) use the ranking as **move ordering**
so the scan visits promising pairs first (enabling a cheaper first-improvement
variant, §7.2). A GRASP variant randomizes the pick within a top-α restricted
candidate list, giving each restart a *different* strong seed. `O(325)` cheap-model
sweep per key — negligible vs the climb.

**Why it helps at 50 chars.** The identity-board single-plug landscape is the
smoothest the surface ever is (the `-S iq` rationale); ranking on it gives a
data-driven prior on which letters are "active" and concentrates the limited
signal.

**Honest payoff.** Low–medium, and explicitly *adjacent to an already-open idea*:
`-S iq` already does something similar implicitly, so the increment is per-restart
seed diversity. Cheap enough to be worth measuring; unlikely a step change alone;
stacks with consensus (§3.1).

**Experiment.** `make crackquality` at L40–80: seeded vs identity start at equal
climb budget; α ∈ {1 (deterministic greedy control), 3, 5}; confirm via `SPLIT=1`
it reduces search failures.

### 4.3 Evidence-restricted move set (MEDIUM priority; novel, exact-speedup flavor)

*Merged from two researchers (identifiability restriction + provably-inert-move
pruning).*

**Form in this codebase.** For a ~50-char message far fewer than 26 letters carry
signal. A switch on `{a,b}` can only change the decode if `a` or `b` appears on
the ciphertext-input side (`ct[i]`, fixed and precomputable) or on the current
output side (`rows[i][steck[ct[i]]]`, trackable as a small per-board set). Two
uses:

1. **Exact pruning (zero quality change):** skip candidate moves whose *both*
   endpoints are absent from both sets — their score delta is provably zero. This
   shrinks the 325-move grid at no risk. Guard with an assert-mode full scan.
   (§4.6 generalizes this from the `k=0` special case to a graded `|Δscore|`
   bound `≤ n·k·(vmax−vmin)`, so the same idea also *ranks* and *soft-prunes*.)
2. **Soft down-weighting (quality change):** on very short messages, deprioritize
   or refuse *new* plugs on letters occurring 0–1 times in the ciphertext, since
   they are essentially unidentifiable and a classic source of spurious plugs.

**Why it helps at 50 chars.** The fraction of provably-inert moves is largest
exactly in the short regime; removing them is a free throughput win. The soft
form attacks spurious-plug local optima directly.

**Honest payoff.** The exact-pruning half is a clean, risk-free constant-factor
speedup that shrinks as length grows. The soft-restriction half is medium and
must be handled carefully: **a plug letter also appears in the *decoded* text**,
so a letter absent from the ciphertext can still be a real plaintext partner —
frequency must be taken over *both* sides (or iterated), or the restriction
wrongly forbids real plugs. Make it a soft weight with a wide threshold, gated
below a length cutoff.

**Experiment.** Exact pruning: `make bench hillclimb` (fewer moves) + `make
crackquality` must be **byte-identical**. Soft restriction: `make crackquality`
at L40–70 (restricted vs full) with a no-regression check at L90–120 where all
plugs are identifiable.

### 4.4 Surrogate-biased SA proposals (LOW–MEDIUM priority)

**Form in this codebase.** In `anneal_once` (`:1378`), replace uniform
`random_pair` proposals with a distribution biased toward high-surrogate-benefit
pairs (from §4.2's ranking or a running monogram-delta), refreshed periodically.
Keep Metropolis acceptance (detailed balance broken, acceptable for optimization).
Draw from the per-key RNG.

**Honest payoff.** Medium, with a real risk: biasing SA into a basin can destroy
the exploration that makes SA a peer of greedy in the first place. Needs the bias
strength tuned (partial, not hard). Overlaps with the guided-kick idea for the
greedy path (§4.1).

**Experiment.** `make crackquality` at fixed `-A` budget: guided vs uniform;
sweep bias strength; confirm it does not merely collapse SA toward the greedy
result.

### 4.5 Influence-weighted plug selection — ciphertext-exact + plaintext-prior (MEDIUM priority; sharpens §3.6 / §4.1 / §4.3; PARTIALLY EXAMINED, see §4.6)

> One of the three applications below (climb move ordering, item 3 below / §4.6) was built,
> measured, and **rejected** as `--infl-order` — which this repository has since **removed from
> the CLI entirely** (dominated by `-J`; see `CLAUDE.md`'s removed-options list). The other two
> applications (focused `--exhaust` restriction, influence-weighted `--random` kick) were never
> built or measured — they remain genuinely open.

**The idea (Ostwald & Weierud).** Plugs are not equally worth searching: a plug on a
*frequent* letter rewrites many message positions, so getting it right — or ruling it out —
resolves far more of the plaintext than a plug on a rare letter. Weierud's method concentrates
the exhaustive stage on plugs touching high-frequency letters for exactly this reason. This
section turns "influence" into a concrete, computable weight and — the important part — keeps
the **exact** contribution (ciphertext) separate from the **estimated** one (plaintext), because
they are epistemically different.

**Where a plug acts.** Decryption applies the plugboard `S` twice per position:
`c --S--> a=S(c) --R--> b=R(a) --S--> out=S(b)` (R = rotors+reflector, fixed at that position).
Toggling plug (X,Y) changes `S` only at entries X and Y, so a position's output changes iff
**either** application is hit:
- **input event** `c ∈ {X,Y}` — the ciphertext letter enters the plug (this always flips the
  output, since R and S are permutations);
- **output event** `b ∈ {X,Y}` — the pre-output-plug letter is X or Y.

**Two probabilities — one exact, one prior.** For the message in hand:
- `a = c_X + c_Y`, the **exact** ciphertext fractions (counted directly; zero model risk). A
  letter absent from the ciphertext contributes *nothing* on the input side — a hard, certain
  elimination.
- `b = p_X + p_Y`, the plaintext fractions. `b` is the pre-output-plug letter and with a
  roughly-correct board `b ≈ plaintext`, so this is the plaintext-language **prior** — the
  estimated, model-risky part (drifts off in telegraphic/short text).

**The estimate.** `b = R(S(c))` and R steps every character, so across the message `c` and `b`
are ≈ independent; the union of two independent events gives the influenced fraction:

```
influence(X,Y) ≈ 1 − (1−a)(1−b) = a + b − ab      a = c_X+c_Y (exact),  b = p_X+p_Y (prior)
```

expected count = `n·(a + b − ab)`. The union form is the honest one: bounded in [0,1], degrades
gracefully (letters absent from the ciphertext → influence ≈ `b`, output side alone;
plaintext-rare letters → influence ≈ `a`, input side alone), and `−ab` discounts the
double-counting where both are large.

**For ranking it collapses to one cheap table.** `−ab` is second-order, so to *order* candidate
plugs define a per-letter influence `ℓ(L) = c_L + p_L` and weight a plug `w(X,Y) = ℓ(X) + ℓ(Y)`
— one 26-entry table summed two at a time. Use the full `1−(1−a)(1−b)` only when an actual
fraction is wanted. Magnitudes: a uniform plug ≈ 15% of positions (`a=b=2/26`); an E-plug on a
common ciphertext letter ≈ 26%; a plug on two ciphertext-*absent* letters, one being E, ≈ 15%
(entirely via the output side — neither term is droppable).

**Why the ciphertext term is not "just flat."** The *expected* ciphertext distribution is flat,
but the *realized* histogram of the one message in hand is not, and it is **exact** — it is the
plaintext term that is "average statistics" about an unknown. So the ciphertext side adds
message-specific, zero-model-risk information the plaintext prior structurally cannot, and it can
definitively zero out plugs on absent letters (plentiful at short lengths). In the hard short
regime the two terms have comparable *contrast* and the ciphertext one carries no model risk, so
weight it **at least as heavily** as the plaintext prior — not as a minor add-on.

**Where to apply it (priority order).**
1. **Focused `--exhaust` (best fit).** §3.6 is dominated largely because it forces *all* 325
   first pairs, of which only ~10 are true (~315 wasted climbs). Restricting the forced set to
   the top-influence pairs is exactly §3.6's "untested ranked-shortlist variant," now made
   concrete: rank by `w(X,Y)`, exhaust only the top-M. It concentrates the exhaustive budget where
   a correct plug resolves the most plaintext and directly tames the `E≥2` blow-up — the most
   promising home, and could move §3.6's verdict.
2. **Influence-weighted kick (§4.1).** Bias `--random` to draw pairs from the influence
   distribution rather than uniform. Because the kick stays *stochastic*, restarts still get
   different plug sets — so, unlike the static ordering below, it should keep restart diversity
   *if the weighting is gentle*; a too-peaked weight re-injects the same E-plugs every restart and
   slides back toward the §7.2 failure mode, so weight strength is a knob with a sweet spot.
3. **Quantitative form of §4.3's soft down-weighting.** §4.3 flagged that plug frequency "must be
   taken over *both* sides"; `w(X,Y) = (c_X+p_X)+(c_Y+p_Y)` *is* that both-sides quantity, so the
   §4.3 soft restriction becomes "down-weight low-`w` plugs" with a principled weight instead of
   an ad-hoc threshold.

**The one overlap to respect (§7.2).** A **static** informed move order *by ciphertext letter
frequency*, applied to the deterministic climb, was built and **rejected** (collapsed restart
diversity, −4–5pp). Influence weighting is not that, on two axes: it is (a) a *plaintext*-informed
weight (better motivated — influence ∝ positions affected), and (b) aimed at *exhaust/kick*, not
the per-restart climb order. The kick application stays stochastic (diversity-preserving); the
exhaust application is systematic, not a per-restart order at all. So it is a distinct idea — but
the §7.2 lesson bounds how hard the kick may be biased.

**Honest payoff & cautions.** Medium, best at `--exhaust`. It is a **mean-%-correct-friendly**
heuristic: the plugs it de-prioritizes (two rare letters) are the ones that barely move the
plaintext — good for the graded metric, but it *caps exact recovery* (a true Q–J plug is never
reached under a hard restriction). And the ciphertext histogram is noisiest at the short lengths
we care about (few counts), whereas the plaintext prior is length-robust — so lean on the
language `p_L` for the prior part and treat the exact `c_L` as the message-specific correction.
All of it is nearly free (two 26-entry histograms per key).

**Experiment.** `eval/` (per-run) and `make crackquality`, matched `score_iter`, L40–90, 10
plugs: (1) focused `--exhaust` over the top-M influence pairs vs the full 325 and vs greedy `-R`
at equal budget; (2) influence-weighted `--random` vs uniform kick at high `-R`, sweeping the
weight strength and checking with `DIVERSITY=1` (restart basin-collapse) that a gentle weight
does *not* collapse diversity. Judge on mean %-correct per the harness guidance.

### 4.6 Exact board-state influence — the `|Δscore|` bound, prune before order (refines §4.5) — ⚠️ PARTIALLY MEASURED

Two of the three proposed uses below were built and measured to a clear verdict (soft/exact
prune: ❌ dead end at realistic lengths; climb move ordering: ❌ built as `--infl-order`, dominated
by `-J`, since **removed from the CLI**). The third (focused `--exhaust` restriction by influence)
was never built or tested — same gap as §4.5's item 1, still open.

**Form.** §4.5 weights a plug by `a + b − ab` with `a = c_X+c_Y` exact and `b = p_X+p_Y` a
*language prior*. But that prior is only needed *before* a decrypt exists. **Inside the climb
every candidate board already has a potential plaintext**, so replace `b` with the exact letter
counts of the *current* decrypt: both halves become exact and board-specific, recomputed as the
board evolves. For a plug on two currently-free letters the set of positions it can change is
exactly

```
infl(X,Y) = |{p : ct_p ∈ {X,Y}}  ∪  {p : pt_p ∈ {X,Y}}|
```

(add move, free letters; for a *move*/*merge* test the pre-output-plug set `rows[i][steck[ct[i]]]`
— equivalently `S⁻¹` of the plaintext — since the "off" state no longer maps X→X). The
doubly-absent case `infl = 0` is §4.3's provably-inert move (exact-zero delta), now the `k=0` end
of a graded quantity rather than a special case.

**Influence bounds the delta — so the ranking has a proof under it.** An additive n-gram score is
a sum over windows; a move that changes the decode at `k = infl(X,Y)` positions perturbs at most
`n·k` windows (`n` = model order), each a bounded log-prob in the uint8 `[vmin, vmax]` range:

```
|Δscore(X,Y)|  ≤  n · infl(X,Y) · (vmax − vmin)
```

A low-influence move **provably cannot** be a large improvement. This upgrades "rank by
importance" from a heuristic order into a **branch-and-bound-style prune with a quantified
worst-case loss** (the lever §7.4 gestures at): sort by the bound; any move whose bound is below
the current best improvement can be skipped *exactly*, or below a tolerance `ε` skipped *softly*
for a loss bounded by `ε`.

**Prune before order — the two uses split on the §7.2 fault line.**
- **Soft prune / down-weight (recommended first).** Skip or defer the low-`infl` tail. It is a
  *throughput filter with a bounded sacrifice*, not a per-restart move order, so §7.2's
  diversity-collapse argument does not apply, and the `|Δ|` bound states exactly what is given up.
  Cheapest safe win; `ε=0` is pure inert-move skipping (byte-identical).
- **Focused `--exhaust`.** Rank the 325 first pairs by `infl`, force only the top-M — systematic,
  not a per-restart order, so §7.2-immune (this is §4.5 point 1 with the exact `infl`).
- **Climb move ordering — the trap.** Ordering the per-move sweep by `infl` is where §7.2 bites:
  the *ciphertext half* (`ct_X+ct_Y`) is **fixed across the whole search and identical every
  restart**, and ordering by ciphertext letter frequency is exactly the static informed order
  §7.2 built and **rejected** (−4–5pp, diversity collapse). The *plaintext half* is what varies
  per restart / per step and could rescue it — but on a wrong board the decrypt is ~flat, so that
  half is weakest exactly at the start of the climb where the order matters, and only sharpens
  once the board is nearly solved.

**Why it is still worth testing (not just a §7.2 rerun).** (1) It is **per-step dynamic** —
recomputed as the decrypt changes — which neither §7.2's static order nor `-J`'s once-per-restart
order does; (2) it is **nearly free** (two histograms), against `-J`'s +24% full-move pre-scan.
The trade is signal quality: `-J` orders by *measured* score-delta (strong, expensive); `infl`
orders by a *bound* on `|Δ|` (weaker — it flags what could move the score in *either* direction,
not what helps — but almost free). Honest prior: as a hard *order* it likely underperforms `-J`
(bound ≠ benefit, plus the ct-half diversity risk); as a *prune* it is a clean, safe throughput
gain. Do the prune first.

**Measured — the prune is a dead end at our lengths (offline, english, 40 runs/length). ❌**
Inert-move fraction (`infl=0`, both letters absent from the ciphertext *and* the current-board
decrypt) and low-`infl` fractions, bracketed by a start-of-climb (empty board) and a solved board:

| L | inert % (`infl=0`) | `infl≤2` % | `infl≤4` % |
|---|---|---|---|
| 40 | 0.2–0.6 | 5–9 | 26–33 |
| 50 | 0.0–0.2 | 1–4 | 11–18 |
| 60 | ~0 | 0.6–1.7 | 4.5–10 |
| 70 | ~0 | 0.1–0.5 | 1–5 |
| 90 | ~0 | ~0 | 0.1–1 |

Two findings kill the exact prune and demote the soft one:
1. **The `ε=0` exact prune is worthless from L50 up** (~0 inert moves; ≤2 pairs even at L40). The
   union of the ~flat ciphertext and ~flat wrong-board decrypt covers ~25 of 26 letters
   (`|active|≈25`), so almost nothing is doubly-absent. The pruning fires only at L≲30–40.
2. **The `|Δ|` bound is rigorous but far too loose to justify a *bounded* soft prune.** The
   per-window quad range is ~7 log10 units, so even a `k=1` move has bound `≈ 4·7 ≈ 28` log10 —
   enormous next to real per-move improvements (~1 log10). So the bound is *tight only at `k=0`*;
   for any `k≥1` no move is provably skippable for a small `ε`. A prune that drops `infl≤4`
   (~15% of moves at L50, ~33% at L40, ~0 by L90) is therefore a **heuristic**, not the
   bounded-loss guarantee the `≤ n·k·(vmax−vmin)` framing suggested — the loose constant is the
   catch. Net: no rigorous prune lever at L50, and only a modest heuristic short-length one, so the
   prune is not pursued.

**Measured — the influence order underperforms `-J`, as predicted (built as `--infl-order`;
matched compute, 4 languages, L40–90, 2 seeds, 120 runs/cell). ❌** `--infl-order` ranks the
first-improvement sweep by `w(a,b)=ct_count[a]+ct_count[b]+pt_count[a]+pt_count[b]` (two 26-bin
histograms, ~free) instead of `-J`'s measured score-delta. At ~55k `score_iter` (`-J -R24` ≈
`--infl-order -R30` ≈ `-I -R32`, pooled mean %-correct):

| L | `-J` R24 | `--infl-order` R30 | `-I` R32 | infl−`J` (±SE) |
|---|---|---|---|---|
| 40 | 25.3 | 24.2 | 19.7 | −1.1 ± 2.0 |
| 50 | 37.5 | 41.5 | 31.7 | +4.0 ± 2.6 |
| 60 | 59.7 | 53.9 | 45.4 | −5.9 ± 2.7 |
| 70 | 71.3 | 66.0 | 60.5 | −5.3 ± 2.7 |
| 90 | 90.0 | 85.7 | 78.6 | −4.3 ± 1.9 |

- **Confirms the prior:** influence-order **ties `-J` through the short range and loses from
  L60 up.** A dense L40–60 fill-in (L45/L55 added, `infl−J`: −1.1, −0.1, +4.0, −0.1, −5.9 at
  40/45/50/55/60) shows the earlier L50 "+4" was an **isolated noise spike** — bracketed by ties
  at L45 and L55 — not a short-length win. So the real picture is a **tie for L40–55, then a
  clean loss from L60 up** (−4 to −6pp at L60–90; `infl_order_short.png`). `-J`'s score-delta
  order beats influence's cheaper bound-based order wherever there is enough signal to rank on,
  and that signal sharpens with length — so the two only tie where the message is too short for
  `-J`'s order to help, and `-J` pulls away as soon as it can. The per-step-dynamic / near-free
  angles do not overcome the weaker ordering signal.
- **But influence-order clearly beats plain `-I`** everywhere (+5 to +10pp): the informed order
  *is* genuinely useful — a cheap, free upgrade over lexicographic — just dominated by `-J`.
- **Verdict:** `-J` stays the recommended order. `--infl-order` was kept as an experimental,
  documented-dominated opt-in for a time (bench-neutral, default path untouched) — it has since
  been **removed from the CLI entirely**, since a dominated-with-no-niche-use-case flag is dead
  weight rather than a useful diagnostic. The measurements above remain the record of why it
  lost.
  (Methodology note: the first pass was polluted by stale `i4q10.R24.J`/`R32.I` rows from earlier
  matched-compute runs sharing those labels — filter the clean grid by `git_sha`, or use fresh
  `config_label`s, for a paired read.)

### 4.7 3-plug re-pair barrier cross (`--repair3`) — ❌ MEASURED, DOMINATED

`try_repair` generalised from two plugs to three: at convergence, once the cheap toggle
climb **and** the 2-plug `try_repair` have both stalled, `try_repair_3()` rematches three
existing plugs (six letters) into a different pairing — the 8 genuine count-neutral
reshufflings that share no pair with the original — and keeps the best strictly-improving
one. A deeper local-optima escape than the single toggle or the 2-plug re-pair. Shipped
opt-in (`--repair3`, default off, baseline byte-identical; needs `-c`).

**Measured at matched *compute*, it loses.** Recovery-vs-`score_iter` curves (baseline
interpolated to `--repair3`'s actual `score_iter`, so every comparison is at equal compute;
4 languages × L40–70 × `-R {16,24,40,64}` × 2 seeds, `results-2026*.tsv`): **pooled −2.06pp**
mean %-correct vs the baseline at equal `score_iter` (11 of 16 cells negative). The 3-plug
scan costs ~960 `score_iter` per converged climb (~1.5× per-climb), and at matched compute
that budget buys **more restarts, which win** — the loss is worst at the highest compute
(`--repair3 @ R40` consistently −7…−11pp, where extra restarts help most) and only
occasionally positive at the very lowest. Single cells swing ±15pp (noise); an early
one-cell +17.6pp (german L50) did not survive the full grid (−7.8pp there).

**Verdict — same as `--exhaust` (§3.6) and the IC+mono portfolio (§6.10): a deeper
barrier-cross is dominated by spending the compute on diversity instead.** The mechanism is
correct and clean (byte-identical default, `-Werror` g++/clang++, clang-tidy/ASan/UBSan
clean, 182 tests pass), kept as a documented-dominated opt-in, not recommended.

### 4.8 "Fix-and-finish" — pin a converged board, free the suspects, re-climb — ❌ MEASURED, DOMINATED

The idea motivated by the convexity / basin-gap story (§6.11–6.13): once a restart climb has
converged to a board that is *mostly* right (the outlier-score, near-solution regime — ~50%+
correct letters, 5–6+ correct plugs), don't throw it away and restart blind. Instead **fix**
the plugs that look trustworthy, **free** the few that look wrong, and re-climb from that
seed — a targeted finish that should need far less compute than another full restart. Tested
as a Python prototype over `tests/eval.py` (never shipped to `enigma.cc`), at **matched
`score_iter`** (Phase-A restarts + Phase-B finish vs the same total spent purely on restarts;
english+german × L50–70, 60 problems/cell). Three constructions, each fixing a different
mechanism, all lose:

1. **Consensus pins** (fix plugs shared across the top converged boards): **Δ −4.3 … −11.3pp**.
   At short lengths the top boards are *junk that agrees on junk* (the basin-gap prediction,
   §6.11) — 19–47% of the "consensus" pins are wrong, poisoning the finish.
2. **Quadgram-crib pins** (per-position quad badness of the best decrypt aggregated onto plug
   contacts; fix the low-badness plugs, free the high-badness ones): **Δ −0.4 … −2.6pp** — a
   break-even improvement, but the crib still runs on a junk board at the lengths where help
   is wanted (pins 13–41% true at L50).
3. **Quad-crib + adaptive gate + K=1** — finish *only* on outlier-score boards (a per-problem
   MAD gate on the Phase-A restart-score distribution, so the finish fires exactly where the
   board is genuinely near-solution), freeing just the single most-suspect plug: still
   **negative even on the gated subset** — gated-only Δ **+0.1 / −2.9 / −0.9** (english
   L50/60/70), **−5.0 / −11.5 / −2.9** (german), with fixed-pin accuracy a healthy **72–94%
   true** on that subset. The gate worked — it isolated boards averaging 91% correct — and the
   finish *still* lost to more restarts.

**Verdict — fix-and-finish is dominated, even in the near-solution regime it was built for.**
Two mechanisms sink it, both already mapped this file: (a) even on gated boards the pins are
only 72–94% true, so the crib fixes *some* wrong plugs; and (b) a fixed plug is
**irreversible** (`-s` held during the climb) — one mis-pinned plug permanently locks the
finish into a wrong basin, whereas a plain restart keeps the full 325-move freedom and can
wander out. Trading that freedom for a narrow search that is only as good as its crib is a bad
trade *precisely because* the score gradient near junk is weak (§6.11–6.13): the crib cannot
reliably separate a wrong plug from a right one at exactly the short lengths where the finish
would need to. Three independent constructions all land negative — a greedy restart climb
remains the thing to beat, and pinning a few plugs from a converged board never earns back the
compute it costs. Not shipped.

### 4.9 2-plug re-pair (`try_repair`) still pays at short lengths, matched compute — ✅ MEASURED (`eval/`)

The always-on 2-plug `try_repair` barrier cross (§9 item 7 in `archived/CODE_REVIEW_HISTORY.md`)
was originally validated only at **long** lengths (L140–250), where its gated convergence scan
is negligible ("~zero cost"). At **short** lengths a climb converges fast, so that fixed scan is
a *larger* fraction of the work — measured **~15% of `score_iter`** at L40–70 (e.g. 2321 vs 2001
at `-R 1`). So its value there had to be re-checked at *matched compute*, not assumed. The
`--no-repair` flag makes this a clean one-binary A/B (default vs the move disabled;
`eval/repair2_matched.py`, `eval/plots/repair2_matched.png`).

**It wins, and the win grows with budget.** en+de, L40/55/70, 10 plugs, 100/cell, `-J -S i4q10`,
`-R {1,2,4,8,16}`, with the no-repair curve interpolated to the with-repair `score_iter` so the
~15% extra cost is charged against it:

| `score_iter` | with `try_repair` | `--no-repair` | Δ (matched) |
|---:|---:|---:|---:|
| 2,321 | 12.8 | 12.5 | **+0.26** |
| 4,597 | 16.1 | 15.9 | +0.21 |
| 9,166 | 22.2 | 21.1 | +1.08 |
| 18,200 | 27.4 | 26.3 | +1.10 |
| 36,422 | 35.7 | 32.9 | **+2.78** |

Mean %-correct Δ **+0.2 → +2.8 pp**, positive at every budget and rising with restarts (exact
recovery likewise, e.g. 24.5% vs 20.7% at `-R 16`). This is the **first clearly-positive
matched-compute barrier-cross in the whole §3.6/§4.7/§4.8 string** — and it is exactly the
opposite verdict from the *3-plug* `--repair3` (§4.7, dominated). The reason the 2-plug move pays
where the 3-plug doesn't: `try_repair` fires at **every** convergence throughout the climb, so it
is a broad, cheap local-optima escape that lifts the whole distribution of climb endpoints — not
a rare, expensive last-resort. That the Δ *grows* with `-R` fits: more restarts → more
convergences → more barrier-crosses. So the 2-plug re-pair is the last worthwhile move rung at
short lengths too, not just the long ones where it was first validated. Reproduce:
`python3 eval/repair2_matched.py`.

### 4.10 Directed plug repair via quadgram "gain" (gain-cascade) — ✅ SHIPPED as `--cascade` (§4.11 ships the best-board finisher, `--polish`)

§6.14 showed the residual near-solution failures are re-pairing *tangles* a greedy single-toggle
climb can't cross. This explores a **directed** finisher: use the quad model to point at *which*
plug is wrong and fix it, instead of blind restarts. The mechanism was built up from Enigma
structure; each step below is a measured improvement (Python prototype over `tests/eval.py` and
real tool-converged boards from `eval/results*.tsv`; exact rotor core extracted from the tool via
26 empty-plugboard decodes of `x*n` — Enigma stepping is content-independent).

**The pipeline.**
1. **Per-position gain** — for the current decode, the best quad-score improvement obtainable by
   changing one output letter, and to what (`bx`); only the ≤4 covering quadgrams change, so it's
   `O(n·26·4)` lookups.
2. **Dual (exit + entry) generation** — a position's output can be corrected two ways, because the
   plugboard sits on both sides of the rotors: **exit** re-plug `{S[pt[j]], bx}`, or **entry**
   re-plug `{ct[j], core_j[S[bx]]}` (reciprocal). Exit-only generation covers a correct plug 75–95%
   on synthetic boards; adding the entry lever lifts it to **97–100%**.
3. **No-self-encryption prune** — Enigma never maps a letter to itself (`P = S∘C∘S`, C
   fixed-point-free), so any suggestion `bx == ct[j]` is impossible and pruned for free.
4. **Full-plug (input-aware) ranking** — rank candidates by the *whole-message* re-decode Δ (both
   contacts, freed partners), not the exit-only vote. Given coverage this ranks the correct plug at
   #1 **~95%** on synthetic boards (the raw vote alone ~40%). Ranking is essentially solved;
   generation is the bottleneck.

**The real-board reality checks** (`eval/results*.tsv`, L40–70, 10 plugs):
- **Near-solution boards are rare** — 168k converged boards: 32% solved, and of the non-exact,
  **76% are deep junk (0–20% correct)**, only **~4.5% near-solution** (the basin gap, §6.13).
- **Many "almost done" boards are scoring failures** — 86% of the 90–100% bucket have
  `recovered_score ≥ true_score`: the converged board scores *at least as high as the truth*, so
  the missing plug can't be found by any score-based method (the information floor, not a search
  failure). The mid-range (50–80%) is 95%+ genuine search failures — fixable.
- On fixable search-failure boards, single-plug hit@1 is **44–67%**.

**The pair-coverage wall, and the cascade that breaks it (the key finding).** A converged board is
a local optimum *for single moves*, so the correct fix is usually 2 plugs *together*. But scoring
2-plug **combos** is capped at **32%** by pair-coverage — on a 2-plug tangle the two wrong plugs
corrupt overlapping positions, so each masks the other's gain signal; both are rarely in the
shortlist at once. The **cascade** sidesteps it: apply the one visible plug *even though it's
downhill* (Δ<0), which **un-masks** the second, then accept the pair only if the net is positive.
Correct-pair rate **32% → 62%**. (Removal candidates — freeing a spuriously-plugged letter — were
added and measured **no effect**: forming a plug already ejects the old partner, so "add-with-eject"
subsumes pure removal.)

**The reclimb amplifies (the payoff).** The cascade only needs to *cross the barrier*; once a
tangle is fixed and the score jumps, the ordinary climb resolves the rest. On real near-solution
search-failure boards with **2–4** wrong plugs, **cascade-fix + reclimb solves 53%** (vs **8%**
for reclimb alone), lifting mean correct-plugs **6.8 → 8.6**.

**Now implemented in-tool as the opt-in `--cascade` flag** (renamed from the original `--gainfix`;
byte-identical default; needs `-c`; quad-only). The cascade fires at each quad-climb convergence and,
on success, hands the improved board back to the cheap climb to finish — the reclimb amplification
comes for free from the existing `do/while(progress)` loop. It reuses the tool's precomputed rotor
core (`rows[j]`) so the entry-side (reciprocal) candidate is machine-exact, and is `-T`-deterministic
(no RNG, fixed candidate order). It is **gated by a near-solution per-symbol score threshold**
(`--cascade=GATE`, default `-4.9`, English-quad calibrated: junk ~-5.3, near-solution 60%+
~-4.8…-4.2) so it spends its ~`CAP + N1·CAP` `score_iter` per fire only on promising boards and skips
the ~76% junk — verified: on an easy (solvable) message gated `--cascade` is byte-for-byte the
baseline `score_iter`, while ungated (`--cascade=-99`) adds ~7%. Correct/clean: 182 tests pass,
`-Werror` g++/clang++, ASan/UBSan and clang-tidy clean.

**Matched-compute verdict — a small but consistent WIN, because the gate makes it near-free.**
Recovery-vs-`score_iter` A/B (`-J -S i4q10 -R {8,16,32}` ± `--cascade`, English L40–50, 60/cell,
the no-cascade curve interpolated to the cascade `score_iter`): **+0.2–0.3pp mean %-correct and
+0.5–0.6pp exact** at matched compute, positive at every budget R≥8.

| `score_iter` | baseline %corr | `--cascade` %corr | Δ (matched) |
|---:|---:|---:|---:|
| 18,159 | 14.32 | 14.59 | **+0.27** |
| 36,264 | 19.52 | 19.77 | **+0.25** |
| 72,738 | 25.28 | 25.51 | **+0.23** |

The win exists *because of the gate*: `score_iter` stays within 0.3% of baseline at every budget
(the cascade fires only on near-solution boards and finds nothing on junk/solved), so the small
recovery gain is essentially free. **Ungated it is dominated** (fires on every convergence including
junk — the same wall as `--repair3`); the gate is what turns it from a loss into a win. This makes
it the **second clearly-positive matched-compute barrier-cross** (with the 2-plug `try_repair`,
§4.9), and the opposite verdict from `--repair3`/`--exhaust`/fix-and-finish — for the same reason
`try_repair` pays: near-zero cost, so any gain is net-positive.

A **second seed (N=100)** confirms and, on *exact* recovery, strengthens it: mean Δ +0.17/+0.21/+0.71
at R 8/16/32, and **exact recovery 5.0→5.7, 6.0→7.3, 10.7→13.0** — a **+2.3pp (relative +21%) exact
gain at R32**. The exact-recovery gain **grows with `-R`**, as expected: more restarts produce more
near-solution boards for the gated cascade to finish. So across two seeds the mean gain is a steady
+0.2–0.7pp and the exact gain +0.6…+2.3pp (largest at high `-R`), at ~zero added `score_iter`.

**Verdict — a validated component chain (53% solve on real fixable boards), shipped as the opt-in
`--cascade`, a small matched-compute win on short messages when gated.** Directed, reversible, and
it addresses the specific failure modes that sank fix-and-finish (§4.8: irreversibility) and the
badness heuristic (undirected). The gain is modest (the near-solution regime it targets is a thin
slice of the search), so it is opt-in, not default. The gate default (`-4.9`) is English-quad
calibrated; other languages/lengths tune it via `--cascade=GATE`. Reproduce the component
measurements: `eval/gain_cascade_probe.py` (dual generation, prune, full-plug ranking, cascade, and
the cascade+reclimb solve rate, all against the real `eval/results*.tsv` boards).

**A best-board-only variant — this measurement's `--gainfix-best`, later folded into and renamed
`--polish` (§4.11 adds the 3-ply escalation on top) — the fixed-cost alternative.** Instead of firing
the gated cascade at *every* near-solution convergence,
this variant runs the cascade **once, unconditionally (no score gate)**, on the single best board
after all `-R` restarts, then hands it to one finishing climb. It reconstructs that board's machine
from the winning key + recorded stecker (recorded at the merge), so it costs a **fixed** ~960
`score_iter` **independent of `-R`** — whereas per-convergence `--cascade` costs scale (weakly, via
the gate) with the number of near-solution convergences. A 3-way matched-compute A/B, full `-R`
sweep (English L40–50, **N=100/cell**, `-J -S i4q10`; `score_iter` within ~0.2–0.3% across modes at
every `-R`, so these are matched):

| `-R` | base %corr / exact | `--cascade` %corr / exact | best-board-only %corr / exact | Δmean (cascade / best) |
|---:|---:|---:|---:|---:|
| 8    | 13.48 / 3.0  | 13.65 / 3.3  | 13.63 / 3.0  | +0.17 / +0.15 |
| 16   | 17.69 / 6.3  | 17.85 / 6.7  | 17.98 / 6.3  | +0.16 / +0.29 |
| 32   | 21.78 / 10.7 | 21.94 / 11.0 | 22.19 / 11.0 | +0.16 / +0.41 |
| 40   | 23.51 / 12.0 | 23.75 / 12.7 | 23.96 / 12.7 | +0.24 / +0.45 |
| 80   | 27.65 / 15.0 | 28.34 / 17.0 | 28.16 / 16.0 | +0.69 / +0.51 |
| 160  | 33.40 / 21.3 | 33.87 / 22.0 | 33.85 / 22.0 | +0.47 / +0.45 |
| 320  | 43.00 / 30.7 | 43.47 / 31.0 | 43.66 / 31.7 | +0.47 / +0.66 |
| 640  | 50.78 / 39.7 | 51.32 / 40.3 | 51.49 / 40.3 | +0.54 / +0.71 |
| 1280 | 57.20 / 47.7 | 57.38 / 47.3 | 57.74 / 48.0 | +0.18 / +0.54 |
| 2560 | 65.27 / 57.3 | 65.80 / 57.3 | 65.75 / 57.3 | +0.53 / +0.48 |

**The mean gain persists across the whole `-R` range** — +0.2–0.7pp with no decay to zero, right out to
`-R 2560`. The two variants are peers, best-board-only generally ≥ per-convergence `--cascade` (it wins
or ties 8 of 10 rows; `--cascade` edges ahead only at `-R 80/160`): the best-board finish concentrates
its one unconditional cascade on the reliably-near-solution best board, and its cost is **fixed** (~950
`score_iter`) so it is ~free at high `-R`, whereas per-convergence `--cascade`'s cost scales (still
<0.3%). Both opt-in and mutually exclusive; the best-board variant runs only under the simple sweep
(not `-F`/`--exhaust`, whose `best.idx` does not carry the key×restart reconstruction).

> **This ~950–960 `score_iter` figure is for the 2-ply-only best-board variant measured here, and is
> now stale as a description of the shipped `--polish`.** §4.11 folds in a 3-ply escalation plus a
> "sacrifice + reclimb" step on top of this same once-only best-board pass, which costs more —
> re-measured at a flat **~6,500 `score_iter`** (L40–L190, `-R` 160/640; see `CLAUDE.md`'s `--polish`
> entry). The 2-ply-only numbers above are kept as the historical A/B that motivated shipping a
> fixed-cost finisher at all; they are not `--polish`'s current cost.

**What *does* saturate is exact recovery, not the mean.** At extreme `-R` the restart budget alone
finds essentially every recoverable board, so *new exact* solves from the finisher dry up — at
`-R 2560` all three modes tie at 57.3% exact (the residual is the §6.13/§6.14 **scoring-failure**
floor: true board not top-scoring, uncrossable by any score-based method). But the cascade keeps
lifting near-solution **non-exact** boards, so the **mean %-correct stays ahead** even where the exact
rate has converged. (An earlier `-R {40…2560}` sweep at N=16–40/cell had suggested the modes go
*byte-identical* at `-R ≥ 1280` — that was small-sample noise: those few high-`-R` cells happened to
contain no fixable board. The N=100 sweep here corrects it — the finishers still help at every `-R`.)

**A saturation exact-loss — diagnosed as over-plugging, and *fixed*.** An earlier build showed the
best-board finisher (later shipped as `--polish`) *reducing* exact recovery at very high `-R`: on the
tsv `-S m4q10` L40 baselines (scoring failures *removed*, so not pre-existing floor cases) it
converted a handful of `b_ex=1 (100%) → 95–97.5%` solves (2–3 per 40-trial cell), so exact dipped
`−2.6…−5.1pp` at `-R ≥ 1280`. The cause was **not** the count-neutral cascade but the **finishing
climb running uncapped** (`asize/2` = 13 pairs) instead of at the schedule's target-stage cap — so on
an already-solved 10-plug board it **added spurious plugs 11–13** that raise the noisy short-message
quad score while corrupting the truth. Capping the finish at `opt_stages[last].cap` (like every other
finisher/quench in the tool) removes it: re-measured on the identical boards, **every negative Δexact
goes to ≥ 0** (the four dips −2.6/−5.1/−5.1/−5.0 → 0/0/+2.6/0), Δmean improves at the mega-`-R` cells,
and it becomes **Pareto-neutral-or-better across the whole `-R 20…81920` range** (Δmean ≥ ~0, Δexact ≥
0 every cell).
The finisher is also **monotonic in score by construction** — the best board is replaced only when the
finish scores strictly higher, so it never returns a lower-*scoring* board than the search found. (A
residual truth-vs-score chase — a *count-neutral* cascade re-pair to a higher-scoring-but-wrong board
at the information floor — is possible in principle but was not observed after the cap fix; it is
unfixable by any score-only rule, since the wrong board genuinely scores higher.)

---

### 4.11 Deeper tangles: 3-ply escalation, and the "sacrifice + reclimb" reformulation — ✅ SHIPPED as `--polish`

The 2-ply cascade (§4.10) crosses **2-plug** tangles. The residual near-solution failures are **3-plug
tangles** — three wrong plugs mutually masking, so no single pair un-masks the fix. This extends the
finisher to them and lands a **better** design than a naïve deeper search.

**The cost reality that framed the whole exploration.** `gainfix_candidates` (the gain scan) does
`n·26·4` `quad8` lookups per position ≈ **~104 score_iter-equivalents** (a full `score_iter` is ~`n`
quad lookups). It does **not** call `score_iter`, so the `score_iter` counter *undercounts* the cascade's
real cost ~5×; wall-time is the honest axis. Two cheap-generation ideas were tested and **rejected**:
(a) **reuse** the original candidate list for plug2 (skip the per-plug1 regeneration) — 9× cheaper but
reverts to the 32% pair-coverage wall (§4.10): solve **57%→33%**, because only **45%** of accepted plug2
are in the original list — the regeneration *is* the un-masking. (b) **incremental vote update** (patch
only the plug positions a plug swap dirties) — *exact* (identical candidate list, same 57% solve) but on
short messages a swap dirties **~75%** of positions, so only ~25% cheaper. The generation cost is
largely irreducible on short text.

**Beam tuning — plug3 is undisputed at top-1.** Sweeping the explicit 3-ply beams on real near-solution
search-failure boards (`eval/results*.tsv`, english+german): plug1 saturates at N1≈6 (winning plug1 is
top-3 89% of the time), plug2 at N2≈6, and **plug3 = top-1 (N3=1) is undisputed** — solve rate flat
across N3 1/2/3. The completing plug is *always* the top move after un-masking: un-masking doesn't just
surface the third plug, it makes it the highest-scoring one. Explicit 3-ply lifted component solve
**55%→78%** on 2–4-missing boards.

**The reformulation (the actual design).** Since the completing plug is the top improving move the
*ordinary climb* would find anyway, the plug3 search is redundant. `--polish` instead: rank the
`(plug1,plug2)` **sacrifice** pairs by their 2-plug score, and for the **top-K (K=8)** commit the
sacrifice (both plugs, possibly downhill) and run a full **plain reclimb** — letting the climb find the
completing plug(s) *and* shed spurious ones — keeping the best result. No plug3 beam. Measured on the
escalated (2-ply-fail) boards, "commit best-K + reclimb" **matches** the explicit 3-ply at K=6 (48%) and
**beats** it at K=12 (61%): a full climb per sacrifice recovers more than committing one fixed completing
plug. The winning sacrifice is *not* reliably top-ranked by 2-plug score (spread across ranks 0–11), so
K must be moderate, not 1. Two K-sweeps in the real capped tool (`-R 80`, english+german L40) fix
`GAINFIX_K3 = 8`, but the reason is **compute-parity, not a diminishing-returns plateau**. At the shipped
beam (`N1=N2=6`, so only 36 sacrifice pairs exist) mean/exact rise steeply through K=8 (+2.0pp mean /
+3.3pp exact vs 2-ply) then **appear to flatline K=8→20** — but that flat is an *artifact of the 36-pair
ceiling*, not a knee. Widening the beam to `N1=N2=13` (up to 169 pairs) shows recovery keeps rising
**monotonically all the way to K=169** (reclimb every pair): mean %-correct 31.7 → 32.7 → 34.1 → 34.6 →
36.5 at K=8/16/48/96/169, exact 17.5 → 25.0. The tail is real — there is no plateau. **But on wall time
high-K is dominated, not merely un-winning** — and here `score_iter` and wall time *disagree*, because the
counter never sees the gain scan (~100 quad-lookups per cascade call, ×K sacrifices). By `score_iter`
K=169 (413k) sits *between* `--polish` at R160 (363k) and R200 (453k), so on that proxy it looks a
wash. But by measured wall time (T=4, min of 3 reps) K=169 is the **most expensive of the four at 131 ms**,
*above* R200's 114 ms despite R200 doing more `score_iter` — the uncounted gain scan is the gap. So
`--polish` at R200 **Pareto-dominates** K=169: better on both axes (37.4 vs 36.5 mean, 25.8 vs 25.0
exact) at lower wall (114 vs 131 ms), and R160 nearly matches K=169's quality at 110 ms. More restarts win
outright. (The restart wall cost is near-flat here — R80→R200 is only 102 → 114 ms despite 2.5× the
`score_iter` — because at L40/T4 the fixed overhead dominates and marginal restarts are cheap, whereas the
K=169 finisher adds a chunky +29 ms.) The *free* win therefore lives entirely in the small-K, fixed-cost
regime (~+8000 `score_iter`, one gain scan, the whole point of a once-only best-board finisher). `K=8` is
kept as that near-free operating point, not because further sacrifices stop helping. Implementation: the internal reclimb
reuses `hillclimb` with the cascade/polish flags saved+cleared (plain climb, no recursion), capped at the same
`max_pairs`; `-T`-deterministic.

**Real tool, capped @10, exact recovery.** On ≥80%-base non-exact boards, best3 solves **56% of the
fixable** vs 2-ply's **41%** (+15pp) — the cap hurts 2-ply more than best3 (2-ply leans on an unconstrained
reclimb; best3 commits the right plugs directly). By base bucket the best3-over-best edge is **+23pp at
70–80%, +16pp at 80–90%, +0 at 90–100%** — it fades at the top only because **88%** of 90–100% boards are
scoring failures (the fixable few sit right on the scoring-failure boundary: their per-symbol score margin
is ~0.003 vs ~0.011 at 70–80%, so there's almost no signal to commit). Cost is negligible: the finisher
fires once, ~+2–3 ms wall / ~+8000 `score_iter`, **<1% for R ≥ 640**.

**End-to-end matched-compute A/B** (english+german L40, scoring failures removed, both `score_iter` and
wall-time): the reformulated best3 beats 2-ply by **~+1.3pp mean %-correct (English, consistent across
R)** and **+2…+5pp exact recovery** — roughly 3× the old explicit-plug3 best3's ~+0.4pp, at ~2–3 ms wall.
The gain is real but still a **thin-slice** effect: it concentrates on near-solution 3-plug tangles, so
averaged over *all* trials (mostly deep junk) it is the modest end-to-end number, not a headline. Shipped
as the opt-in `--polish` (originally `--gainfix-best3`; renamed — see `CLAUDE.md`'s `--polish` entry).

**4-ply (3-plug sacrifice) — ❌ MEASURED, REJECTED. The depth stops at 3-ply.** The "sacrifice +
reclimb" idea *mechanically* generalises to deeper tangles — commit a **3-plug** sacrifice
(`plug1+plug2+plug3`, all downhill), rank `(plug1,plug2,plug3)` triples by 3-plug score, reclimb the
top-K — with no combinatorial plug search, since the reclimb handles completion at any depth. Probed
env-gated (`gain_cascade_4ply`, english+german L40, R80) with a proper **K-sweep** (the 2-plug sweep's
lesson — the winning sacrifice is not reliably top-ranked, so a small K under-tests). At the `6/6/6`
beam (216 triples) sweeping `K = 6,8,12,16,20,24`, 4-ply recovers **at most ~1 of the 90 search-failures**
in the `best3` residual (fixes by K: `1,1,0,0,1,1` — **flat noise, no K-trend**), for mean/exact of
**33.7/20.8** at K=6 vs `best3`'s **33.25/20.0**. This is the qualitative tell: the depth-2 K-sweep rose
**monotonically to K=169** (genuine winning pairs spread across ranks), whereas the depth-3 sweep is flat
— there essentially are no winning triples the 2-plug sacrifice + reclimb didn't already reach, so the
odd +1 is a single lucky board, not a systematic effect. (A wider `13/13/6, K=8` probe found **0** — an
artifact of taking the top-8 from a ~1000-deep triple pool; the tighter beam is what surfaces the ~1.)
And it stays **Pareto-dominated by a wide margin**: the best 4-ply point (K=6, **96 ms**, ~+15 ms/+50%
score_iter over `best3`) is beaten outright by `best3` at **R160 (87 ms, 24.2 exact)** and **R200 (91 ms,
25.8 exact)** — both *cheaper* in wall time and recovering **~4–5 more boards**. The reason is diagnostic:
those 90 residual misses are **wrong-basin** failures — the converged board is not near the truth, so no
*local* directed repair (2-, 3-, or 4-plug) reaches it; only a different **restart** *lands* near the
solution (R200 cracks ~7, a deeper sacrifice ~1 at noise). The gain cascade's entire value is completing
an *already-near* board, and the 2-plug sacrifice + reclimb (3-ply) already reaches everything greedily
completable from there. So the sacrifice depth is fixed at 3-ply — deeper is wasted compute, better spent
on `-R`. (Probe discarded; this negative result is the artifact.)

**depth-1 (`1sac`, the sacrifice+reclimb analogue of the explicit 2-ply cascade) — ⚠️ MEASURED, better
recovery but `-R`-dominated. Closes the finisher family.** The other end of the family: commit **one**
downhill plug1 sacrifice, then a **full reclimb**, over the top-K plug1 candidates — the sacrifice+reclimb
form of what the shipped explicit 2-ply cascade (`gain_cascade`) does by *hand-picking* a single plug2 and
keeping only net-positive pairs. Two clean results (env-gated `gain_cascade_1sac`, english+german L40):
(1) **standalone it beats the explicit cascade** — 32.7 vs 31.2 mean, +0.8→1.6pp exact at K=6…24 — i.e.
"let the full climb complete it" beats an explicit single-plug completion *at depth-1 too*, the same lesson
as depth-3; (2) **swapped into `best3`'s layer-1** (`1sac + 3ply`) it **beats `best3` on recovery** —
**+0.75pp mean / +0.8pp exact at K=8, +1.4 / +1.7 at K=24** (R80) — the *first and only* finisher variant in
this whole exploration to out-recover `best3`. **But it is not free** the way the explicit cascade is: it
runs K *full reclimbs* (vs a single-pass plug2 scan), so `score_iter` jumps 189k→266k→420k and wall +8→+23 ms,
and at matched wall it is **Pareto-dominated by `-R`** — `best3 R240` (92.6 ms) beats `1sac3 R160 K=8` (93.5 ms)
by **+4.5pp mean / +4.2pp exact at lower wall**. And the edge **fades as the baseline R rises**: the `1sac3`
lift over `best3` roughly halves R80→R160 and **vanishes at K=24 by R160** (37.17 vs best3's 37.19), because
restarts subsume the boards it targets. So the explicit cascade stays in `best3` **not because it recovers
more** — `1sac` genuinely recovers more — but because it is **~free**, and `best3`'s whole value is being a
free finisher; `1sac` trades that away for wall time `-R` converts into more. This **closes the
sacrifice-finisher family**: depths 1–4 (and high-K) all lose to restarts at matched compute. The remaining
search frontier is **restart diversity** (better basin-finding: kicks, seeds, SA/tabu/GA) or a **sharper
scoring model** (to lower the scoring-failure floor) — *not* a deeper or better finisher. (Probe kept
uncommitted; this measured-but-dominated result is the artifact.)

## 5. Structural / constraint-based

These exploit machine structure the pure statistical search ignores. The two crib
ideas are a *different axis*: a new **input** (known plaintext) that unlocks
deduction the ciphertext-only search can never match — but they are **invisible to
`make crackquality` as written** (its trials are crib-free clean-prose excerpts),
so they require a new harness tier before they can be measured. Say so; do not
overclaim.

### 5.1 Crib-driven bombe closure deduction (HIGH value *where a crib exists*)

**Form in this codebase.** `setup_mapping()` (`:572`) already yields the exact
rotor-stack permutation `R_i` at each position for the fixed key. Given a crib
letter `p_i` and cipher letter `c_i`, the machine equation is
`plug(R_i(plug(p_i))) = c_i`. Hypothesize one plug for a well-connected menu
letter; each crib position then **deduces** a forced plug (`R_i` is known),
chaining into more. A deduced self-plug, or a letter forced to two partners, is a
**contradiction** that kills the hypothesis (Turing/Welchman menu; the diagonal
board's reciprocity is free because `steck` is stored as an involution).
Enumerate the 25 hypotheses for the most-connected menu letter, keep the
contradiction-free plug sets, hand them to the climb as frozen seeds. Wrong crib
alignment yields all-contradictions — which is *useful*: it rejects offsets,
enabling automated crib-dragging by contradiction density.

**Why it helps at 50 chars.** A correct crib carries enormously more information
per letter than n-gram statistics: a single 8–12 letter crib with an internal
loop pins 3–5 plugs *with certainty*, sidestepping the local-optima problem
entirely for the deduced plugs. This is the historically decisive technique and
the right tool whenever any known/guessable plaintext exists (stereotyped
weather/military German: `WETTER`, `KEINEBESONDERENEREIGNISSE`, `OBERKOMMANDO`,
spelled numbers).

**Honest payoff.** High **where a crib exists; inapplicable otherwise** — no crib,
no menu, no closure. It will **not** move the current crib-free `crackquality`
benchmark. Its payoff is on real operational traffic, arguably the tool's
real-world use case, and it deserves its own harness. `CODE_REVIEW.md` §2 already
ranks a crib capability highest among new scoring work; the menu form is the
structurally correct realization.

**Cost/risk.** Moderate: a crib CLI input, a menu builder over crib loops, a
contradiction checker reusing `mapping[]`. Deterministic. Risk is a wrong crib
genre/language assumption.

**Experiment.** New harness tier in `crack_quality.py`: plant a known 12–20 letter
crib at a random offset per trial; measure recovery at 40–60 chars with vs without
deduction, and crib-drag reliability (how well wrong offsets are rejected). On the
existing crib-free harness, expect — and report — **no change**.

### 5.2 Crib-drag soft seeding without full closure (MEDIUM value, crib-dependent)

**Form in this codebase.** A lighter cousin for short or loop-free cribs. Without
hypothesis-chaining, `plug(R_i(plug(p_i))) = c_i` is still a *soft* constraint per
crib position: initialize the board with the plugs most consistent with the crib
equations, and forbid the climb from proposing any plug that makes a crib position
mismatch (a cheap per-move predicate against precomputed `R_i(letter)` tables).
Composes with `-s` and with n-gram scoring on the non-crib remainder.

**Honest payoff.** Medium, crib-length-dependent; less powerful than full closure
(no certainty) but applicable to any crib. Keep the crib-position list small so the
per-move predicate stays off the hot path's critical cost. Same harness caveat as
§5.1 — invisible to the current benchmark.

**Experiment.** Same crib tier: seed+constrain vs full-bombe vs no-crib across crib
lengths; bench the per-move predicate cost.

### 5.3 Cross-key plug marginalization (❌ DE-PRIORITIZED — correlated-noise argument, maintainer decision)

> **De-prioritized, not pursued.** The premise — "a plug that helps *many*
> candidate keys is likely true" — assumes the candidate keys carry a shared
> plugboard signal. They do not: under a *wrong* rotor key the decrypt is garbage,
> so its best-fitting plugboard is **noise** uncorrelated with the truth, and
> marginalizing over the top-N (mostly-wrong) keys aggregates mostly noise. This is
> the **correlated-wrong-basin** failure that already sank §3.1 cross-restart
> consensus (built, measured, rejected); §5.3 is the cross-*key* twin of that
> cross-*restart* idea and inherits the result. The expensive cross-key experiments
> are therefore not worth running. The one cheap survivor of the full-crack tier —
> a one-time check of whether the *objective* misranks under an unknown key — is
> kept as the scoring-failure gate in `CRACKQUALITY_TESTS.md` §1. The original
> write-up is retained below for the record.

**Form in this codebase.** Exploit that the plugboard is **identical for every
rotor key** (it is the message's stecker, key-independent) — the single
highest-leverage structural fact none of the shipped work uses. This is the
generalization of `-F`'s nested hill-climb (score each rotor key by a cheap partial
plug climb): after the `-F` cheap-IC tier, accumulate a key-agnostic plug prior by
marginalizing plug appearances over the top-N ranked keys' cheap boards — a plug
that helps many candidate keys is likely true; one that helps a single key is
overfit. Seed each finalist's full climb from this shared prior; optionally
EM-iterate (refine ranking given prior, refine prior given rankings).

**Why it helps at 50 chars.** A single key gives too little signal; pooling weak
plug evidence across many candidate keys is the sample-size multiplier the
single-key climb structurally lacks. This is the one non-crib lever that *adds
information* rather than spending compute.

**Honest payoff.** Potentially high **for full crack** — but **not applicable** to
the current fixed-known-key tier (where all misses are search failures). It is a
prime reason to build the full-crack harness tier (see §9).

**Experiment.** Build the full-crack `crackquality` tier first, then compare
cross-key-seeded finalists vs the independent-per-key baseline at equal compute.

---

## 6. Scoring

> **Measured — §6 is effectively closed.** Three independent results all say the
> plugboard tier is **search-bound, not scoring-bound**, and no scoring change moves
> it: (1) the unknown-key **scoring-failure gate** (`CRACKQUALITY_TESTS.md` §1) finds
> `scoring-fail% = 0` at L ≥ 50 and only ~7.5% at L40 (near-zero recovery regardless);
> (2) **§6.2** back-off smoothing — built, measured worse-to-neutral, rejected; (3)
> **§6.1** trigram-target — measured, rejected (quad wins at the realistic plug count
> with the tuned recipe). The two remaining items (§6.3 MDL prior, §6.4 quad+λ·IC)
> inherit this strong negative prior and both carry hot-path cost and a risk of
> *introducing* scoring failures, so neither is worth building ahead of the search
> ideas (§3/§7). **Do not promote §6 above §3/§7.** The larger scoring payoffs, if any,
> await a future full-crack tier where rotor-key discrimination is in play.

Per the §1 diagnosis, purely *monotone* rescales (likelihood-ratio null
normalization, z-scoring, per-length normalization) **provably cannot change
per-key ranking** and cannot move the plugboard tier. Only ideas that **reshape
the surface** (smoothing, lower-order/adaptive order, combined IC+quad, finer
quantization) can help the climb, and even those help only insofar as a smoother
surface lets the *same* climb reach the basin. **Every item in this section ranks
below every §3/§7 item** in the shortlist: the tier is search-bound, so scoring can
only help as *landscape smoothing*, measured solely by whether the search-failure
share drops. The larger scoring payoffs await the full-crack tier.

### 6.1 Adaptive n-gram order by length; trigram target at the short end (❌ MEASURED, REJECTED)

**The idea.** All four scorers exist; a scheduling change would make the
target/ranking model a function of `textlength` (short → trigram, long → quad), on
the "lower order = denser cells = smoother surface = fewer local optima" argument,
plus the folk result (attributed to Williams) that a lower-order statistic *beats*
trigrams for plugboard recovery because the plugboard maximally disrupts
higher-order frequencies.

**Measured (`crackquality`, plugboard tier, `-R 8`, 40 trials, English).** Trigram
vs quad, both run with the **tuned staged recipe** `-S i4q10` / `-S i4t10` (only the
target model swapped):

| | L50 | L60 | L90 | L120 |
|---|--:|--:|--:|--:|
| 6 plugs — quad `i4q10` | 57.6 | 74.9 | 95.7 | 100 |
| 6 plugs — trigram `i4t10` | 54.5 | 80.4 | 97.0 | 100 |
| 10 plugs — quad `i4q10` | 12.9 | 24.4 | **67.0 / 62.2** | 88.3 |
| 10 plugs — trigram `i4t10` | 12.9 | 21.7 | **57.6 / 53.3** | 89.9 |

*(L90/10-plug shows both seeds.)* At 6 plugs it is a wash (mixed ±3–5pp); at the
realistic **10 plugs quad wins**, decisively at L90 — **+9.4 and +8.9pp across two
seeds** — and elsewhere ties within noise. **Quad-as-used is correct; the docs'
"quad is the recommended model" stands.** Not shipped.

> **Methodology lesson (the reason this nearly shipped as a false win).** A first,
> *un-staged* pass — bare `-q` vs bare `-t` — showed trigram beating quad by up to
> +8pp, at *all* lengths. That was an artifact of a **weak baseline**: bare quad
> without the IC pre-pass is far worse than quad as actually used (staging lifts
> quad ~+40pp at L60/6-plug — `35.2 → 74.9`), so "trigram beats bare quad" is not
> "trigram beats quad." **Any scoring-model comparison must use the tuned staged
> recipe (`-S i4qK`), never bare models** — otherwise a weak baseline manufactures
> a win. With the fair comparison the trigram edge collapses and quad leads at the
> realistic plug count.

This also fits the broader §6 picture: the tier is search-bound (the §1 gate), the
"denser/smoother surface helps" premise was already falsified by §6.2, and lower
order trades its (real) density for a lower discrimination ceiling that costs most
exactly where there is signal to lose (L90, 10 plugs).

### 6.2 Back-off / interpolated smoothing to replace the flat hapax floor (❌ BUILT, MEASURED, REJECTED)

**The idea.** `ngrams_read()` floors every unseen quadgram at one fixed value
`log10(1/total)`. On ~50 letters most of the ~47 quadgrams are unseen and all
contribute the *same* constant — so the score is blind to *which* unseen quadgram
appeared. The hypothesis: replace the flat floor with an interpolated/back-off
estimate that keeps the lower-order signal, **precomputed into the same `quad8`
table at load time** (hot loop and bench byte-identical, only the build differs),
would reshape the short-text surface and pull the climb into the true basin.

**Built behind an opt-in `--backoff` flag and measured (`crackquality`, plugboard
tier, 6 plugs, `-R 8`, 40 trials/length). Two formulations, both fail:**

- **Interpolated *conditional* LM** — `P(d|abc)=Σ λ_k P_k`, backing off through the
  tri/bi/mono tables. **Decisively worse: −11pp L50, −24pp L70, −20pp L90, and
  unmoved by the weights** (even `λ4=0.97`). Cause: the conditional `count4/count3`
  *rewards "locally predictable" gibberish* — a rare trigram context whose one
  frequent continuation appears scores high conditionally, though the quadgram is
  vanishingly rare jointly. Conditional normalisation systematically inflates decoy
  scores and shrinks the gap to the truth.
- **Joint-floor, downward** — keep the exact joint surface for *seen* quads; push
  only *unseen* quads below the floor by their suffix-trigram implausibility
  (`λ=0` reproduces the flat floor byte-identically; verified). **Neutral: ±1–3pp,
  mixed sign, pure trial noise** across `λ∈{0.5,1,2}`, `cap∈{1,2}`.

**Verdict — the flat hapax floor's harshness is a *feature*, not a bug.** Punishing
every unseen quadgram equally hard is exactly what makes only genuinely
language-like (attested-quad-rich) decrypts win; any smoothing that *lifts* unseen
values rewards plausible-looking gibberish (the conditional form), and even the
mechanistically-sound *downward* variant doesn't help because — per the §1
scoring-gate result — the tier is **search-bound, not scoring-bound**, so a
better-conditioned unseen-quad surface has no search failures to convert. Not
shipped; `--backoff` removed (the `load_counts()` refactor it introduced is kept).
The related "denser surface" idea for the *final* model at the short end survives
only as §6.1 (trigram target), and even that inherits this negative prior.

### 6.3 Soft MDL / plug-count prior (LOW–MEDIUM priority; weakest fit to the diagnosis)

*Merged from three researchers.*

**Form in this codebase.** Add a small per-plug penalty to the objective in
`score_iter` (`score − λ·plug_count`), a soft, always-on, unknown-count analogue
of SA's hard cap. Maintain a running `plug_count` so the hot-loop cost is `O(1)`
(verify with `make bench`; must not touch IC, which is dimensionless).

**Why it helps at 50 chars.** A noisy short-text quad score rewards adding plugs
that fit the sample (overfitting), producing high-scoring *wrong* boards — a
mechanism that manifests *as* a search failure (the wrong optimum out-scores nearby
correct-cardinality boards). A prior reshapes the landscape so the
correct-cardinality region is favored, and it needs no known count.

**Honest payoff.** Low–medium, and **honestly the weakest fit** to the "misses are
search failures" diagnosis: on this tier the true count is fixed at 10 and the true
board already out-scores the reached one, so a fewer-plugs prior may not help. Most
valuable in the full-crack tier or when the true count is small/unknown. Include as
a knob, not a headline; too-large λ clips real plugs.

**Experiment.** `make crackquality SPLIT=1` sweeping λ; add a low-true-count
variant to the harness; compare against the hard cap at the true count as an oracle
upper bound; confirm the scoring/search classification shifts without raising
search failures.

### 6.4 Fused score: weighted all-order + λ·IC — ✅ SHIPPED as `-f`

**The idea.** Instead of *staging* IC then the n-gram model, fuse them:
`score = per-symbol ngram + λ·IC`. With only a plug or two set the quad/weighted
surface is nearly flat — almost every 4-gram is unseen and floored at the hapax value,
so a toggle barely moves the score — while IC still responds as soon as the letter
histogram starts to skew. Fusing gives one continuous surface instead of a hard
handover: the n-gram term dominates where it has signal, λ·IC supplies gradient where
it does not.

**Why IC is the right term to add.** It is **permutation-invariant** — it reads only
the multiset of letter counts, never which letter holds which count. That matters
because the plugboard *is* a letter permutation: any identity-sensitive unigram
objective can be driven by choosing the permutation rather than by finding the truth,
which is exactly how χ²-monogram collapsed to 12.5% tier-1 recall against IC's 68.8%
(archived §9 item 2). IC is the one unigram signal the board cannot manufacture. It is
also language-independent, which is why the result does not inherit a register bias.

**Measured — the largest short-message scoring gain in this codebase.** Paired within
instance, 300 trials per length per family, two seed families, both arms calibrated to
200k `score_iter` with identical `-R`:

| corpus | λ=30 vs no blend | n |
|---|---:|---:|
| wehrmacht | **+4.4 pp** [+3.1, +5.6] | 1800 |
| english prose | **+3.0 pp** [+1.7, +4.4] | 1800 |
| german prose | **+3.1 pp** [+1.9, +4.3] | 1800 |

**The first scoring change here that is not register-dependent** — contrast the mono
pre-pass (+2.2pp telegraphic / −2.2pp German prose, §6.10) and the SA staged pre-pass
(+2.3pp / −2.6pp, §3.11). For scale, `-a` itself was +1–2pp.

**λ: baked at 30, and the scale is the whole story.** Per-move quad deltas run ~0.07
early to ~2.0 late; per-move IC deltas are ~0.005. So λ must be ~20–40 to matter at
all, and this section's *original* proposed sweep of λ ∈ {0, 0.25, 0.5, 1} is
**inert** — λ=1 and λ=10 give byte-identical output to λ=0. Running the section as
first written would have produced a false negative. Measured curve (wehrmacht,
n=1800/point): 10→+2.1, 20→+3.6, **30→+4.4**, 40→+3.8, 60→+2.0, and 80→−8 (n=40).
The plateau is broad — paired blend-vs-blend puts λ30−λ20 at +0.81 [−0.12,+1.74] and
λ30−λ40 at +0.62 [−0.29,+1.53], i.e. **20/30/40 are statistically indistinguishable**
— so λ is baked like `-a`'s order weights rather than exposed as a knob
(`ENIGMA_IC_BLEND` overrides it for experiments, as `ENIGMA_LOGLIN` does for `-a`).
A finer scan is *not* worth running: resolving ~0.5pp would need ~7× the trials to
locate a difference no user could feel.

**λ's optimum is length-dependent, which is why it is not set higher.** λ60−λ30 costs
**−4.22 pp [−5.87,−2.56] at L50** but only −0.90 (ns) at L90; the milder λ40−λ20 step
even turns positive at L90. IC's sampling noise scales ~1/√n, so on 50 letters over 26
bins the term is mostly noise. λ=30 ties λ=20 at L50 and beats it at L90, so it
dominates across the range — the length-dependence argues against going above ~40, not
for making λ adaptive.

**It is a better CLIMB, not better discrimination — this is the load-bearing finding.**
The fused score does two separable jobs: it shapes the climb, and it ranks converged
boards. Decomposed (`ENIGMA_IC_BLEND_MODE`, since removed; wehrmacht, same instances):

| component | effect |
|---|---:|
| surface (blend the climb, rank pure) | **+3.4 pp** [+2.2, +4.6] |
| selection alone (climb pure, rank blended) | **−0.0 pp** [−0.6, +0.5] |
| consistency (rank blended *given* a blended climb) | +1.0 pp [+0.4, +1.6] |

Blending the ranking is worth **nothing** on its own; it adds ~1pp only once the climb
is blended too, which is objective-*consistency* (rank by what you optimised), not
discrimination. **So §6.15's ~1% discrimination floor is NOT contradicted.** What
§6.15 does over-claim is that the climb surface is tapped: its smoothness probe swept
the `-a` **order weights** 8× and moved search-fail <1pp, but that is one direction.
An orthogonal, permutation-invariant term moves recovery +3.4pp. The surface was
under-explored along a single axis, not exhausted.

**Cross-key: no harm.** Every measurement above fixes the rotor key. With `-g` partly
wildcarded so the score must rank *keys* (10 plugs, `-R 20`, compute matched within
~2%): +2.8pp at 26 keys/L70, +6.3pp at 26 keys/L90, +5.8pp at 676 keys/L90. The gain
matches the fixed-key case, which — given selection contributes −0.0pp — says the
benefit is still the per-key climb carrying over, and that the fused objective does not
distort key selection. *Scope limit:* 676 keys is far short of the 17,576-key full
start wildcard and further short of the real 60-order keyspace.

**It does not replace staging.** Three arms, same 1800 instances, matched compute:
`m4a10` (no blend) 33.9, `a10`+blend (one pass) 35.3, `m4a10`+blend 38.3. The single
blended pass *ties* the staged recipe (+1.4pp [−0.4,+3.2]) — it earns ~1.7× the
restarts, since dropping the pre-pass is ~0.59× the cost (§6.10) — but keeping both is
**+3.0pp [+1.3,+4.7]** over it. Pre-pass and fusion are complementary: the pre-pass
selects a basin on a smoother surface, the fused term shapes the gradient on the target
surface.

**Cost.** Wall-time neutral: `-R 400`, min of 3 — off .213s, λ=10 .209s, λ=20 .210s,
λ=30 .205s. The 26-bin histogram is cheap beside the gather-bound decode, and it is
accumulated in the *same* decode pass as the n-gram sum (a two-pass version would have
inflated wall time per `score_iter` and quietly unfaired every matched-compute A/B).
IC cannot be folded into the table the way `-a`'s four orders are: those are additive
over positions, IC is quadratic in the whole-message histogram.

Reproduce: `eval/eval_sa_vs_greedy.py` (STAGE=3), `eval/results-ic-blend*.txt`.

### 6.5 Finer score accumulation on short text (LOW priority)

**Form in this codebase.** `quad8` quantizes to uint8 at `ngram_scale=32`
(~0.03 log10/step). On 50 letters the score is a sum of only ~47 steps, so two
competing moves can tie or invert purely from quantization and steepest-ascent
stalls on a quantization plateau. Offer a uint16 (≈8× finer) table used **only** on
short messages / only in the final climb.

**Honest payoff.** Low–medium, skeptical: quantization error is roughly zero-mean
and may wash out. **This directly fights the documented cache-residency choice** —
a uint16 quad table is 26⁴×2 ≈ **0.9 MB** vs the current 26⁴×1 ≈ 457 KB, so it
roughly *doubles* the terminal-gather working set and *will* cost throughput; gate
it to short/final-climb only. Bench under both compilers is mandatory.

**Experiment.** `make crackquality` with the uint16 table at L40–70 for exact
recovery; `make bench` (g++ and clang) to quantify the throughput hit and confirm
it is confined to the short/final path.

### 6.6 Telegraphic / operational corpus — ✅ SHIPPED as the `wehrmacht` language (`eval/`)

**Form in this codebase.** The bundled prose tables are generic web-corpus
statistics. Real Enigma plaintext is telegraphic: `X`/`Y`/`J` separators, spelled
numbers (`EINSNULL`), no punctuation, fixed procedure words. `X` alone dominates
real n-gram statistics in ways absent from prose.
`eval/build_telegraphic_ngrams.py` bends the bundled prose German tables toward
the published telegraphic statistics in Sullivan & Weierud's 2005 Appendix C
(single-letter + top-400 trigram frequencies over ~20,000 letters of 1941
decrypts) by marginal-matching the quad table's folded low-order marginals to
telegraphic strength. It ships as a first-class scoring **language** —
`ngrams/wehrmacht_*.txt`, selected with `-l wehrmacht` alone — rather than a
parallel data directory, so it needs no `-d` and composes with a custom one; the
Appendix-C source tables live in `eval/` beside the generator.

**Measured payoff.** Validated on the full 69-message held-out set
(`eval/eval_telegraphic.py`; rotor key fixed, plugboard hidden and
hill-climbed): **+20.9 pp mean %-correct** on real 1941 Wehrmacht traffic
(wins 36 / loses 12 of 69), biggest in the 70–119-letter band. The mirror
control (`eval/eval_prose_inverse.py`) confirms it is a *register*, not a
general-German upgrade: the same tables lose **−10.2 pp** on ordinary prose
German, so `-l german` remains correct for prose and for the `make
crackquality` benchmark — `wehrmacht` is for real Wehrmacht/telegraphic traffic
only. Full writeup and tables: `eval/MODERN_BREAKING_NOTES.md` §6.

This was invisible to `make crackquality` as originally scoped (it samples
clean prose), so it was validated by the two dedicated real-traffic harnesses
above rather than by the standard suite — a real-world-fidelity win, not a
`crackquality` benchmark win. `make crackquality` itself still correctly uses
`-l german` (prose) and is unaffected.

### 6.7 Ceiling-limited (do not expect plugboard-tier movement)

- **Positional / message-structure priors** (soft cribs for stereotyped
  openings): only helps traffic with the assumed structure; useless on generic
  prose; a wrong prior actively hurts. A real-message crib-mode feature — see §5.
- **Bayesian LLR / z-score calibration:** a per-length *constant* offset, so it
  **cannot change per-key ranking** by construction. Its value is *infrastructure*
  for the full-crack tier — a principled, cross-key-comparable confidence to set the
  `-F` cutoff (a score margin instead of a top-N count) and a full-crack accept /
  early-stop threshold. Build it there, not here.
- **5-gram / HMM / NN final re-ranker:** on the plugboard tier the top candidate is
  already the argmax of the same statistics, so a re-ranker rarely overturns a
  *search* failure (the true board was never reached to be re-ranked). Only the
  portable 5-gram variant respects the single-TU/CPU constraints; the NN breaks
  them — reject unless a full-crack tier justifies it. (5-grams were already
  rejected as a *primary* model: too sparse.)

### 6.8 Lower-order intermediate `--score` stage (mono pre-pass) — ❌ MEASURED, REJECTED

**The idea.** The staged climb already runs `i4q10` (IC cap-4 pre-pass → quad
uncapped). Insert a *third*, even-lower-order stage between them —
`-S i4m4q10` (IC cap-4 → **monogram cap-4** → quad) — on the §6.1 "denser cells =
smoother surface" logic: a monogram surface is maximally dense, so a mono stage
should steer the first few plugs into a better basin before quad refines. A bigram
or trigram intermediate (`-S i4m4b6t8q10`) is the natural generalisation.

**Why it looked like a win (the weak-test trap).** On the first pass — **English
only, few seeds, bare steepest-ascent climb** — `i4m4q10` beat `i4q10` by a large,
consistent margin (up to +11pp mean %-correct at L50). That is exactly the
configuration in which a scoring tweak is most likely to *look* good and least
likely to *be* good, and it is the same trap the trigram-target probe fell into
(§6.1): a bare climb is a weak, high-variance baseline that the tuned recipe
easily beats.

**Measured properly and it collapses.** Re-judged the way every scoring change
must be — **10 plugs** (the `crackquality` default / standard Wehrmacht), **all
four languages, six seeds, matched `score_iter`, with `-J`** (the shipped recipe,
not a bare climb). The compute match uses baseline `-J -S i4q10 -R13` vs mono
`-J -S i4m4q10 -R10` (both ≈ 30k `score_iter`/climb on English):

- **English:** marginal and seed-mixed — roughly +1–3pp at L50/L70, near-zero and
  sign-flipping across seeds at L90. Not a robust win even in its best language.
- **German L90: −8.0pp, all six seeds negative** (`------`).
- **Danish L90: −4.9pp, all six seeds negative** (`------`).
- **French:** nominally positive but at near-zero signal (both configs recover
  almost nothing) — uninformative.

**And the loss is worse than it prints.** The `-R10`/`-R13` compute match was
calibrated on English, where the climb is most expensive per restart. The
non-English climbs are cheaper, so the German/Danish mono runs actually consumed
**~15–17% *more* `score_iter`** than their baselines — and still lost at L90. A
change that reverses sign across languages *while holding a compute advantage* is
not a win.

**Verdict — reject; the recipe stays `i4q10`.** A mono (or bi/tri) intermediate
stage does not generalise past English; it is a robust loss at the length that
matters (L90) in German and Danish. The mechanism is the §6.2 negative prior
again: the tier is **search-bound, not scoring-bound**, so a smoother
intermediate surface has few search failures to convert, and the extra stage just
spends compute steering plugs by a language-generic-then-mismatched signal.
Two methodology lessons, both now paid for twice (here and §6.1):

1. **Compute must be matched *per language*, not once on English.** Per-climb cost
   varies enough by language that an English-calibrated `-R` match silently hands
   the challenger a double-digit-percent compute edge in other languages.
2. **A bare-climb / few-seed / English-only win must survive `-J` + all four
   languages + more seeds before it earns a place in the recipe.** Both the
   trigram-target (§6.1) and the mono-stage passed the weak test and failed the
   strong one.

### 6.9 German scoring was crippled by a table-loading bug (fixed); model order is **not** language-dependent after all — ✅ MEASURED (`eval/`)

**This subsection corrects an earlier wrong conclusion.** The eval log first showed
German badly **scoring**-bound under quad (a wrong plugboard out-scoring the truth in
~50–60% of short-message misses) while lower orders recovered far better — reading as
"German wants bigram, not quad." That was an **artifact of a table-loading bug**, not a
property of German.

**Root cause.** `load_counts()` stopped reading an n-gram file at the first record
whose gram contained a non-A-Z character. The tables are frequency sorted and German
interleaves umlaut/eszett grams (`ä ö ü ß` as single symbols) from near the top, so the
table was **truncated** there. Measured records actually loaded:

| german table | loaded / total | % of count kept |
|---|---|---|
| monograms | 22 / 30 | 97.9% |
| bigrams | 114 / 895 | 73.7% |
| trigrams | 95 / 23,484 | 27.5% |
| **quadgrams** | **29 / 366,266** | **4.9%** |

So the "german quad" scorer ran on its **29 most frequent quadgrams**, flooring the
other 95% as hapax — and the apparent bigram > trigram > quad ordering was pure
truncation (lower order = first umlaut appears later in the frequency ranking = more of
the A-Z table survives). English (26 letters, no accents) loaded fully and was never
affected.

**Fix and re-measurement.** `load_counts()` now **folds each accented gram to its A-Z
base and accumulates counts** (`é→E`, `ü→U`, `ø→O`, `ß→S`, …; an initial skip-the-record
fix was refined to this so the folded accented grams add to their base instead of being
dropped). The plaintext/ciphertext readers fold the same way and warn on non-mappable
characters. All 182 tests pass; English (no accents) byte-identical. German quad,
before → after:

| L | quad BEFORE (truncated) | quad AFTER (full table) |
|---|---|---|
| 50 | 10.6 / 0 / 60% | **33.0 / 12 / 0%** |
| 90 | 24.7 / 1 / 50% | **91.1 / 71 / 0%** |
| 120 | 48.4 / 10 / 60% | **95.5 / 76 / 0%** |
| 160 | 62.8 / 28 / 31% | **100 / 80 / 0%** |

**Corrected conclusion.** With the full table, **German quad works** — search-bound
(0 scoring failures), fully solved by L160, the same regime as English — and the natural
order returns (at L90, quad 91.1 > tri 85.9 > bi 81.2). Quad, tri and bi are now all
close for German prose (within a few pp; bigram edges quad only at the very shortest
lengths, L50/L120), so **model order is *not* meaningfully language-dependent** once the
tables load correctly. **Use `-q` for German as for English.** The lower-order preference
is retracted. (The bug also silently truncated Danish and French — 29/42-symbol tables —
so this fix helps every non-English language: re-run under the fix on an orthogonal
four-language grid, german/danish/french all crack comparably to English, all reaching
100% exact by ~L200 and *easier* than English at short lengths — `eval/`.)

**What survives.** Genuine *telegraphic* German (the Dönitz P1030681 message, the 1930
manual message) is still harder than prose even after the fix — real operational
orthography (`Q`-for-`CH`, dense `X` separators, `ae/oe/ue/ss` transliteration that the
prose-built table doesn't match) is off-distribution for the prose tables. That residual
is the genuine §6.6 operational-corpus argument, now cleanly separated from the
table-loading bug.

### 6.10 Pre-pass value is restart-budget-dependent — IC dominated at high R, pure-quad diversity suffices — ✅ MEASURED (`eval/`)

**§6.8 is a *low-R* result; at high R the pre-pass picture inverts.** §6.8 correctly
rejected the mono pre-pass — but it measured at `-R 10/13` (and as an *inserted*
`i4m4q10` stage). Re-run the pre-pass model itself across the **high-restart regime**
(`-R 1280/2560`, the budget short-message cracking actually uses) and the conclusion
flips: the IC pre-pass is the *worst* choice, and the pre-pass barely matters at all.

**The grid** (`-J -S <pp> -R2560`, plugboard-recovery tier, 10 plugs, prose corpora,
4 languages × L40–70 × 3 seeds × 20 runs = 6720 problems; `eval/prepass_grid.sh`,
shard `results-20260707-201051.tsv`), pooled mean %-correct:

| L | `q10` | `i4q10` | `m4q10` | `m6q10` |
|---|---|---|---|---|
| 40 | 72.5 | 72.5 | **80.5** | 78.2 |
| 45 | 82.2 | 81.8 | **92.5** | 85.0 |
| 50 | 91.8 | 86.2 | **92.8** | 92.2 |
| 60 | 98.2 | 95.8 | 99.5 | **99.8** |
| 70 | 99.5 | 98.5 | 99.5 | — |

At *matched R*, mono leads and the IC default trails — but `q10` (no pre-pass) is
**0.594× the `score_iter`** of `i4q10` (it skips the pre-pass stage entirely; `m4q10`
is 0.983×). So matched-R silently under-resources pure quad by ~40%.

**Matched-*compute* settles it.** Give `q10` the ~1.7× restarts its cheapness affords —
`q10 @ R4240` vs `m4q10 @ R2560` (`score_iter` matched within 0.5%; `eval/prepass_grid.sh`
with `CONFIGS="q10" R=4240`, shard `results-20260707-203354.tsv`), pooled mean %-correct:

| L | `q10 @2560` | `q10 @4240` | `m4q10 @2560` | `i4q10 @2560` |
|---|---|---|---|---|
| 40 | 72.8 | 78.9 | **80.4** | 72.7 |
| 45 | 82.4 | 90.0 | **92.3** | 81.6 |
| 50 | 91.7 | **95.6** | 92.5 | 86.2 |
| 55 | 95.7 | **97.5** | 97.3 | 94.4 |
| 60 | 98.1 | 98.5 | **99.5** | 95.6 |
| 70 | 99.5 | **100.0** | 99.6 | 98.6 |

**Findings.**

1. **The IC pre-pass (`i4q10`, the shipped default) is dominated at high R** — below
   both mono *and* pure quad by 5–10pp at L50+, at 1.7× `q10`'s cost. It was tuned for
   the low-R default (`-R 0`, where §6.8 keeps it) and does **not** carry to high R.
2. **At matched compute, pure `q10` ties or beats mono for L≥50** and closes most of the
   L40–45 gap; mono keeps only ~1.5–2.3pp there (≈1 SE at N=60, near noise). Most of
   mono's matched-*R* lead was the compute artifact of q10 being 40% cheaper.
3. **So at high R the pre-pass value collapses — diversity is the currency** (the same
   conclusion the kick-size sweep reached: outcome diversity, not a smarter start, is
   what more budget buys). This is *not* a §6.8 reversal: the mono/IC ordering is
   genuinely **restart-budget-dependent** (IC ≥ mono at `-R 10`; mono ≥ IC, and both
   ≈ pure-quad at matched compute, by `-R 1280`).

**The full R ladder makes the regime boundaries explicit.** Sweeping all three configs
at **L40 across `-R 10 … 81920`** (english+german, `eval/prepass_grid.sh`, shards
`results-20260707-211936.tsv` + `-213222.tsv`, plot `eval/plots/l40_highr_configs.png`)
draws three crossing curves, and the win region is a clean function of `-R`:

| regime | best pre-pass | why | evidence (L40) |
|---|---|---|---|
| **very low R (≈10, ≈ single-shot)** | **`i4q10`** (IC) | almost no restart diversity, so the *seed* is everything and IC's gentle, language-agnostic pre-pass is the most robust single climb | IC wins english L40/50/70 and german L40/70 at `-R 10`; it wins **nowhere above `-R 640`** |
| **low–mid R (≈640–5120)** | **`m4q10`** (mono) | a sharper, language-specific seed that pays off once a few restarts can exploit it | mono leads the mid band both languages |
| **high R (≳10k)** | **`q10`** (pure quad) | seed stops mattering; raw diversity wins, and quad is cheapest per restart | q10 overtakes past ~`-R 5k`, hits ~100% by `-R 81920` (L40 essentially solved) |

`q10` is the **worst** config at low R (no seed help, too few restarts) — the exact mirror
of it being best at high R. `i4q10`'s entire win region is `-R ≤ 640` (almost all at
`-R 10`), which is precisely the regime the default was tuned in (§6.8, and `eval.py`'s
default `-R 10`): it is optimal for a restart-starved default and superseded once real
budget is spent, not wrong.

**Recipe — make it `-R`-aware.** The recommended pre-pass should track the restart budget,
because the harness default targets low R while anyone cracking a hard short message cranks
`-R` high:

- **default / restart-starved (`-R 0 … ~10`):** keep **`-S i4q10`** — the robust single-shot seed.
- **modest budget (`-R ~10² … few·10³`):** **`-S m4q10`** — the language-specific seed pays off.
- **high budget (`-R ≳ 10⁴`, hard short messages):** **pure `-S q10` with a large `-R`** —
  matches/beats mono at matched compute, no cap tuning, cheapest per restart (so more
  restarts per unit time). Avoid the IC pre-pass here.

The **50/50 IC+mono portfolio was measured and is dominated** (lands between IC and mono,
below pure mono — `eval/prepass_portfolio.py`): once mono is the stronger base, mixing in
the weaker IC half only dilutes it.

**Bigram/trigram pre-passes (`-S b4q10` / `-S t4q10`) were also measured and do not beat
the i/m/q envelope** (L40, `-R 10/2560/10240/40960`, english+german, `results-20260707-*.tsv`).
Being sharper than mono, they inherit the worst of both ends: they over-commit and lose at
low R (b4 < m4 at `-R 10`), and they are not diverse like pure quad, so they don't win at
high R. They land *inside* the envelope everywhere — often below mono in german — and the
lone apparent edge (english L40 `-R 40960`: t4 96 vs m4 94, +2pp) is within the n=40 noise
and does not replicate in german. So the useful span of pre-pass orders is exactly the trio
**IC → mono → none**; the intermediate orders just fill the gaps without beating it.

**Scope / caveats.** Plugboard-recovery tier, 4 languages (L40 ladder english+german),
L40–70, `-R 10 … 81920`; the residual mono edge at L40–45 and the `-R 10` IC margins are
within noise (SE ~3–5pp), so the *trend* (crossover, q10→~100%) is the robust signal, not
any single cell. The shipped low-R default is unchanged.

**Follow-up — the same question under the SHIPPED regime (weighted target, R≈85): the
answer is register-dependent, and `m4a10` is not universally right — ✅ MEASURED.**
Everything above used a **quad** target at **R=2560**. The shipped recommendation is
`--score m4a10`: a **weighted** target at R≈40–90. Re-run as a paired greedy-vs-greedy
A/B in exactly that regime (`STAGE=3` in `eval/eval_sa_vs_greedy.py`; L50/70/90, 300
paired trials × 2 seed families per corpus, both arms calibrated to the same 200k
`score_iter`, so the cheaper schedule earns more restarts):

| corpus | pooled `i4a10` − `m4a10` | n | verdict |
|---|---:|---:|---|
| english prose | −1.4 pp [−3.2, +0.5] | 1800 | tie (mono nominally ahead) |
| german prose | **+2.2 pp [+0.7, +3.6]** | 1800 | **IC better** |
| wehrmacht | **−2.2 pp [−3.9, −0.5]** | 1800 | **mono better** |

So the shipped `m4a10` is **right for telegraphic traffic, neutral on English prose, and
measurably worse than `i4a10` on German prose** — the same register-dependence seen in the
SA pre-pass probe (§3.11) and in the scoring tables themselves (§6.6: +20.9pp real traffic
/ −10.2pp prose). It is *not* a universal recommendation, and the README presents it as
one.

The effect is modest (~2pp either way) and both prose cells are near ceiling at L90
(english 95.8/96.1, german 98.8/99.4), which compresses it; the German result is carried
by L50–70. Caveat as in §3.11: the prose corpora are ~477 letters, so excerpts overlap
heavily and the prose CIs are optimistic relative to the wehrmacht ones. **A pilot at n=8
showed mono ahead by +33pp on English; it vanished entirely at n=300** — the third
small-sample result in this work to invert or evaporate, and a standing argument for not
reading single cells.

### 6.11 Correct plugs → text recovery is strongly convex — ✅ MEASURED (`eval/`)

Aggregating **every** plugboard-recovery row across all shards (~289k runs, all
lengths/languages/configs; `eval/plugs_vs_pct.py`, plot
`eval/plots/correct_plugs_vs_pct.png`) gives the mean % letters correct as a function of
how many of the 10 true plugs were recovered:

| #correct | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| mean % letters | 7 | 9 | 14 | 22 | 32 | 42 | 53 | 65 | 81 | 95 | 100 |

The curve is **strongly convex** — it bows well below the linear chord, and each correct
plug is worth *more* than the last (marginal gain ~+2pp at the bottom, ~+16pp at 7→8),
tapering only in the final plug or two. Half the plugs (5/10) buys only ~42% of the text.

- **Why convex:** the plugboard is applied **twice** on each letter's path (input + output),
  so a position decodes correctly only if *both* its contacts route right — roughly a
  squaring effect, so partial board recovery gives sub-linear text recovery.
- **Spurious plugs cost you:** at a fixed #correct, boards with **no** spurious plugs score
  higher (a clean 9-plug subset ≈ 98% vs 9-correct-with-a-spurious ≈ 90%) — a wrong plug
  actively corrupts letters.
- **This is the shape under the "5–9 correct-plug basin gap"** (restart-diversity analysis):
  because text gain per plug is tiny at low #correct, the text-driven climb score has almost
  no gradient to follow until ~5–6 plugs are right, which is why partial boards are hard to
  climb out of and the true basin is a small target for restarts to hit.

**The gap vs length** (`eval/plots/basin_gap_vs_length.png`; english, 20 msgs × R=1000 ×
`-S i4q10`, ~20k converged boards/length). Dumping the converged optima at L40/50/60/70 shows
the distribution is **bimodal at every length** — a junk cluster (0–4 correct, ~99% of boards)
and a solution peak (~10), with **5–9 essentially empty**:

| k=10 (solution) reached | L40 | L50 | L60 | L70 |
|---|---|---|---|---|
| % of restarts | 0.1 | 0.5 | 1.1 | 3.3 |

The **gap location is ~length-invariant** (~5–9); what changes is the **size of the true
basin** — the solution peak grows ~30× from L40 (0.1%) to L70 (3.3%). So a shorter message
does not move the tipping point higher; it *shrinks the watershed* that drains to the true
peak (less signal per plug → fewer kicks land in it), which is the quantitative reason L40
needs ~30× the restarts of L70 (matching the restart sweeps). The L40 aggregate also hides a
zero-basin tail: some short messages have the true board *not* at the score maximum (the
scoring-failure / information-floor case, §6.8 / the `DM` example), unrecoverable at any R —
why L40 exact-recovery plateaus below 100% rather than merely needing more restarts.

### 6.12 Score separability, in bits/symbol — ✅ MEASURED (`eval/`)

The scorer's per-symbol average is a **quadgram cross-entropy in dits** (log₁₀); × log₂10 =
3.322 gives **bits/symbol** (per-position 4-gram surprisal, *not* the ~1–1.5 bit/char Shannon
entropy of English — each position scores a full overlapping quadgram). Measured
(`eval/plots/score_separation_bits.png`, english):

- **Real language ≈ 13.9 bits/sym**, flat with length; **pure random letters ≈ 26.6 bits/sym**.
  So language vs *uniform* gibberish is trivially separable — **0 % overlap at every length**
  (d′ ≥ 12.7 even at L40).
- **The cracking-relevant separation is language vs the best *optimized* junk** (wrong-key
  decrypts the climb has maximized). The true board stays at ~13.9 bits; the best climbed junk
  rises with length — **16.4 bits at L40 → 22.7 at L300**, so the margin **grows from ~2.5 to
  ~8.8 bits**. Short messages give the climb room to overfit a wrong board toward language, so
  the margin thins; at L40 it is only ~4–5 σ and the tails touch — a minority of junk boards
  out-score the truth. That thin short-message margin **is** the scoring-failure / information
  floor (§6.8, §6.11) expressed in score units, and it is why long traffic is easy and L40 hard.

### 6.13 Quad score vs #correct plugs is convex — the three views are one — ✅ MEASURED (`eval/`)

Pairing each converged board's **#correct plugs** with its **quad score** (english, all shards,
`eval/plots/score_vs_correct_plugs.png`) gives a monotonic but **strongly convex** curve: the
score barely moves below ~5 correct plugs, then drops steeply toward the true-board value.
All lengths converge to the same ~13.9 bits/sym at 10 correct (the language score, length-free)
and fan out at 0 correct (junk is worse at longer length — L40 ≈16.5, L120 ≈20.7 bits), so the
**score span per plug grows with length**.

The flat low end **is** the mechanism behind the basin gap: below ~5 correct the score gain per
plug (~0.1 bits at L40) is *smaller than the per-run noise* (±~0.7 bits, §6.12), so the climb
has no gradient to follow and stalls; above ~6 correct each plug is worth ~1–1.5 bits, well
above noise, so the climb cascades to 10 (nothing rests in 6–9). This unifies the three
convexity findings as one underlying shape seen through different variables:

- **§6.11** #correct → **% letters** (convex),
- **§6.12** length → **bits margin** true-vs-junk (grows),
- **§6.13** #correct → **quad score** (convex; span grows with length).

Plotting the same score against **% letters correct** instead of #plugs
(`eval/plots/score_vs_pct_letters.png`) is monotone but *flatter / more linear* — because
%-letters is itself a convex function of #plugs (§6.11), so composing the two convexities
partly cancels. It converges to the same ~13.9 bits at 100 % (length-free) and fans out by
length below that, so score discriminates text recovery far better at long lengths than short.

It is also why *best-by-score works*: score is a faithful, if noisy, monotone proxy for
#correct plugs — but it offers almost no gradient until past the ~5-plug knee, which is why
restart **diversity** (landing past the knee by luck) beats a smarter local climb.

### 6.14 What the residual "few wrong plugs" failures actually are — re-pairing tangles, not the info floor — ✅ MEASURED (`eval/`)

Zooming in on the *near-solution* residual: every converged board that lands **1–3 plugs wrong**
(true rotor key fixed, plugboard climbed best-of-`-R 40` with the standard `-J -S i4q10`; en+de,
L55/60/65, 10 plugs, 720 problems). Two questions per board, plotted in
`eval/plots/few_wrong_tangle.png`.

**1. Search failure or scoring failure?** `gap = true_score − converged_score` (both quad). Of
the **36** few-wrong boards, **35 are search failures** (`gap > 0` — the true board scores
strictly higher, the climb stuck below it); only **1 is a scoring failure**, and it is a near-tie
(gap +0.02 dits, one plug off at 89 % correct — the genuine information floor at L55). The gaps
are *large*, not marginal: even at **1** wrong plug the true board scores **+0.15…+0.95
dits/symbol** higher. So the score is a faithful uphill guide here — this is the opposite of the
information-floor worry; the search simply is not reaching the top.

**2. What are the wrong plugs?** Classifying each wrong plug by whether its letters are steckered
in truth: **TANGLE 61 %** (both endpoints steckered, wired to the *wrong partner*), **HALF 30 %**
(one endpoint steckered), **SPURIOUS 10 %** (neither — an invented plug). So **~90 % of
wrong-plug endpoints are letters the climb correctly identified as steckered** — it just
cross-wired them (e.g. truth G–M, B–P; the climb wired G–Z, M–P). The "swap-component" — the set
of letters that must move **simultaneously** to go converged→truth — has **mean 5.6, median 5.5,
max 10** letters. A ~6-letter component is a **3-plug simultaneous re-pairing**.

**This is a connectivity problem, not an information problem.** Every *partial* step of that
3-plug swap scores lower (the plugboard-applied-twice convexity, §6.11/§6.13), so the
single-toggle greedy climb — which moves 2 letters and accepts only improvements — cannot cross
the valley, even though the score rewards the far side.

**But the geometric picture — "so a 3-plug re-pair would fix it" — does NOT hold operationally
(measured, `eval/repair3_on_tangles.py`).** Re-running the same 720 problems with `--repair3`
(both at `R 40`, so `--repair3` just pays its 1.55× compute) solves only **7 of the 36 tangles
(19 %)**, improves none of the rest, and the swap-component size does *not* predict which get
fixed (≤6 letters: 4/23; >6: 3/13 — roughly equal). Three measured reasons the move is not the
cure: (a) `try_repair_3` is **count-neutral** (it only reshuffles the endpoints of 3 *existing*
plugs), so it cannot express the 30 % HALF / 10 % SPURIOUS structure — those need an
add/remove, not a re-pair; (b) it fires **along the whole trajectory**, so "best-of-R with
`--repair3`" is a *different search*, not a surgical unknot of the identified board; (c) it can
climb to a **scoring-failure** optimum (it broke 5 baseline solves). Its overall gain (+24 exact,
304→328) is mostly **junk→solve escapes (~22), not tangles (7)** — a general deep-optima escape,
not a tangle specialist — and at matched compute those restarts win (§4.7). Same story for
**fix-and-finish** (§4.8): the score points to truth, but *selecting* which letters form the
tangle from a partly-wrong board is unreliable.

**Recognizing a tangle is easier than curing one (measured, `eval/tangle_detector.py`).** Score
features separate a message whose best-of-R board is a solve from one stuck in a tangle at
**AUC ≈ 0.90–0.95** (raw score within a length 0.95; best−2nd 0.91; best−median 0.90) — because a
genuine solution *towers* over its restart pack while a tangle only pokes above it (oracle gap
tiers cleanly: solve +0.00, tangle +0.63, junk +1.22 dits). Consensus (how many restarts hit the
best board) is weak (AUC ~0.67 — short-message solution basins are too small to be re-hit). So a
*detector* is on the table; what is missing is a **profitable action** once flagged — fixing
failed (§4.8), `try_repair_3` catches only ~1/5, and the one untested lever is **reallocating
restart budget** toward flagged-unfinished messages. Absent that, restart diversity stays the
best use of compute. Reproduce: `python3 eval/few_wrong_tangle.py` (characterization),
`eval/repair3_on_tangles.py` (the `--repair3` A/B), `eval/tangle_detector.py` (separability).

---

### 6.15 Weighted all-order scoring `-a` — ✅ SHIPPED (the short-message scoring win); and why it closes the scoring frontier

This is the resolution of the "sharper scoring model" that §6.1–6.14 kept pointing at as the one
lever able to move the short-message floor. The arc, and the ceiling analysis that followed, are
summarized here; the negative rungs are in PR #105, the win in PR #106.

**The dead ends (unseen-tail smoothing) — ❌ MEASURED (PR #105).** Everything that only re-scores
the *unseen / rarely-seen* tail is neutral-to-negative: Laplace add-one and Lidstone add-δ (any δ)
are **neutral** (a flat floor change; a transient +0.6pp German blip at 200 trials collapsed to
~0 at 2000); the **graded** floors — `background` (unseen graded by the monogram product) and
`overlap` (graded by the linear-chain estimate `tri(ABC)·tri(BCD)/bi(BC)`, capped below a hapax) —
are **harmful**, in proportion to the table's unseen fraction (english 15% → ~0; danish 64% → −4
to −6pp at L40–70). Two different unseen-gram estimators (crude monogram, good tri/bi) give the
same negative: it is the *existence* of gradient in the unseen tail that roughens the climb
surface, not the estimator quality. Raising/flattening the floor (`ENIGMA_FLOOR=T`, merge counts
≤ T) is **neutral** — and `T=1 ≡ T=0` byte-identically (a hapax already sits on the floor). So the
low-count tail is closed from both directions, and the discriminating signal lives in the
**well-observed** grams. Reproduce (env probes): `ENIGMA_SMOOTHING=laplace|background|overlap`,
`ENIGMA_DELTA`, `ENIGMA_FLOOR`.

**Jelinek-Mercer (linear) interpolation — ❌ MEASURED (PR #106, `ENIGMA_INTERP`).** Mixing the
orders as *probabilities*, `P(D|ABC) = λ4·c(ABCD)/c(ABC) + λ3·c(BCD)/c(BC) + …`, forces a
**conditional** model (to make the orders dimensionally mixable), reframing the baseline's summed
*joint* `log p(ABCD)`. Uniformly worse (avg Δ L40–140: english −1.6…−4.3, danish −6…−7.5), with a
**non-monotonic** shape (a little mixing is worst, more partially recovers) that fingerprints the
**conditional reframing** — dividing each window by the noisy `count(ABC)`, discarding the joint
information — as the cost, not the interpolation.

**Log-linear interpolation → `-a` — ✅ SHIPPED (PR #106).** Mixing the orders as *log-scores*,
`v = a·log p(ABCD) + b·log p(BCD) + c·log p(CD) + d·log p(D)` — a **geometric (Product-of-Experts)
mixture** (Klakow 1998) that stays in joint space (weights `(1,0,0,0)` = plain quad, byte-identical)
and folds once into a quad-shaped `all8` table at load, so the per-character scorer and the
`--cascade`/`--polish` gain scan are unchanged. **Symmetric folding**: every sub-gram a window contains, each order divided
by its window-multiplicity (tri/2, bi/3, mono/4), so the leading edge grams are included and edge
grams down-weighted. Tuned weights **`(1, 0.6, 0.3, 0.15)`** — an ablation showed all four orders
contribute (`full` +1.68 > `tribi` +1.25 > `tri06` +0.99 aggregate Δ over L40–100), a lighter tri
beats heavy, and the lower orders add **robustness** (tri-only *hurts* danish at short lengths;
bi+mono flip it positive), so `full` is the only non-negative-everywhere choice. **Measured the
first short-message scoring win**: +~1–2pp mean %-correct at L40–100 across all four languages
(2000-trial German confirmed all lengths positive, +2.0 at L50/+1.8 at L100), neutral by L≥190.
Why log-linear wins where linear failed: the geometric mixture is **conjunctive** (a candidate must
look plausible at *every* order at once — sharper key discrimination) and needs no cross-order
normalization (weights absorb the scale in log space). Shipped as the model `-a` / schedule token
`a`; `-S m4a10` is byte-identical to the tuned env recipe. Reproduce: `-a` vs `-q` under
`crackquality`; `ENIGMA_LOGLIN`/`ENIGMA_LOGLIN_SYM` are the experimental weight overrides.

**Why this closes the scoring frontier — the ceiling probes (✅ MEASURED).** With `-a` shipped, two
probes show it is near-optimal on *both* scoring axes, so there is no headroom left for a further
scoring model (learned weights, added features, MERT):

> **Narrowed by §6.4 (`-f`).** The discrimination half of this section stands: the
> fused model adds **−0.0pp** when it only ranks converged boards, so it does not move
> the floor below. The *smoothness* half does not: the sweep below varies the `-a`
> **order weights**, one direction in the space, and an orthogonal permutation-invariant
> term (IC) moves recovery **+3.4pp**. The surface was under-explored along a single
> axis, not exhausted.

- **Discrimination floor ~1%** (`crackquality SPLIT`, MODEL=a). Of every short-message miss, the
  fraction that is a *scoring* failure — the true plugboard not scoring highest even with the
  correct rotor key — is **~0.3–2.3% at L40–60 and ~0 beyond**; the rest (86–96% of trials at L40)
  are *search* failures. The true board **already scores highest ~99% of the time**, so better
  discrimination has almost nothing to recover. Notably `-a`'s scoring-fail % ≈ `-q`'s — **`-a` won
  by cutting *search* failures** (a smoother climb surface, more true boards made reachable), not by
  lifting an information ceiling.
- **Surface smoothness is flat.** An 8× sweep of the order weights (sharp tri 0.3 → smoothest tri
  2.4), single stage at fixed R, moves search-fail% by **<1pp** (within 300-trial noise), with the
  baseline `(1,0.6,0.3,0.15)` at the shallow optimum for both languages. There is no interior
  smoothness that reaches more true basins, and no smooth-vs-exact tradeoff to convert into a
  continuation (staging already supplies the smooth start). Reproduce: `eval/surface_probe.py`.

Verdict: the scoring lever is **tapped** — `-a` is the win and is near-optimal on both discrimination
and surface shape.

### 6.16 The short-message frontier is compute-bound, with no selectable search shortcut — ✅ MEASURED

Because the residual is ~99% *search* failure (§6.15), the question becomes whether a *smarter* use
of restarts beats raw compute. Three measurements say no.

- **Coverage is compute-bound, not floored.** exact-recovery vs R (recommended `-a` config, hard
  lengths) is **still climbing at R=256** — ~+15–25pp per 4× R, no plateau (english L50 15.8→38.3→
  60.8; german L60 53.3→78.3→91.7). The true basin *is* reachable; it is a rare deep target hit
  stochastically, so more shots keep recovering more. Reproduce: `eval/restart_probe.py`.
- **The restart "diversity" is mostly illusory.** At R=64, ~60/64 distinct *exact* boards — but
  deduped by *correct* plugs (oracle), only **~15 distinct progress-states** (a 4× overcount; the
  rest is spurious-plug noise on a few basins). Per-restart depth is mean ~0.7/10 and best ~5–7/10;
  the truth is assembled only in the **union (~9/10)**, never in one board. So the failures are not
  redundant shots clustering on the good basin (which spreading would fix) — they are shots
  scattered across many shallow/wrong basins. Reproduce: `eval/basin_oracle.py` (and the harness
  `DIVERSITY=1`).
- **No truth-free selection signal exists.** Every smart lever — GA recombination (§3.10), a
  truth-targeted kick, or coarse basin-repelling to reclaim the 4× collapse — needs a way to tell
  the ~9/10 real plugs from the noise *without* the oracle. The available proxies fail: **per-plug
  consensus across restarts is only ~1.1/10 correct** (the frequent plugs are decoys), and
  board-fitness picks ~2.5/10 (§3.10). This is the same selection floor that measured GA down.

Verdict: the **only reliable short-message lever is raw compute** — more restarts via `-T`, which
Part 1 shows scales predictably. This is a *clean* close (there is provably no clever search or
scoring trick hiding at short lengths), not a failure to find one. Both the scoring frontier
(§6.15) and the search frontier are now mapped and closed to smarter methods.

### 6.17 The wehrmacht quadgram table had an unbounded reweighting blow-up + a silent uint32 overflow (fixed) — ✅ FIXED

**How the question came up.** Asked how well `wehrmacht_quadgrams.txt` (built by
`eval/build_telegraphic_ngrams.py` from the prose German table, reweighted toward the
published Sullivan & Weierud Appendix-C statistics — §"Overview") actually matches the
quadgrams in the 69 authentic held-out messages. Measuring it directly (not just the
end-to-end recovery rate) surfaced two compounding bugs, not just "sparse published
evidence."

**Root cause 1 — the reweight ratio was unbounded.** The generator scales each prose
quadgram count by `w = r1(A)^0.5 · r1(B)^0.5 · r1(C)^0.5 · r1(D)^0.5 · r3(ABC)^2 · r3(BCD)^2`,
where `r1`/`r3` are telegraphic/prose frequency ratios for single letters and Fig 18's
400 published trigrams. `r3` is a ratio of two *small* counts for most of those 400
trigrams (median prose trigram count in the teens) — for the minority with a near-zero
prose denominator the ratio is denominator noise, not signal. Measured: `r3` ranges
0.26 to **4.08 × 10⁶** over the 400 entries (the max, `QTX`, from a prose trigram count
of 1), and a quadgram multiplies *two* such ratios, squared. The single worst case,
`QSXA` (prose quadgram count 1), reweighted to **8.3 × 10²⁰**.

**Root cause 2 — the C++ loader silently saturated instead of erroring.** `load_counts()`
parsed each count straight into a 32-bit `unsigned` via `sscanf("%u", ...)`. Parsing a
value that doesn't fit the target type is **undefined behaviour** in C; empirically, on
this glibc, it **saturates to `UINT32_MAX`** rather than failing. Consequence, measured
directly on the checked-in table: **843 of 366,266 quadgram entries (0.23%) were tied at
the exact same clamped value** — including real telegraphic markers (`NULL`, `XEIN`,
`SEQU`/`NSEQ`, `XKON`, `DERX`, `XBOX`) that should have had distinct, informative
weights. Those 843 tied entries alone held **~68.5%** of the table's total probability
mass as actually loaded, and the resulting log-probability dynamic range (~9.6 log10
units, vs. the prose table's ~6.6) forced the uint8 quantizer's adaptive
`ngram_scale`/`ngram_bias` (§"Performance notes" — `scale = 255/(vmax−vmin)`) into a
coarser step, blurring resolution across the other 99.97% of the table.

**Measured practical impact (69-message held-out corpus, 6,582 quadgram instances):**
using the counts exactly as the (buggy) binary loaded them, the true plaintext's own
quadgram stream scored **worse on average under the wehrmacht table than under plain
prose** — mean log10 P(actual quadgram) −8.03 (wehrmacht) vs. −5.85 (prose), i.e. the
"telegraphic" table assigned *lower* likelihood to real telegraphic text than the prose
table did, at the raw quad level. 7.9% of the corpus's actual quadgram instances (204
distinct grams) landed on one of the saturated, artificially-tied cells. This didn't
show up as a broken end-to-end result because the tool's measured wehrmacht win
(CLAUDE.md, +20.9pp on real messages) comes overwhelmingly from the **monogram
marginal** (`X` ~7% telegraphic vs. ~0.07% prose, taken verbatim from Fig 17 and
untouched by this bug) folded in by `-a`/`-f`, not from the quad table's own fit — so a
materially broken quad table was masked by a much stronger, unrelated signal.

**Fix.**
- `eval/build_telegraphic_ngrams.py`: clip the per-gram weight, `w = min(w, W_MAX)`
  with `W_MAX = 1000` (overridable via env). Chosen to sit above the well-evidenced
  bulk (median weight ~1.1, p90 ~7) and below the denominator-noise tail (p99 ~99,300),
  and to keep every possible output count under ~4.7 × 10⁸ — 11% of `UINT32_MAX`,
  comfortable headroom against the largest prose quadgram count (~3.7 × 10⁶) even at
  the cap.
- `enigma.cc`'s `load_counts()`: parse into `unsigned long long`, explicitly clamp to
  `UINT32_MAX` with a printed warning if a (future/external) table still overflows,
  instead of relying on `sscanf("%u", ...)`'s undefined/implementation-defined
  overflow behaviour.

**Re-measured after the fix:** the new table's max count is 4.65 × 10⁸ (10.8% of
`UINT32_MAX`, no entries clamped), dynamic range 8.7 log10 units, and its top quadgrams
are now interpretable telegraphic content (`EINS`/`VIER`/`DREI` — spelled-out numbers)
rather than a denominator-noise artifact. Fit to the held-out corpus improved
materially: mean log10 P(actual quadgram) −6.07 (wehrmacht) vs. −5.85 (prose) — near
parity instead of a 2.2-log10-unit deficit — and the Pearson correlation between
observed corpus frequency and table probability (grams seen ≥3×) rose from 0.196 to
**0.267**, now *exceeding* prose's 0.193.

**End-to-end recovery is unchanged within noise**, as expected given the win's real
source is the untouched monogram marginal — paired A/B on the same 69 messages
(`eval/eval_telegraphic.py`, `-c -R 100 -J --polish -a`, prose vs. old-table vs.
new-table, `-T 4`):

| band | n | prose | old (buggy) table | new (fixed) table |
|---|---:|---:|---:|---:|
| <40 | 11 | 9.8 | 18.3 | 8.4 |
| 40–69 | 14 | 16.0 | 26.1 | 23.6 |
| 70–119 | 20 | 33.1 | 58.4 | **68.7** |
| ≥120 | 24 | 75.1 | 96.1 | 95.4 |
| **ALL** | 69 | 40.5 | 58.6 | 59.2 |

Pooled mean is a wash (58.6 → 59.2, well within n=69 noise), with the realistic
70–119-letter band improving (+10.3pp) and the sparse `<40` band (n=11) moving the
other way — too small a sample to read as a regression. All 205 tests pass; `-T`
determinism and the uint8 quantization pipeline are unaffected (this is table content
only, not a hot-path change). The value of the fix is **correctness and future
robustness**, not a measured recovery win: the table is now a well-formed distribution
(no single quadgrams silently tied at an arbitrary ceiling, no reliance on
undefined/implementation-defined parsing behaviour), which matters for any future
retuning of `A`/`B`/`W_MAX` or a similarly-constructed table for another register.

Reproduce: `python3 eval/build_telegraphic_ngrams.py` (regenerates
`ngrams/wehrmacht_*.txt`); `R=100 T=4 python3 eval/eval_telegraphic.py` for the
recovery comparison.

### 6.18 Five languages added (swedish, finnish, icelandic, polish, spanish); `fold_codepoint()` extended for thorn and Polish diacritics — ✅ FIXED

**What was added.** N-gram tables (mono/bi/tri/quad, from the same Practical
Cryptography source as the existing four languages) for `swedish`, `finnish`,
`icelandic`, `polish` and `spanish` — 20 files, already in this tool's native
`<GRAM> <count>` format with no conversion needed.

**What was found while checking them.** `fold_codepoint()` (the function that
folds a table's or the plaintext's accented Unicode letters to an A-Z base —
§"Conventions", the same mechanism §6.9 fixed for German) covers the Latin-1
Supplement block (U+00C0–00FF: à/ä/ö/ø/ñ/ü/ß etc.), which is everything Swedish,
Finnish and Spanish need, and everything Icelandic needs **except one letter**.
Two gaps, both silent (an unfolded code point makes `fold_gram` return -1, and
that record is dropped, counted only in an easy-to-miss "skipped N records"
note):

- **Icelandic þ/Þ (thorn)** *is* in the Latin-1 block (U+00DE/00FE) but the
  existing `lat1[]` table had it as the explicit "not a letter" placeholder
  (shared with the ×/÷ non-letter symbols) — nobody had mapped it, because no
  supported language needed to before now. Measured impact if left unmapped:
  1.5% of the monogram table's mass, rising to 5.4% of the quadgram table's
  (longer grams have more chances to contain a þ).
- **Polish `Ą Ć Ę Ł Ń Ś Ź Ż`** (8 of its 9 diacritic letters — only `Ó` happens
  to land in Latin-1) are in the **Latin Extended-A** block (U+0100–017F),
  a different block `fold_codepoint()` never looked at. Measured impact: 5.5%
  of monograms up to **20.4%** of the quadgram table's mass — not a rounding
  error, close to a fifth of the table silently discarded.

**Fix.** `lat1[]`'s thorn cells now map to `T` (pairing with eth → `D`, mirroring
the voiced/voiceless dental-fricative pair the two letters represent in
Icelandic). Eight new `switch` cases fold the Polish Extended-A letters to their
base (diacritic stripped, same convention as `ß`→`S`; `Ź`/`Ż` — acute vs.
dot-above — both fold to `Z`, same "closest base letter" precedent). No other
new-language code point falls outside the existing coverage (verified by
enumerating every distinct non-ASCII character across all 20 new files and
checking each by hand against the fold table before writing the fix, not just
by absence of a warning after).

**Verification.** All 20 tables load with **zero** "non-mappable character"
records (previously silent data loss, now directly checked — `tests/run_tests.sh`
gained a load-cleanliness guard per language/order, §"Conventions" precedent for
regression-guarding a table-loading bug). A round-trip smoke test per language —
own diacritics folded exactly as the table's (Polish `WŁAŚCIWIE`→`WLASCIWIE`,
Icelandic `ÞETTA`→`TETTA`, `MJÖG`→`MJOG`) — hill-climbs a hidden 2-pair plugboard
back exactly. 230/230 tests pass under g++ and clang++; ASan+UBSan clean, including
a synthetic worst-case string packing every new special character together.

**Not done (flagged, not silently skipped or silently added).** The five new
languages are **not** folded into the full `crack_langs` start-position +
7-model hill-climb matrix (§"Overview" — currently `german english danish
french`): that matrix uses long (~450–480 letter), curated public-domain prose
passages per language, which these five don't have yet, and adding them would
roughly double that matrix's already-noted-as-slow (8+ minutes under
sanitizers) runtime. The lighter load-cleanliness + single-model smoke test
added here is a deliberately smaller guarantee — a real user should not expect
these five to have had the same depth of correctness testing as the original
four until that passage-sourcing work is done.

Reproduce: `./enigma -q -l polish ...` (or any of the five); `bash
tests/run_tests.sh`.

### 6.19 The wehrmacht tables weren't frequency-sorted (cosmetic, fixed) — ✅ FIXED

**Not a scoring bug** — `ngrams_read()`/`load_counts()` parse `<GRAM> <count>` line by
line into a table indexed by the gram itself, so row order has zero effect on loaded
probabilities, scoring, or recovery. Every other bundled table happens to be sorted
descending by frequency (an inherited property of the Practical Cryptography source
files), and `eval/build_telegraphic_ngrams.py` broke that convention silently: it
reweights each prose count (§6.4, §6.17) but wrote rows in `german_<suffix>.txt`'s
original (pre-reweight) order, so up to **~44%** of `wehrmacht_quadgrams.txt`'s rows
ended up out of order relative to their own (new) counts — and monograms were sorted
*alphabetically*, not by frequency, from the start.

**Fix.** `reweight()` now collects `(gram, count)` pairs and sorts by descending count
before writing; the monogram writer sorts by descending `FIG17` percentage instead of
alphabetically. Verified byte-for-byte identical *content* (`sort ngrams/wehrmacht_X.txt`
diffs empty against the pre-fix file) — this is purely a reordering, confirmed 230/230
tests still pass unchanged.

Reproduce: `python3 eval/build_telegraphic_ngrams.py`.

---

## 7. Speed / throughput

At 50 chars, compute budget ≈ number of `score_iter` calls (restarts × passes ×
moves), and the inner loop is **gather-latency-bound**: the dependent chain
`steck[ct[i]] → rows[i][…] → steck[…] → quad8[…]` walks two large tables — the
457 KB per-wheel `subst_array`/`rows` block *and* the 457 KB `quad8` terminal
gather — each a cache-miss-prone dependent load. Every speedup here converts
directly into more restarts — but **any trajectory-changing idea must be validated
at matched wall-clock, not matched `-R`**, and any hot-path edit A/B'd under **both
g++ and clang** per the standing struct-layout cautions.

### 7.1 Restructure the per-pass move-evaluation loop (❌ CLOSED — all three candidates measured/expected negative)

> **Measured outcome (7.1a built and tested).** Item **(a)** — surrogate-ranked
> steepest ascent — was implemented and measured. **The surrogate *ranking* was
> rejected; the incremental *delta* shipped as the opt-in `-D` flag for the mono/IC
> models, then was *removed* — a long-message-only accelerator is net-negative for a
> short-message tool (see the removal note below).** Findings, in order:
> - **The IC surrogate is a poor *ranker*** of quad moves — recovery collapses (L90
>   exact 38% at K=32 vs 52% baseline). The **monogram** surrogate preserves recovery
>   at K≈16–32 (L90 53% vs 52%). So the doc's "monogram *or* IC" was half right:
>   monogram ranks, IC does not.
> - **But surrogate ranking is a net *loss* at the ~50-char target** — ~1.5× *slower*
>   than the plain quad scan, and it only crosses over to a win at ≥150 chars. Reason:
>   once the quad table is warm from climbing one key, a 47-lookup quad decode is so
>   cheap that eliminating 7–13× of them does not offset the per-pass index build +
>   branchy per-candidate delta. The "gather-latency-bound" premise holds for *cold*
>   long-message decodes, not warm short-message ones. **Rejected.**
> - **The delta arithmetic itself was sound** — used *exactly* (not as a surrogate) for
>   the mono/IC models it was byte-identical to the full scan, faster on long ones (mono
>   up to ~27% at 500 chars, IC ~5–7% at ≥300) but ~1.5× *slower* at the ~50-char target.
> - **Removed (shipped, then reverted).** `-D` was net-negative for this tool: it only
>   accelerated mono/IC (never quad, the recommended model — delta-quad was ~2× slower),
>   only won at ≥250–300 chars (slower at the short target this tool exists for), and its
>   `delta_switch_scan` was a second, intricate implementation of the hottest loop that
>   every climb change had to keep consistent (it fought the toggle-fold and forced `-M`
>   to disable it). The capability was speculative and the maintenance cost recurring, so
>   the flag and function were removed; this write-up preserves the finding.
>
> The takeaway that generalizes: **at the ~50-char target, per-candidate scoring is
> not the bottleneck to cut** — the decodes are already cheap; the first-order lever
> stays *more restarts* (raise `-R`). **Item (b) has since been micro-benchmarked and
> rejected too** (a single board already saturates the memory-level parallelism batching
> would add — below), and (c) is expected to re-lose for the same reason (a). With all
> three candidates negative, §7.1 as a whole is closed: the per-pass scan is not where the
> short-message win lives.

*The nominal plan (unchanged for context): three researchers proposed three rewrites
of the **same** per-pass loop over the **same** inverted index; largely mutually
exclusive, so competing implementations of one item, not stackable wins.*

Each `hillclimb()` pass full-quad-scores the 325 `toggle a-b` moves (add / move / merge /
remove, unified) against a constant base board. That is the cost to cut. Candidates:

- **(a) Surrogate-ranked steepest ascent — ❌ built, measured, rejected** (see the
  banner above). Rank all candidates with a **cheap surrogate** and full-quad-score
  only the top-K. Monogram ranks well (K≈16–32), IC does not; but the whole scheme is
  ~1.5× slower at 50 chars (only wins ≥150), because warm short-message quad decodes
  are too cheap to be worth skipping. The exact mono/IC delta form was briefly shipped
  as `-D`, then removed (long-message-only win, net-negative for the short-message target).

- **(b) Memory-level-parallel batch scoring — ❌ micro-benchmarked, measured, rejected.**
  The proposal: score N *different* candidate boards position-major so their N `quad8`
  gathers are in flight together, hiding each other's latency (scalar latency-hiding, not
  the rejected SIMD). **The premise was wrong.** It rests on the score loop being "a
  *dependent* gather chain," but it is not: within one board the `quad8[a][b][c][d]` loads
  are **independent across iterations** — the address chain is the decode window, and the
  only cross-iteration dependency is the `isum` accumulator (a reduction, which does not
  block issuing the next load). So a single board *already* keeps several `quad8` gathers
  in flight; there is little latency left for batching to hide. An isolated micro-benchmark
  of the exact hot loop (base + 325 toggles; warm `quad8`; L = 50/100/250; N = 1..16; g++
  and clang) confirmed it: batching is **slower at every point** — 0.43× at N=1 (pure
  per-board bookkeeping), recovering only to ~**0.85–0.90×** at N=16, never reaching parity.
  This is a strict *upper bound* on (b): the micro-benchmark has zero integration cost,
  whereas the real scan would add board materialisation, cap/fixed filtering and
  best-tracking — so it can only do worse. Not built in-tree. (The bench lives outside the
  repo; this write-up preserves the finding.)

- **(c) Amortized per-pass delta (LOW — expected to re-lose; last resort).** Within
  one pass the base board is constant; precompute the base decode + each window's
  base quad contribution once, and let each candidate recompute only the
  `O(#affected windows)` delta. **This is expected to re-lose for the same reason the
  rejected per-move delta did:** each candidate still does old+new `quad8` lookups on
  affected windows, which is exactly the cost that lost to the 47-gather fused
  rescan — amortizing the *base* scan across the pass does not change per-candidate
  cost. Only revisit if (a)'s surrogate route disappoints, and demand a
  byte-identical `crackquality` as its correctness test.

**Experiment (whichever candidate).** `make bench hillclimb` under g++ and clang
(throughput up; identical scores for (b)/(c)); then `make crackquality` at
**matched wall-clock** (scale `-R` so both configs use equal compute) — the honest
test is recovery at equal compute. Sweep K (for (a)) / N (for (b)).

### 7.2 First-improvement climb — ✅ SHIPPED as `-I` (the matched-compute win)

> **The first idea in this document that beats the baseline at the ~50-char target.**
> Two mechanisms were separable — first-improvement move selection, and don't-look
> bits. First-improvement was built and shipped as opt-in `-I` (and informed move ordering
> as `-J`); don't-look bits were later built and **rejected** (not exact on this global
> objective; neutral-to-negative at matched compute — below).

**What shipped.** `hillclimb()` was steepest-ascent — a full 325-move scan per
accepted move, taking the single best. `-I` switches to **circular first-improvement**:
a cursor sweeps the same fixed 325-pair `toggle a-b` list (each pair covers add / move /
merge / remove by the current state of a and b), applies the **first** improving move, and
**continues from where it accepted** (never restarts at the top). Continuing (vs restarting) is what makes it both efficient (each move examined
~once per sweep, no redundant re-scan of unchanged moves) and unbiased (attention rotates
evenly instead of always favouring low letters). Converged = a full cycle accepts nothing.
No data structure — which is *why it wins where the §7.1a surrogate/delta forms lost*: those cut decode count
but added bookkeeping that a warm short-message decode is too cheap to justify;
first-improvement cuts the *number* of evaluations with zero overhead. Deterministic
(fixed order + acceptance, no RNG) → `-T`-independent; not byte-identical (different
trajectory), so judged on recovery, not equality.

**Measured (the key result).** ~2.8× fewer `score_iter` and ~1.8–2.6× faster wall at 50
chars. It recovers *worse per restart* (a noisier trajectory lands in worse optima), so
it is a **throughput multiplier, not a free win**: pair it with more `-R` and the extra
restarts (restarts never plateau through 256) more than repay the per-restart loss. At
**matched compute** (steepest `-R 8` ≈ first-improve `-R 22` ≈ 55k `score_iter`; `-S iq`;
500 trials; two seeds):
- **Exact recovery:** +8pp at L90 (60.2 vs 52.2) for 10 plugs; +16–24pp for 6 plugs.
- **Mean %-correct:** never worse; +1pp at the hardest 10-plug/L40–50 corner (near the
  §2 information floor, where little is recoverable by *any* method), growing to +6–7pp
  by L60; **+19–23pp across L40–60 for 6 plugs** (wherever real signal exists).

**Because it recovers worse per restart, `-I` is opt-in** — a user at the default `-R 0`
(a single deterministic climb) who enables it gets *worse* results. Documented as "pair with more `-R`."

**Refinements — one rejected, one shipped, one open. The pair is the interesting part:
move ordering helps or hurts depending entirely on whether it varies *per restart*.**

- **Static informed move order — ❌ built, measured, rejected.** Ordering the switch
  moves by ciphertext letter frequency (well-attested/identifiable letters first, §2) —
  the **same order for every restart**. Measured across L40–60 × {6,10} plugs × 2 seeds it
  is **neutral-to-worse**: a tie at the 10-plug/L40–50 corner, **−4-5pp mean %-correct**
  elsewhere, still losing at matched compute (freq `-R 25` vs lex `-R 22`: 45.0 vs 48.7).
  A fixed informed order makes first-improvement commit greedily to high-frequency plugs
  *and does so identically every restart*, converging ~12% faster but to worse optima and
  **collapsing the restart diversity** the regime feeds on.

- **Dynamic (per-restart) best-first order — ✅ SHIPPED as `-J`.** The user's idea: each
  climb first scores *all* moves once against its (perturbed) starting board, sorts, and
  runs the circular first-improvement in that order. The critical difference from static:
  the order is **rebuilt per restart**, so it front-loads good moves *without* collapsing
  diversity — different restarts get different orders. It costs +24% `score_iter`/climb
  (the extra scan), so it is compared at matched compute (`-J -R 18` ≈ `-I -R 22`).
  Measured (500 trials, 2 seeds): a **robust win in the realistic ~10-plug regime**,
  **+2 to +6pp mean %-correct at L40–60**, and a **loss at 6 plugs** (−2 to −7pp, where
  best-first over-commits when few plugs are truly needed). Opt-in, because it is
  regime-dependent; the win lands on the hardest/most-realistic case (10 plugs is the
  crackquality default and standard Wehrmacht). **The prediction that "greedier ⇒ worse"
  was wrong** — it holds only when the order is *fixed across restarts*; a per-restart
  order gets the front-loading benefit while keeping the diversity.

  **The 6-plug loss is over-plugging, and the existing plug cap fixes it — decisively.**
  Capping the climb at the true count (`-S i6q6`, the same known-plug-count prior already
  shipped for `-A`) turns `-J`'s −5pp 6-plug loss into a **+~30pp win vs the uncapped
  baseline at matched compute** (measured both seeds: `-J -R29 -S i6q6` vs `-I -R22 -S iq`,
  ~52k `score_iter`, PAIRS=6 — L40/50/60 +31/+28/+15pp and +32/+28/+17pp). The cap is the
  first-order lever here (both lex and dyn gain ~+20-30pp from it; `-J`+cap beats lex+cap by
  a further ~+3-7pp) — capping shrinks the search space to the identifiable plugs and stops
  the noisy short-message quad score rewarding spurious ones. So the recipe is
  count-dependent: **~10 plugs → `-J` uncapped; known-few plugs → `-J -S iKqK`.** No new
  code — `-S qK` already exists; this is a usage finding.

- **Don't-look bits** (Bentley) — ❌ **built, measured, rejected.** The idea: a move is
  skipped once evaluated-and-inert, revived only when an accepted move touches one of its
  letters. Prototyped as move-level bits on the `-I`/`-J` cursor (revive every move incident
  to the ~4 letters a move changes), behind an opt-in `-K`.

  **The premise was wrong.** Don't-look bits are exact only for a **separable** objective
  (TSP tour length — a move's delta is *local* to its two cities). The plugboard score is a
  **global, overlapping n-gram** objective: toggling `(a,b)` shifts quadgram windows that
  overlap *other* letters, so a move's improvement can depend on a change to a letter it is
  **not** incident to. Skipping an "inert" move can therefore miss a real improvement — the
  filter is a **heuristic, not the pure/trajectory-preserving speedup claimed above**. Direct
  evidence: on *easy* keys `-I` and `-I -K` converge to the identical plaintext (so the
  implementation is correct), but on *hard* keys they land in **different local optima**.

  **As a heuristic it does not pay at matched compute.** It does cut `score_iter` — `-I` to
  ~0.64×, `-J` to ~0.82× (the smaller `-J` saving is because DLB cannot touch `-J`'s up-front
  ordering scan) — buying ~1.2–1.6× more restarts. But at matched `score_iter` (crackquality,
  PAIRS=10, L60/70/80, 60 trials × 2 seeds): **`-I` is noise-level neutral** (−4pp exact at
  L70, +5pp at L80, tie at L60) and **`-J` is a consistent small loss** (−2 to −5pp exact at
  every length). Recovering *worse per restart* (a noisier trajectory) is not repaid by the
  extra restarts — the same failure mode as the static-ordering idea above. Reverted; not
  shipped. A restricted variant (bits active only during the terminal confirming cycles,
  where nothing improves so the heuristic risk is lowest) is conceivable but untested.

### 7.3 Amortize the `-F` IC pre-pass into tier 2 (MEDIUM priority; clean win)

*Merged from two researchers. History §6 latent opportunity #2.*

**Form in this codebase.** With `-F`, `filter_worker` (`:1754`) runs a capped IC
climb per key, then **throws the converged board away** and passes only the key
index; `finish_worker` (`:1826`) re-inits an empty board (`key_to_machine` →
`init_steckerbrett`), and under `-S iq` tier-2 restart 0 re-derives essentially the
same IC seed from scratch. Instead carry each shortlisted key's tier-1
IC-converged `steckerbrett` (26 bytes/key) into tier 2 and seed **restart 0** from
it, skipping the IC pre-pass for survivors. Only restart 0 uses it (others still
perturb) so restarts stay decorrelated.

**Honest payoff.** Medium, bounded by (IC-prepass cost)/(tier-2 cost per key), and
only helps when `-F` is on — but a clean, low-risk win there. Determinism preserved
(tier 1 is already deterministic).

**Experiment.** `make crackquality BASE=<ref>` with `-F -S iq` (recovery equal or
better) and `make bench` (tier-2 time down); confirm `-T` determinism unchanged.

### 7.4 Branch-and-bound early-exit in move scoring (LOW priority)

**Form in this codebase.** During a pass the running best `move_score` is known and
`quad8` values are bounded. While accumulating a candidate, if
`partial_sum + remaining_positions·max_quad8` can no longer beat the incumbent,
abort (check every ~8 positions). Applies to move-ranking scores only.

**Honest payoff.** Low — the message is short (~47 quadgrams) so the abort saves few
positions and per-check overhead may eat it. More attractive after §7.1 provides a
good incumbent. Bound is exact → `crackquality` unchanged; pure speed check.

**Experiment.** `make bench hillclimb`; numerically identical results.

### 7.5 Data-dependent prefetch of the `quad8` inner row (LOW priority)

**Form in this codebase.** In `quadgram_score_decode` (`:656`) the next row base
`quad8[b][c][d]` is known one iteration before consumption (the sliding window
shifts), so issue a portable `__builtin_prefetch` for it. A few lines, easily
reverted.

**Honest payoff.** Low and uncertain — may be swallowed by out-of-order execution
already covering the latency; prefetch can also hurt by evicting useful lines. Try
and discard.

**Experiment.** `make bench search` and `hillclimb` under g++ and clang; keep only
if both improve or stay flat.

### 7.6 Finer work items for `-F` load balance (LOW priority)

**Form in this codebase.** `finish_worker` hands out one key at a time; each runs
its `-R` restarts serially, so a straggler key can idle threads. Emit
`(key, restart)` work items. Determinism holds because restart RNG is seeded from
`opt_seed + flat key index` (thread-independent); reduce per-key then into global.

**Honest payoff.** Low–medium — only matters when the `-F` shortlist size is
comparable to or smaller than the thread count, or restart cost varies a lot
(re-pair / SA tails). Marginal when keys ≫ threads.

**Experiment.** `make bench SCALE=1` with `-F` and high `-R` on a short message;
utilization at `-T=4/8` vs the key-granular path; `crackquality` unchanged.

### 7.7 Not recommended: shrinking / re-laying-out the quad table

Storing only top-frequency quadgrams in a compact hash adds an indirection on the
critical gather (likely net-negative) and risks discarding quadgrams that matter
for discrimination (a quality regression). The layout-tiling sub-variant is safer
but likely small payoff. Only pursue as a both-axes win under `make crackquality` +
`make bench`; low priority.

---

### 7.8 Cap-as-target climb rule — ✅ SHIPPED as `-M`

**Idea.** The per-stage `-S` plug cap is enforced only as a *growth ceiling*: at/over
the cap the switch scan blocks a brand-new **add** (both ends free) but still allows
count-preserving **reshuffles** (endpoint-moves, `Δ=0`). So when a big per-restart kick
(`--random K`, default `K=10`) lands on a small stage cap (`i4`, `q6/q10`), the board *arrives
over the cap* and the climb can converge still holding more plugs than the cap — the cap
never pulls it down. `-M` makes the cap a strict **descent target**: at/over the cap, only
**count-reducing** moves are allowed — a **merge** (both ends already plugged to different
partners, `Δ=−1`) or a **remove** (`Δ=−1`) — blocking adds *and* reshuffles, so the climb
must shed plugs to the cap. The **merge** is deliberately kept (not "removal-only"): it is
the strongest descent move (−1 *and* score-improving in one step); dropping it (a strict
removal-only rule, "variant 2") was reasoned to be worse and not built.

**Result (matched compute, mean %-correct — the graded metric; `--score i4qK --random N`).** Because
`-M` is *cheaper* per climb (up to ~2.7× fewer `score_iter` in the `q6` regime — quad
converges from a tidy ≤cap basin), it is compared at matched compute (baseline `-R 26` vs
`-M` at the higher `-R` its lower per-climb cost buys). The win **grows as the true plug
count falls below the cap**:

| regime | N4 | N6 | N8 | N10 |
|---|---|---|---|---|
| `PAIRS=10`, `q10` cap, 4 seeds | +4.5 | −0.4 | −0.1 | **+2.6** |
| `PAIRS=6`,  `q6`  cap, 4 seeds | +2.7 | +3.9 | +5.8 | **+7.1** |

On realistic 10-plug boards it is neutral at the sweet-spot kicks (`N≈6–8`, within noise)
and a solid **+2.6pp at `N=10`** (the true-count kick, the best operating point; robust
across all lengths). On **known-few-plug** boards (`q6`) it is a large win that grows with
kick size and **concentrates at the short/hard end** — `N=10` gives **+20.6pp at L40**,
+8.9 at L50, tapering as length eases. Every miss the baseline makes here is it wasting the
climb reshuffling an over-cap board; `-M` spends that budget descending to the true count.

**Why the IC-cap plateau (§ "already shipped") is real without `-M`.** The reason
`-S i3q…`…`i6q…` tie by default is exactly this: on the dominant perturbed restarts the
board is already over the IC cap, so the cap can't build *or* prune it — it only gates a
re-add. `-M` is what makes a tight cap actually bite. So the recipe is regime-dependent:
`~10 plugs → --score i4q10 --random N` (kick near 10, `-M` optional, small gain); `known-few → --score i4qK -M`
(cap at the true count `K`, `-M` on — the large win). Off by default (needs `-c`),
`-T`-deterministic, full suite green.

---

### 7.9 Plugboard-keyed score memoization (a `score_iter` transposition table) — ❌ MEASURED, REJECTED (net loss); shipped as the `--score-tt` diagnostic

**Idea.** Within one rotor key and one scoring model, `score_iter(m)` is a pure function
of the *plugboard* alone (the rotor stack `m.rows` is fixed per key). So a
**transposition table keyed by the plugboard** could serve a board's score from a cache
the second time it is reached, skipping the fused decode/score loop entirely — the
canonical TT-as-memoization move. Reuses the §6.14 / PR #100 Zobrist board hash. The
question this settles: **how much of the plugboard climb's scoring is actually repeated
work a cache could recover?**

**Build (`--score-tt`, off by default).** `score_iter` computes `board_hash(steckerbrett)`,
probes a per-worker direct-mapped table, and on a matching entry — exact board *and* the
current rotor-key generation *and* the scoring model all agree — returns the stored score.
Any mismatch (hash collision, a new key, a different stage's model) is a miss, never a
wrong value, so the cache is **semantically transparent**: results are byte-identical and
`-T`-invariant with it on or off (verified: `-T1 ≡ -T4`, cache-on ≡ cache-off decrypt,
182/182 tests green). The generation counter is bumped in `setup_mapping` (once per key),
so the cache persists across a key's restarts — exactly where cross-restart reuse would
show up — and is invalidated in O(1) when the key changes. It reports the hit rate (the
fraction of `score_iter` served from cache) at the end.

**Measurement (English, true rotor key fixed, plugboard hill-climbed, `-q`, min-of-reps).**

| length | R=1 (intra-climb only) | R=640 | cross-restart Δ | wall @R640 (off → on) |
|---|---|---|---|---|
| L50  | 6.7% | 7.2% | **+0.5 pp** | 0.36 s → 0.71 s (**~2× slower**) |
| L171 | 8.9% | 11.3% | **+2.4 pp** | 1.04 s → 1.21 s (slower) |

**Findings.**
- **Only ~7–13% of scores are cacheable, and the rate is flat in restarts and in table
  size.** R=1→640 (640× more climbs) adds just 0.5–2.4 pp; a 32× larger table (2^23 = 8M
  slots) adds ~0–2 pp — so eviction is *not* the limiter. Almost every hit is a repeat
  *within a single climb*; **cross-restart score reuse is essentially nil.** The boards a
  plugboard climb probes are ~90% unique within a climb and ~99% unique across restarts.
- **It is a net wall-time loss at every config.** A TT's whole value is cross-branch reuse,
  and there is almost none here to harvest; meanwhile every call pays a `board_hash`
  (26 XORs) + `memcmp` + `memcpy` against a multi-MB cache-cold table, which costs more
  than the ≤13% of decodes it saves. Larger tables make it worse (the 400 MB variant ran
  ~5× slower from memory thrashing).

**Independent confirmation at a heavy, realistic config (20 × L50 english, pooled).** Rerun
with `-c --polish -S m4q10 -R 5120 --random 10` (8× the restarts, a two-model staged
climb, and the best-board finisher — i.e. every knob that *could* create cross-restart reuse):
the hit rate is **7.9%**, barely above the 7.2% at R=640, and the T=1-vs-T=8 gap — the direct
measure of how much cross-restart reuse a single pooled cache can concentrate — is **+0.05 pp**
(7.91% at T=1 vs 7.86% at T=8). And the cost gets *worse* with the bigger budget: cache-on is
**~3× slower** here (0.52 s → 1.59 s/msg at T=8, vs ~2× at R=640), because every one of the
~22 M `score_iter` calls pays the hash+compare while only ~8% skip a decode — a real TT
amortises *better* with more work, this one amortises worse. Recovery is byte-identical across
cache-on/off and T=1/T=8 (99.5% mean, 95% exact), reconfirming the transparency. So more
restarts sharpen the verdict rather than soften it.

**Why (the diagnostic part).** This is the same wall §6.14 / PR #100 hit, one level lower:
at `--random 10` the *converged* restart boards are near-totally distinct (basin
diversity), and this shows the boards *probed along the way* barely overlap either. The
plugboard climb is a nearly-injective walk over board space — it does not revisit — so
there is no transposition structure for a table to exploit. The `-R` restart budget is
better spent on *more restarts*, not on remembering ones already done (consistent with the
"restarts are the primary quality lever" playbook).

**Disposition.** The speedup is rejected, but `--score-tt` is kept as an off-by-default
**diagnostic** (like `--restart-tt` / `--true-key`): it directly measures "is there
score-reuse to exploit?" for the diversity-search line of work, and the answer — *no,
~7–13% and all intra-climb* — is the artifact. Needs `-c`; quad or any model; per-worker
cache so no locking; the default path is byte-identical.

### 7.10 Leftmost wheel's ring × start collapses to a pure offset — ✅ SHIPPED (rotor-keyspace, not plugboard)

**A different axis from everything else in this section.** §7.1–7.9 are all about the
per-character plugboard score loop; this is about the size of the *rotor-key* search
space itself — `build_key_space()` — and it applies whenever ring **and** start are both
wildcarded for a wheel, independent of scoring model or plugboard climb. Origin: a user
observation that for most texts only the *offset* between a wheel's ring and start
matters, and asked whether ring enumeration could be thinned out.

**The mechanism, precisely (verified against `setup_mapping()`, not assumed).** The
notch/stepping check for each wheel — `notch[w1][g1]` (wheel 1, checked every character)
and `notch[w2][g2]` (wheel 2) — tests the **raw window position**, never the
ring-adjusted offset; ring only enters at the final lookup,
`sa[mod26(g0-r0)][mod26(g1-r1)][mod26(g2-r2)]`. So shifting a wheel's ring and start by
the same δ (preserving the offset used for its own substitution) leaves that wheel's
per-character substitution exactly correct throughout the message — *provided nothing
downstream depends on that wheel's own absolute window value*. That proviso is met by
**wheel 0 (leftmost) and only wheel 0**: it has no notch check of its own (there is no
wheel further left for it to step), so nothing about its trajectory feeds forward into
any stepping decision — its own advancement is driven entirely by wheel 1's notch, a pure
additive step-count wholly untouched by ring0 or start0. Wheels 1 and 2's own window
values, by contrast, gate further stepping (wheel 1's notch triggers wheel 0 *and* its own
double-step; wheel 2's notch drives wheel 1), so shifting *their* ring+start together
shifts *when* those downstream events fire — an approximation, not an equivalence.

**Measured, to confirm the derivation rather than trust it.** Ring+start shifted together
by δ = 1..25, single characters compared against the true decode (127-letter message,
enough for wheel 0 to step several times over):

| wheel shifted | δ=1 | δ=2 | δ=3 | δ=5 | δ=13 | δ=25 |
|---|---:|---:|---:|---:|---:|---:|
| **wheel 0 (leftmost)** | 0 mismatches | 0 | 0 | 0 | 0 | 0 (exact at every δ) |
| wheel 1 (middle) | 26 | 51 | 77 | 27 | — | — |
| wheel 2 (rightmost, 98-letter msg) | 2 | 6 | 10 | 18 | 49 | — |

Wheel 0 is **exact at every tested δ, unconditionally** — not "usually," not "unless an
unfortunate step occurs." Wheels 1/2 show real, generally-growing corruption even at
small δ (wheel 1's is large and non-monotonic because its own notch feeds the double-step
directly) — confirming they need the cautious, *empirically-validated-before-shipping*
treatment, not this unconditional one. (A companion simulation swept realistic rotor/notch
combinations for wheel 0's stepping alone, finding it never steps at all — an even
stronger, simpler win *when true* — in 70–90% of cases at the 40–90-letter lengths this
tool targets; but the shift-equivalence above makes that distinction moot for wheel 0: an
exact win either way, whether it steps or not.)

**Shipped: `build_key_space()`**, right after the per-wheel ring/start range setup — when
`opt_ringstellung[0]=='.'` **and** `opt_grundstellung[0]=='.'` (both wildcarded; if only
one is, every value of it is a distinct necessary offset and no redundancy exists),
`ring0`'s range collapses from the full 0–25 to the single sentinel `0`, leaving `start0`
to enumerate the 26 offsets directly — the exact same pattern already used for the M4
Greek wheel's `(start − ring)` collapse (`offset_list` a few lines above), because the
underlying reason is identical: nothing depends on that wheel's *absolute* value, only
relative offset. No other code changes needed — `rc`/`gc`/`rsize` are already generic
products over the per-wheel ranges, so pinning `rc[0]` to 1 propagates through
`search_worker`'s mixed-radix decode and the parallel chunking automatically. Applies
uniformly to standard, Norway, and M4 (M4's wheel 0 is the leftmost of its 3 *stepping*
wheels — distinct from the already-collapsed static Greek wheel).

**Measured reduction and verification.** `-r ..Z -g ..P` (wheel 0 both wildcarded, wheels
1/2 fixed): 456,976 combinations → 17,576 (exactly ÷26, as predicted), 0.387s → 0.094s
single-threaded on the same key. Correct plaintext recovered in every mode tested
(standard, Norway, M4; single wheel-order and full reflector+wheel-order+ring0+start0
wildcard together). Reported ring position for wheel 0 is always `A` — the direct
analogue of the Greek wheel's already-documented unidentifiable ring. 230/230 tests pass;
ASan+UBSan clean.

**Scope — this is the "safe half" of the user's idea; the risky half is still open.**
Wheels 1 and 2 do **not** get this treatment (confirmed above: real, non-trivial
corruption even at small δ) — extending sparse/approximate ring-sampling to them, betting
that n-gram scoring tolerates the resulting handful of wrong letters, is a genuinely
different, riskier lever that needs a real `crackquality`-style matched-compute A/B
(does skipping ring values net-improve recovery once the freed compute buys something
else, or does the corruption occasionally cost the true key against an unrelated wrong
one?) before it could ship. Not yet built or measured.

---

## 8. Novel / higher-risk

- **Distributional-assignment plug seed (§6.1's cousin, via Hungarian/matching).**
  The rotor-only decrypt's monogram/symmetrized-bigram histogram is the true-language
  histogram permuted by `steck`. Build a 26×26 benefit matrix (log-likelihood gain of
  each swap toward the `-l` tables) and solve the best *symmetric* matching (greedy
  max-weight, or Kuhn-Munkres restricted to involutions) as a seed, wired where the IC
  pre-pass runs. *Why 50 chars:* injects the actual language shape and can place
  several correct plugs *simultaneously* from one global assignment. *Honest payoff:*
  medium — monogram matching is sampling-noisy at 50 letters; bigram matching still
  carries signal; even 2–3 correct seeded plugs change the basin. May merely tie IC.
  Deterministic, race-free. *Measure:* swap the IC pre-pass for the assignment seed,
  `crackquality SPLIT=1`, L40–120, matched `-R`; also report the fraction of true
  plugs present in the seed.

- **Sinkhorn continuous relaxation.** One-liner / research bet: relax `steck` to a
  doubly-stochastic matrix, optimize a smooth mono/bigram surrogate by
  projected-gradient/Sinkhorn, project to the nearest involution, hand to the climb.
  The true quad objective is not cheaply relaxable, so only a mono/bigram relaxation
  is tractable and it **risks degenerating into the assignment seed above** with much
  more machinery — the highest-complexity item here. Build the assignment seed first;
  escalate only if it wins but leaves headroom.

- **Modified-Lam adaptive SA schedule.** Replace the fixed geometric cool-down with
  a Lam/Delosme schedule targeting a ~44% acceptance rate, adjusting T online from the
  measured accept ratio — retiring the hand-tuned `χ0=0.12`. *Honest payoff:* low–medium
  — unlikely to beat a *well-tuned* `χ0` at the two lengths it was tuned on, but should
  match it there and win at lengths/board-sizes/languages it was never tuned for. A
  robustness / one-fewer-magic-number win, not a capability jump; the shipped SA already
  does acceptance-ratio calibration for the *start* T, so gains may be marginal.
  *Measure:* `crackquality` across the full L40–250 range and a second language/board
  size the current `χ0` was **not** tuned on, at equal `score_iter` budget; the claim is
  "matches tuned `χ0` where tuned, beats it elsewhere."

- **Score-guided adaptive neighborhood.** Bias `(a,b)` proposals (SA and greedy
  ordering) toward letters sitting in the lowest-scoring quadgram windows of the current
  decode — spend moves where the plaintext looks least language-like
  (`archived/SIMULATED_ANNEALING.md` §14). *Honest payoff:* low — speeds convergence (more useful
  moves/budget) rather than changing the reachable optimum; helps mainly the tight-budget
  corner and risks self-reinforcing bias. **Hot-path hazard:** per-letter blame
  attribution must come from the fused scorer *without* a second pass / `num_plaintext`
  round-trip (the documented layout caution) — easy to regress. `make bench` first, then
  `crackquality` at small budgets.

---

## 9. Prioritized shortlist and measurement plan

### Open items at a glance

Every idea below is genuinely open — audited against `enigma.cc`'s option surface and
`eval/`'s scripts; none has a hidden shipped/measured verdict in its body text. (Two
sections that looked open, §6.6 and §4.10/§4.11, turned out to already be shipped and
have been corrected; §4.5/§4.6's climb-ordering sub-idea was found built/rejected as the
now-removed `--infl-order` and is noted below, but its other two applications are still
open.) Full detail is in each section; this table is a scan-only index, not a substitute.

| § | Idea | Priority | One-line status |
|---|---|---|---|
| 3.3 | ILS with incumbent-walk acceptance | HIGH | top of "if you do three things" below; cheapest plausible win over `-R` |
| 3.4 | Parallel tempering / replica exchange | MEDIUM | "a better SA," so inherits SA's ceiling as a peer of `-R` |
| 3.5 | Tabu search over the climb | MEDIUM–LOW | survey consensus: comparable to SA/hill-climb, not superior |
| 3.7 | Multi-seed IC basin-hopping | LOW–MEDIUM | cheaper cousin of §3.6 exhaustion; small M limits coverage |
| 3.8 | Cross-entropy / EDA plug marginals | LOW | gated behind §3.1, which was built and rejected |
| 3.9 | Adaptive restart budget / early-stop | LOW | throughput/allocation only, not a new capability |
| 4.1 | Guided (ILS-style) kick | MEDIUM | refines the already-tuned uniform `k=8` kick |
| 4.2 | Informed single-plug seeding (GRASP) | LOW–MEDIUM | adjacent to what `-S iq` already does implicitly |
| 4.3 | Evidence-restricted move set | MEDIUM | exact-prune half risk-free; soft half needs both-sides care |
| 4.4 | Surrogate-biased SA proposals | LOW–MEDIUM | risks collapsing the exploration that makes SA useful |
| 4.5/4.6 | Influence-weighted `--exhaust`/kick | MEDIUM | climb-order variant tried & removed (`--infl-order`); exhaust/kick untested |
| 5.1 | Crib-driven bombe closure deduction | HIGH (crib-only) | real deduction, unlike shipped `--crib-file`; needs a new harness |
| 5.2 | Crib-drag soft seeding | MEDIUM (crib-only) | lighter cousin of 5.1; also needs the new harness |
| 6.3 | Soft MDL / plug-count prior | LOW–MEDIUM | doc's own "weakest fit to the diagnosis" |
| 6.5 | Finer (uint16) score accumulation | LOW | conflicts with the shipped uint8 cache-residency win |
| 7.3 | Amortize `-F` IC pre-pass into tier 2 | MEDIUM | confirmed: `finish_worker` still discards the tier-1 board |
| 7.4 | Branch-and-bound early-exit | LOW | exact bound; distinct from §4.6's rejected approximate prune |
| 7.5 | `quad8` row prefetch | LOW | may already be covered by out-of-order execution |
| 7.6 | Finer `-F` work items (key,restart) | LOW | confirmed: `finish_worker` still one key at a time |
| 7.7 | Quad table shrink/relayout | LOW | a priori judgment call, no empirical test — not really "open" |

### Planned test additions: `CRACKQUALITY_TESTS.md`

The earlier top item here — *"build the full-crack tier that gates everything"* —
has been **de-scoped**. Its headline justification was cross-key plug
marginalization (§5.3), now **de-prioritized** (the correlated-noise argument that
sank §3.1 — see §5.3). What survives is three focused, cheap test additions,
specified concretely in the dedicated root-level doc **`CRACKQUALITY_TESTS.md`**:

- **A one-time scoring-failure gate (unknown key).** The cheap survivor of the
  full-crack tier: run *once* at the `START` scope (wheels/reflector fixed, ring
  pinned `AAA`, start wildcarded, **unfiltered** so `-F` filter-recall cannot
  confound the split), ~15 min, to answer whether the *objective* ever misranks a
  wrong (key, board) above the true one. If it never does, §6 stays parked; if it
  does, §6 (and its MDL prior §6.3 / calibration §6.7) re-opens. This is the only
  reason left to wildcard the rotor key, and it is a diagnostic, not a suite.
- **`-F` prefilter validation.** Throughput is documented; **recall is not** — how
  often tier-1's capped-IC climb drops the *true* key before tier-2. A `--true-key`
  hook + a `recall@N` sweep (tier-1 only, cheap) and a matched-`score_iter`
  filtered-vs-unfiltered recovery A/B. The thorough `-F` test the docs lack.
- **Restart-diversity diagnostics.** Measure directly how often restarts collapse
  into the same local optimum (distinct-optima / global-best-hit per key) and rank
  the *shipped* knobs (`--random` kick size, steepest/`-I`/`-J`) by basin coverage
  at matched compute. Instrumentation + comparison only — no new anti-convergence
  algorithm yet (deferred), just the measurement that would size one.

The identifiability facts these rest on are verified (turnover reads the absolute
start with no ring term, `enigma.cc:668–680`, so with ring pinned `AAA` the start
is identifiable and the left ring/start are a pure ×26 degeneracy). Read
`CRACKQUALITY_TESTS.md` before building any of the three.

### If you do three things (on the existing plugboard tier)

> The original #1 here — cross-restart consensus / plug fixation (§3.1) — was built
> and **measured as compute-neutral-to-negative** (it loses to a higher `-R` at equal
> compute, and becomes a no-op as `R` grows because best-of-`R` saturates). It is
> rejected; see §3.1. The list below is the reassigned top three.

1. **~~Portfolio `max(greedy, SA)` (§3.2)~~ — built, measured, REJECTED.** The reassigned
   #1 met the same fate as the original. It was *not* the expected failure (nested
   solved-sets): greedy and SA are genuinely complementary (+10–17pp union at double budget).
   But at matched compute the budget split cancels that gain — `max(greedy@½, SA@½)` is
   neutral-to-negative vs the best single solver @full (~−3pp avg, worst −7pp over 2 seeds),
   and strictly worse whenever one solver dominates (SA usually did). The lesson: run the best
   *single* solver at full budget; capturing the real complementarity needs a single blended
   trajectory, not a post-hoc max (→ item 3, ILS). See §3.2.

2. **A per-climb throughput lever — first-improvement (§7.2). ✅ DONE, shipped as `-I`.**
   The prediction held: cutting the *number* of evaluations (not the cost of each) with
   zero overhead is what beats the baseline at 50 chars, where §7.1a's decode-cheapening-
   plus-bookkeeping lost. `-I` is ~2.8× cheaper per climb; paired with more `-R` it wins
   at matched compute (+8pp exact / +1–23pp mean, scaling with available signal — §7.2).
   Opt-in, because it recovers worse per restart. **Dynamic per-restart best-first move
   ordering shipped as `-J`** (a matched-compute win on the realistic ~10-plug regime,
   +2–6pp mean; §7.2). **Don't-look bits were built and rejected** — not exact on a global
   n-gram objective (unlike TSP), so a heuristic; neutral for `-I`, a small loss for `-J` at
   matched compute (§7.2). *Static* (fixed-across-restarts) informed ordering was **rejected**
   too — greedier *and* diversity-collapsing; per-restart `-J` keeps the front-loading (§7.2).

3. **True ILS with incumbent-walk acceptance (§3.3).** The cheapest *structural*
   change to how restarts are spent: instead of always relaunching from the fixed seed,
   carry a walk incumbent and accept within `δ`, so small perturbations chain across
   plateaus. It changes the restart *trajectory* rather than post-processing its output
   (where §3.1 failed), and `δ=0, k=8` reproduces today's `-R` exactly as a control.

Then, as budget allows: quick trigram-target tuning (§6.1) and back-off smoothing (§6.2)
to de-risk the scoring axis; the `-F` IC-pre-pass amortization (§7.3, clean win under
`-F`); and partial first-pair exhaustion (§3.6) or parallel tempering (§3.4) once the
throughput headroom from item 2 makes their extra cost affordable.

### For real operational traffic (a different goal than the current benchmark)

The crib/bombe-closure work (§5.1–5.2) and the telegraphic corpus (§6.6) are the genuine
50-char-barrier breakers. The telegraphic corpus is **done** — shipped as the `wehrmacht`
language and measured at +20.9pp on real traffic via its own harness
(`eval/eval_telegraphic.py`) — while crib/bombe-closure remains open. Both are/were
**invisible to `make crackquality` as written**, which needs its own harness tier per
idea (crib-planting; the telegraphic real-message set already exists). Do not expect
either to move the `crackquality` numbers; do not judge them by it.

### Measurement discipline (applies to every idea above)

- **Compute-normalize on total `score_iter` calls** (the per-machine counter exists) or
  on matched wall-clock — never matched `-R` — for any idea whose point is to buy more
  restarts. The baseline is always **a higher `-R` at equal compute**, since `-R` never
  plateaus through 256.
- **`make crackquality SPLIT=1`** at L40–120 (+ shorter/longer bins where noted) is the
  arbiter of recovery; watch exact-recovery, mean %-correct, L50/L90, and the
  scoring-vs-search split.
- **`make bench` under both g++ and clang** for anything touching `hillclimb`,
  `anneal_once`, the scorers, or `struct machine` — the hot-path layout is load-bearing
  (20–60% swings on clang/ARM if disturbed).
- **Keep all randomness on the per-key splitmix64 stream** (`opt_seed + flat key index`)
  so `-T` results stay byte-identical; pin `ENIGMA_SEED=0` for A/B runs.
- **Exact-speedup ideas** (§4.3 pruning, §7.1c, §7.4) must produce **byte-identical**
  `crackquality` output — that byte-identity *is* their correctness test.

### Do not re-propose (already shipped or measured-and-rejected)

Shipped: steepest-ascent moves, `-R` restarts, `-S iq` staging + caps, `-F` pre-filter,
`-A` simulated annealing, `-s` fixed plugs, uint8 tables + hapax floor,
**`-I` circular first-improvement** (opt-in; ~2.8× cheaper/climb; a matched-compute
recovery win when paired with more `-R` — §7.2), **`-J` first-improvement + dynamic
per-restart best-first ordering** (opt-in; +2–6pp mean on the realistic ~10-plug regime
at matched compute; regime-dependent — §7.2), **`-M` cap-as-target** (opt-in; at/over the
`-S` cap only merge/remove moves; matched-compute win growing as true plugs fall below the
cap — neutral-to-+2.6pp at 10 plugs, +3–20pp known-few-plug; cheaper per climb — §7.8).
Rejected (with reason): **mono pre-pass for `-A`** (`ENIGMA_SA_STAGES`, §3.11 — built
and measured on both substrates: +2.3pp on telegraphic but **−2.6pp on English prose**,
register-dependent, and it improves the *losing* arm without making SA beat greedy at
any length; kept as an off-by-default probe, no CLI flag);
**static (fixed-across-restarts) informed move order** (greedy
*and* diversity-collapsing — §7.2); **ciphertext/plaintext-influence move ordering**
(`--infl-order`, §4.6 — ties `-J` at L40–55, a clean −4…−6pp loss from L60 up; *removed*
from the CLI, not just deprioritized); **§7.1a surrogate-ranked ascent** (built; ~1.5× slower at 50
chars — warm short-message quad decodes too cheap to skip; only wins ≥150 chars; the IC
*ranker* also collapses recovery — §7.1); **cross-restart consensus / plug fixation
(§3.1** — built and measured compute-neutral-to-negative; loses to a higher `-R` at
equal compute and no-ops as `R` grows because best-of-`R` saturates; swept over threshold
× elite size × `R` × PAIRS × length × seed**)**; incremental **quad** delta-scoring
(~2× slower); chi-squared scoring/tier-1 (gameable, far worse recall); 3-opt / 3-plug re-pair (cost >
gain); SIMD / `-march=native` (latency-bound — but note §7.1b is scalar MLP, orthogonal);
GPU (gather-bound, breaks the portable design); rotor-stepping reuse (slower); SA reheating
/ chain-length sweeps (no help); `χ0=0.8` (lost 2×); plugboard-free IC scan pre-filter
(~0% recall); primary 5-grams / 4-bit scores (too sparse / coarse); uncapped tier-1 IC
climb (overfits); **back-off / interpolated quadgram smoothing** (`--backoff`, §6.2 —
conditional form −20pp, joint-floor form neutral; harsh flat floor is a feature);
**trigram-target-at-short-end** (§6.1 — quad wins at 10 plugs with the tuned `-S i4qK`
recipe; the bare-model "trigram wins" was a weak-baseline artifact);
**lower-order intermediate `--score` stage** (`-S i4m4q10` mono pre-pass, §6.8 —
seductive English-only/bare-climb win that reverses to −5…−8pp at L90 in
German/Danish under matched-per-language compute + `-J`; does not generalise).

---

## References

Provided by the research inputs; several origin PDFs returned HTTP 403 through the
egress proxy, so specifics were cross-checked against search-result abstracts and
training knowledge. **Citations support *evidence* for a direction, not claimed
measurements in this repo, and page numbers / exact figures marked "unverified" below
should be confirmed before being quoted as fact.**

- J. Gillogly. *Ciphertext-only cryptanalysis of Enigma.* Cryptologia 19(4), 1995.
  (~647-letter demo: IC on an empty board + separate plugboard hill-climb. *Page range
  not verified — cite volume/issue only.*)
- H. Williams. *Applying statistical language recognition techniques in the ciphertext-only
  cryptanalysis of Enigma.* Cryptologia 24(1), 2000. (~500-letter / 10-plug; improved
  statistic. The claim that the *unigram Sinkov statistic beat trigrams* for plugboard
  recovery is attributed here but **unverified** — treat §6.1's use of it as a hypothesis
  to test, not a result. *Pages/figures unverified.*)
- O. Ostwald and F. Weierud. *Modern breaking of Enigma ciphertexts.* Cryptologia
  41(5):395–421, 2017. (Three-pass IC → bigram → trigram hill-climb; partial plugboard
  exhaustion; ~6 self-steckered letters ⇒ ~10% plaintext leaked from an empty board;
  designed for the 250-letter limit; "rarely successful for short messages.")
- G. Lasry et al. *Nested hill-climbing + SA for Enigma* (Cryptologia 2019; 2015 Poznań
  contest; cryptocellar "Hillclimbing the Enigma Machine"). (Divide-and-conquer over
  scrambler settings + per-key plugboard hill-climb with period-authentic trigram tables;
  architecturally the same shape as this tool — and the source for framing `-F` as the
  nested-hillclimb primitive and §5.3 as its cross-key generalization.)
- A. Sommervoll and L. Nilsen. *Genetic algorithm attack on Enigma's plugboard.*
  Cryptologia 45(3), 2021. (GA with IC / "Progress IC" fitness; a success dip near 310–318
  chars — a possible IC length artifact.)
- S. Garg. *GA, Tabu Search and SA: a comparison for transposition cipher.* JATIT.
  (GA ≈ SA ≈ tabu for transposition cryptanalysis.)
- *Survey on Metaheuristics for Cryptanalysis of Substitution and Transposition Ciphers.*
  (No metaheuristic a demonstrated strict winner; PSO/ACO not shown superior.)
- H. Lourenço, O. Martin, T. Stützle. *Iterated Local Search.* arXiv:math/0102188.
- J. Lam and J.-M. Delosme. *Performance of a new annealing schedule*; V. Cicirello,
  *Self-Tuning Lam Annealing*, Applied Sciences, 2021.
- R. Rubinstein and D. Kroese. *The Cross-Entropy Method.*
- D. Wales and J. Doye. *Basin hopping / global optimization.*
- J. Bentley. *Fast algorithms for geometric TSP* (don't-look bits).
- A. Turing and G. Welchman. Bombe menu and diagonal board (plugboard reciprocity as a
  constraint); Rejewski. See codesandciphers.org.uk and en.wikipedia.org/wiki/Bombe.
- R. Kneser and H. Ney. *Improved backing-off for m-gram language modeling*, 1995
  (modified Kneser-Ney smoothing).
- Practical Cryptography. *Quadgram statistics as a fitness measure* (source of the shipped
  n-gram tables).
- bytereef.org M4 project (known M4 messages, telegraphic-German validation).

Repo-internal references: `CODE_REVIEW.md` §1, §2 (live search/scoring roadmap);
`archived/CODE_REVIEW_HISTORY.md` §9 and the archived §6/§7 (shipped-feature rationale, rejected
experiments); `archived/SIMULATED_ANNEALING.md` §6.2, §12, §14, §15, §16 (SA design and tuning
evidence). Code anchors cited inline by `enigma.cc` line number (verified against the
current source: `ngrams_read` :307, `setup_mapping` :572, `quadgram_score_decode` :656,
`hillclimb` :975, `perturb_steckerbrett` :1166, `anneal_once` :1378, `hillclimb_restarts`
:1478, `filter_worker` :1754, `finish_worker` :1826).
