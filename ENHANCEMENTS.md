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

**5. Repeated text with an X between — SHIPPED as `--self-crib-seeds`, and it
beats `-R`.** The seeder is now in `src/crib.cc` (see `CLAUDE.md`), which closes
the item's last open half. Two things came out of building it that the probes
could not see, and one of them is a correction to the numbers below.

*The measured sweep cost was ~10× too high, against the seeder.*
`eval/seeded_sweep.py` drove the method from Python, one process per (key,
seed). `--polish` is a **once-per-run** finisher costing ~10 700 `score_iter`,
so that rig made every key pay a full finisher, while the baseline arm swept all
676 keys in one process and paid it once. In the tool both arms pay it once. The
honest sweep numbers (`-g A..`, 676 keys, 30 trials,
`eval/self_crib_ab.py`):

| arm | mean %-correct | exact | `score_iter` | per key |
|---|---:|---:|---:|---:|
| `-R 1` | 15.2 | 3/30 | 1 543 131 | 2 283 |
| `-R 4` | 31.1 | 8/30 | 6 165 494 | 9 121 |
| `-R 8` | 40.7 | 11/30 | 12 329 190 | 18 238 |
| `-R 16` | 56.5 | 16/30 | 24 670 103 | 36 494 |
| **`--self-crib-seeds 1`** | **70.9** | **20/30** | **284 536** | **421** |
| `--self-crib-seeds 3` | 77.3 | 22/30 | 897 999 | 1 328 |
| `--self-crib-seeds 5` | 83.7 | 24/30 | 1 549 424 | 2 292 |

`K = 1` beats `-R 16` — 20/30 against 16/30 — at **87× less compute**, and is
5.4× cheaper than the *cheapest* baseline while recovering 20/30 against 3/30.
The Python rig had this at 13/20 against 4/20 with the seeded arm merely
cost-*matched*; correcting the finisher accounting turns a matched-compute win
into a nearly two-orders-of-magnitude one.

*`--self-crib-length` is a knob that only loses — the default is 4.* The floor
drops short hypotheses, and the reasoning that motivated it (an `L = 4` menu
rejects nothing) does not survive contact: per-key cost is **flat** across the
whole range (384–454 `score_iter`), because the cost is the `K` climbs and not
the deduction. Recovery at `K = 1`, 30 trials:

| `L` | 2 | **3** | **4** | 5 | 6 | 7 | 8 |
|---|---:|---:|---:|---:|---:|---:|---:|
| exact | 18/30 | **20/30** | **20/30** | 18/30 | 17/30 | 14/30 | 7/30 |

3 and 4 tie on a plateau and everything else loses. 4 is the default because it
is also the shortest signature the corpus contains (`HOCK`); the flag stays so a
caller who *knows* a network's names are long can say so, not as a tuning knob.
The weak short hypotheses cost nothing because the IC ranking already discards
them.

*The X between the copies is REQUIRED, and dropping it is viable — measured,
not built.* `--self-crib-seeds` guesses `steck[X]` and seeds `self_crib_try`
with exactly that, so the equality edges cannot start anything until an anchor
propagates the guess into the message. A doubling with **no separator at all**
— `SIEGFRIEDSIEGFRIED` — therefore forms no hypothesis and is invisible. Three
of the 66 corpus messages carry a 6+ separator-free doubling and no
X-separated one anywhere: `SIEGFRIED`, `OSTROW`, `ROSENOW`.

The algebra does not need the X. An equality edge gives
`steck[c_j] = core_j[core_i[steck[c_i]]]`, which relates two *ciphertext*
letters' plugs and mentions no plaintext; guess `steck[c_0]` instead of
`steck[X]` and the same closure runs. What is lost is edges, not grounding.
`eval/selfcrib_noanchor.py`, true key, 200 trials, 10-pair board, 6+ letters —
the middle arm is the controlled one, the same word at the same alignment with
only the assertion withheld:

| arm | survivors | cables | correct exists | rank 1 | top-5 |
|---|---:|---:|---:|---:|---:|
| `W X W`, separator asserted | 4.9 | 4.2 | 197/200 | 167 | 192 |
| `W X W`, anchor withheld | 6.6 | 3.7 | 195/200 | 142 | 182 |
| `W W`, no anchor at all | 8.7 | 3.4 | 195/200 | 117 | 168 |
| `W W` + left flank asserted | 7.2 | 3.6 | 195/200 | 136 | 182 |

**The anchor is worth sharpness, not existence.** Withholding it costs 1.3×
more survivors and half a cable; the genuinely separator-free case costs 1.8×
more survivors (8.7) and 0.8 fewer cables. What barely moves is **whether a
correct hypothesis exists at all** — 197/200 against 195/200 — so recall is
essentially intact and only the *ranking* degrades, top-5 96% → 84%. At the
shipped `K = 10` operating point 8.7 survivors are all climbed anyway, so the
extra cost is close to nothing in absolute terms.

**A tandem repeat is not anchorless in practice, and that is the design point.**
It has no separator, but it usually has an X *before* it — 4 of 4 in this
corpus, matching the 96% left-flank rate already recorded for the separated
case. Asserting that flank recovers most of the loss: top-5 goes 168 → **182**,
level with the separated word whose own anchor is withheld. So the variant
worth shipping is *tandem with flanks*, not tandem bare.

**It should be OPT-IN, and the cost decides that rather than the benefit.**
Enumerating gap 0 alongside gap 1 **doubles the hypothesis count** — 45 024 →
45 552 more, +101% over the corpus — and per-key cost tracks hypothesis count
almost linearly (2 196 hypotheses ↔ 2 428 µs, 1 328 ↔ 1 065). That would take
the seeder from ~2 428 µs per key to ~4 900, past the 2 901 µs of the `-R 16`
baseline it is measured against, i.e. it would cost the feature its headline.
The coverage bought is **3 of 66 messages, +4.5pp** (`SIEGFRIED`, `OSTROW`,
`ROSENOW`). A doubling of cost for 4.5pp is the classic opt-in profile.

Two caveats before acting on it. The `W W` pool is **4 messages**, so those rows
rest on a narrow base however many trials are drawn from them — and that is a
corpus limit, not a sampling one, so more trials cannot fix it. And this is the
**true key**: whether wrong keys also survive more — which would cost
discrimination on a sweep — is unmeasured, the same gap the crib-seeds work had
to close separately, and it needs the implementation to measure.

*The C++ closure is checked against the probe, not just against outcomes.* A
wrong deduction could still recover messages by luck, so the two are compared on
the **number of distinct surviving seeds per key** — a structural quantity —
and they agree exactly, 0 mismatches over 6 keys spanning 1 to 43 seeds.

**5. Repeated text with an X between — the REPORTER is shipped as
`--doubling-report L`; the SCORE BONUS and the SELF-CRIB FILTER are measured
down; the SIGNATURE SEEDER beats `-R` at matched compute (**at ~100 letters —
at 167 it does not; see the `--self-crib-seeds` entry in `CLAUDE.md`**) and is
SHIPPED as `--self-crib-seeds`.** The split
is
the whole story of this item. As a *confirmation signal* — report the doubling,
change no ranking — it works and is now in `src/progress.cc` as
`--doubling-report L`
(gate at `--doubling-z Z`, default 3): see the entry in `CLAUDE.md`, note
(a)/(b)
below for why that shape is the defensible one, and `tests/run_tests.sh` for
the checks. As a *score bonus* (note (e)) it
is dead: 140 genuine 17 576-key sweeps found **zero** trials where the climb
recovered the plaintext and the score still lost, so there was nothing for a
post-climb bonus to act on.

