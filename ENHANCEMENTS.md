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

**DEMONSTRATED ON REAL TRAFFIC — and it beat this writeup.** ALVPM and ALRHG
(09.09.1941) gave up their shared day key to an ordinary ciphertext-only attack;
`GEHRG`, unbroken for twenty years and broken elsewhere only in July 2026 with
no key published, then fell in **0.08 s on one thread** as a bare 17 576-key
**start-position** sweep, margin +6.90 sd. The argument below is that `N`
messages from one day are `N` observations of a keyspace that does not *grow*;
in practice, once the shared key is known the remaining messages collapse to
their own start position — a reduction of ~1.6e8, not a factor of `√N`. **The
value of a same-day group is front-loaded**: break one message the hard way and
the rest of the day is nearly free. That does not remove the work below (the
joint *statistic* over `N` messages, for when no single message breaks alone),
but it changes the priority — try the day's easiest message alone first.
→ `eval/MODERN_BREAKING_NOTES.md` §5j.

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

**4. Known-word and X-segmentation bonuses — MEASURED DOWN; do not add them to
the score.** The idea: after each rotor setting's climb, score the candidate
plaintext for whole known words and for the X word-separator rate, and add that
as a bonus. It is what a human reader does with a decrypt the quadgram model has
undervalued, and it looked strong — on FTNBK, the message that prompted it, the
combination lifts the true key from **z = 0.90 to 11.21**, across the 6.15 bar a
160M-key sweep sets.

*It does not survive the corpus.* Measured over all 46 authentic messages with a
known key (48 wrong keys each as a per-message null, board hidden on both arms,
`-R 32`), the median gain is +1.64 z — and that median is the wrong summary.
Split by whether the message was already breakable on quadgrams alone, it
inverts:

| | n | median gain | helped |
|---|---:|---:|---:|
| already above the bar | 25 | **+7.75** | 20 of 25 |
| **below** the bar | 21 | **−0.33** | 7 of 21 |

`corr(quad z, gain) = +0.40`. The net effect on breakability is **one message**,
25 → 26 of 46, with a flip in each direction (up FTNBK, FDTZP; down RDNAQ).

*The mechanism is what closes it, not the size of the effect.* Among the 21
below-bar messages, **18 have a word-feature z of about −0.2** — the "no words
found at all" floor. The word bonus only fires once the climb has *already*
recovered readable plaintext; where the climb fails at the true key, the
true-key decrypt is as wordless as the wrong-key ones. So this is not a
weighting problem to be tuned out: no reweighting extracts signal from a feature
that is flat across the null and the truth alike. The regressions are the
mirror — an equal-weight sum of standardised features adds two near-noise terms
to a quad z of 36, inflating the composite's own sd and *lowering* z (−5.38,
−5.26, −4.51 on the three strongest messages).

*The same denominator caveat as item 5 applies here, and it does NOT overturn
this entry.* The below-bar population these numbers are computed over is
overwhelmingly *search* failures, so "the word bonus rarely fires there" is
partly a statement about that population rather than about the feature. What
closes item 4 regardless is the **form**: an additive, standardised score term
*dilutes* a good signal when it contributes noise (the −5.38/−5.26/−4.51
regressions above), so it can lose outright. A one-sided flag cannot, which is
why item 5's variant survives in that shape and this one does not.

*FTNBK is real but narrow*, and worth knowing as a class: its climb **does**
produce readable plaintext at the true key, and the pathology is specifically
that quadgrams will not reward what it recovered. Across the corpus that shape
is one clean case plus FDTZP (already at 5.29 and needing only a nudge). A
bounded opt-in *rescue* — re-rank the top-N converged boards on known words,
cost capped, failure mode "no change" — is the only form still defensible, and
it is close to
`--crib-rerank`, which is already measured down for the same reason.

*The general lesson, worth more than the result:* for anything whose value
depends on the search having **partly succeeded**, report the split by baseline,
never the median. Such a feature is necessarily strongest in the half where it
cannot change the outcome.

→ `eval/word_segment_probe.py` (the reproducer; `summarise()` re-reads the saved
JSON so the reading can be revisited without repeating the climbs),
`eval/results-word-segment.txt` / `.json`.

**5. Repeated text with an X between — MEASURED, and on its ACTUAL target it
works: 9 of 9 real scoring failures rescued, 0 false positives in 8496.**
Telegraphic German doubles important words around the X separator:
`ZANDERSXZANDERS`, `FORDXFORD`, the `LNKXLNKX` in Nr 214. The test is that two
runs are identical **to each other** — not that either is a word anyone listed
in advance — so unlike item 4 it needs no vocabulary.

