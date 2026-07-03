# performance.md — Improving short-message plugboard recovery

> **Scope and relationship to the other docs.** Despite the file name, ~80% of
> this document is about **cracking quality** (recovering the plugboard on short
> ciphertext), not raw throughput — in this repo `make bench` is the "performance"
> harness and `make crackquality` is the recovery harness, and most ideas below are
> measured by the latter. Read this as the forward-looking roadmap that **extends
> `CODE_REVIEW.md` §1–§2** (the live "cracking quality — search / scoring" items)
> and the archived **`CODE_REVIEW_HISTORY.md` §9** (the shipped-feature rationale and
> the measured-and-rejected list). The simulated-annealing design and its tuning
> evidence live in **`SIMULATED_ANNEALING.md`**; where an idea here touches SA it
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

**Already shipped** (do not re-propose; tune only): steepest-ascent hill-climb
(switch / remove / gated re-pair moves); random restarts `-R` with a fixed
`k≈8`-pair kick; staged schedule `-S` with the best recipe `-S iq` (IC pre-pass
then quadgrams) and per-stage plug caps; key pre-filter `-F` (cheap capped-IC
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
slower).

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

### 3.2 Portfolio: per-key `max(greedy, SA)` (HIGH priority)

**Form in this codebase.** The repo's own finding is that SA (`-A`) is a *peer* of
greedy `-R -S iq`, not a strict win — which is the textbook precondition for a
**portfolio**: two solvers that fail on *different* keys. For each rotor key, run
the greedy restart climb at half the budget and one SA trajectory at half the
budget, and keep the better board. No new algorithm, no new landscape, no new
tuning — just call both existing solvers per key and take the max. Deterministic
(both already ride the per-key RNG stream).

**Why it helps at 50 chars.** If greedy solves set A of keys and SA solves set B,
and A ≠ B, then `max(greedy, SA)` solves `A ∪ B` — strictly at least as many as
either alone, for the same total compute (each at half budget). The "SA is a peer,
not a win" result is precisely the evidence that A and B differ; a strict winner
would make the portfolio pointless, but a peer makes it near-free upside.

**Honest payoff.** Potentially the **highest ROI in the document**: zero new
mechanism, trivial determinism, and it directly monetizes the shipped SA work that
otherwise sits as a mere alternative. The risk is the opposite — if the two
solvers' solved-sets turn out to be nearly *nested* (SA solves a strict subset of
greedy's keys), the union adds nothing and half-budget-each simply weakens the
better solver. That is the one thing to measure first.

**Cost/risk.** Very low (an orchestration change; both solvers exist). The only
subtlety is the budget split — half each is the neutral default; sweep it.

**Experiment.** `make crackquality SPLIT=1` at L40–120 at **equal total
`score_iter`**: portfolio (greedy@½ + SA@½) vs greedy-only@full vs SA-only@full.
The claim is "fewer misses because the union of solved keys is larger"; the
falsifier is "the solved-sets are nested, so the union is no bigger than the
better solver." Also log per-trial *which* solver won, to confirm the sets differ.

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

### 3.6 Partial plugboard exhaustion — forced first pair (MEDIUM priority; literature-backed)

**Form in this codebase.** The core diversification device in Ostwald & Weierud
(2017) and its predecessors. Instead of starting every climb from a
perturbed board, systematically **force each candidate first pair** —
`C(26,2)=325`, or cheaper a shortlist of ~150 pairs on the most frequent cipher
letters — pin it (as `-s` already pins plugs), run the full IC→quad climb, keep
the best. Deterministic (no RNG). Compose with `-F` so the 325× cost lands only
on shortlisted keys.

**Why it helps at 50 chars.** It *guarantees* the climb is launched from inside
325 distinct basins, one of which reliably contains the first true plug — whereas
an 8-pair random kick samples basin space blindly. A structured superset of the
random-restart idea already shown to keep paying through R=256.

**Honest payoff.** High evidence, but calibrate against §2: at 50 chars with 10
plugs the information may simply not be there for *full* recovery. Expect the
largest gains in **mean %-correct** and in the **60–120 char band**, not in exact
recovery at 40–50. Up to ~325× a single climb per surviving key, so it is only
affordable paired with `-F`, and it must beat spending that same 325× as raw `-R`.