Everything else below is still `eval/` probes only — no flag and no `make bench`
number for the anchored variant, the self-crib deduction, or the score bonus.
Where this entry says "the settled setting" it means the configuration those
probes standardised on. The headline that used to sit here — *15 of 15 real
scoring failures rescued, 0 false positives in 8 928* — is the probe's number
for the DETECTOR, and it is the reason the reporter was worth building; it is
not a claim about the bonus, which the sweep settled separately.

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

*That "0.000%" is a floor, not a rate, and it was the wrong population*
(`eval/doubling_null_probe.py`). 20 000 shuffles expect **0.28** hits at this
rule, so zero was the likely outcome and bounded nothing — and shuffled real
decrypts are not what the rule would ever be applied to. **The population that
matters is decrypts at wrong rotor keys with the plugboard hill-climbed**, and
its null is:

> **≈ 5e-6 — one false hit per ~200 000 climbed candidates.**

*Where that comes from.* A window needs an X at the centre (probability `p`)
and `2L` non-X flanking letters with ≤1 mismatch, so with `A` = P(two letters
equal and neither X) and `B` = P(both non-X),

    E[hits] = Σ_L (n − 2L) · p · [ A^L + L · A^(L−1) · (B − A) ]

Each extra letter costs `B/A ≈ 16`, so the threshold `L = 6` sets the rate
almost alone, and within it the one-mismatch term dominates. On the climbed
population (`X` = **2.41%**, `A` = 0.0508) that gives **4.9e-6**.

*Why this population is SAFER than the shuffle null, not worse.* The rate is
roughly linear in the X-rate, because the rule needs an X separator. A
plugboard climb maximises an n-gram score and German prose is X-poor, so
climbing a **wrong** key drives X *down* — **2.41% against 5.58%** in true-key
decrypts and 6.84% in the shuffles. The one letter the rule depends on moves
the safe way.

*And the closed form is confirmed, not assumed.* It presumes memoryless
letters, which real German badly violates — a bigram chain fitted to the corpus
fires ~9× more often than its letter frequencies predict. That penalty does
**not** carry over, because climbed wrong-key text is nearly structureless: its
bigram IC is **1.07×** what independence predicts, against **1.48×** for real
decrypts. Fitting both an i.i.d. and a bigram-Markov generator to the climbed
population and running 1.5 M trials each gives **16 hits in 3.0 M = 5.3e-6**,
95% CI [3.0e-6, 8.7e-6] — the closed form sits inside, and the Markov/i.i.d.
gap is not significant.

The direct count over the stored climbed decrypts — **0 in 5 888** — expects
0.03 and so demonstrates nothing on its own. Neither did the "0 false positives
in 8 928" quoted below, which expects 0.04.

*What the rate allows.* Expected false hits = candidates × 4.9e-6:

| climbed keys | 10 000 | **203 000** | 10⁷ | 10⁸ |
|---|---:|---:|---:|---:|
| expected false hits | 0.05 | **1.0** | 49 | 493 |

**So it cannot run across a whole sweep.** A real unknown-key run climbs
10⁷–10⁸ keys, which is tens to hundreds of spurious hits — against *one* true
key that carries a qualifying doubling only **28% of the time** (13 of 46
true-key decrypts). Swamped on both terms. It is sound as a **confirmer on a
shortlist**, below roughly **200 000 candidates**, which is the regime the
entry proposes it for.

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

**(a) Anchor on the X's first — MEASURED, and DOMINATED. Do not build it.**
The pattern telegraphic German is supposed to write is `XPARISXPARIMX`, not
`PARISXPARIM`, so the alternative to scanning every `(i, L)` window is to split
on X and compare adjacent segments — the segmentation fixes the candidate
lengths and the length loop disappears. `eval/doubling_anchored_probe.py`
measures the three predictions this entry used to carry.

| prediction | verdict |
|---|---|
| ~100× cheaper | **true** (133×) — and irrelevant |
| shorter `L` viable | **true on the null** — nothing to spend it on |
| little or no recall lost | **false** — and this is the axis that decides it |

*1. Cost is true and does not matter.* The scan is 911 window comparisons per
message; one plugboard climb on the same 151-letter message scores **18 441
boards × 151 letters = 2.78 M operations**, and the check would run once per
*converged* board. So the scan is **0.36% of a single climb** and anchoring
takes that to 0.003%. There is nothing on this axis to win.

*2. The null does fall, but the premise was wrong.* Anchoring cuts the chance
rate ~100× where it can be measured (`L≥3`: 4.765% → 0.045%; `L≥4`: 0.495% →
0.005%). But `L ≥ 6` was never forced by the null — the settled `len≥6, mm≤1`
already measures **0.000%**, so there was no chance rate to relieve. And under
anchoring a shorter length adds nothing anyway: 6 → 5 gains **+1** message
unanchored and **+0** anchored. The binding constraint is the X-enclosure, not
the length.

*3. Recall is lost, and one-directionally.* At item 5's setting the anchored
form finds **21 of 54 against 25**; at `mm≤1` the loss runs −3 to −8 across
`L = 7…3`. The head-to-head the trade was actually about — spend the
enclosure to buy a shorter word — is **unanchored `L≥6`: 25 messages, 0.000%
null** against **anchored `L≥5`: 21 messages, 0.000% null**. Worse on recall,
no better on the null. And the anchored hit set is a strict **subset** at every
setting tried (0 only-anchored against 4–5 only-unanchored), which is
structural rather than a sample-size accident: an adjacent equal-length segment
pair *is* a window hit at that `(i, L)`, so anchoring can only ever remove
recall.

**Why, and it is the informative part: the doubled word is not always enclosed
by X.** The four messages between unanchored `L≥6` and anchored `L≥5` are all
genuine doublings, and none is an end-of-message case:

```
GEHRG  ...NULLNULLUHRIN[ROMANOVKA]X[ROMANOVKA]GKLAMMXPOLA...
MNQBH  ...BITTEANTWORTAQX[ZANDERS]X[ZANDERS]VONDORNTELEFON...
ABGUX  ...NAQMUSTERSEQSZUM[SIOBEN]X[SIEBEN]XEIXSZWOCULLNS...
ABNAQ  ...NTLLNULLGEGJNDX[WASKOWA]X[WASKTWA]EINSZWOKMSUEDWEST...
```

Operators run the doubled word together with what follows, so the boundary an
anchored rule needs is often simply not written. Measured on the corpus, a
doubling is flanked on **both** sides only **71%** of the time (24 of 34
instances at `len≥6, mm≤1`; 24% left only, 3% right only, 3% neither) — so this
entry's own summary that "the real pattern is `X W X W X`" holds for about
seven doublings in ten, and its recorded "0% neither" is stale at 3% since
GEHRG joined the corpus.