*The rule, precisely* — `W` `X` `V` with `|W| = |V| = L`, **6 ≤ L ≤ 16**,
neither copy containing an X, exactly one X between them, and at most **one
position-wise substitution** between `W` and `V`. So `BERLINXBERLIM` fires and
`BERLINXBERLMM` does not. Substitutions only, not indels — which matches the
physics, since Enigma has no diffusion and a corrupted ciphertext letter
corrupts exactly one plaintext letter, so transmission garbles *are*
substitutions; an operator dropping a letter misaligns the copies and is missed.
**`MAXLEN` was 12 and silently missed the corpus's longest real doubling**
(`STUERZBAECHER`, 13, in DAFPX): a 13-letter repeat does not decompose into a
matching 12-letter one, because sliding the window puts the copies out of
alignment. 16 catches it at no null cost and saturates there.

*The text-level precondition* (`eval/doubling_probe.py`, 50 messages with a
clean recorded plaintext):

| min len | mismatches | real messages | shuffled-null rate |
|---:|---:|---:|---:|
| 4 | 0 | 17 of 50 (34%) | 0.005% |
| **6** | **≤1** | **22 of 50 (44%)** | **0.000%** |

**`len≥6` with one mismatch dominates `len≥4` exact** — more real messages at a
null rate no higher — because real traffic is garbled and an exact test discards
genuine hits (`PLYUSSA`/`PLJUSSA`, `ZANDEYS`/`ZANDERS`, `SIOBEN`/`SIEBEN`).
Never ship the exact form. The hits are `KUSOW`, `SAGOSKA`, `STARAJARUSSA`,
`OPOTSCHKA`, `TSCHEDINOVA`, `WASCHBUSCH`, `ROMANOWO`, `ZANDERS` — Russian
village names and German surnames, the operationally specific material no fixed
vocabulary carries.

*The result, on the population the feature is FOR*
(`eval/scoring_failure_probe.py`): 186 trials, short excerpts of authentic
telegraphic German cut to contain a doubling, random rotor key, random 10-pair
board, `-R 256`, 48 wrong keys each as a per-trial null. Every trial is
classified by separating the climb from the score:

| outcome | n | feature fires on the true key |
|---|---:|---:|
| break (climb recovers, z > bar) | 66 | 65 of 66 (98%) |
| **SCORING failure** (climb recovers, z ≤ bar) | **15** | **15 of 15 (100%)** |
| search failure (climb does not recover) | 105 | 0 of 105 (0%) |

**Rescue rate on scoring failures: 15 of 15**, where "rescue" means it fires on
the true key and on none of that trial's wrong keys, so the true key is
identified outright. **False positives: 0 of 8928** climbed wrong-key decrypts,
consistent with the 0 of 5888 measured separately below.

> **An earlier version of this entry reported "1 of 9" and called the rescue
> application DEAD. That was the wrong denominator and the conclusion was
> wrong.** The feature targets *scoring* failures; it was scored against every
> message below the detection bar, a population that is overwhelmingly *search*
> failures — 21 of 22 in that corpus. A search failure cannot be rescued by any
> plaintext-side feature, because there is no plaintext in the decrypt to read,
> so including them measures nothing about the feature. On the target population
> it was 1 for 1 there, and 9 for 9 here.

*Shorter messages DO expose scoring failures — measured, and this also corrects
an earlier claim here.* Conditional on the climb recovering at all:

| L | climb recovers | of those, SCORING failures |
|---:|---:|---:|
| **60** | 14 of 84 | **11 (79%)** |
| 100 | 38 of 69 | 4 (11%) |
| 140 | 29 of 33 | 0 (0%) |

At 60 letters a recovered message is *usually* a scoring failure, against 11% at
100 and none at 140 — consistent with a unicity distance of ~23 characters. The
earlier
note that shortening "buries scoring failures under search failures rather than
exposing them" was wrong as stated: search failures do dominate the raw trial
count (70 of 84 at L=60), which makes harvesting expensive, but the *rate*
conditional on recovery is an order of magnitude higher, not lower. Sample short
if you want scoring failures; just expect to pay ~10 trials per usable one.