**Experiment.** `make crackquality SPLIT=1` at L40/50/60/80/120, comparing
`-c -S iq -F N` vs the exhaustion arm at **equal wall-clock** (shrink `-R` for
the exhaustion arm to match compute). Expect the search-failure share to drop.

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
  Only pursue if an oracle probe first confirms two elite boards *jointly* cover
  the true pairs while neither does alone — otherwise consensus (§3.1) captures the
  same signal more cheaply.
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

---

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

### 5.3 Cross-key plug marginalization (HIGH value — but needs the full-crack tier)

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

Per the §1 diagnosis, purely *monotone* rescales (likelihood-ratio null
normalization, z-scoring, per-length normalization) **provably cannot change
per-key ranking** and cannot move the plugboard tier. Only ideas that **reshape
the surface** (smoothing, lower-order/adaptive order, combined IC+quad, finer
quantization) can help the climb, and even those help only insofar as a smoother
surface lets the *same* climb reach the basin. **Every item in this section ranks
below every §3/§7 item** in the shortlist: the tier is search-bound, so scoring can
only help as *landscape smoothing*, measured solely by whether the search-failure
share drops. The larger scoring payoffs await the full-crack tier.

### 6.1 Adaptive n-gram order by length; trigram target at the short end (MEDIUM priority)

*Merged from three researchers.*

**Form in this codebase.** All four scorers already exist; this is a scheduling
change, **distinct from `-S iq`** (which is about the IC *pre-pass*, not the final
ranking model). Make the target/ranking model a function of `textlength`:
`<~60` letters → trigram, mid → a bi→tri phase, long → quad. Ostwald & Weierud
deliberately end on trigrams for the operational limit. Lower order = denser cells
= smoother surface = fewer local optima.

> There is a folk result (attributed to Williams) that the *unigram* Sinkov
> statistic **beat trigrams** for plugboard recovery, on the logic that the
> plugboard maximally disrupts higher-order frequencies. **This is unverified
> here** (the origin PDF was not reachable; see References) — treat it as a
> hypothesis to *test*, not a settled fact, and do not let it alone justify going
> below trigram. If it holds, it strengthens the "lower order is smoother" case;
> if it does not, trigram-at-short-end still stands on the density argument.

**Why it helps at 50 chars.** ~47 quadgrams, many at the hapax floor, give a
high-variance spiky surface; ~48 trigrams with far denser counts give a
lower-variance, better-conditioned landscape for the *climb*.

**Honest payoff.** Medium — plausibly the single most likely *scoring* lever to
move the plugboard tier, because it changes surface roughness (the search lever).
Skeptical caveats: (a) the harness reports the true board already out-scoring
under quad at 50 chars, so quad is not *mis*-ranking — trigram's payoff here is
landscape smoothing, likely modest, and larger at 30–40 chars and in the future
full-crack tier; (b) `-S iq` already shows lower-order is not a free lunch (IC
pre-pass beat a bigram pre-pass), so the win, if any, is in the *final* model at
the shortest lengths only. A length threshold is a new magic number — validate
across all four languages, not just the harness's one passage.

**Experiment.** `make crackquality SPLIT=1` comparing `-S iq` vs `-S it` vs
`-S ibt` across L40–160; watch mean %-correct and the search/scoring split at the
short end. Ship the length switch only if trigram wins at 40–70 and quad wins
longer.

### 6.2 Back-off / interpolated smoothing to replace the flat hapax floor (MEDIUM priority)

**Form in this codebase.** `ngrams_read()` (`:307`) floors every unseen quadgram
at one fixed value `log10(1/total)`. On 50 letters a large fraction of the ~47
quadgrams are unseen and all contribute the *same* constant — the score is driven
by only the handful of attested quadgrams and is blind to *which* unseen quadgram
appeared. Replace the flat floor with an interpolated/back-off estimate
(`P4 ≈ λ4·P4 + λ3·P3(·|abc) + λ2·P2 + λ1·P1`, or modified Kneser-Ney), **precomputed
into the same `quad8` table at load time**, so the hot loop is byte-identical
(still one `quad8[a][b][c][d]` gather, no throughput risk).