**(c) Let the SEPARATOR be garbled too — ONE instance, after a withdrawal.**
The rule requires the separator to be a literal `X`. This note originally cited
two counter-examples; **one has been withdrawn**. `HENNING(J)HENNING` in ALVPM
turned out to be *our* transcription error — the message form reads
`HENNINGXHENNING`, and the `J` came from a `b` misread as `t` (§5j of
`eval/MODERN_BREAKING_NOTES.md`). What survives is `ROMANOVKA(G)KLAMM` in GEHRG,
whose ciphertext was not transcribed here. The argument still holds on its own
terms — Enigma corrupts one letter per corrupted ciphertext letter and the
separator is just another letter, so there is no reason to privilege it — but it
now rests on **one** observation, not a rate. Get more instances before costing
the change.

**(d) Real doublings can differ in LENGTH — CONFIRMED against the form.** The
matcher takes `|W| = |V|`, which the no-diffusion argument appears to justify:
one corrupted ciphertext letter corrupts exactly one plaintext letter, so
transmission garbles are pure substitutions. ALVPM breaks that from the other
side. Its doubled surname is `SCUHNACHER` (10) against `SCHUHMACHER` (11) —
*Schumacher* against *Schuhmacher*, **the operator spelling the name two ways**.
Verified against the form: the short copy would need ciphertext `x→j` and `c→t`,
which are not plausible misreadings, and the form's stated length of 177 matches
the transcription exactly, so nothing was dropped in the ciphertext either. So
indels do occur in real doublings — just from the keyboard, not the channel,
which is the half the no-diffusion argument never covered. Length-tolerant
matching would need edit distance rather than Hamming, which is more expensive;
one confirmed instance is still not a rate.

**(b) Cost against the hillclimb — negligible, IF it runs in the right place.**
Rough arithmetic, to be confirmed on wall time: the check as designed is ~11
`score_iter`-equivalents, and one restart is ~1250 `score_iter` (inferred from
`--polish`'s measured ~6500 being 2.8–3.3% of a run at `-R 160`). So **~0.9% of
a single restart**, less if run once per key rather than per restart, and under
one `score_iter`-equivalent in the X-anchored form. Placement is what decides
it: as a **confirmation signal it would run once per converged climb** and is
noise; put it inside the climb loop, per scored board, and it becomes ~1% on
the hot
path, which this repo treats as a real regression needing a `make bench` A/B
under both compilers. Note `score_iter` would **not** count it in either case —
the same blind spot documented for `--polish`'s gain scan — so judge it on wall
time, not on the counter.

*What it is, precisely:* a **one-sided, zero-false-positive confirmation
signal**. If it fires the key is right; if it does not, nothing is learned. That
is exactly the shape item 4 identified as the only defensible one — a non-firing
cannot dilute a good score the way an additive term does. It is **not** a score
term, and it cannot help a search failure.

**(e) Fold it into the SCORE as a length-graded bonus behind a z gate —
SWEPT AND MEASURED DOWN; do not build it.** The sweep the design asked for has
been run (140 genuine 17 576-key sweeps): the constants turn out not to be the
question, because a post-climb bonus needs a trial where the climb recovered
the plaintext and the score still lost, and there were **zero** of those. See
the sweep verdict at the end of this note. The design below is kept because
its reasoning is what the sweep tested. The
confirmation signal above is one-sided: it identifies the true key when it
fires and says nothing when it does not. The alternative is to add it to the
score, so a doubling *helps the true key win* rather than merely flagging it.
Two parameters:

    gate    apply only to candidates with z > T          (T = 3 tried)
    bonus   M x [ 5.02 + 1.2*(L-6) ] decades, on the LONGEST doubling
                 ^ the MULTIPLIER form, not an additive offset -- see below,
                   that part is settled and does not depend on M

*The slope is determined, not tuned.* Measured Bayes factors over the 54
authentic decrypts against the closed-form null give a log-likelihood ratio
**linear in L at +1.2 decades per letter** — which is exactly `log10(B/A) =
log10(16.4)`, the same factor that sets the null rate. `L = 6` is worth **5.02
decades** (1e5:1) and `L = 13` worth 13.4. There is nothing to fit in the
*shape*; only the multiplier `M` and the gate `T` are free.

*Use the longest doubling, not a sum.* Overlapping windows in one doubled word
are not independent evidence — the `L` and `L-1` hits are the same fact.

*Why a multiplier is needed at all.* At `M = 1` the bonus is correct and
useless. Against the 15 recorded scoring failures (`results-scoring-failure.
json`) — of which **only 8 actually lose to a wrong key**, the other 7 being
classed as failures purely by the `√(2 ln K)` bar — the gaps run **4.8 to 69.4
decades**, median ~20, while an `L = 6` doubling is worth 5. So the honest
weight closes the smallest gap in the set and nothing else:

| `M` | scoring failures rescued (of 8) | P(a wrong key steals a win) |
|---:|---:|---|
| 1 | 1 | 1 in 1 500 000 |
| 2 | 3 | 1 in 190 000 |
| 3 | 4 | 1 in 28 000 |
| **5** | **7** | **1 in 1 000** |
| 8 | 7 | 1 in 300 |
| 14 | 8 | 1 in 35 |

at `T = 3` over a 10⁷-key sweep. **`M = 5` is the knee** — 8 buys no extra
rescue for 3× the risk, and 14 (the only value that takes HOEPG's 69-decade
gap) is plainly too hot.

*The gate is what makes any of this affordable.* Without it, `M = 5` puts 11%
of currently-successful breaks within reach of a wrong key carrying a chance
doubling. With `z > 3` a wrong key needs **three** things at once: clear the
gate (13 498 of 10⁷), carry a chance doubling (4.9e-6 of those — 0.067 per
sweep), and land within 25 decades of the truth, which sits at a median z of
10.0 with σ_total ≈ 12.6 decades.

*A MULTIPLIER is the right shape, and that is settled independently of its
value.* The obvious alternative is to keep the honest slope and add a fixed
offset — `calibrated(L) + C` — or to drop the length term entirely and use a
flat constant. The three differ only in *which lengths* they over-weight, and
the null decides between them: it falls by a factor `B/A ≈ 16` per letter, so
**94% of chance doublings are `L = 6`** and the false-positive exposure is set
almost entirely by `bonus(6)`. Hold that fixed and the forms separate:

| form | L=6 | L=8 | L=10 | rescued of 8 | risk |
|---|---:|---:|---:|---:|---|
| **`M × calibrated`, M = 5** | 25.1 | 37.1 | 49.1 | **7** | 1 in 1 038 |
| `calibrated + C`, C = 20.1 | 25.1 | 27.5 | 29.9 | 6 | 1 in 1 032 |
| flat constant 25.1 | 25.1 | 25.1 | 25.1 | 5 | 1 in 1 038 |

**The multiplier wins because its extra weight lands where the null is thin.**
A chance `L = 10` doubling is ~10⁵ times rarer than a chance `L = 6` one, so
scaling the whole curve buys a large bonus at long lengths for almost no risk;
an offset hands long and short doublings the same boost and therefore spends
its budget at the one length where chance actually competes. It matters here
because the hard cases *have* long doublings — the two gaps above 27 decades
are `L = 10` and `L = 8`, which the offset cannot reach without raising
`bonus(6)` and paying at the crowded end. (A fixed `+25` on top of the
calibrated curve does rescue 7, but at **1 in 487** — twice the risk of
`M = 5` for the same result.)

Two qualifications, both pointing the same way. It is **8 cases**, so "7 vs 6
vs 5" is one or two messages; the *mechanism* is robust and the counts are not.
And the ordering leans on the length distribution of those 8 — if real failures
at operational length carry shorter doublings, the multiplier's advantage
shrinks toward the offset's, which is the same population question the sweep
has to settle anyway.

*Both constants were swept jointly, and the sweep says do not build it — the
population it acts on does not occur.* `eval/doubling_bonus_sweep.py`, 140
genuine 17 576-key sweeps of authentic telegraphic German cut to contain a
doubling (100 at `-R 8` over L = 60–140, 40 at `-R 64` over L = 80–120), each
under `-c -f -l wehrmacht -S i4f10 -J --polish --dump-all`, so the competitors
are the **top-scoring keys of a real search** rather than the 48 random ones
the numbers above rest on. Full output in
`eval/results-doubling-bonus-sweep.txt`.

**The finding is placement, not calibration.** A bonus applied after the climb
needs a trial where the climb *recovered the plaintext* and the score still
lost. In 140 sweeps there were **zero**: 35 trials recovered ≥90% of the
letters, and in **35 of 35** the true key was already top. Every other failure
was a search failure.

That is not luck, and the mechanism is the reason the whole idea does not
work. The climb is steered by the **same score** the bonus would adjust, so a
true key that does not stand out is also a true key whose plugboard the climb
cannot find. Scoring failure therefore presents as a *search* failure first:

| oracle outcome (true board vs best wrong) | trials | climb recovered |
|---|---:|---:|
| truth wins big (< −100 dec) | 48 | 46% |
| truth wins (−100…0) | 34 | 35% |
| truth loses (0…+50) | 23 | **4%** |
| truth loses big (> +50) | 35 | **0%** |

**0 of 35.** In every trial the feature was built for, the climb never
delivers a decrypt to read a doubling out of. To help at all the evidence
would have to **steer the climb** rather than rescore its output — which is
the ~1% hot-path cost note (b) prices, on a signal that fires on almost no
board mid-climb.

**And the grid cannot choose the constants, because the cost axis is
unmeasurable at this scale.** Every cell of every grid read `−0` stolen, so
the optimiser simply runs to the largest `M` offered: it picked *gate off,
M = 14* — 45/50 on train and **46/50 held out** against a baseline of 28. That
looks like a strong validated result and is an artefact of the grid's range.
A held-out split validates only the side that has data, and here that was the
rescue side, which was never in doubt.

The risk had to be **decomposed** instead, since the coincidence itself is too
rare to observe: chance doublings per sweep (0.040 ungated, 0.010 at `z > 3`)
times the chance the truth's lead is thinner than the bonus (10% under 25
decades, 17% under 70; minimum observed lead **1.2 decades**):

