# ENHANCEMENTS.md — the open issues

What is still worth doing. Everything else — how each of these was arrived at,
what was tried and measured down, and the pitfalls list — is history and lives
in `archived/`, chiefly `archived/IMPROVEMENTS.md` (open work and pitfalls as of
archiving), `archived/PERFORMANCE.md` (every measurement), `archived/cribs.md`
(the crib programme) and `archived/refinement.md` (the derived `--ring-stride`
refinement). **`archived/` is read-only** — cite it by section, never edit it.

**Where things stand.** The tool is correct, warning-free under the project's
flag set on g++ and clang, clean under ASan/UBSan/TSan/valgrind/cppcheck/
clang-tidy, multi-threaded, and supports standard / Norway / M4. Nothing below
is a known bug. Two measurements frame the list and both say the same thing —
**there is little apparent headroom left**, though neither is a proof and this
conclusion has been drawn before and been wrong:

- **Search looks compute-bound.** Essentially every miss on the
  plugboard-recovery tier is a *search* failure, and the exact-recovery curve is
  still climbing at `-R 256`. No truth-free way to exploit the restart
  population is known, so raw `-R` bought with `-T` remains the dependable
  lever.
- **Scoring looks near its ceiling.** The discrimination floor is ~1% at L40–60
  and ~0 beyond, and the residue at L40 is information rather than model —
  unicity distance is ≈25 characters. But this was the stated position
  immediately before `-f` shipped and gained +3.0–4.4pp, so treat "no headroom"
  as a prior, not a finding.

Recommended recipe: `-c -S m4f10 -J --polish -f -l <lang> -T <cores> -R <high>`.

---

## Search and scoring

**1. ILS with incumbent-walk acceptance.** Nominally open, a long shot:
converged boards are *scattered* rather than clustered near the truth, and
clustering is the structure ILS would need. → `archived/IMPROVEMENTS.md` §2.

**2. The narrow L40 scoring re-opening.** The only observed scoring failures sit
at the identifiability floor — 5–10% at L40, 0 elsewhere. Length-sensitive
scoring could only help at L ≲ 40, where recovery is already near the
information floor, so the payoff is small. → `archived/IMPROVEMENTS.md` §2, §4.

**3. Attack several messages from ONE DAY jointly.** Every measurement in
this repo attacks a single message, but real traffic came in **day keys**: every
message on a net that day shared reflector, wheel order, ring settings and
plugboard, and differed only in the per-message start position. So `N` messages
from one day are `N` independent observations of the *same* key over a keyspace
that does **not** grow — which is the one lever that attacks the limit `-R`
cannot move.

*Why it should be strong.* `--confidence` already supplies the arithmetic: a
break needs the true key's `z` to clear `√(2 ln K)`. Score a candidate day key
against `N` messages and the true key's signal accumulates over `N·L`
characters while `K` — and so the bar — is unchanged, i.e. `z` grows roughly as
`√N`. The measured residue at L=167 is a **5% scoring floor** plus climb
failure (`CLAUDE.md`, "The unknown-key break rate"), and the floor is exactly
what a `√N` gain erodes: two messages should beat any affordable increase in
`-R`. The plugboard is where most of it lands, since it is the expensive half
and is shared across the whole day.

*Why it is affordable.* The obvious objection is that `N` messages carry `N`
independent start positions, so the joint space looks like `shared × 26^(3N)`.
It is not: for a **fixed** shared key the best start for message *i* does not
depend on message *j*, so each message is optimised on its own and the cost is
`shared × N × 26³` — **additive in `N`, not exponential**. That is the whole
reason this is worth building.

*The subtlety to get right.* Because each message contributes a **maximum** over
its own ~17 576 starts, the joint statistic is a sum of maxima rather than a sum
of scores, and each maximum carries its own `√(2 ln 26³)` chance inflation. The
correction is a constant `N·√(2 ln 26³)` offset, not a `√(2 ln K)` over the
joint space — getting this backwards would make a joint sweep look significant
on noise. `--confidence`'s null machinery is the right place to put it.

*Is there a target? Checked — yes, but the biggest group is the doubtful one.*
Six of the 18 challenge ciphertexts are no longer unbroken (§5a), which leaves
twelve, and among those the same-day groups are:

| day | messages | letters |
|---|---|---:|
| **30 Sep 1941** | BYQMZ (167), FKQLZ (107), XFEDT (97) | **371** |
| 11 Jul 1941 | AWTZK (49), ZNLZT (69) | 118 |
| 29 Sep 1941 | QTXMA (155), SZAEJ (51) — *not transcribed here* | 206 |

So the capability does have a target, and 30 Sep is a group of three sharing a
batch as well as a day. **But the J test (§5b) says FKQLZ and XFEDT are probably
not a 26-letter cipher at all** — zero J in 204 pooled letters against 7.8
expected — so a joint attack on that group would be pooling one Enigma message
with two probable non-Enigma ones, which is worse than attacking BYQMZ alone. Do
not read "three messages, 371 letters" as `√3` of signal without settling that
first. 11 Jul is clean but short (118 letters total), and the 29 Sep pair needs
QTXMA transcribed before it is reachable at all — which is independently the
best thing to add to the challenge set.