**Why it helps at 50 chars.** Short text is exactly the regime dominated by
unseen/rare quadgrams; back-off keeps the lower-order signal that *is* attested for
those positions, so it reshapes the surface (not a monotone rescale) and can pull
the climb into the true basin.

**Honest payoff.** Medium. Per the search-failure diagnosis, this helps only
insofar as a denser surface lets the same climb reach the basin; the larger win is
the future full-crack tier (rotor-key discrimination). KN-over-flat is usually a
few-percent likelihood gain, not transformative. New tunable (interpolation
weights), refit per language; keep the flat-floor table for A/B.

**Cost/risk.** Low: offline table build, hot path unchanged.

**Experiment.** `make crackquality SPLIT=1` at L40–100: exact-recovery, mean
%-correct, and whether the scoring-failure count stays ~0 (confirming surface, not
discrimination, moved). `make bench` must be flat.

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

### 6.4 Combined weighted fitness quad + λ·IC (LOW–MEDIUM priority)

**Form in this codebase.** Instead of *staging* IC then quad, fuse a term:
`score = quad_loglik + λ·IC` (both cheap; IC is one pass over the same decoded
stream). IC is language-independent and roughly monotone in "how many plugs are
right," so a small weight tilts the surface toward the IC gradient where quad is
flat, while quad still picks the winner.

**Honest payoff.** Low–medium. Introduces λ (tune per length/model); too large
blurs quad's true peak. IC and quad already coexist via staging, so the marginal
gain over `-S iq` may be small. Cheap to sweep. The extra IC accumulation is a
measurable hot-path cost — bench it.

**Experiment.** `make crackquality` sweeping λ ∈ {0, 0.25, 0.5, 1} at L40–100;
require it to beat both plain quad *and* `-S iq`. `make bench` to bound the added
per-score cost.

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

### 6.6 Telegraphic / operational corpus (HIGH value for real traffic; invisible to the current harness)

**Form in this codebase.** The shipped tables are generic web-corpus statistics.
Real Enigma plaintext is telegraphic: `X`/`Y`/`J` separators, spelled numbers
(`EINSNULL`), no punctuation, fixed procedure words. `X` alone dominates real
n-gram statistics in ways absent from prose. Build German/English tables from
decrypted Enigma traffic and ship them as a selectable table set.

**Honest payoff.** High for *real* messages — a sharper matched model needs fewer
attested grams for the true key to stand out. But the current harness samples clean
prose with the matching-language table, so this is **not visible in `make
crackquality` as-is**; it requires adding a telegraphic test corpus. A
real-world-fidelity win, not a benchmark win. Keep prose tables to avoid
over-fitting one traffic style.

**Experiment.** Add an X-separated German corpus and matching table set to
`crack_quality.py`'s corpora; cross-check on the bytereef.org M4 known messages.

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

### 7.1 Restructure the per-pass move-evaluation loop (HIGH priority — one item, three candidate implementations)

> **Measured outcome (7.1a built and tested).** Item **(a)** — surrogate-ranked
> steepest ascent — was implemented and measured. **The surrogate *ranking* is
> rejected; only the incremental *delta* survives, as the opt-in `-D` flag for the
> mono/IC models.** Findings, in order:
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
> - **The delta arithmetic itself is sound and reusable.** Used *exactly* (not as a
>   surrogate) for the mono/IC models — find the best switch by an
>   O(affected-positions) delta over a per-pass inverted index, then score that one
>   winner exactly — it is **byte-identical** to the full scan and shipped as `-D`
>   (off by default). Same crossover: slower at short messages (baseline mono/IC
>   decode is trivially cheap there), faster on long ones (mono up to ~27% at 500
>   chars, IC ~5–7% at ≥300), so it is opt-in. Quad/bi/tri keep the baseline scan
>   (delta-quad was already measured ~2× slower). `-T`-deterministic; TSan-clean;
>   no baseline hot-path regression (`make bench BASE`: search −0.5%, hillclimb −0.1%).
>
> The takeaway that generalizes: **at the ~50-char target, per-candidate scoring is
> not the bottleneck to cut** — the decodes are already cheap; the first-order lever
> stays *more restarts* (raise `-R`). Items (b)/(c) below share the same premise the
> measurement undercut; expect the same short-message crossover.