| gate | M=3 | M=5 | M=8 | M=14 |
|---|---|---|---|---|
| z>2 | 1 in 369 | 1 in 246 | 1 in 184 | 1 in 148 |
| z>3 | 1 in 1475 | **1 in 983** | 1 in 738 | 1 in 590 |

So `z > 3, M = 5` was the defensible cell — close to what was proposed — had
there been anything for it to rescue.

Three subsidiary results worth keeping:

- **Requirement 4, the independence assumption: mildly violated, not
  catastrophically.** 5 chance doublings in ~471 000 genuine competitors =
  1.1e-5, about **2×** the 4.9e-6 operational null. Run A's 4 hits all sat
  above `z = +2`; run B's single hit did not, so the enrichment is real in
  direction and weak in size. See the false-positive table below.
- **`margin > 0` is the wrong shape for a gate.** It subtracts `√(2 ln K)`, so
  it means `z > 4.42` on a 17 576-key sweep and `z > 5.68` on a 10⁷ one, while
  the true key's z does not grow with `K` (median 3.76 at L=80, 7.59 at L=100).
  A keyspace-scaled gate closes hardest exactly where the feature was wanted.
- **The scoring ceiling gets LOWER as the search gets stronger.** At L = 100
  the median oracle gap moved from −34.8 decades at `-R 8` to +3.2 at `-R 64`:
  extra restarts help the true key's climb, but they also let all 17 575 wrong
  keys overfit their plugboards harder, and there are far more of them. The two
  runs draw different instances so this is directional, not a paired number.

*Where the false positives would have come from, had it been built.* Two
knobs move them, and they are not equally priced. Per full 230 M-key rotor
sweep (`-r A..`, wheels I–V, one reflector), with the measured **0.559%** of
keys clearing `z > 3` — not the 0.135% a Gaussian gives, since the real upper
tail is fatter:

| gate | keys above gate | FP at `L ≥ 6` | FP at `L ≥ 7` | rescues at `M = 5` |
|---|---:|---:|---:|---|
| `z > 3` | 1.3 M | 93 | **6** | +10 / +6 |
| `z > 2` | 7.8 M | 370 | 23 | +10 / +6 |
| `z > 1` | 35.5 M | 470 | 29 | +10 / +6 |
| `z > 0` | ~100 M | ~800 | ~50 | +10 / +6 |
| off | 230 M | ~1 100 | ~70 | +10 / +6 |

(rescues are run A of 100 / run B of 40. Counts are exact for `z ≥ 1`, where
each sweep's complete top-3000 slice covers the whole above-gate population;
the two loose rows are bounds from the independent 4.9e-6 null.)

**Two things fall out, and both say the proposed settings were already
right.** The rescue column is **flat at `M ≤ 5`** — `z > 3` recovers exactly
the same trials as no gate at all, for a fifth of the false positives — so
loosening buys literally nothing at the multiplier one would want. It pays
only at `M ≥ 8`, which is also where the bonus is large enough to be stolen;
gain and risk arrive together, which is why the grid ran to `M = 14`. The
reason is visible in the true key's z: once the climb has recovered the
plaintext z is 7–16, nowhere near the gate, and the trials below `z = 3` are
the ones where the climb failed and there is no doubling to find.

And the growth is concentrated between 3 and 2 (a 4× step); below `z > 2` the
count barely moves, because all five observed doublings sit between `z = 1.85`
and `3.57`. So the **length floor is the cheap lever and the gate is not**:
each extra letter is another **16×** (the null falls by `B/A ≈ 16.4` per
letter, so ~94% of chance doublings are `L = 6`), and it costs the true key
almost nothing, since a real doubled word is a whole word — `ROMANOWO` is 8,
`KOCHLING` 8, `SCHUHMACHER` 11. Tightening the gate throws the true key out
along with the chaff; raising the floor does not.

*A measurement trap this sweep walked into, worth the warning.* Double stepping
makes two grundstellungen the **same key**: with the middle wheel on its notch
the first keypress steps the middle *and* left wheels, so `(g₀, N, g₂)` and
`(g₀+1, N+1, g₂)` agree for the whole message (it fails if the right wheel is
also on its notch). Such a start is not a competitor — it is the true key under
another name — and it fires on ~7% of trials. Left in the candidate list it
ties the true key's score exactly, so a strict `>` test scored a **loss** on
trials the search got right, and it carried the plaintext's **real** doubling,
which was then counted as a **chance** doubling among the wrong keys. Three of
the first seven "chance doublings" were this. Uncorrected, the doubling rate
appeared to climb steeply with z and peak in the tail — which reads as the gate
*concentrating* false positives rather than excluding them, the exact opposite
of the truth. `alias_starts()` in the harness; verified against an exhaustive
17 576-start search, 0 mismatches.