→ `eval/MODERN_BREAKING_NOTES.md` §5a/§5b; `CLAUDE.md` "The unknown-key break
rate", `--confidence`.

## Keyspace reductions

The two-notch collapse that used to head this section has **shipped** and is
no longer an issue: `CLAUDE.md` "Two-notch wheels" and the CHANGELOG carry it.

**4. Does the middle-wheel collapse's saving convert?** The §7.12 reduction is
3–5× at short lengths and the compute is saved; whether spending it on `-R`
raises recovery is untested. The same question was asked of `--ring-stride` and
answered "a wash". → `archived/IMPROVEMENTS.md` §2.

**5. A `--ring-stride` for the middle wheel — premise measured, not built.**
Striding `ring1` costs 3.1% (K=2) / 5.1% (K=3) of the true `offset1`, roughly
competitive with the right-wheel stride, and the two compose multiplicatively.
**Read the failed attempt first**: striding `ring1` directly measured 2.4×
*worse than not striding*, because thinning `ring1` switches off §7.12's exact
4.86× collapse. The sound axis is to thin the *representatives* §7.12 already
produces — which the measured numbers do **not** cover. →
`archived/IMPROVEMENTS.md` §2.

**6. Read the right wheel's phase off one decrypt instead of searching it —
signal confirmed, does not localise yet.** Shifting the right wheel's phase
(ring2 and start2 together by `δ`) leaves `offset2` and so the whole
substitution untouched; only the notch timing moves, displacing the middle wheel
by exactly one step on a contiguous cyclic block of columns that repeats with
period 26. The same columns are corrupt in every 26-letter window. So the phase
could in principle be *derived* — fold the per-position score by `i mod 26`,
find the low-scoring block, and its boundary is the notch position — in O(1)
decrypts, where `--tune-phase` spends O(676) scorings on a frozen board.

Probed with the board known and the offsets correct, the best case
(`eval/turnover_localise_probe.py`):

- **The physics holds exactly.** `δ=1` and `δ=25` both give a 1-column block,
  `δ=13` gives 13 — so the block width is `min(δ, 26−δ) ∈ [1,13]`.
- **The signal is real**: corrupt columns score **0.6–0.8 log10/char** below
  clean ones.
- **But the fit overfits.** Over 26 starts × 13 lengths against 26 noisy
  columns, the best-separating block beats the true one — at L=167, **1.02
  against 0.68** at the truth. Exact block recovery is **0% at L=167**, 8% at
  L=400, 25% at L=900. ~6 periods is not enough for a two-parameter fit.
- **A confound the idea does not account for: the left wheel.** Displacing the
  middle wheel also moves when *it* passes its own notch, so the left wheel
  steps elsewhere and everything after is corrupt, breaking the mod-26
  periodicity the method assumes.

**The untested refinement is the interesting one.** Scoring a block only asks
"is this region bad". In the corrupt block the middle wheel is off by *exactly
one*, so re-decrypting that stretch with the middle wheel stepped ±1 should turn
it into clean German — a **verification** rather than a ranking, which cannot
overfit the way a two-parameter score fit does. That is where this should be
picked up if it is picked up at all. → `eval/turnover_localise_probe.py`.

## Cribs

Detail for all three: `archived/cribs.md` §13 — which also carries the
X-separator variant, dropped here for want of confidence in the premise
that word-boundary positions are any easier to come by than a phrase.

**7. Crib supply at network scale.** The library covers 83% of held-out
messages, but on a 58-message corpus, and 47 of the 57 hits are 8–11 letters —
seed-only lengths. Whether a real network yields *long* cribs is the question
the whole feature rests on, and no larger corpus is available.

**8. Reject or rank?** The deduction rejects a rotor setting outright. Ranking
would tolerate a slightly-wrong crib — which matters, because garbling is real
(two of five `SIEGFRIED` messages are corrupted) and exact matching cannot see
through it.

**9. Menu reuse across alignments.** Shifting a crib by one position changes
every edge, so probably not — but worth checking before assuming the alignment
sweep pays full price each time.

## Measurement gaps

The **null distribution of every scoring model is now measured**, and
`--confidence N` reports it per run. Random text scores a well-defined mean with
sd falling as `1/√L`, so signal separation grows as `√L`; the best of `K` wrong
keys sits at `μ + σ·√(2 ln K)`, matched to within 0.01 for quad and fused over
12 signal-free sweeps. Detection needs roughly **`L ≳ 0.92·ln K`** characters —
~9 for a start-position sweep, ~19 for the full rotor keyspace, ~49 once the
plugboard is included, which is where the documented scoring-failure floor sits.
Two things fell out: quad and fused separate signal from noise **equally well**
(21.4σ against 21.7σ at L=200), independently confirming that `-f`'s gain is a
better climb rather than better discrimination; and IC's null is right-skewed,
so the Gaussian tail understates its best-of-K (6.1σ observed, 4.4 predicted).