*The nominal plan (unchanged for context): three researchers proposed three rewrites
of the **same** per-pass loop over the **same** inverted index; largely mutually
exclusive, so competing implementations of one item, not stackable wins.*

Each `hillclimb()` pass full-quad-scores ~325 switch + ~13 remove moves against a
constant base board. That is the cost to cut. Candidates:

- **(a) Surrogate-ranked steepest ascent — ❌ built, measured, rejected** (see the
  banner above). Rank all candidates with a **cheap surrogate** and full-quad-score
  only the top-K. Monogram ranks well (K≈16–32), IC does not; but the whole scheme is
  ~1.5× slower at 50 chars (only wins ≥150), because warm short-message quad decodes
  are too cheap to be worth skipping. The reusable remnant is the exact `-D` mono/IC
  delta climb (byte-identical, long-message-only win).

- **(b) Memory-level-parallel batch scoring (alternative to (a), or stackable if
  (a)'s top-K is still >1).** The score loop is a *dependent* gather chain, but
  scoring N *different* candidate boards is N *independent* chains. Restructure to
  be position-major over a batch of N boards: at each position issue all N boards'
  gathers before consuming any result, keeping N running quad accumulators. All
  boards share `ct[]`/`rows[]` and differ only in 4 `steck` entries, so the loads
  overlap in the load-store queue and hide each other's latency. This is scalar
  latency-hiding, **not** the rejected SIMD (which targeted arithmetic throughput
  and failed because the loop is latency-bound — that distinction is real and is
  why this is worth trying). *Payoff:* medium-high, ~1.5–3× on the score-bound
  section *if* enough MLP is exposed; erodes past some N from register/LLC pressure
  (sweep N). High code complexity (a batched scorer distinct from the fused
  single-board one; must stay portable). Numerically identical → `crackquality`
  unchanged.

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
> bits. First-improvement was built and shipped as opt-in `-I`; don't-look bits and
> informed move ordering remain open (below).

**What shipped.** `hillclimb()` was steepest-ascent — a full ~338-move scan per
accepted move, taking the single best. `-I` switches to **circular first-improvement**:
a cursor sweeps a fixed 351-move list (325 switch pairs + 26 remove-by-letter), applies
the **first** improving move, and **continues from where it accepted** (never restarts at
the top). Continuing (vs restarting) is what makes it both efficient (each move examined
~once per sweep, no redundant re-scan of unchanged moves) and unbiased (attention rotates
evenly instead of always favouring low letters). Converged = a full cycle accepts nothing.
No data structure — which is *why it wins where §7.1a/`-D` lost*: those cut decode count
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

**Because it recovers worse per restart, `-I` is opt-in** — a user at the default `-R 1`
who enables it gets *worse* results. Documented as "pair with more `-R`."

**Still open (the other half of §7.2, and its refinements):**
- **Don't-look bits** (Bentley): a per-letter active flag; skip moves incident only to
  letters that a full sweep already found inert, re-activating the ~4 letters an accepted
  move touches. Composes with the `-I` cursor (skip cursor positions whose letters are
  inactive) and should cut the confirming final cycle further. Not yet built.
- **Informed move order.** `-I` uses lexicographic order; a *static* order by ciphertext
  letter frequency (identifiable letters first, §2) is free and might reduce the
  per-restart quality loss — recovering even more of the win. A *dynamic* scored/ranked
  queue was analysed and set aside: it reintroduces exactly the per-pass overhead that
  sank §7.1a at short messages.

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
  (`SIMULATED_ANNEALING.md` §14). *Honest payoff:* low — speeds convergence (more useful
  moves/budget) rather than changing the reachable optimum; helps mainly the tight-budget
  corner and risks self-reinforcing bias. **Hot-path hazard:** per-letter blame
  attribution must come from the fused scorer *without* a second pass / `num_plaintext`
  round-trip (the documented layout caution) — easy to regress. `make bench` first, then
  `crackquality` at small budgets.

---

## 9. Prioritized shortlist and measurement plan

### The one infrastructure item that gates everything: build the full-crack `crackquality` tier

Before *and above* the algorithmic shortlist below stands a **measurement**, not an
algorithm: a `make crackquality` tier that **wildcards the rotor key too** (not just
the plugboard). It is the highest-value infrastructure item because:

- Every "scoring is fine, the tier is search-bound" conclusion in this document is
  scoped to the plugboard-recovery tier. Rotor-key discrimination may expose genuine
  *scoring* failures that would re-justify all of §6.
- It is the prerequisite for **cross-key plug marginalization (§5.3)** — the one
  non-crib lever that *adds information* at 50 chars — and it is where the Bayesian
  calibration (§6.7) and MDL prior (§6.3) actually pay off.
- It is cheap: it reuses the existing harness plumbing and simply removes the
  fixed-key assumption.

Build this first; several ideas below only become measurable once it exists.

### If you do three things (on the existing plugboard tier)

> The original #1 here — cross-restart consensus / plug fixation (§3.1) — was built
> and **measured as compute-neutral-to-negative** (it loses to a higher `-R` at equal
> compute, and becomes a no-op as `R` grows because best-of-`R` saturates). It is
> rejected; see §3.1. The list below is the reassigned top three.

1. **Portfolio `max(greedy, SA)` (§3.2).** Near-free upside that directly monetizes
   the shipped "SA is a peer, not a win" finding: run both existing solvers at half
   budget each per key and keep the better board. Zero new algorithm, trivial
   determinism. The one thing to check is that the two solvers' solved-sets actually
   *differ* (not nested); the finding that SA is a *peer* is exactly the evidence they
   do. **Unlike consensus, this does not re-climb a shared seed** — it keeps two
   genuinely independent trajectories, so it is not subject to the best-of-`R`
   saturation that sank §3.1.

2. **A per-climb throughput lever — first-improvement (§7.2). ✅ DONE, shipped as `-I`.**
   The prediction held: cutting the *number* of evaluations (not the cost of each) with
   zero overhead is what beats the baseline at 50 chars, where §7.1a's decode-cheapening-
   plus-bookkeeping lost. `-I` is ~2.8× cheaper per climb; paired with more `-R` it wins
   at matched compute (+8pp exact / +1–23pp mean, scaling with available signal — §7.2).
   Opt-in, because it recovers worse per restart. **Remaining upside on this axis:**
   don't-look bits and a static frequency-informed move order (both §7.2, unbuilt).

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
50-char-barrier breakers, but both are **invisible to `make crackquality` as written** and
each needs its own harness tier (crib-planting; telegraphic corpus). Do not expect them to
move the current numbers; do not judge them by it.

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
`-A` simulated annealing, `-s` fixed plugs, uint8 tables + hapax floor, **`-D` exact
mono/IC delta-scoring** (opt-in; byte-identical; a long-message-only speedup — §7.1),
**`-I` circular first-improvement** (opt-in; ~2.8× cheaper/climb; a matched-compute
recovery win when paired with more `-R` — §7.2).
Rejected (with reason): **§7.1a surrogate-ranked ascent** (built; ~1.5× slower at 50
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
climb (overfits).

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
`CODE_REVIEW_HISTORY.md` §9 and the archived §6/§7 (shipped-feature rationale, rejected
experiments); `SIMULATED_ANNEALING.md` §6.2, §12, §14, §15, §16 (SA design and tuning
evidence). Code anchors cited inline by `enigma.cc` line number (verified against the
current source: `ngrams_read` :307, `setup_mapping` :572, `quadgram_score_decode` :656,
`hillclimb` :975, `perturb_steckerbrett` :1166, `anneal_once` :1378, `hillclimb_restarts`
:1478, `filter_worker` :1754, `finish_worker` :1826).