*The SELF-CRIB DEDUCTION — BUILT AS A PROBE AND MEASURED DOWN AS A FILTER.*
`eval/selfcrib_probe.py`, results in `eval/results-selfcrib.txt`. The algebra
below is correct and the deduction works; what fails is the sweep, and the
reason is worth recording because it is the same trap §4.2a describes.

**The measurement.** Per-alignment rejection on wrong keys looks strong — 0.32
bare, 0.36 with the separator, 0.68 adding the left flank, **0.89** with both
flanks. Extrapolating `(1-p)^950` over the unknown alignment gives ~0 survivors,
i.e. a filter that rejects everything wrong. **That extrapolation is worthless
and the direct measurement says so: 0 of 160 wrong keys were rejected**, on four
messages with 1 000–2 000 hypotheses each. The true key survived every time, as
it must.

**Why.** The per-hypothesis rate is strongly length-dependent, and a sweep is
dominated by its *weakest* hypotheses, not its strongest:

| L | `sep` | `sep+L` | `sep+L+R` |
|---:|---:|---:|---:|
| 6 | **0.030** | 0.203 | 0.528 |
| 9 | 0.253 | 0.672 | 0.930 |
| 12 | 0.609 | 0.924 | 0.995 |

A key is rejected only if **every** hypothesis contradicts, and ~1 200 of 2 855
survive per wrong key — consistently ~42%. The 0.89 that looks decisive is only
available at the true alignment, the true length and the true flanks, which is
exactly what a sweep does not know. Measuring at the true alignment is a
selection effect on the measurement itself; that is why the swept number had to
be taken directly.

**Two things the probe corrected in this note's own reasoning.** The claim that
a distinct-letter doubling is a rejection-free forest is **wrong** — the bare
menu rejects 31.6% per alignment, because the diagonal board makes disjoint
edges interact through the matching constraint, the same mechanism recorded at
`archived/cribs.md` §4.1 (a loop-free 12-letter menu rejects 88%, against 0%
without the board). And the flanking X's are a **hypothesis, not a fact**: they
hold 93% (left) and 63% (both) of the time here, so a sweep must enumerate them,
which multiplies the alignment count — the first version of the probe asserted
them unconditionally and rejected the true key on 3 of 40 messages
(`ROMANOVKA` is flanked by `N` and `G`).

*The TERMINAL SIGNATURE variant — measured too, and also down.* Half the
corpus's doublings are a signed surname closing the message (`RENNER`,
`MATHIAT`, `STEINECKE`, `STUERZBAECHER`, `HENNING` after `GEZ`): **10 of 66
decrypts end with one**, always with 0 or 1 trailing letters. That pins the
alignment, which is the only thing that killed the swept version — the
hypothesis set falls from ~2 800 to **~19**, the word's length being the only
unknown.

It still does not work, for a different reason each time you look:

- **The short members are unfalsifiable.** `L = 4` and `L = 5` have 6–7 edges
  and reject **0.000** of wrong keys, against 1.000 at `L ≥ 8`. A key survives
  if *any* hypothesis does, so the weakest member sets the floor and it is
  zero. 12–16 of 18 hypotheses reject on every key; never all 18.
- **Raising the floor does not fix it, because rejection is
  message-dependent.** At `minL = 7` the aggregate is still 89% kept, and the
  spread across messages is enormous — 18%, 38%, 92% on three messages with
  the same setting. Whether a menu has the collisions that create
  contradictions is a property of *that ciphertext*, not of `L`: on one message
  the `L = 9` hypothesis rejects 1.00, on another 0.27, and one weak member
  admits everything.
- **The premise costs nothing because it buys nothing.** The true key survives
  10/10 on messages that do end with a doubling — as it must — and **8/8 on
  messages that do not**, where the hypothesis is false. A filter too weak to
  reject the true key on a false premise is too weak to reject wrong keys on a
  true one.

And the comparison that settles it: if you are willing to assume the message
ends with a doubled surname, `--crib-list` with a name list is strictly better
— a 7-letter crib at a *known* position rejects 99.9%. The self-crib's whole
advantage was needing no vocabulary, and it buys a keyspace reduction between
1.1× and 5.5× depending on the message. Not worth building.

*The SWEPT SEEDER — measured, and down.* Rejection
being dead does not by itself kill seeding: `--crib`'s hybrid pins deduced plugs
and lets the climb find the rest (92% of letters recovered against 8%
unseeded), and that needs only the true hypothesis to *rank highly* among the
~2 800, not to be the sole survivor. But the requirement is sharper than it
looks, because **a wrongly pinned plug is worse than no pin at all** — the climb
cannot undo it, and `--crib`'s pins deliberately survive `--polish`. So the
number that matters is the *precision* of the top-ranked seeds, not the recall
of the true one.

Ranking the surviving `(hypothesis, guess)` seeds at the TRUE key by **plugs
deduced**, which is the ranking this note proposed:

| n | seeds | rank of first correct seed | top-5 plugs (correct) | precision |
|---:|---:|---:|---|---:|
| 48 | 2 091 | 12 | 20(0) 18(0) 18(0) 18(0) 18(0) | **0.00** |
| 101 | 2 946 | 157 | 22(2) 20(1) 20(0) 19(0) 19(0) | 0.03 |
| 172 | 6 084 | 17 | 21(0) 21(0) 20(0) 20(0) 19(0) | **0.00** |
| 111 | 2 804 | 3 | 20(2) 20(0) 20(0) 19(19) 19(19) | 0.41 |

The seeds deducing the *most* plugs are the ones deducing them *wrongly* — a
hypothesis that forces 20 assignments is over-constrained by a false premise,
not close to the truth. Seeding from the top five would pin ~20 wrong plugs.

A second ranking, **menu selectivity**, fails too. At the true key the true menu
survives with exactly 1 guess of 26 — but the *median false* menu survives with
**0**, so 60–87% of false menus are more selective than the true one, and
~30–40% still survive, leaving thousands of candidates. The deduction's own
outputs do not identify which hypothesis is real.

*The SIGNATURE SEEDER — measured UP, and this is the open half of item 5.* The
paragraph above used to end by closing the item, on the argument that scoring
~2 000 seeds costs about what one extra `-R` restart costs, and restarts are
measured to deliver. That argument is right about the *swept* seeder and wrong
about this one, because **pinning the alignment to the end of the message
changes the seed count by two orders of magnitude**. Half the corpus's doublings
are a signed surname closing the message (`… X RENNER X RENNER`), so the
alignment is not unknown — only the name's *length* is, which leaves ~19
hypotheses rather than ~2 800, and ~28 surviving seeds rather than ~2 000.
Scoring 28 decrypts is not one restart; it is a rounding error.

Measured over 200 trials (the 10 corpus messages that end with a doubling × 20
fresh keys and 10-pair boards, `eval/selfcrib_probe.py`):

- **Recall is perfect: 200/200 trials have exactly one fully correct seed** —
  every assignment it makes agrees with the true board. It pins 12.7
  assignments on average (4–24), of which 5.4 (1–10) are actual cables; the
  rest are deduced *no*-cable findings, which `--crib`'s hybrid pins too.