**10. `--tune-phase` below its saturation length — MEASURED, and the question
has moved.** The open half was whether the L=200 trade (more breaks, lower mean
%-correct) survives at operational lengths. It does at **L=300** — 74/80 exact
against 65/80, McNemar p = 0.049, mean −4.2pp with CI [−8.9, +0.6] — and is gone
by **L=450**, where the two arms are indistinguishable (100.0/100.0 mean, 39/40
against 38/40, one discordant pair). The catastrophic misses fall 12/80 → 4/80 →
0/40 as the capture radius predicts, but the exhaustive arm's hit zero *first*
(at L=300), so the trade dissolves because the problem stops being hard for
either arm, not because the flag pulls ahead. Bucketing by distance from the
true ring to the nearest starting phase explains the *rate* but not *which*
trials fail, so raising `N` is not the indicated fix. → `CLAUDE.md` "How the
split moves with length".

*What is left of it.* Matched compute stops discriminating once both arms
saturate, and `--tune-phase` gets there from a keyspace **125× smaller** — so at
L≥450 the question is how far **below** the exhaustive sweep's cost it can go
and still break the message, which is where a 125× keyspace reduction would
actually pay. `eval/tune_phase_budget.py` measures it (sweeps `-R` over the same
instances a paired results file drew, so the numbers sit directly against that
file's arm B column). Unanswered as of writing.

**11. `--ring-stride` with a hidden plugboard at K=13.** The one cell where
anything moved: 4 losses in 69 trials against 0 in 72 for a paired given-board
control, direction consistent across two seeds but **p ≈ 0.13 — suggestive, not
established**, and only at a stride already outside the recommended K≤3.
Settling it needs ~200 trials (~3–4 h) and buys nothing operational. →
`archived/PERFORMANCE.md` §7.11; `eval/ring_stride_scope_probe.py`.

**12. Report a CLOSE-MATCH rate beside the mean and the exact rate.** Every
harness here reports two numbers: mean %-of-letters-correct (graded, low
variance) and exact recovery (coarse, the operator's metric). Neither says how
often a run lands *recognisably close* — enough of the plaintext to read as
language, and plausibly resolvable to the exact answer by a local
re-optimisation afterwards. That is a third, operationally distinct outcome, and
it is the population `--polish` exists to convert, so a rate for it would
measure the finisher's target directly instead of by its effect.

*Use **50%** — half the letters — and do not agonise.* Pooling every non-exact
outcome from the three `--tune-phase` A/B runs (both arms, n=70): 22 sit at or
under 10%, 41 at or above 90%, and **three** in between (59.3, 66.7, 75.7).
Nothing at all lands between 10% and 50%, so any cut from ~20% to ~55%
classifies this data identically. 50% is the round number inside that
insensitive band; 60% is where trials start to move (it is the first cut that
clips the 59.3 case). The bimodality is structural on this problem — a wrong
*offset* scrambles everything, while a right key with a few wrong plugs keeps
most letters — so the threshold is not where the care is needed. Report how many
outcomes fall in the band beside the rate: a threshold is only interesting when
something sits near it.

*Where it should actually bite is the plugboard-recovery tier* — `make
crackquality`, rotor key given, board hidden — whose failures are partial plug
recoveries rather than all-or-nothing, so intermediate scores are genuinely
populated there (its documented means run 24.7% to 91.1%). That is where a
close-match rate would add information the existing two metrics do not already
carry, and where the "resolve it to exact afterwards" follow-up is testable:
take the close-but-wrong boards and measure what fraction a finishing pass
converts. `tests/crack_quality.py` and
`eval/tune_phase_vs_restarts_report.py` are the two places to add it.

## Maintainability and packaging

All 🟢, none urgent. → `archived/IMPROVEMENTS.md` §2.

**13. `-Wconversion` (~52 warnings) deliberately deferred.** 43 are `int →
unsigned char` narrowings in the hottest loops; that many casts clutter the hot
path for a low-value nit on deliberately C-style code. A future ratchet, not a
bug.

**14. No `install` target**, and the n-gram files are not declared as build/run
dependencies. Fine for development; add if the tool is packaged.

**15. Single-file distribution.** Embedding the tables was declined once, but
the shipped uint8 tables are ~4× smaller than the float tables that analysis
assumed, so a blob is much cheaper now. Keep `-d` / `$ENIGMA_DATA` as the
override.

**16. The `Scoring:` line can exceed 79 columns** when the `-d` path is long.
Path length is unbounded and cannot be shortened without hiding it; every other
status line is guaranteed to fit.

---

## Before changing search or scoring code

Read `archived/IMPROVEMENTS.md` §4 — **"Measured down — do not re-attempt"** —
and §5, the pitfalls. Between them they record what has already been built and
lost, and the traps that produced confident wrong answers here: benchmark deltas
that were really startup costs, byte-counting width checks, regression tests
that passed against the buggy binary, harnesses whose corpus moved underneath
them, and equality checks that compared everything except the thing that
changed.