*The end-to-end number needs the coverage multiplier*, because the trials above
were **constructed** to contain a doubling. Measured over random windows of
authentic plaintext, the fraction carrying one at `len≥6, mm≤1` is **25.8% at
L=60**, 42.1% at L=100, 52.6% at L=140 and 61.4% at L=200. So operationally the
feature resolves ≈26% of scoring failures at L=60 and ≈42% at L=100 — the 100%
above is *conditional on the doubling being present*, and must not be quoted
without this factor.

*Two design questions noted, not yet measured.*

**(a) Anchor on the X's first, then compare segments — do not scan (i, L).**
The pattern to look for is `XPARISXPARIMX`, not `PARISXPARIM`, and finding the
X positions first is not merely an optimisation: **the segmentation determines
the candidate lengths, so the length loop disappears entirely.** Split the text
on X, then compare adjacent segments (and, cheaply, non-adjacent ones — a word
can repeat later in the message with other words between). Three consequences,
all reasoned rather than measured:

- *Cost.* The present scan is ~`N × 11` window comparisons — about 1650 on a
  150-letter message. Telegraphic German runs ~6% X, so the same message holds
  ~9 X's and ~9 adjacent segment pairs. Roughly **two orders of magnitude
  cheaper**.
- *Shorter words become viable*, which is the real prize. `L ≥ 6` is a threshold
  forced by the **unanchored** scan's chance rate, not by anything linguistic:
  the table above shows `len≥4, mm≤1` reaching 48% of messages but at a 0.355%
  null. Two flanking X's are two extra constraints at ~1/16 each in telegraphic
  text, so anchoring should cut that null by ~250×, making `L = 4` or `5`
  affordable. Measure the anchored null before trusting the factor.
- *Recall is not the obstacle.* The flanking measurement says 71% both sides,
  25% left only, 4% right only and **0% neither**, and the left-only cases are
  largely end-of-message — a boundary segmentation gets for free. So an
  X-anchored form should lose little or no recall.

The one new fragility: anchoring depends on the **X's themselves** decrypting
correctly. Irrelevant for this feature's target population, where the climb
recovers ~100% of letters, but it would matter for any partial-recovery use.

**(c) Let the SEPARATOR be garbled too — evidenced twice on real traffic.** The
rule requires the separator to be a literal `X`. The 09.09.1941 breaks (§5j of
`eval/MODERN_BREAKING_NOTES.md`) contain **two** counter-examples in nine
garbles: `HENNING(J)HENNING` in ALVPM and `ROMANOVKA(G)KLAMM` in GEHRG. The
matcher misses both. Harmless in those messages, since a second clean doubling
carries each, but a message whose only doubling has a garbled separator is
missed entirely. Enigma corrupts one letter per corrupted ciphertext letter and
the separator is just another letter, so there is no reason to privilege it:
fold it into the mismatch budget. Two of nine garbles landing on X, against X
being ~6% of letters, is a small sample but it points the right way.

**(d) Real doublings can differ in LENGTH, and the cause is the keyboard.** The
matcher takes `|W| = |V|`, which the no-diffusion argument seems to justify —
one corrupted ciphertext letter corrupts exactly one plaintext letter, so
transmission garbles are pure substitutions. ALVPM breaks that assumption from
the other side: its doubled surname is `SCUHNACHER` (10) against `SCHUHMAXHER`
(11), i.e. `SCHUMACHER` against `SCHUHMACHER` — **the operator spelled the name
two different ways**. A dropped *ciphertext* letter is ruled out, since it would
desync the rotor stepping and wreck the remaining 137 letters, which decrypt
cleanly. So indels do occur in real doublings, just not from the channel. Any
length-tolerant matching would have to be edit distance rather than Hamming,
which is more expensive; whether it is worth it depends on how common this is,
and one instance is not an estimate.