- **It can be found without knowing the truth.** Ranking the ~28 seeds by their
  decrypt's score puts the correct one **first in 150 of 200 trials** under the
  index of coincidence, and in the top three 168 times.

Ranking by every scoring model, paired over the same 200 trials (`vs -i` counts
trials where only IC ranked it first / only that model did):

| signal | top-1 | top-3 | mean rank | vs `-i` | McNemar |
|---|---:|---:|---:|---:|---:|
| **`-i`** | **150/200** | **168/200** | **2.0** | — | — |
| `-m` | 138/200 | 162/200 | 2.5 | 28 / 16 | p = 0.096 |
| `-b` | 118/200 | 143/200 | 3.9 | 47 / 15 | p = 0.0001 |
| `-t` | 128/200 | 150/200 | 3.2 | 37 / 15 | p = 0.003 |
| `-q` | 129/200 | 154/200 | 3.1 | 36 / 15 | p = 0.005 |
| `-a` | 128/200 | 153/200 | 3.0 | 38 / 16 | p = 0.004 |
| `-f` | 144/200 | 165/200 | 2.2 | 22 / 16 | p = 0.42 |

**The index of coincidence ties the fused model and beats every other one** —
significantly for bi/tri/quad/weighted, marginally for mono. That is worth more
than the small margin suggests, because IC is the one model that needs no
language and no n-gram table at all: the seed ranking can be free and
language-independent even when the search's target model is not. The mechanism
is the same one that makes `-F`'s tier 1 an IC climb rather than an n-gram scan
— a partially-correct board yields a *partially* unscrambled decrypt, where
"the letter distribution is no longer flat" is answerable and "does this read as
German" is not yet.

The `eval/` scorers used for the table are anchored against the binary
(`--check-scorers`): all seven agree to ≤0.04, the size of the uint8
quantisation (the step is `(vmax − vmin)/255` for that table, so at most about
±0.016 per gram on the widest one), and IC — which is not quantised — to 4
decimals.

*The matched-compute A/B against `-R` — the seeder WINS, and it is the first
thing in this repo to do so.* `-R` has beaten every challenger it has been put
against (`--cascade`, `--polish`, `-F`, SA, the GA precondition), so this was
the measurement that decided the item. 300 paired trials (the 10 corpus messages
that end with a doubling × 30 fresh keys and 10-pair boards), recommended recipe
`-c -f -l wehrmacht -S i4f10 -J --polish`, true rotor key pinned, board hidden
(`eval/seeder_vs_restarts.py`, `eval/results-seeder-vs-restarts.txt`):

| arm | mean %-correct | exact | `score_iter` | cost vs B |
|---|---:|---:|---:|---:|
| baseline `-R 8` | 53.6 | 130/300 | 28 156 | 4.9× |
| baseline `-R 32` | 68.9 | 178/300 | 79 883 | 13.9× |
| baseline `-R 64` | 74.9 | 197/300 | 150 954 | 26.2× |
| baseline `-R 128` | 78.3 | 207/300 | 294 592 | 51.1× |
| baseline `-R 256` | 83.6 | 223/300 | 582 450 | 101× |
| **B** seeded, IC-top, `-R 8` | 74.2 | 204/300 | **5 767** | 1× |
| **B3** best-by-score, top 3 seeds | **85.3** | **238/300** | 24 997 | 4.3× |
| *O* oracle-correct seed | *97.3* | *275/300* | *6 152* | *1.1×* |

- **B3 beats the `-R 256` baseline on both metrics at 1/23 of its
  `score_iter`** — 85.3 against 83.6 mean, 238 against 223 exact — and it costs
  *less than the `-R 8` baseline*. Against that baseline it is +31.7pp (95% CI
  [+26.1, +37.3] per trial, [+15.7, +44.9] clustered on the messages) and +108
  exact recoveries, McNemar p < 0.0001. (B3 here runs each of its three seeds at
  `-R 8`; the hedge-curve section below drops that to `-R 0` and raises `k`,
  which is strictly better — read the recommendation from there.)
- **B alone matches `-R 64`'s mean at 26× less compute** and beats its exact
  rate (204 against 197). So even the single-seed form is worth ~26× of `-R`.
- **Equal `-R` is NOT equal compute here, which is why the baseline is swept.**
  Pinning k letters removes them from the 325-toggle scan and from the set the
  climb must converge, so a seeded restart is ~5× cheaper than a bare one.
  Matching on `-R` would have handed the seeded arm most of the budget.

**Where the remaining gap is, and it is not the seeding.** The oracle arm —
the same thing seeded with the *correct* seed whatever the ranking said — scores
97.3 / 275, beating the 101×-cost baseline by 13.7pp and 52 recoveries. The
whole distance between B and O is the ranking, and splitting on it is stark:

| | trials | baseline `-R 8` | B | B3 | O |
|---|---:|---:|---:|---:|---:|
| ranking right | 215 | 57.2 | **99.2** | 99.2 | 99.2 |
| ranking WRONG | 85 | 44.5 | **10.9** | 50.3 | 92.6 |

**A wrong seed is worse than no seed** — 10.9 against the baseline's 44.5 —
because `-s` pins and the climb cannot undo a pin. That is the single most
important practical fact here, and it is what B3 exists to hedge: the correct
seed is in the top three in **253/300** trials, so running three seeded climbs
and keeping the best *by score* recovers most of the loss (50.3) while still
costing less than `-R 8`. Ranking by the converged score is much sharper than
the IC pre-ranking because by then each seed's climb has actually run.

*The soft seed was built as the better fix, and it is NOT one — my prediction
here was wrong.* This entry used to argue that `-s` pinning was the problem and
that a **soft** seed — a starting board the climb may modify — "would likely
land near the oracle without needing three runs". `--soft-plug` now exists (see
`CLAUDE.md`), and measured over the same 300 trials it does not:

| arm | mean %-correct | exact | `score_iter` |
|---|---:|---:|---:|
| **B** hard seed, `-R 8` | 74.2 | **204/300** | 5 767 |
| **Bm** hard seed, `-R 24` | 74.2 | 204/300 | 13 041 |
| **S** soft seed, `-R 8` | 74.6 | 188/300 | 17 358 |
| **B3** hedged, top 3 hard | **85.3** | **238/300** | 24 997 |
| *O* hard oracle seed | *97.3* | *275/300* | *6 152* |
| *SO* soft oracle seed | *87.7* | *228/300* | *16 143* |

At matched compute the soft seed is **+0.4pp of mean and −16 exact recoveries**
against the hard seed, for 3× the `score_iter`. The half of the prediction that
held is the failure mode, and it held strongly — split on whether the ranking
picked the correct seed, soft is **+21.5pp** where hard collapses (32.4 against
10.9). What sank it is the other half, which the prediction did not consider at
all: **soft loses 7.9pp where the seed is RIGHT** (91.3 against 99.2), and the
seed is right 72% of the time. The oracle pair says the same thing without the
ranking in the way — a *correct* seed is worth 97.3 pinned and only 87.7 soft.

The mechanism is the thing to keep. **Pinning is not merely a commitment, it is
a search-space reduction**: `-s` takes 14 letters out of the 325-toggle scan, so
the climb solves a 12-letter residual instead of a 26-letter one. Unpinned, the
climb re-searches everything and, at 100–170 letters, the score signal is not
sharp enough to hold correct plugs in place — so it wanders off them. Two
corollaries fall out:

- **Hard seeding saturates at essentially ZERO restarts.** Arm Bm at `-R 24` is
  *identical* to arm B at `-R 8` — 74.2 / 204 on both — and sweeping downward
  shows the plateau reaches all the way to a single unkicked climb. The hard arm
  had run at the recipe's defaults throughout while only the soft arm was ever
  tuned, so this is the grid it should have had (300 trials, same set):

  | restarts × kick | mean %-correct | exact | `score_iter` |
  |---|---:|---:|---:|
  | **`-R 0`, no kick** | 73.6 | **201/300** | **2 734** |
  | `-R 1 --random 3` | 73.2 | 200/300 | 2 866 |
  | `-R 2 --random 10` | 74.0 | 203/300 | 3 317 |
  | `-R 4 --random 3` | 74.1 | **204/300** | 3 690 |
  | `-R 8 --random 3` | 74.1 | 204/300 | 5 157 |
  | `-R 8 --random 10` | 74.2 | 204/300 | 5 767 |

  **One unkicked climb gets 201 of the 204 exact recoveries at 2.1× less
  compute** (McNemar p = 0.25), and `-R 4 --random 3` matches 204 at 1.56× less.
  Kick size is worth nothing on this arm either — and structurally so, because
  the pins leave only ~8 free letters, so `--random 10` is clamped to ~4 pairs
  and is barely distinguishable from `--random 3` to begin with. Every
  difference in the grid is nested (no cell ever recovers a trial `-R 8
  --random 10` misses), which is what a plateau looks like from below.

  Extra restarts on a hard-seeded climb therefore buy almost nothing; that
  compute belongs on more hypotheses instead. **Following that through is what
  produced the recommendation below**, since a `-R 0` climb makes the hedge
  ~2× cheaper and testing more than three hypotheses affordable.

*The hedge curve, and the recommendation.* With each seed costing one unkicked
climb, run the top `k` and keep the best **by converged score** (a far sharper
judge than the IC pre-ranking, since by then each climb has run). Every `k` is
read off the same runs — 300 trials, the whole seed list climbed once:

| k | mean %-correct | exact | right / WRONG | `score_iter` |
|---:|---:|---:|---|---:|
| 1 | 73.6 | 201/300 | 98.4 / 11.1 | 2 734 |
| 2 | 81.3 | 225/300 | 98.4 / 38.1 | 7 797 |
| 3 | 84.4 | 234/300 | 98.4 / 49.1 | 13 161 |
| **5** | **88.4** | **245/300** | 97.7 / 64.9 | **23 983** |
| 10 | 91.3 | 254/300 | 97.7 / 75.2 | 50 587 |
| all (~27) | 93.8 | 264/300 | 97.7 / 83.9 | 169 371 |

Against the baselines (`-R 8` = 53.6 / 130 at 28 156; `-R 64` = 74.9 / 197 at
150 954; `-R 256` = 83.6 / 223 at 582 450):

- **`k = 5` is the recommendation: 245/300 for 23 983 `score_iter`** — *less*
  than a bare `-R 8`, which recovers 130. It also beats a bare `-R 256` by 22
  recoveries at **1/24 of its compute**.
- **`k = 10` (254/300 at 50 587) beats `-R 256` by 31 at 1/11.5**, and climbing
  *every* seed (264/300 at 169 371) beats it by 41 at 1/3.4. There is no `k` in
  the range where more hypotheses stop paying, which is the opposite of what
  `-R` does on this problem.
- **The gain is entirely in the failure mode `k` is aimed at.** The `right`
  column barely moves (98.4 → 97.7) while `WRONG` goes 11.1 → 83.9: raising `k`
  does nothing for messages the ranking already got right and almost everything
  for the ones it did not.

**The residual is the scoring floor, not the ranking.** A correct seed exists in
**300/300** trials and is in the top 5 in 268 and the top 10 in 281, yet
climbing *all* of them still recovers only 264 — so in ~36 trials the correct
seed's converged board does not score highest. That is the same information
limit `-a`/`-f` were measured against and no amount of seeding or searching
crosses it; the oracle arm's 275/300 is the same statement from the other side.
- **The right hedge tests hypotheses, it does not soften one.** B3 beats every
  soft variant because it spends its budget asking *which* seed is right rather
  than hedging against a single seed being wrong.

*Tuning the kick does not rescue it, and an earlier claim here was wrong.* The
obvious suspicion is that the soft arm is handicapped by the default
`--random 10`, which is sized for an empty board and would scatter a good seed.
A 60-trial exploration said exactly that — 72.7 → **79.0** at `--random 3` — and
that number went into `CLAUDE.md` and `CHANGELOG.md`. **It does not survive 300
trials.** The full sweep, same trial set as the table above:

| kick | mean %-correct | exact | right / WRONG | `score_iter` |
|---|---:|---:|---|---:|
| `--random 10` | 73.0 | 183/300 | 90.4 / 29.1 | 18 010 |
| `--random 5` | 74.3 | 186/300 | 91.5 / 30.7 | 17 821 |
| `--random 3` | 74.6 | 188/300 | 91.3 / 32.4 | 17 358 |
| `--random 2` | 74.1 | 187/300 | 91.0 / 31.5 | 16 934 |
| `--random 1` | 74.1 | 187/300 | 91.8 / 29.2 | 16 623 |
| **no kick (`-R 0`)** | 72.5 | 185/300 | 90.0 / 28.3 | **11 409** |

The whole range spans **1.6pp** end to end, and `r3` against `r10` is 188
against 183 exact — McNemar p = 0.40, nothing. So the kick was never the
soft arm's problem; the 6.3pp was small-sample noise. Two things the sweep does
establish:

- **No kick at all is the cheapest sensible setting**: `-R 0` (one climb
  straight from the seed — `--random 0` would make every restart identical,
  since the kick is the only per-restart randomness) costs ~2pp of mean for
  **35% less compute**, 11 409 `score_iter` against ~17 000.
- **The kick is not what separates soft from hard.** Even the best soft cell
  (74.6 / 188 at 17 358) loses to the hard seed's 74.2 / **204** at **5 767** —
  the same mean, 16 more exact recoveries, at a third of the compute. Nothing in
  the `--random` range closes a gap that comes from the search-space reduction
  pinning provides.

`--soft-plug` is kept: it is a small, tested, off-by-default option, it is the
natural way to express a guess, and its measured behaviour (graceful when
wrong, weaker when right, insensitive to the kick) is the useful
characterisation of a whole class of seeding ideas. It is simply not the
recommendation here.

*The unknown-rotor-key sweep — measured, and the seeder wins there too.* Every
table above hides only the plugboard, which is the tier this repo tunes on and
the seeder's *best* case: a sweep pays the deduction and `k` climbs at **every**
key, multiplying across the keyspace in the opposite direction from the win. The
smallest honest test is `-g A..` with wheels, reflector and ring fixed — 676
start positions, so the true key must outscore 675 competitors. 20 trials, same
10 messages, `eval/seeded_sweep.py`:

| arm | mean % | exact | `score_iter` | per key | true-key rank |
|---|---:|---:|---:|---:|---|
| baseline `-R 1` | 20.0 | 2/20 | 1 548 342 | 2 290 | — |
| baseline `-R 2` | 29.2 | 4/20 | 3 091 517 | 4 573 | — |
| baseline `-R 4` | 43.2 | 7/20 | 6 168 615 | 9 125 | — |
| baseline `-R 8` | 43.8 | 8/20 | 12 318 051 | 18 222 | — |
| **seeded k=1** | **71.8** | **13/20** | 3 316 286 | 4 906 | 1st in 14/20 |
| seeded k=3 | 81.3 | 15/20 | 10 423 853 | 15 420 | 1st in 16/20 |
| seeded k=5 | **85.6** | **16/20** | 17 655 464 | 26 118 | 1st in 17/20 |

- **At genuinely matched compute, `k = 1` more than triples exact recovery**:
  3 316 286 `score_iter` against `-R 2`'s 3 091 517 (0.93×, so the seeded arm is
  the *cheaper* one) for **13/20 against 4/20** and +42.6pp mean, McNemar
  p = 0.012.
- **It also beats `-R 8` at 3.7× less compute** — 13 against 8 recoveries for
  3.3M against 12.3M — and the baseline has already saturated by then (`-R 4` →
  `-R 8` buys one recovery for double the compute, while `k` 1 → 3 → 5 buys
  13 → 15 → 16).
- **The per-key cost multiplier is real but small: 2.14×** (4 906 against
  2 290 `score_iter`). That is the number the worry was about, and it is
  keyspace-independent, so the trade should not change shape as `K` grows.

**The discrimination worry was the right one to have, and it resolved the good
way.** A wrong key's deduction pins *wrong* plugs, which could either depress
its score (helping) or flatter it (fatal, and invisible at a pinned key). It
depresses: the true key's rank under seeding is **median 1**, first outright in
14/20, 16/20 and 17/20 as `k` rises. Seeding is acting as a weak filter as well
as a seed — which is what the self-crib *filter* failed to do on its own, and it
arrives here for free.

Two caveats on this table. `K` = 676 is small, and the bar a true key must clear
grows as `√(2 ln K)`, so discrimination at 10⁵–10⁸ keys is extrapolation rather
than measurement; the per-key *cost* ratio is not, since it is a property of one
key. And the CI is per-trial over 20 trials drawn from 10 distinct messages, so
it is optimistic in the same way the pinned-key tables' per-trial intervals are.

**What still bounds the result.** It applies only to messages ending in a
doubled word, which is
**10 of the corpus's 66 messages — 15%**. (An earlier version of this sentence
said "10 of the 18 corpus messages carrying a doubling", which reads as a
66-message corpus being 18 messages long. 18 is the count carrying a doubling of
6+ letters, i.e. the probe's working set, not the corpus. The ~15% was right by
arithmetic — 10/66 — but the denominator it was attached to was not.) The rates
in full, over all 66:

| minimum doubling length | carry one | **end with one** |
|---:|---:|---:|
| 4 | 20 (30%) | **10 (15%)** |
| 5 | 18 (27%) | 9 (14%) |
| 6 | 18 (27%) | 8 (12%) |
| 7 | 16 (24%) | 7 (11%) |
| 8 | 13 (20%) | 4 (6%) |

The trial set is the `minlen = 4` row, and nothing is lost to the probe's
`≥6` pre-filter: measured directly on all 66, the count ending with a doubling
of 4+ letters is 10, the same 10 the filtered set yields. Wall time is
reported in the results file but says nothing at this size: a run is ~0.1 s of
which ~0.05 s is the one-off n-gram load, and B3 pays that three times because
it is three processes.

The algebra, for the record. It sidesteps the failure above entirely, because it
works on the **ciphertext** and needs no correct decrypt at all.

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
`eval/doubling_null_probe.py` (the null, three ways),
`eval/doubling_anchored_probe.py` (note (a), measured down),
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

**12. IC-rank the surviving hypotheses, as `--self-crib-seeds` does — BUILT,
as `--crib-seeds K`.** `crib_unit()` used to run a **full plugboard climb on
every surviving (alignment, hypothesis) pair**, which is what the self-crib path
did before ranking. It now dedupes, ranks by the index of coincidence of the
decrypt under the deduced partial board, and climbs the top `K`; `0` is off and
leaves the old path byte-identical. It is length-agnostic: any crib the tool
already accepts (2 letters and up) can be seeded, and nothing filters by
length — the table below is what to expect at each, not a restriction.

**On the sweep — the number that decides it** (`eval/crib_seeds_ab.py`, 20
trials, 10-letter crib, board hidden, 676-key sweep): `K=10` recovers **19/20
against the unseeded 19/20 with zero discordant trials, for 12.1× fewer
plugboards**. `K=3` gives up 3 breaks for 43.6×, `K=1` gives up 4 for 138×. So
**`K=10`** — the same operating point `--self-crib-seeds` reached, arrived at
independently.

The ranking measurement that predicted it, at the true key, 40 trials per
length, 10-pair board, alignment swept (`eval/crib_ic_rank.py`):

| crib | surviving hyps | rank 1 | top-10 | median rank |
|---:|---:|---:|---:|---:|
| 8 | 438.6 | 15/40 | **23/40** | 6 |
| 10 | 90.7 | 25/40 | **37/40** | 1 |
| 12 | 8.3 | 40/40 | 40/40 | 1 |
| 14 | 1.5 | 40/40 | 40/40 | 1 |

**The window is ~10 letters, and it is bounded on both sides.** At 12+ the
deduction already rejects nearly everything and there is no population left to
rank — 8 hypotheses, then 1.5. At 8 the population explodes to ~440 and IC
degrades with it: the top 10 keeps only 57% of correct hypotheses, so a 44×
cut costs 42% of them. Only at **10 letters** are both true at once — 91
hypotheses, top-10 keeps 92.5%, a **9× cut for ~7% loss**.

That is worth having, because it is the regime the swept crib is currently
locked out of ("16 letters is the swept floor"), and short cribs are the ones
most likely to be *present* (93% of messages carry an 8-letter crib, 3% a
20-letter one).

Both cautions raised before building it were addressed. The true-key figures
above could not see the sweep, where wrong keys are also ranked and cut — hence
the end-to-end A/B, which is what `K=10` rests on. And the dedupe is keyed on
the (board, pinned-letter-set) pair as the self-crib path is, since two
hypotheses can agree on cables while one additionally proves a letter carries
none.

**Building it also turned up a crash that predates it.** `--crib` with `-s`
could deduce `A–D` while `-s` said `A–B`, overwrite `steck[A]`, and leave a
board that was not an involution — which then smashed the stack in
`format_plugboard`, sized for the 13 pairs an involution can have. The
deduction now starts from what `-s` and `--no-plug` already fix, so a
contradicting hypothesis is rejected rather than silently applied.

> A caution on the measurement itself: a 12-trial pilot at a different seed
> read L=8 as 12/12 in the top 10 with median rank 1, and the 40-trial run
> above reads 23/40 and median 6. The pilot was not wrong about 10 and 12; it
> was wrong about the length where the population is largest and the ranking
> hardest, which is the one the decision turns on. A hypothesis deducing no
> cable at all is not scored correct — it is consistent with any board and
> would inflate every figure here.

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