**(b) Cost against the hillclimb — negligible, IF it runs in the right place.**
Rough arithmetic, to be confirmed on wall time: the current check is ~11
`score_iter`-equivalents, and one restart is ~1250 `score_iter` (inferred from
`--polish`'s measured ~6500 being 2.8–3.3% of a run at `-R 160`). So **~0.9% of
a single restart**, less if run once per key rather than per restart, and under
one `score_iter`-equivalent in the X-anchored form. Placement is what decides
it: as a **confirmation signal it runs once per converged climb** and is noise;
put it inside the climb loop, per scored board, and it becomes ~1% on the hot
path, which this repo treats as a real regression needing a `make bench` A/B
under both compilers. Note `score_iter` would **not** count it in either case —
the same blind spot documented for `--polish`'s gain scan — so judge it on wall
time, not on the counter.

*What it is, precisely:* a **one-sided, zero-false-positive confirmation
signal**. If it fires the key is right; if it does not, nothing is learned. That
is exactly the shape item 4 identified as the only defensible one — a non-firing
cannot dilute a good score the way an additive term does. It is **not** a score
term, and it cannot help a search failure.

*Not attempted, and now the only live form: the SELF-CRIB DEDUCTION.* It
sidesteps the failure above entirely, because it works on the **ciphertext** and
needs no correct decrypt at all.

Decryption is `p_i = steck[core_i[steck[c_i]]]`, with `core_i` the involution
`setup_mapping()` already tabulates as `rows[i]`. A classic crib knows `p_i` and
rearranges to `steck[p_i] = core_i[steck[c_i]]`. A self-crib knows only that two
positions carry the *same* letter, `p_i = p_j`. Substituting and cancelling
`steck` from both sides (it is an involution):

    core_i[steck[c_i]] = core_j[steck[c_j]]

**The plaintext letter has vanished from the equation** — that is the whole
idea. Since `core_j` is an involution it rearranges to a propagation rule with
`σ = core_j ∘ core_i`, computable from the rotor key alone:

    steck[c_j] = σ(steck[c_i])

Guess `steck[c_i]` and `steck[c_j]` follows: two plug assertions, which the
diagonal board doubles (`steck[x]=y ⟺ steck[y]=x`, no shared partners).

*Why it is weaker than it looks, and this is the part to internalise before
building anything.* Rejection power comes **only from loops** in the menu. A
tree is always satisfiable — guess the root, propagate, never contradict — while
a loop imposes `σ_loop(x) = x`, which fails unless σ_loop has a fixed point. If
the `2L` ciphertext letters of a length-`L` doubling are distinct, the menu is
`L` **disjoint edges**: a forest, zero loops, **zero rejection power**. Loops
appear only when ciphertext letters repeat among those positions or a deduced
endpoint collides with another menu letter — a birthday trickle, not a designed
structure. Same lesson as `archived/cribs.md` §4.1 from the other side: the
diagonal board does the work, not menu length.

*What rescues it: the flanking X is REAL known plaintext.* Measured on the
corpus (`doubling_probe.py`), the pattern is not `W X W` but **`X W X W X`** —
**96% carry an X immediately left and 71% on both sides**, 0% neither. Those
X's are crib letters, and all of them share a left-hand side:

    steck[X] = core_{i-1}[steck[c_{i-1}]]
             = core_{i+L}[steck[c_{i+L}]]
             = core_{i+2L+1}[steck[c_{i+2L+1}]]

So guessing `steck[X]` (26 ways) deduces three plugs at once and **anchors** the
otherwise-floating equality edges. The hypothesis is really a 3-letter crib plus
`L` equality constraints — a menu with anchors rather than a forest. Two prunes
come free from self-encryption: any alignment with `c_{i+L} = X` is impossible
outright, and if `c_{i+t} = c_{j+t}` then `core_i[a] = core_j[a]` must hold,
satisfiable only at a fixed point of σ.

*The obstacle that decides it — measure this FIRST.* The alignment is unknown,
so for a 150-letter message with `L` ∈ 6..12 it is ~950 hypotheses, and a key is
rejected only if **every** one is contradictory. Rejections multiply, so what
matters is `∏ p_h`: even at a per-hypothesis rejection of 0.99, `0.99^950 ≈
7e-5`, i.e. essentially nothing rejected. This is not speculation — it is the
documented compounding that takes a 12-letter crib from 99.9% pinned to **5.3%**
swept, where the product of the measured per-alignment rates predicted 5.2%
against 5.3% observed (`archived/cribs.md` §4.2a). **The per-hypothesis
rejection rate has to be extraordinarily close to 1**, far higher than an
ordinary crib needs, purely because there are so many alignments. It is
unmeasured; do not estimate it, measure it.

*If the sweep fails, two fallbacks.* Use it as a **seeder rather than a filter**
— `--crib`'s hybrid already pins deduced plugs and lets the climb find the rest,
measured 92% of letters recovered against 8% unseeded; 950 climbs per key is
impossible, but ranking hypotheses by plugs deduced and seeding from the top few
is not. And note the **cost regime**: ~950 alignments × 26 guesses is roughly a
plugboard climb's worth of work per key — negligible beside a `-c` climb, but
~1000× the cost of a scanned key, so it cannot ride along on a plain scan.

*Reusable artifact:* `eval/results-doubling-climb-texts.json` holds every
decrypt from the run (46 true-key + 5888 wrong-key, climbed with the board
hidden). Item 4's probe threw its texts away and had to be re-run from scratch
to ask one new question of the same data; this one should not. `REUSE=1` re-runs
the analysis without re-climbing.

→ `eval/doubling_probe.py`, `eval/doubling_climb_probe.py`,
`eval/results-doubling.txt`, `eval/results-doubling-climb.txt`; item 4 above;
`archived/cribs.md` §4.2a.

## Keyspace reductions

The two-notch collapse that used to head this section has **shipped** and is
no longer an issue: `CLAUDE.md` "Two-notch wheels" and the CHANGELOG carry it.

**6. Does the middle-wheel collapse's saving convert?** The §7.12 reduction is
3–5× at short lengths and the compute is saved; whether spending it on `-R`
raises recovery is untested. The same question was asked of `--ring-stride` and
answered "a wash". → `archived/IMPROVEMENTS.md` §2.

**7. A `--ring-stride` for the middle wheel — premise measured, not built.**
Striding `ring1` costs 3.1% (K=2) / 5.1% (K=3) of the true `offset1`, roughly
competitive with the right-wheel stride, and the two compose multiplicatively.
**Read the failed attempt first**: striding `ring1` directly measured 2.4×
*worse than not striding*, because thinning `ring1` switches off §7.12's exact
4.86× collapse. The sound axis is to thin the *representatives* §7.12 already
produces — which the measured numbers do **not** cover. →
`archived/IMPROVEMENTS.md` §2.

**8. Read the right wheel's phase off one decrypt instead of searching it —
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

**9. Crib supply at network scale.** The library covers 83% of held-out
messages, but on a 58-message corpus, and 47 of the 57 hits are 8–11 letters —
seed-only lengths. Whether a real network yields *long* cribs is the question
the whole feature rests on, and no larger corpus is available.

**10. Reject or rank?** The deduction rejects a rotor setting outright. Ranking
would tolerate a slightly-wrong crib — which matters, because garbling is real
(two of five `SIEGFRIED` messages are corrupted) and exact matching cannot see
through it.

**11. Menu reuse across alignments.** Shifting a crib by one position changes
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



**12. `--tune-phase` — MEASURED at three lengths and below saturation;
CLOSED.** The open half was whether the L=200 trade (more breaks, lower mean
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

*Now answered, and it pays outright.* Matched compute stops discriminating once
both arms saturate, so the useful question was how far **below** the exhaustive
sweep's cost `--tune-phase` can go. Swept over the same 40 instances at L=450
(`eval/tune_phase_budget.py`): `-R 8` matches the exhaustive arm's **38/40 for
23.4 s against 171.5 s — 7.3× cheaper** — and saturates there, `-R 16` being an
identical outcome for double the time; `-R 4` gives up one break for 14.5×. So
at operational lengths the flag is not a trade: it is the same result for a
seventh of the compute, at a *low* restart count nowhere near the `-R 42`
matched compute forced. Two of the three residual misses are flat across every
`-R`, so the residual is not budget-limited either. → `CLAUDE.md` "How the split
moves with length".

**13. `--ring-stride` with a hidden plugboard at K=13.** The one cell where
anything moved: 4 losses in 69 trials against 0 in 72 for a paired given-board
control, direction consistent across two seeds but **p ≈ 0.13 — suggestive, not
established**, and only at a stride already outside the recommended K≤3.
Settling it needs ~200 trials (~3–4 h) and buys nothing operational. →
`archived/PERFORMANCE.md` §7.11; `eval/ring_stride_scope_probe.py`.

**14. Report a CLOSE-MATCH rate beside the mean and the exact rate.** Every
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

**15. `-Wconversion` (~52 warnings) deliberately deferred.** 43 are `int →
unsigned char` narrowings in the hottest loops; that many casts clutter the hot
path for a low-value nit on deliberately C-style code. A future ratchet, not a
bug.

**16. No `install` target**, and the n-gram files are not declared as build/run
dependencies. Fine for development; add if the tool is packaged.

**17. Single-file distribution.** Embedding the tables was declined once, but
the shipped uint8 tables are ~4× smaller than the float tables that analysis
assumed, so a blob is much cheaper now. Keep `-d` / `$ENIGMA_DATA` as the
override.

**18. The `Scoring:` line can exceed 79 columns** when the `-d` path is long.
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
