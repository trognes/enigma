# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/): the major number changes when
existing command lines can behave differently or stop working.

## Unreleased

### Changed

- **`make bench` gains an `icscan` tier — the default model had no coverage.**
  `-i` is what a bare `./enigma < cipher.txt` runs, and nothing in the harness
  touched `ic_score_decode`: `search` and `crib` run `-q`, `fused` runs `-f`,
  and under `-c` the low-order models are served by the histogram/`cooc_col`
  path rather than by decoding at all. The scorer loops were rewritten twice
  in this release with the tool's default invocation unmeasured.
  - **Paired with `search`** — identical ciphertext and key space, differing
    only in the model, the same relationship `fused` has with `hillclimb` — so
    a delta between the two rows is the scorer and nothing else. 2 741 856
    keys quick, 27 418 560 long, matching `search` exactly.
  - **The cleanest tier in the harness**, because IC needs no language and so
    loads no n-gram table: ~6 ms of startup against a ~0.8 s scan (99.2%),
    where `search` carries ~32 ms (96.3%). `min_time` wraps the whole
    invocation, so that margin is the tier's immunity to startup effects
    being misread as throughput.
  - Costs ~3 s on the quick suite and ~20 s on the long one (doubled under
    `BASE=`), which is the price of covering the default model.

- **`make bench` warms the n-gram page cache and both binaries before the
  first tier.** An A/B reads **two separate ~27 MB copies** of the tables
  (`HEAD_DATA` and `BASE_DATA`), and `min_time` wraps the whole invocation, so
  a cold read lands inside a measured run rather than beside it. `icscan`
  reads no table at all, so an uncontrolled cache hits `search` and cannot
  touch `icscan` — which would make the two scan tiers differ by more than the
  model they exist to isolate.
  - Measured on repeated base-vs-base controls (identical binaries): `search`
    quick on g++ x86_64 read **−7.5% then −6.7%** without the warm-up and
    **+1.4%** with it, and the whole matrix tightened — `search`'s spread goes
    from −7.5…+9.1% across two runs to **−0.2…+2.1%**, with 36 of 40 cells
    within ±1%. One run with the treatment, so the size is provisional.
  - ~0.3 s, once, before any timing.

- **The pure-gather scorer loops are unrolled 4x — 12–16% faster under g++
  and 4–6% under clang on arm64.** `quadgram`, `allgram`, `trigram`,
  `bigram` and `monogram` carry a `SCORE_UNROLL` pragma —
  `#pragma GCC unroll 4` / `#pragma clang loop unroll_count(4)`. Measured
  against a same-hour base-vs-base control: g++ `search` −8.2 / −8.9%,
  `hillclimb` −10.1 / −8.9%.
  On the arm64 CI cells — the only ones that reproduce (see below) — `search`
  long is −12.5% (g++) / −3.7% (clang) and `hillclimb` long −15.6% / −6.4%,
  each with a spread under 1.5 points across three runs. Byte-identical — the
  accumulator is an integer sum, so splitting it changes nothing.
  - **The three scorers that also do `freq[d]++` are deliberately NOT
    unrolled** — `ic`, `monoic` and `ngram_ic` (`-f`). Unrolling those
    *regresses*: g++ `fused` long read **+11.7%** with all eight loops
    unrolled, against +2.7% with the store loops left rolled. Four `freq[]`
    increments scheduled together collide about 30% of the time across 26
    bins, turning store-to-load forwarding into a stall. Unrolling helps the
    load dependency and hurts the store one.
  - So `-q`, `-a`, `-t`, `-b`, `-m` and every scan path gain from the pragma.
  - Two earlier attempts on the same loop measured down and are recorded in
    `eval/results-scoreloop-insns.txt`: shaving 21% of the instructions bought
    0% (the loop is not front-end bound), and removing one of six loads gained
    7% under g++ but cost clang 3–5% on a model the recipe does not recommend.

- **`-f` is unrolled too, by giving each unrolled copy its own histogram —
  worth nothing to 19% depending on the machine.** `ngram_ic_decode`
  accumulates into
  four private counter arrays (`f0`…`f3`) that are summed over the 26 bins at
  the end, so the four `freq[]` increments in one iteration touch four
  different arrays and the store-forwarding collision above is removed by
  construction rather than avoided by leaving the loop rolled. The loop stays
  single-pass and the merge is O(alphabet), not O(message). Byte-identical:
  the counts are the same integers whichever array they land in, and the
  n-gram terms are added in the same order into the same `long`.
  - **The size depends on the target.** The Bench matrix ran three times on
    byte-identical code, so its run-to-run spread is known — and the arm64
    runners are deterministic (the same base binary times to within 0.06 s
    across all three) while the x86_64 runners vary by 28–76% in absolute
    throughput and cannot resolve a change this size. Taking the arm64 cells
    as the instrument, `fused` long is **−18.7% (g++ arm64, ±0.3)** and
    **−13.7% (clang arm64, ±0.9)**; on x86_64 it reads −8.5% under clang
    (3/3 negative) and **−0.7% under g++, i.e. nothing**.
  - The hot loop goes 25.0 → 18.2 instructions per character under g++ and
    24.0 → 18.5 under clang, loads unchanged at ~6.
  - The preceding entry's conclusion that the `freq[d]++` loops "cannot be
    unrolled profitably" held for the loop as written and not for the work it
    does: the serialised store was an artefact of four copies sharing one
    accumulator. `ic` and `monoic` keep the rolled form: the same treatment
    would apply to them, but during a climb they are served by the
    histogram/`cooc_col` path rather than by decoding, so their loop runs
    once per climb start and resync rather than per toggle and there is
    little there to win.

### Added

- **`--biased-random T` — draw the restart kick from the single-plug index of
  coincidence instead of uniformly.** Each of the 325 possible single plugs is
  scored on its own IC, z-scored per key, and the kick's pairs are drawn with
  probability `exp(z / T)` rather than uniformly. Off by default; needs `-c`,
  `-R ≥ 1` and `--random ≥ 1`, and is refused with `-A`, `--crib`/`--crib-list`,
  `--exhaust` and `--self-crib-seeds`, which install their own starting board.
  - **There is signal in a single plug**, which is what the option rests on:
    a true plug lands in the top 10 of 325 12.1% of the time against 3.1% by
    chance, and its mean rank is 110 of 325 against 163. That is not in
    tension with the *joint* four-plug IC optimum being a decoy that outscores
    the truth — sampling exploits the marginal signal without committing to
    the joint one, which is why this is a weighted kick and not a beam.
  - **Measured +9.3% of breaks pooled over `-R` 1–5** (699 → 761, z = +2.56)
    and +3.4% over `-R` 6–10 (z = +1.46, not established), on authentic
    telegraphic German at 100 letters with the rotor key given, 600 paired
    trials per cell (`eval/results-biased-kick.txt`). All ten cells positive,
    none resolving alone. The bias substitutes for the diversity that restarts
    supply, so it fades as restarts grow.
  - **The scores cost nothing.** They come from a per-key co-occurrence table
    rather than from decoding — ~15 µs at 100 letters against ~44 µs of
    ordinary decoding, and **flat in message length** where decoding is linear.
    `make bench` is unchanged: the branch is taken once per work item and never
    inside the score loop.
  - **`T ≈ 1` is the safe setting** and both extremes are worse. At `-R 1`,
    note that `-R 0` (one unperturbed climb, no kick) beats *both* arms — the
    bias recovers about half of what a single kick gives away.

### Changed

- **The low-order climb stages (`-S i`/`m`/`k`) now score from a histogram
  instead of by decoding — byte-identical output, and a staged climb runs
  1.2× faster at 100 letters and ~1.28× at 167.** IC, monogram and the
  mono+IC blend are all functions of one 26-bin histogram of the decrypt taken
  *before* the exit plugboard, and that histogram is a sum of columns of a
  per-key co-occurrence table — so a plugboard toggle costs O(26) where
  decoding costs O(*L*). Nothing about a run's *result* changes; the pre-pass
  simply stops being the larger half of the climb.
  - **It is an identity, not an approximation.** The plugboard is an
    involution, hence a bijection, so the histogram the decoders build is the
    same one permuted: the index of coincidence is blind to the exit board
    entirely and the monogram score only relabels its coefficients. Both
    accumulate in integers before their single float division, so summing over
    26 bins instead of over *L* positions yields the same integers and the
    identical result — the same bits, not a close approximation.
  - **The pre-pass is now flat in message length.** Per restart it costs
    214 → 124 µs at *L* = 60 and **530 → 119 µs at *L* = 400**, where it
    previously scaled with the message. The high-order target stage is
    untouched — bigrams and above are additive over positions and have no
    histogram form — which is why the whole-climb figure is 1.2–1.3× rather
    than the several-fold speedup the pre-pass alone shows.
  - Measured per schedule, `ENIGMA_HIST=1` against `0` on one binary, three
    seeds per cell against a noise-floor control: at *L* = 100, `m4f10` 1.11×,
    `i4f10` 1.20×, `k4f10` 1.25×; at *L* = 167, 1.27× / 1.30× / 1.27×. The win
    grows with length because the histogram form is flat where decoding is
    linear.
  - **The payoff is restarts at fixed wall time, not quality per restart** —
    the output is identical, so it buys exactly what `-R` buys.
  - **`ENIGMA_HIST=0`** sends the same climb back through the decoders. A
    measurement-and-test switch rather than an option: it is what lets the test
    suite check the identity on every run rather than only against a
    hand-built reference binary.
  - `--biased-random` already built the same table to score its 325 single
    plugs and now shares it, rather than keeping a second copy.

- **A numeric option given a non-number is now rejected instead of read as
  0.** `atoi`/`atof` cannot
  report failure — they return 0 for a string that is not a number at all —
  and 0 is the *off* value for most of these options, so a typo did not fail:
  it silently disabled what was asked for.
  - `-R 64O` (letter O for zero) ran with **no restarts**, and left no trace:
    at `-R 0` the settings echo omits the restart line entirely. `--confidence
    nope` printed nothing about confidence at all. `-A qqq` fell back to the
    greedy climb, `--crib-weight zzz` disabled the crib bonus, and `-e
    notanumber` selected seed **0** — which is not merely "some seed" but the
    historical deterministic stream the tests and harnesses pin.
  - Only `-T`, `-x` and `--ring-stride` caught this before, and only by
    accident: their valid ranges exclude 0, so the *bounds* check rejected
    what the *parse* had let through. `--doubling-z` was the single place
    that checked deliberately; `parse_opt_int` / `parse_opt_double` /
    `parse_opt_u64` in `common.cc` generalise it to all 22 numeric inputs.
  - Trailing text is rejected too, so `-R 12x` fails rather than reading as
    12 — the realistic typo, and the one that leaves a plausible number
    behind. Range errors fail at the parse rather than arriving at the bounds
    check as an implementation-defined value.
  - **Values that were always legal still are**, including the two where 0 is
    a real setting: `-R 0` (one deterministic climb) and `-e 0`. `-F N%` keeps
    its one legal trailing character, stripped before parsing rather than
    tolerated by the parser.
  - The same applies to the measurement-only environment overrides, where the
    consequence was worse than a failed run: `ENIGMA_IC_BLEND=typo` silently
    set the blend to 0, turning `-f` into `-a`, so a probe would have quietly
    measured the baseline instead of the variant it was set up to test.
    `ENIGMA_LOGLIN` had the same hole through an `sscanf` whose return value
    was dropped — a partial vector left the rest at 0, and an all-zero vector
    is silently replaced by `(1,0,0,0)`, i.e. plain quad. For all of these an
    **empty** value now means *unset*, matching what `$ENIGMA_SEED` already
    did, so `FOO=` still turns a probe off rather than aborting the run.
  - 18 rejection checks added to `tests/run_tests.sh`, each asserting the exit
    status *and* that the message names the option, plus checks that the legal
    values still pass. Verified by reverting two call sites to `atoi` and
    watching four of them fail.
  - Search output is unaffected: all 49 reference cases byte-identical, 570
    tests pass, `make bench` flat. Clears 23 of the 26 `clang-tidy` findings
    (`bugprone-unchecked-string-to-number-conversion`); the 2 remaining are
    pre-existing and only reported by newer clang-tidy than CI runs.

- **Bounded the self-crib's headline claim, and documented when it backfires.**
  `--self-crib-seeds` was described as "the one lever measured to beat `-R` at
  matched compute". Measured across doubling length with the message length
  held fixed (`eval/selfcrib_vs_restarts.py`, 40 paired trials per cell,
  676-key sweeps), that holds **at ~100 letters with a 7+ doubling — where it
  ties or beats `-R 128` at ~9× less wall time — and not at 167**, where
  `-R 128` already reaches 36–40 of 40 and there is no headroom left to
  convert.
  - **A doubling shorter than `--self-crib-length` is a near-total loss, not a
    wash**: 1/40 and 0/40 at doubling 4 and 5 against `-R 128`'s 19 and 16
    (p = 0.000). The seeder hypothesises doublings of the configured length
    *anywhere* whether or not one exists, pins the deduced (wrong) plugs, and
    those pins survive the climb — so it does not degrade to an ordinary climb.
  - Since only ~27% of authentic messages carry a 6+ doubling and nothing tells
    you which yours is, `README.md` now recommends **running the seeder first
    and falling back to `-R`**: ~11% over the restart run alone, taking the
    union, which beats either arm at every doubling length.
  - No code change — documentation and measurement only.

- **`README.md` now says how to choose the keyspace for a real attack**, which
  matters more than `-R` because the part left out cannot be recovered by any
  amount of climbing. Pinning `ring0` (`-r A..`) is **lossless** — a wildcard
  `start0` already enumerates all 26 offsets — but pinning `ring1` (`-r AA.`)
  excludes about **28% of distinguishable keys** and is a hard ceiling no `-R`
  lifts. Measured on a 167-letter message with reflector B and wheels I–V,
  `-r A..` + `--ring-stride 3` is 79.6 M keys against `-r AA.`'s 9.5 M; the two
  are close at a ~24 h budget and `-r A..` wins beyond it. Also records the two
  restrictions the 1941 HG Nord traffic justifies rather than assumes: all 75
  recovered keys use reflector B, and all 25 wheel orders use wheels I–V.

### Added

- **`--seed-dedup` — skip the target climb when this restart's stage-0 seed was
  already climbed for this key.** Under a staged `--score` the board after
  stage 0 is a deterministic function of `(key, restart)`, so two restarts
  reaching the same seed produce a byte-identical result and the second climb
  is pure waste — 17% of seeds at `-R 100` and **73% at `-R 10 000`**.
  - **Per-key Bloom filter, 8-byte blocks**: a lookup is one `uint64` load, one
    AND and one compare, and 8 divides 64 so an aligned word never straddles a
    cache line. The per-key region rounds up to 8 bytes rather than 64, which
    is what lets memory track `-R` continuously — the payoff then *grows* with
    the restart budget (+8.0% distinct seeds at matched wall time at `-R 64`,
    +10.6% at 100, +32.5% at 1000) instead of peaking and reversing.
  - **`--seed-dedup-bits N`** sets bits per item `[8]`; **`--seed-dedup-max
    BYTES`** caps the memory and **refuses**, naming what would fit, rather
    than thinning the filter into the range where it costs more coverage than
    it saves. `k` is chosen numerically from the *blocked* false-positive rate,
    which is not the textbook `0.693 × bits` — small blocks scatter, so the
    optimum sits lower.
  - **The run reports the skips** (`Skipped N full climbs on duplicate seeds of
    M (P%)`), because nothing else in the output identifies the feature. The
    unit is a *seed*, not a key, and it reports *climbs*, not compute: the
    cheap stage ran on every seed.
  - **No lock and no atomic on the filter.** The sweep now runs one restart
    pass at a time and a key appears exactly once per pass, so the
    `run_parallel` join at each pass end is the barrier; `-T` independence is
    preserved and the skip count is part of that contract.
  - Off by default and byte-identical when absent; the long-tier benchmark is
    flat on all four tiers. Needs `-c` and a staged schedule; rejected with
    `-F`, `--exhaust`, `--crib`, `--self-crib-seeds`, `--tune-phase` and `-A`.
    `--ring-stride` **is** supported: its coarse pass is filtered and its
    refinement runs unfiltered.
  - **Measured faster at a fixed restart count**: −8.1% of wall time at
    `-R 1000` and −21.0% at `-R 10 000` on 26 keys, with the recovered
    plaintext unchanged and an off-vs-off control under 1%
    (`eval/results-seed-dedup.txt`). The saving is about half the skip rate,
    since the cheap stage runs on every seed and only the target continuation
    is skipped. **Not** shown to convert into breaks — those runs hold `-R`
    fixed, and the end-to-end comparison was retired before it ran.

- **`--self-crib-tandem` — hypothesise a doubled word with no separator**
  (`SIEGFRIEDSIEGFRIED`), which `--self-crib-seeds` could not see at all: its 26
  guesses are on `steck[X]` and the separator anchor is what carries that guess
  into the message.
  - **Opt-in on cost, not on whether it works.** Recall barely moves (a correct
    hypothesis exists in 195 of 200 trials against the separated case's 197),
    but gap 0 has as many alignments as gap 1, so enumerating both roughly
    **doubles the hypothesis count** (+101% over the corpus) — which would take
    the seeder past the `-R 16` baseline it is measured against. It buys **3 of
    66 corpus messages, +4.5pp** — four carry a tandem doubling, but one also
    carries a separated `ZANDERS`, so the default already seeds it.
  - **Measured end to end** (60 paired 676-key sweeps per pool, board hidden,
    `--self-crib-seeds 10`; `eval/selfcrib_tandem_ab.py`). On the payoff
    population — messages with a tandem doubling and no separated one — exact
    recovery goes **3/60 → 22/60**, 19 only-on against 0 only-off, McNemar
    **p = 3.8e-6**. On the risk population, where every tandem hypothesis is
    wrong by construction and competes for the same `K` seed slots, 38/60 →
    36/60 shows **no measurable loss** (0 only-on, 2 only-off, p = 0.5).
    Corpus-weighted that is ~+0.6pp for **2.6× the wall time**, which is the
    arithmetic that keeps it opt-in.
  - **Plugboards scored go *down*** (2.42 M → 2.31 M) while wall time rises
    2.6× — the whole added cost is the uncounted deduction, so `score_iter` is
    the wrong axis for this flag and reports the opposite sign.
  - **A tandem repeat is not anchorless**: it has no separator but nearly always
    has an X *before* it (4 of 4 in the corpus), so the left flank is asserted
    instead. That recovers most of the sharpness — top-5 168 → 182 of 200 —
    which is why the variant is usable at all.
  - Demonstrated on the corpus message carrying `SIEGFRIEDSIEGFRIED`: with the
    board hidden and the rotor key given, the default reaches 82.7% of letters
    and misses, `--self-crib-tandem` recovers it exactly, and *faster* (0.20 s
    against 0.77 s) because the correct seed converges at once.
  - Refused with `--self-crib-signature`, which asserts the copies *are*
    separated by an X closing the message — a contradiction, not a narrowing.

- **`--crib-seeds K` — IC-rank an ordinary crib's hypotheses and climb only the
  best K**, exactly as `--self-crib-seeds` does. `crib_unit()` ran a full
  plugboard climb on *every* surviving (alignment, hypothesis) pair, and a swept
  short crib leaves a great many: measured at the true key, **438.6 survivors at
  an 8-letter crib, 90.7 at 10, 8.3 at 12, 1.5 at 14**.
  - A correct hypothesis pins several correct plugs, which lifts the index of
    coincidence of its decrypt before any climbing — so the ranking needs no
    language and no n-gram table.
  - **The window is narrow and bounded on both sides.** At 12+ ranking is
    perfect and pointless (nothing left to cut); at 8 the top 10 keeps only 57%
    of correct hypotheses; only at ~10 letters are both true at once — 91
    survivors, top-10 keeps 92.5% (`eval/crib_ic_rank.py`).
  - **On the sweep, `K=10` costs nothing**: 20 trials, 10-letter crib, board
    hidden, 676-key sweep — 19/20 exact against the unseeded run's 19/20 with
    **zero discordant trials**, for **12.1× fewer plugboards**. `K=3` gives up 3
    breaks for 43.6×, `K=1` gives up 4 for 138×. Use `K=10`, the same operating
    point `--self-crib-seeds` reached (`eval/crib_seeds_ab.py`).
  - `0` = off and leaves the historical path byte-identical, including the count
    of plugboards scored. `-T`-deterministic.

- **Pre-flight: is this ciphertext even Enigma?** (on by default;
  `--no-preflight` turns it off). Enigma is a permutation
  cipher, so its output is near-flat; a ciphertext carrying residual language
  structure was not produced by one and has **no key to find**. The index of
  coincidence and the number of unused letters are now computed from the
  ciphertext and compared against a length-dependent null, and the verdict is
  printed before the sweep. It reports **for a search — a wildcarded key —
  and only then**: with a full key the tool is encrypting or decrypting, and
  on encryption the input is plaintext, which is language-like by definition.
  - Measured the expensive way: a 28-hour, 75.2M-key sweep of the QTXMA
    challenge message returned nothing, and the reason was visible up front —
    IC 0.0577 against the 0.0385 ± 0.0018 of 3000 simulated Enigma
    encryptions at that length (z = +10.9), and 4 letters unused where 0.06
    are expected
    (P = 8.5e-08). See `eval/MODERN_BREAKING_NOTES.md` §5l.
  - **The null is length-dependent**, and it has to be: IC variance goes as
    `1/C(n,2)`, so two of the four *broken* (genuinely Enigma) 1941 messages
    sit at z = +4.2, at 47 and 74 letters. A fixed threshold would condemn
    them. No tables are needed — both statistics have closed forms under a
    uniform multinomial that match simulated Enigma within 1–2% from n = 40
    to 600.
  - Warns at z(IC) > 6.0 or P(unused) < 1e-4, set from the **measured** tail of
    genuine Enigma rather than a nominal p-value: across one sample of 18 000
    simulated ciphertexts (authentic 1941 German, random keys and boards — not
    real traffic) the largest z seen was 5.89 and neither test fired once.
    `eval/preflight_null.py` reproduces the calibration.
  - No pre-flight line may look like a progress line: the `--confidence`
    margin extractor greps stderr for `^ *[+-][0-9]`, and a continuation line
    reading `  +10.95 sd; …` was read as the run's last margin. The
    statistics line now opens with `(` and no line begins with a digit.

- **`--self-crib-seeds K` / `--self-crib-length L` / `--self-crib-signature` —
  self-crib seeding, which beats `-R` at matched compute at ~100 letters with a
  7+ doubling** (and not at 167, nor below that doubling length — see the
  Changed entry above).
  A doubled word is a *self*-crib: it says only that two positions carry the
  same plaintext letter, which cancels out of `p = steck[core[steck[c]]]` and
  leaves `steck[c_j] = core_j[core_i[steck[c_i]]]` — computable from the rotor
  key alone, with no known plaintext anywhere in it.

  As a filter that is worthless (0 of 160 wrong keys rejected). As a seeder it
  is decisive. Per key the tool deduces every board the rule allows under all 26
  guesses for `steck[X]`, ranks the survivors by the **index of coincidence** of
  their decrypt, and climbs the top `K` with those plugs pinned. IC is the
  ranking because it measured as good as the fused model (150/200 against
  144/200 top-1) and needs no language or n-gram table.

  **The default hypothesises the doubled word anywhere in the message**, and
  `--self-crib-signature` narrows it to one closing the message — a signed
  surname. That is the same shape as `--crib` (sweeps every alignment) and
  `--crib-at` (pins one): the default assumes nothing, the flag adds knowledge.
  Restricting is ~15× cheaper — 20 hypotheses against ~2 200 — but only wins
  when the assumption holds. Over every corpus message carrying a doubling
  anywhere, restricting to the signature breaks **16/40** against the default's
  **26/40**, with a bare `-R 16` at 19/40.

  Measured on 676-key sweeps with a 10-pair board hidden. Against the baseline,
  `K = 10`:

  | arm | mean % | exact | per key |
  |---|---:|---:|---:|
  | `-R 1` | 17.9 | 4/32 | 334 µs |
  | `-R 16` | 48.9 | 13/32 | 3 345 µs |
  | **`--self-crib-seeds 10`** | **81.1** | **23/32** | 1 650 µs |

  — ten more messages than `-R 16` in half the wall time. `K` = 5/10/20/35/50
  breaks 21/23/23/23/24 of 32: steep to 10, flat through 35, one more at 50 for
  +120% time. **Use `K = 10`.** That plateau is the tell — raising `K` lifts the
  best-of-`K` score of the *wrong* keys too, so extra recall converts into
  discrimination only slowly.

  **`--self-crib-length` defaults to 6**, and the anywhere default dominates the
  signature restriction at every length. Fixed population — all 20 corpus
  messages carrying a 4+ doubling anywhere, 40 trials, `K = 10`, 676-key sweeps
  — exact recoveries of 40: signature 16/14/12/11/6/4 against anywhere
  28/27/26/24/22/19 as `L` runs 4→9, at 570→202 µs per key against 7 595→1 065.
  A bare `-R 16` gets 19/40 at 2 901 µs, and `-R 0`/`-R 1` get 5/40. Every
  anywhere cell beats `-R 16` except `L=9`, which ties it at 2.7× less time; the
  signature restriction is worse than `-R 16` at every length, which is why it
  is the flag and not the default. `L=6` is the best value (26/40 still under
  `-R 16`'s per-key cost); `L=8` is the cheapest cell that beats it at all
  (22/40 for 2.4× less).

  `score_iter` is the wrong axis for these flags and says the opposite of the
  truth: a swept `K=1` run scores *fewer* plugboards than a signature-
  restricted one while taking 15× the wall time, because its seeds are more
  constrained so the climbs are cheap and the uncounted deduction dominates.
  Judge on wall time.

  Rejected with `--crib`/`--crib-list`, `--exhaust`, `-A`, `--soft-plug`, `-F`
  and `--tune-phase`. `-T`-deterministic.

- **`--soft-plug AB…` — plugboard pairs that are a GUESS, not knowledge.** Same
  shape as `-s` and the opposite contract: the pairs are laid on the board each
  restart starts from and then left free, so the climb may move, merge or remove
  them. `-s` marks its letters in `plug_fixed[]` and forbids every move that
  would touch them; `--soft-plug` marks nothing.

  The reason to have both is that their failure modes are not comparable. A
  wrong `-s` pin cannot be undone by anything downstream — the pins deliberately
  survive `--polish` — so one bad guess poisons the whole run, whereas a wrong
  soft guess costs only the moves the climb spends walking back out of it. That
  is exactly the trade a *deduced* seed needs, one right most of the time but
  not always.

  The `--random` kick needed no change: it draws only from self-steckered
  letters, so a soft-seeded pair is invisible to it and the kick adds pairs
  among the letters the seed left alone. Kick size turns out to matter little —
  across `--random` 10/5/3/2/1 the mean %-correct spans 73.0–74.6 over 300
  trials, and no kick at all (`-R 0`) costs ~2pp for 35% less compute.

  Fatal on an odd number of letters, a repeated letter, a non-letter, no `-c`, a
  letter that `-s` also pins or `--no-plug` also marks, and on
  `--exhaust`/`--crib`/`--crib-list`/`-A`, each of which installs its own
  starting board. `-T`-deterministic.

- **A live progress line for the main sweep** — percentage, key rate and ETA,
  rewriting itself in place:

  ```
  Progress:   50% (5.94M / 11.88M keys) 10.12M/s, 1s left
  ```

  The score lines report how *well* the search is doing and nothing about how
  far it has come — and because each one needs a new best, they thin out to
  nothing exactly when a run is longest, leaving no way to tell a slow sweep
  from a stuck one.

  It updates **about every 5 seconds**. An earlier version redrew on each 1%
  boundary, which is the wrong clock — 1% of the work takes longer the bigger
  the sweep, so the line updated most rarely on exactly the runs that need it:
  measured, one update every 5.8 s over 1.05M keys but 2.5 minutes over 27.4M
  and 21 minutes over 230M. The per-thread accounting block is now regime-aware
  too (64 items under `-c`, 4096 in a scan), since a climbed item costs four
  orders of magnitude more than a scanned one and a fixed block had a thread
  reporting only once every nine seconds.

  No flag: it appears whenever stderr is a terminal, exactly like the `-F`
  pre-filter's existing line, and vanishes when the sweep ends. Redirect stderr
  and it is not emitted at all — not the text and not the carriage return — so
  logs, pipelines and the test suite see byte-for-byte what they saw before. It
  is also suppressed under `--dump-all`, whose rows are the machine-readable
  form and print under a different mutex. A sweep finishing in under half a
  second draws nothing rather than flashing a line up and wiping it.

  Score lines and the progress line share stderr, and the line steps aside for
  them rather than being written over: every score line in the program is
  printed by one function, and every caller of it already holds the best-result
  mutex, so that is where the erase goes.

  Counts are shown in **keys** while the percentage runs over **work items**
  (`keys × restarts`, what the sweep actually distributes) — the two differ by a
  constant factor, so the percentage is the same either way, but reporting items
  would read `8×` high against the `Analysed N rotor combinations` line under
  `-R 8`.

- **`--confidence N`** — answers "is this score better than chance?", which the
  tool could not do before. It samples `N` keys from the resolved key space,
  scores each exactly as the search scored them, and reports the winner's
  distance above that null, the distance the **best of K keys** is expected to
  reach by chance (`μ + σ·√(2 ln K)`), and the margin between them.

  **The margin replaces the `Score` column in the progress lines**, with the
  header renamed to match, so the answer is where you are already looking and a
  saved log still explains itself. Zero is the line that matters: negative means
  the board is no better than luck over the whole sweep. `--dump-all` keeps raw
  scores as the machine-readable form.

  The margin is the number to read. A raw score means nothing on its own: every
  model scores *something* on gibberish, and because a search reports a maximum,
  the bar rises with the keyspace. A bare z-score would not do either — a
  progress line is a running maximum, so z reads 3–5 σ well before anything is
  found. The margin subtracts the chance best of the **whole** key space, which
  also keeps it a constant offset from the score: monotone, so the search order
  is untouched, and independent of thread timing.

  Measured: real English over 17 576 keys gives
  a margin of +17.0σ; the identical sweep on signal-free ciphertext gives +0.5σ;
  and a hidden plugboard searched without `-c` — which cannot recover it — gives
  −0.8σ, correctly reporting a failure.

  Samples are hill-climbed when `-c` is on, because a climbed key is drawn from
  a much higher distribution than a scanned one and calibrating against the
  wrong one would make every run look significant. The Gumbel yardstick was
  checked against 12 signal-free sweeps and matched to within 0.01 for quad and
  fused; the index of coincidence does not follow it (its null is right-skewed),
  and the printed p-value says so under `-i`.

  A second use falls out: the margin ranks the scoring **language** on one
  message. On telegraphic German it measured +15.4σ for `wehrmacht`, +8.6σ for
  `german` and +2.5σ for `english`.

  **Use `N` = 256, and never below 128.** `N` buys precision in the sampled null
  and nothing else, and too small an `N` makes the flag report the very thing it
  exists to rule out: measured over 12 seeds per cell, a signal-free ciphertext
  reports a *positive* margin at `N` ≤ 64 (+1.7σ at 16, +1.2σ at 64), while 128
  never crosses zero and 256 has real headroom. Past 512 the error left is the
  null's departure from Gaussian, not the sample size. The spread follows
  `SE ≈ √((1 + z²/2)/N)`, from which `N` needs no adjustment for keyspace size
  or message length. Calibration is free without `-c` and costs 1.5–1.7 ms per
  sample with it — ~1% of a real run, but single-threaded, so its share grows
  with `-T`.

- **`--tune-phase N`** (0–26, default 0 = off) — hill-climb the middle and right
  wheels' *phase* instead of enumerating it. A wheel's phase is its ring and
  start shifted together, so its offset — and with it the wheel's whole
  contribution to the substitution — stays put and the only thing that moves is
  when its own notch fires. With the flag on, the sweep enumerates offsets alone
  (26³ per wheel order instead of 26⁵) and each work item climbs the plugboard,
  **freezes that board**, scans all 26 × 26 phases, re-climbs at the winner, and
  repeats until neither improves.

  The order matters: a rotor key scored without a plugboard is noise — a
  rotor-only decrypt under a full board is ~95% scrambled — so the board is
  recovered first and only then held fixed. Measured with 10 plugs hidden at
  L=439, the frozen-board score peaks at the true phase in 8/8 trials when the
  starting phase is within 5 of it, and the capture radius grows with length
  (roughly `0.4 × length / 26`), which is what `N` starting phases per wheel are
  for. It is an *approximation* and says so in the echoed settings.

  Needs `-c`, and both `-r` and `-g` must wildcard the middle and rightmost
  positions; rejected with `--ring-stride`, `-F`, `--exhaust`, `--crib` and
  `-A`. Off by default and byte-identical to the previous release when off.

  Measured against the alternative use of the compute — the same wall time spent
  on `-R` restarts over the full ring enumeration, 80 paired trials at 200
  letters: `--tune-phase` **breaks more messages** (63/80 exact against 51/80,
  McNemar p = 0.043) but scores a **lower mean %-correct** (85.5 against 91.0,
  CI spans zero). The failure shapes are opposite and that is the whole
  difference: the exhaustive sweep always has the true rotor key in its
  keyspace, so its misses are plugboard misses that still return 76–98% of the
  letters, while `--tune-phase` can settle on the wrong offset, which nothing
  downstream can repair. It fails less often and worse. Prefer it when only a
  full break is useful; prefer the exhaustive sweep when a partial answer has
  value.

  **Below matched compute it pays outright.** Once both arms saturate the
  matched-wall-time question stops discriminating, so `-R` was swept over the
  same 40 instances at L=450: `-R 8` matches the exhaustive sweep's 38/40 for
  **23.4 s against 171.5 s, 7.3× cheaper**, and saturates there (`-R 16` is an
  identical outcome for double the time); `-R 4` gives up one break for 14.5×.
  At operational lengths the flag is therefore not a trade at all — the same
  result for a seventh of the compute — and the right operating point is a *low*
  restart count, nowhere near the `-R 42` that matched compute forced.

- **`--doubling-report L`** — report every converged climb whose decrypt carries
  a **doubled word** of `L`+ letters around an X — `ENGELMANN X ENGELMANN`,
  telegraphic German's own error correction — printed as the ordinary progress
  line with the preview replaced by the marker, the length and the word:

  ```
  +13.97 B231 AAA QMW AB CD EF                               >> 9 ENGELMANN
  ```

  A **confirmation signal, never a score term**, and that distinction is why it
  exists in this form: it enters no ranking, so a false positive costs a second
  look and cannot promote a wrong key. The score-bonus form of the same
  evidence was swept over 140 genuine 17 576-key sweeps and measured down — a
  post-climb bonus needs a trial where the climb recovered the plaintext and
  the score still lost, and there were **zero**; the climb is steered by the
  same score a bonus would adjust, so scoring failure presents as *search*
  failure first. Reporting has no such dependency: it fires on the key that
  *is* right, whatever the search's high-water mark, so the true key can be
  reported while another board still leads (the settings echo says so).

  Two companion knobs, both documented with the numbers so they cannot be
  turned in ignorance. **`--doubling-z Z`** (default 3) gates the check on the
  raw sigma count over the `--confidence` null — only ~0.56% of keys clear
  z ≥ 3, which is what makes the check free (measured at the noise floor at
  every gate down to 0). Chance reports fall ~16× per extra letter of `L`, so
  a 230 M-key rotor sweep expects ~6 spurious reports at `L = 7` against ~90
  at `L = 6` — **raise `L` before touching the gate**; a true key whose climb
  has recovered the plaintext sits at z = 7–16, nowhere near it.
  **`--doubling-mismatches N`** (default 1) is the positions the two copies may
  differ in: 1 is the channel's error and no more (Enigma has no diffusion, so
  a garble corrupts one letter in one copy), and raising it was measured on
  2 M null texts — `N = 2` multiplies false reports ~49× and finds nothing the
  default misses, matching the corpus, where 18 of 25 real doublings have no
  mismatch, 7 have one and none has two. An indel (`SCUHNACHER` against
  `SCHUHMACHER`) misaligns the copies and is missed by design. The scan is
  capped at 30 letters (the longest real doubling is 13; the cap is what keeps
  the scan O(30·n) instead of O(n²), and `L` above it is refused rather than
  silently searching nothing). Needs `-c` and `--confidence`; `--full-text`
  expands a report like any progress line. Verified against an independent
  Python reference on 4 000 random strings — 0 mismatches.

- **The `--confidence` bar is stated before the sweep, not only after it:**

  ```
  Confidence: margin 0 is z = 6.0, the best of 75198240 keys by chance
  ```

  The progress lines print a *margin*, and a reader watching them had no way to
  convert one back to the raw sigma count — the number every other account of
  a result is quoted in — until the run finished. Same figure the summary
  reports; a test asserts the two agree.

- **`--crib TEXT` — a known-plaintext key filter, and with `-c` a climb seed.**
  A crib is a guess at part of the plaintext together with where it sits.
  Decryption is `p = steck[core_i[steck[c]]]` and the rotor core is an
  involution, so it rearranges to `steck[p] = core_i[steck[c]]` — one lookup on
  a table the search already builds. Guess a single plug, chain it along every
  crib position, and add reciprocity (`steck[x]=y` ⇒ `steck[y]=x`, and no two
  letters share a partner — **Welchman's diagonal board**, free because the
  board is an involution). A contradiction kills the guess; all 26 dead means
  the rotor setting cannot have produced the crib, so the search skips it
  **without scoring anything** — measured **99.9% of keys** on a 12-letter
  pinned crib.
  - **The diagonal board is what does the work, not menu loops**: a loop-free
    12-letter menu still rejects 88% of settings, against 0% without it.
  - **`--crib-at N`** (1-based) pins the alignment; omitted, every alignment the
    self-encryption filter leaves is tried and a key dies only if *every*
    alignment rejects it. **Rejections multiply across alignments**, which sets
    the usable length: on a 125-letter message a 12-letter crib rejects 99.9%
    pinned but only **5.3%** swept, while 16 letters holds at 99.9% either way.
    16 is the swept floor; below it a crib can only seed a climb.
  - **With `-c` the crib also seeds the climb**, pinning each surviving
    hypothesis's deduced plugs — including letters deduced to carry *no* cable,
    which is a finding and not an absence of one. On an 88-letter message with
    the board hidden and a 12-letter crib: **92% of letters recovered against
    8% unseeded**, and the same 92% swept as pinned, so seeding does not need
    the alignment to be known.
  - **`--crib-list FILE`** runs a whole library, one complete rotor sweep per
    crib, keeping the best board across all of them — crib-outer, because the
    setup a rotor-outer loop would share is 0.6% of a run while early exit is
    worth up to 50×. Three things fatal for a single `--crib` merely skip the
    crib here (longer than the ciphertext, matching it at every alignment,
    rejecting every key): a library is written against a network's vocabulary,
    not one message. A table before each sweep reports a **measured** expected
    gain per crib, from running both units on the same sampled keys.
  - **`--no-crib-reorder`** keeps a library in file order; by default it runs
    cheapest-measured-cost first. The measured cost curve is a **cliff**, not
    the flat-ish one the generator modelled: relative to a no-crib sweep, 8
    letters costs **52×**, 12 costs 0.67×, 25 costs 0.02×. Ordering is a
    preference and not a filter — a `--crib-max-hyps` that *discarded* costly
    cribs was built and removed, because cost is anti-correlated with the chance
    of a hit and it skipped all four cribs actually present in the test message.
  - `--crib-dump` prints each surviving hypothesis, its alignment and the plugs
    it deduces. The menu is walked breadth-first from the anchor rather than in
    crib order, which makes the work track the component instead of the edge
    count (1.6–1.7× on wrong keys, the case a sweep spends its time on).
    `-T`-deterministic and zero cost when off; rejected with `-F`, `--exhaust`,
    `--ring-stride` and `-A`.

### Changed

- **The program is now 19 modules under `src/`, not one 9808-line file.** PRs
  #205–#221 split `enigma.cc` into 19 `.cc` and 19 `.h` (18 module headers plus
  the header-only `result.h`), each header stating what its module owns and,
  more usefully, what it deliberately keeps private. The `Makefile` builds one
  object per source with `-MMD -MP` dependency tracking, so a header change
  rebuilds exactly its dependents.
  - **No user-visible change, and that was verified rather than assumed.** The
    binary is byte-identical (`cmp`) under both compilers across the move to
    `src/`, and a 49-case reference capture stayed byte-identical at every one
    of the eleven steps.
  - **The boundaries were drawn by what must stay private, not by subject.**
    `plug_fixed`/`plug_fixed_ex` are read in the climb's move loop and their
    storage form is measured to matter by ~18%, so simulated annealing lives in
    `plugboard.cc` purely because `apply_toggle()` reads them; the crib menu
    tables are read inside `crib_try`'s loop, so the units that climb from a
    deduced board sit beside them; and `ngrams.cc` never names `quad8`/`all8`,
    the loader taking a destination pointer instead. Splitting any of those by
    subject would export exactly the state the measurements say to keep local.
  - **The split cost nothing measurable**, which took a release-tag comparison
    to establish — the per-PR bench guard sees each step against its own base
    and cannot see the sum. `make bench LONG=1 BASE=v2.1.0` after the split:
    `search` **−60.9%**, `hillclimb` −0.4%, `fused` −5.3%, every cell within
    ~1.5pp of the pre-split reading. The −62% on `search` is fully retained
    across nineteen modules, cross-unit calls and ~30 symbols that became
    `extern`.
  - One real hazard surfaced: `fatal()` lost its visible `exit(1)` across a
    translation-unit boundary, and cppcheck and clang-tidy began reporting
    null-pointer dereferences downstream of it. Declaring it `[[noreturn]]`
    restores what the analysers could previously see for themselves.

- **CI runs once per commit instead of twice per pull request.** A push to a
  branch with an open PR is *both* a push and a `pull_request` event, so a bare
  `push:` ran the whole matrix twice — 24 check runs where 12 say the same
  thing. `push` is now restricted to `master` and `dev`, leaving each commit
  checked exactly once: by `pull_request` while under review, by `push` once it
  lands. The two events are not merely duplicates and `pull_request` is the more
  useful one — it builds the PR head **merged into its base**, i.e. what the
  repository would actually get, where `push` builds the branch head alone. A
  concurrency group additionally cancels a superseded PR run, but deliberately
  **not** on `master`/`dev`, where each push is a different landed commit rather
  than a revision of the same one and cancelling would leave the commit a later
  bisect wants with no recorded result.

- **The climb chain exports only the two template instantiations another unit
  calls.** `hillclimb<true>` and `run_stages<false>` were explicitly
  instantiated in `plugboard.cc` and declared `extern template` in the header
  with no caller outside the file — the first is reached from
  `run_stages<true>`, the second from `optimize_once`. Both now instantiate
  implicitly and stay internal.
  - **Proved codegen-neutral by disassembly, not by benchmark**, which is the
    point of the entry: every remaining function is instruction-for-instruction
    identical under both g++ and clang, `hillclimb<false>` — the hot one, and
    the code whose storage form is measured to matter by ~18% — included. The
    only change is that `run_stages<false>`'s standalone body disappears while
    `optimize_once` stays at its exact 550 (g++) / 508 (clang) instructions,
    showing it had always been inlined there and the explicit instantiation was
    forcing a second, unreachable copy to be emitted. `plugboard.o` shrinks
    ~155 bytes under both compilers.
  - A bench could not have established this: the effect is far below the floor.
    Run anyway, it doubles as a free base-vs-base control on byte-identical hot
    code and read −2.5%…+2.9% across the four quick tiers — a useful reading of
    the day's noise.

- **`-Wmissing-declarations` is now part of `CXXFLAGS`.** A function with
  external linkage and no header declaration is invisible to every gate this
  repo has — `subst_rotors()` sat dead for years in exactly that state, and only
  turned up because the module split happened to make it `static`. The flag
  looks for the condition directly. It found three more on the day it went in —
  `precompute_worker`, `filter_worker` and `finish_worker`, each with a single
  same-file caller — which are now `static`. A clean tree reports none, both
  compilers accept it, and the change is diagnostic only: the 49-case reference
  capture stays byte-identical and `make bench BASE=` reads within noise on
  every tier.

- **The valgrind CI job now covers the paths that derive state and reuse it.**
  It ran three invocations — a scan, a climb and a `-p` compare — and valgrind
  is the *only* gate for **uninitialised** reads, which ASan does not catch. So
  the crib deduction and its seeded climbs, the self-crib closure,
  `--exhaust`'s pin sets, the `--ring-stride` refinement's derived candidates,
  `--confidence`'s sampled null, `--tune-phase`'s re-climbs, and both
  non-standard machines were all unchecked for the failure mode they are most
  exposed to. A second block covers all nine; every one was clean when added,
  so a failure there is new.
  - `--error-exitcode` moved from `1` to **66**. The program's own fatal exit is
    1, so at `1` a run that dies of *"No machine configuration produced a
    score"* was indistinguishable from a valgrind finding — and a mis-specified
    case could look like it was passing a check it no longer performed. The
    TSan job already used 66.
  - Verified by injection, and the injection itself is worth recording: an
    uninitialised **stack** read is folded away at `-O1` (with a
    `-Wuninitialized` warning) and never reaches valgrind, so it reads as a
    passing run. A `malloc`'d read reaching a branch does reproduce — with one
    placed in `refine.cc`, the three pre-existing cases still passed, which is
    the proof the gap was real, and the new `--ring-stride` case failed with 66.

- **Restart is now the OUTER dimension of the work space**: the sweep does
  every key at restart 0, then every key at restart 1, and so on, instead of
  finishing each key's `-R` restarts before moving on. Throughput is unchanged
  — the per-key `setup_mapping` reuse the old order bought is under 1%,
  unresolvable above thread jitter, and `make bench BASE=` reads ±1% on every
  tier — but *when* the answer appears moves a great deal: there is no early
  exit, so front-loading is what lets a watcher kill a long sweep early. On
  the measured climb curve (87% at `R = 16`), found by the quarter mark **40%
  against 22%**, by halfway **64% against 44%**. The progress line reports per
  pass (`pass 2/4, 8728 / 17.6k keys`) — dividing items by restarts, as
  before, would read 6% of keys covered at the moment every key had been
  visited once. The rate stays key-visits per second and the ETA runs to the
  end of the whole sweep; with one restart the pass field is omitted and
  nothing changes. Exactly-tied boards can resolve differently (the tie-break
  is on the work index, which now enumerates in a different order) — still
  deterministic, still `-T`-independent, verified across `-T` 1/2/4/8. The
  winning key is reconstructed from the merged index at two sites (`--polish`
  and the `--ring-stride` refinement); both now share one `work_key()` helper,
  and three checks re-encrypt the reported plaintext under the reported key to
  prove the echoed key still matches the plaintext on stdout.

- **The test suite runs 3.6× faster — 232 s → 64 s — with all 437 checks
  intact.** `tests/run_tests.sh` had a single shared start-position fixture
  `-g $rg` (676 keys, 26 under `TEST_QUICK`) used by 48 checks, but only about
  eight of them were *recovery* checks, where a wide sweep is the point because
  the true key has to beat decoys. The rest asserted that two runs **agree**
  (`-T`-independence, `-R 0` equals the default, `-F 0` is off, the seed
  echoes), which 26 keys establishes exactly as well as 676 — and the sanitizer
  job had always run them at 26 via `TEST_QUICK`, so the plain g++/clang job was
  paying 26× for a duplicate of an assertion already covered. A second narrow
  fixture `$rgd` now serves those, and `$rg` is kept for recovery and for `-F`,
  which needs more keys than it keeps.

  Separately, the three `restart-parallel` checks — **56 s, a fifth of the whole
  suite** — did not test their own property. Their comment says "with a
  fully-specified rotor key the search has exactly ONE key, so `-T` can only
  speed things up by spreading the `-R` restarts across threads", yet they
  passed `-g $rg` and swept 676 keys, never exercising the single-key path.
  Pinning `-g AAA` made them both correct and ~500× cheaper.

- **`--tune-phase` is now measured across message length, and the trade it makes
  dissolves by ~450 letters.** It shipped measured at one length (L=200), where
  at matched wall time it broke more messages than an exhaustive ring sweep but
  scored a lower mean %-correct. Re-run at L=300 (80 paired trials) and L=450
  (40), with the budget re-calibrated at each length as that comparison
  requires:

  | L | B mean / exact | A mean / exact | McNemar |
  |---:|---|---|---|
  | 200 | 91.0, 51/80 | 85.5, **63/80** | p = 0.043 |
  | 300 | **98.8**, 65/80 | 94.7, **74/80** | p = 0.049 |
  | 450 | 100.0, 38/40 | 100.0, 39/40 | p = 1.0 |

  The split survives at 300 letters and is gone at 450, where the arms are
  indistinguishable. **It dissolves because the problem stops being hard, not
  because the flag pulls ahead** — `--tune-phase`'s catastrophic misses (a wrong
  offset, under 20% of letters) fall 12/80 → 4/80 → 0/40, which is what the
  capture radius predicts, but the exhaustive arm's reach zero *first*, at
  L=300: past that length the true key is always in its keyspace and a plugboard
  miss still returns ~94% of the letters. So the guidance is unchanged in kind
  and narrowed in scope: choose between them below roughly 400 letters, and
  above that it does not matter which you pick.

  The obvious mechanism explains the rate but not which trials fail. Bucketing
  by the cyclic distance from the true ring to the nearest starting phase — the
  quantity the capture radius is about — leaves the catastrophes spread over
  distances 3–5 at both lengths, with the *worst* bucket recovering 29/32 at
  L=300, so raising `N` is not the fix that reading would suggest.

  The report script now prints the failure-shape table (misses, of which
  catastrophic, and the partial-miss mean) that made this legible; the headline
  means and exact counts alone show a shrinking gap and hide the fact that the
  two arms fail in opposite ways. Raw data `eval/results-tune-phase-L300.jsonl`
  and `-L450.jsonl`, with `.txt` summaries.

- **The unknown-key break rate is now measured**
  (`eval/unknown_key_headroom.py`) — 55% at 17 576 keys and 53% at 230 million,
  for a 167-letter message with a
  10-pair board hidden at `-R 8`. Every other result in this repo measures
  plugboard recovery with the rotor key *given*, so a negative sweep could not
  previously be read as evidence about the message.

  **Keyspace size is nearly irrelevant**: four orders of magnitude of `K` cost
  two points, because the chance bar grows as `√(2 ln K)` (4.42 → 6.21) while
  the true key's z has a median of 11.5. The limits are climb failure at the
  true key and a scoring floor, both independent of `K` — and separating them
  needs a **high** `-R`, since at a single `-R` a failed climb also produces a
  low z and the two are the same trials. Judged at `-R 64`, **95%** of messages
  are intrinsically breakable at L=167; the floor is 5%, and the rest of the
  residual is climb failure.

  **At matched wall time the middle option wins.** Per 24 h: `-r A..` exact
  affords `-R 4` for a 66% break rate, `-r AA.` affords `-R 34` for 65%, and
  `-r A.. --ring-stride 3` affords `-R 12` for **80%** — because the climb curve
  flattens (50/68/79/87/95/100% at R=2…64) before the coverage penalty does.
  Spend on `-R` until it flattens, then buy coverage.

  It avoids sweeping at all: a break needs the climb to work at the true key
  *and* that key's score to clear the bar, and the second is arithmetic once the
  z is known — 3 s per trial against the ~10 h a real 80M-key sweep costs.

- **Two-notch wheels (VI, VII, VIII) now collapse the RIGHT wheel's ring ×
  start by 13** — always on, no flag. Those three notch at `M` and `Z`, exactly
  13 apart, so their notch *set* survives a shift of 13; and since a stepping
  wheel's absolute position is read by nothing but that notch test, shifting its
  ring and start together by 13 gives a byte-identical decode. The search now
  tests one member of each pair.

  Exact and unconditional — unlike the middle wheel's collapse it has no length
  term, so it is worth **2× at 40 letters and 2× at 900 alike**. It composes
  with the middle-wheel collapse, giving **4×** when VI–VIII sit in both
  positions. Applies only when ring2 and start2 are both wildcarded, and is
  **0% under the default `-x 5`**, since none of wheels I–V has two notches — so
  it pays for Kriegsmarine traffic and nothing else.

  The middle wheel needed no work: its collapse derives classes by simulating
  the stepping rather than from a formula, so it had been picking this up all
  along. Reported ring2/start2 may now be either member of a pair, the same
  class-representative contract the other collapses already carry; the decrypt
  is identical either way, and the settings echo names which collapses fired.

- **`--score i4f10` is now the measured pick for telegraphic traffic at
  operational length**, in place of the recommended `m4f10`. The existing advice
  — a monogram pre-pass beats an index-of-coincidence one on telegraphic German
  by 2.2 pp — was measured with **`-a`** as the target, and `-f` differs from
  `-a` precisely by folding IC into the target score, so the recommendation had
  never been checked against the model the tool actually recommends.

  Against `-f` the ordering **reverses**. On authentic HG Nord decrypts at 167
  letters, 2000 paired trials across five independent seeds: `i4f10` beats
  `m4f10` by **2.81 pp** mean %-correct (95% CI [−4.80, −0.82], z = 2.76) and
  **3.1 pp** of exact recovery (72.2% → 75.2%; McNemar p = 0.021 over the 1800
  trials with logged discordants). All five seeds agree, heterogeneity is
  Q = 1.65 on 4 df, and `score_iter` matched within 2% in every run.

  `m4f10` remains the default elsewhere.

  **The target matters about twice as much as the pre-pass, and the two do NOT
  interact.** The full `{m4,i4} × {a,f}` square was measured at L=167, 1000
  paired trials per cell. Fused over weighted is **+6.56 pp** with a mono
  pre-pass and **+5.20 pp** with an IC one — both above the +3.0…+4.4 pp
  recorded for `-f` over `-a` — while the difference between those two target
  effects is +1.35 pp, 95% CI [−1.25, +3.95], **z = 1.02**.

  The IC pre-pass therefore wins under **both** targets at this length,
  confirmed directly for `-a` as well (−6.40 pp, McNemar p = 0.009). So the
  documented "mono beats IC by 2.2 pp on telegraphic" does not reproduce at
  L=167 under either target, and the explanation points back at **message
  length** rather than an interaction between the two knobs. A single L=60 run
  did lean mono under `-f`, consistent with a crossover somewhere between.
  Reproducer: `eval/prepass_ab.py`, with `--arms` for any two schedules.

- **`--confidence N` is echoed in the settings** (with whether its samples are
  climbed), and the sampling shows a live progress line on a TTY. The flag
  changes what the first column *means* — margin, not score, a difference of
  ~20 on the same run — so a saved log has to say so up front rather than leave
  it to be inferred. The progress line matters because under `-c` each sample is
  a whole plugboard climb: at `N` = 1024 that is a couple of seconds before the
  search prints anything. It is erased when sampling finishes, since the echo
  already gave `N` and the summary gives the result.

- **BREAKING: `--crib-file` is now `--crib-rerank`.** It re-ranks *finished*
  boards by known-word content and has nothing to do with the crib deduction;
  beside the new `--crib-list` the two names would have been one letter apart
  for two unrelated features. The old name is not accepted — **the next release
  carrying this needs a major version bump.**

### Changed

- **CI builds with `g++-14` as well.** The build is `-Werror`, so a warning a
  newer gcc introduces is a hard failure for anyone on a current distro while
  the runners — ubuntu-24.04, gcc 13.3 — stay green. That is how a
  `-Wformat-truncation` warning reached a Debian 13 user unseen. Added as a
  third matrix cell rather than an upgrade of the existing one, and installed
  by name rather than inherited from `ubuntu-latest`, so a compiler bump is a
  deliberate commit here instead of something GitHub does on its own schedule.
  Verified to earn its keep: reverting the buffer fix makes this cell fail the
  build while the other two pass.

- **The `Bench` workflow now blocks on a real regression instead of only
  flagging one.** It was `continue-on-error` throughout, which is how a **+50%
  crib-sweep regression** was reported and merged anyway. It cannot simply
  become a hard gate at one number, because the measurement noise floor is
  per-tier and per-compiler and reaches ±10% on some cells — a gate there would
  fail clean PRs. So there are two levels: `THRESHOLD` **reports** at 10%
  (unchanged, and now the same 10% the script defaults to locally, so the
  documented number cannot drift from the configured one) and `FAIL_OVER`
  **blocks** at 25%, which is 2.5× the worst floor ever recorded here and cannot
  plausibly be scatter. The run that verified the fix demonstrated the need for
  two levels in one shot: with the regression reintroduced on a busy box, `crib`
  read **+45.6%** (blocked, real) while byte-identical `fused` read **+11.9%**
  (flagged only, pure noise). Locally `FAIL_OVER` defaults to `THRESHOLD`, so
  `make bench BASE=…` still exits non-zero at 10% as it always did.

### Fixed

- **`-Wformat-truncation` warning on gcc 14+ (a `-Werror` build failure).**
  `report_doubling()` sized its two scratch buffers on `maxlen / 2` (512 bytes)
  when the value they hold is bounded by `doubling_maxlen` (30) —
  `find_doubling()` clamps its scan to it, so the word can never be longer
  however long the message is. gcc can only reason from the buffer size, so it
  saw `">> %d %s"` writing up to 3 + 11 + 512 bytes into 528 and warned. Sized
  to the real bound instead; padding the destination would have hidden the
  mismatch rather than fixed it. Reported on Debian 13; the CI runners carry
  gcc 13, which does not emit it.

- **`--confidence` was broken by any selective `--crib`: every progress line
  printed the same margin.** A crib-rejected key reports `unit_no_score`
  (`-1e300`) — a sentinel meaning *this unit produced no candidate*, not a
  score — and `calibrate_null()` pushed it straight into the sample. A crib
  worth using rejects 99%+ of keys, so nearly every sample was `-1e300`: the
  mean sat at ≈`-1e300`, the **variance overflowed to `+inf`**, and
  `(s − μ)/σ` came out exactly `0.0` for every board. The visible result was a
  300-digit null in the summary and the identical margin `−z_k` on every line.
  Those keys are not part of the null the search draws from — it never scores
  them — so they are now excluded, and the attempt budget is raised to
  compensate (a rejected draw costs only the deduction, an accepted one a whole
  climb). When a crib is so selective that too few samples survive, the run
  says so, names the crib as the cause, and falls back to raw scores instead of
  reporting nonsense. **The search itself was never affected** — only the
  calibration; the run that surfaced this recovered its plaintext correctly.
  - Residual, unchanged and deliberately conservative: the bar is still
    `√(2 ln K)` over *all* keys rather than the smaller number a crib actually
    lets through to be scored, so a crib run's margin is understated by roughly
    0.5–1.1 σ. Conservative is the safe direction for an "is this a find?"
    test.

- **`--confidence`'s "not a find" note impersonated a progress line.** The
  documented way to pull a run's margin off stderr is `grep '^ *[+-][0-9]'`,
  and the near-zero caveat wrapped as `"… on signal-free text a margin of\n
  +0.5 sd came up …"` — so its **continuation line matched that pattern** and
  an extractor silently read the caveat back as the run's result. `+0.5` is a
  plausible margin, so nothing looked wrong. Found when a sweep of 33 known
  1941 day keys against the unbroken BYQMZ reported *"+0.5 sd came up in 2-5%
  of runs"* as the margin for all 33. Re-wrapped so no line begins with a
  signed number. This is the same bug class the pre-flight lines were already
  guarded against, and the suite now asserts it of the confidence summary too —
  verified by injecting the old wording and watching the new check fail.

- **`--crib` with `-s` could build an impossible plugboard and smash the
  stack.** The deduction started from an empty board, so a hypothesis could
  deduce `A–D` while `-s` said `A–B`; seeding overwrote `steck[A]` and left
  `steck[B]` pointing at `A`. The result was not an involution, and
  `format_plugboard` — sized for the 13 pairs an involution can have — walked
  off its buffer on the 14+ a corrupt board yields, aborting with *"stack
  smashing detected"*. `crib_try()` now starts from what `-s` and `--no-plug`
  already fix,
  so `crib_set` rejects a contradicting hypothesis instead; `format_plugboard`
  additionally refuses to overrun whatever it is handed. Found while testing
  `--crib-seeds`; it predates that work and reproduces on the released code.
  - **It is not only a crash fix — `--crib` with `-s` was doing enormously more
    work than it needed to**, because the contradicting hypotheses it should
    have rejected were instead let through to corrupt the board. Now they are
    rejected by arithmetic, and the saving scales with the number of pins.
    Measured on one 90-letter message, a 12-letter swept crib, 17 576 keys,
    plugboards scored before → after (every arm still recovers the plaintext
    exactly):

    | `-s` pins | before | after | |
    |---|---:|---:|---:|
    | `AB` | 20 736 444 | 211 588 | 98× |
    | `AB CD` | 15 555 774 | 1 851 | 8 404× |
    | `AB CD EF` | 11 425 564 | 36 | 317 377× |
    | `AB CD EF GH IJ` | 6 057 585 | 2 | 3 028 790× |

    Key rejection on that problem goes from 9.3% to 100.0% with three pins —
    every wrong key is now killed by the deduction and only the true one is
    ever climbed.
  - **The other half: a WRONG pin is now fatal instead of free.** It used to be
    overwritten silently and the search still succeeded; now it kills every
    hypothesis at every alignment and the run ends *"No machine configuration
    produced a score"*. On a board of `AB CD EF GH IJ KL MN OP QR ST`,
    `--no-plug UVWXYZ` (true) recovers the plaintext in 1 839 plugboards and
    `--no-plug QWERTYU` (false — Q, E, R, T are plugged) now returns nothing
    where the released code returned the correct plaintext. Failing loudly on
    contradictory input is right, but a *guessed* plug belongs in
    `--soft-plug`, not `-s`.

- **`--full-text` wrapped 2 columns short of the progress line.** The
  continuation width dated from a 79-column target; the progress lines were
  later budgeted to land on exactly 80 in every mode (61+19 for 3 wheels,
  64+16 under `-4`, 65+15 and 68+12 with the crib column) and the two were
  never reconciled, so the wrapped text stopped short of the right margin of
  the preview it replaces. The suite's one-sided "stays within 80" check
  passed 78 as happily as 80 — it now compares the widest continuation against
  the progress line from the same run, so the two cannot drift apart again.

- **An oversized raw score shifted every column after it.** The score field is
  8 characters with zero slack — the weighted models bottom out around −14,
  which fills it exactly — and of the two printers sharing it only the margin
  (`--confidence`) had a width guard. Reachable via `ENIGMA_LOGLIN`, which
  scales the quad table: ×10 printed `-142.3724` and broke the 80-column
  budget. The raw branch now falls back to `%.1e` exactly as the margin does,
  with one check per printer.

- **`make bench BASE=<old tag>` reported a tier the base could not run as an
  infinite regression.** Comparing `dev` against `v2.1.0` printed
  `crib +13268.6% REGRESSION` — but `--crib` postdates that tag, so the base
  binary exited immediately on the unknown option, was timed at 0.00 s, and the
  ratio blew up. The row now reads `n/a — base lacks these options` and does not
  count toward the threshold; skipped tiers are counted and reported at the end
  so partial coverage is never silent. A **head** binary that fails is still a
  hard error, since that is a broken benchmark rather than a skippable row.

  Two drift checks were run with it. Against `v2.1.0` (160 commits back)
  `search` is **−60.8%** and the climb tiers flat. Against `46d4999` — the merge
  of PR #151, the first commit holding all four search optimisations, 58 commits
  back — `search` is **+0.2%** and `hillclimb` **+0.7%**, so the win has been
  fully retained, while `fused` (−6.0%) and `crib` (−10.3%) are faster still.
  The second comparison is the better-conditioned one: against the release a 5%
  regression would hide inside a −62% victory, whereas both sides of the
  post-optimisation pair start from the same baseline and the expectation is 0%.

- **`--confidence`'s p-value was optimistic near zero, and said so only under
  `-i`.** It is the Gaussian upper tail, and the statistic it models — the
  maximum over `K` keys — lives at ~4.4 σ, exactly where a central-limit
  approximation is weakest: the score is a sum over positions, so the CLT gives
  the centre of the null quickly and the tail slowly. Measured on **2000
  signal-free ciphertexts** (L=200, K = 17 576, `--confidence 1000`), a margin
  of **+0.54 came up 2.35% of the time against the 0.70%** the p-value implies,
  and at K = 3 163 680 it came up **4.83%** — the rate rises with the keyspace.
  The null's best-of-K sits +0.21 σ above a Gaussian of the same μ/σ, with a
  95th percentile of +0.40 against +0.11 predicted.

  The caveat is now unconditional rather than IC-only (IC keeps an extra clause,
  being worse again), and a run whose margin is under +2 σ — the measured 99th
  percentile of noise — additionally prints *"below +2 sd is not a find"* with
  the measured rate. Nothing changes far out, where a real break reads +15 to
  +17 σ and a factor of three on 1e-98 means nothing, so the note fires only
  where the number actually misleads. Raising `N` does not help: at N=1000 the
  estimation error is ~0.10 σ and nearly all the spread is the genuine
  fluctuation of the best of K. Reproducer:
  `eval/confidence_false_positive.py`.

- **`--confidence`'s summary reported a key count its own chance bar was not
  built from.** The count was passed to the summary separately from the bar, and
  under `--ring-stride` the caller passed the refinement's keys too — so the
  line read "chance best of 1528334 keys is 5.3 sd" with 5.3 computed for
  1 527 084. The bar now reports the `K` it was built from, and the inclusive
  total stays with the `Analysed N rotor combinations` diagnostic where it
  belongs. Worth 0.00015 σ — `√(2 ln K)` barely moves with `K` — so this closes
  a drift the output could not show rather than a visible error. The refinement
  cannot be folded into the bar instead: it has to exist before the sweep, which
  is what keeps the margin a constant offset from the score, and the
  refinement's keys are chosen conditional on the coarse winner rather than
  drawn independently.

- **`--confidence` on a fully-specified rotor key printed a margin of ~1e13 and
  broke the progress-line columns.** With the key given there is exactly one key
  to sample, so every sample scores the same and σ̂ came out as floating-point
  noise (~1e-15) rather than 0 — passing the `sd > 0.0` guard and making the
  margin `score/1e-15`. The 15-digit result overflowed the 8-wide first column
  and pushed every line to 87 characters. The guard is now relative,
  `sd > 1e-9·(|μ| + 1)`, which sits nine orders below any real null (~0.17) and
  six above the noise; a degenerate null now says there is nothing to measure
  against and the run falls back to raw scores. `showconfig` also falls back to
  `%+.1e` — exactly 8 characters — if a margin ever fails to fit, so no
  arithmetic surprise can shift the columns again.

## 2.1.0

36 commits on the tool since 2.0.0. It adds a **fused scoring model** that is
the first scoring gain here not tied to a writing style, cuts the **rotor
keyspace** on identifiability grounds, and adds **`--ring-stride`** for trading
a little accuracy for throughput on the rightmost wheel.

### Added

- **`-f` / `--fused`, fused weighted all-order + index of coincidence**, and the
  new recommended model when the language is known. It takes `-a`'s table
  unchanged and adds `lambda * IC` to the per-symbol score. Measured **+3.0 to
  +4.4 percentage points** over `-a` on english, german *and* wehrmacht — the
  first scoring change in this tool that does not depend on the writing style,
  which is expected, since IC is language-independent. Wall-time neutral. Note
  it is a better *climb* rather than better discrimination: the gain is surface
  reshaping, so it does not lift the scoring-failure floor. Recommended recipe:

  ```sh
  ./enigma -c -S m4f10 -J --polish -f -l <lang> -T <cores> -R <restarts>
  ```

- **`--ring-stride K`** (1–26, default 1 = off) — test only every `K`th ring
  position of the rightmost wheel, then refine every skipped position. Worth
  using at **`K=2` or `K=3`** when throughput matters: on authentic telegraphic
  German they analyse 1.9× and 2.6× fewer keys for about half a percentage point
  to two percentage points of exact recovery. `K` of 5 or more is not
  recommended. Needs both `-r` and `-g` to wildcard the rightmost wheel's
  position, and is rejected together with `-F`/`--exhaust`.

  The refinement is **derived rather than searched**: the skipped positions'
  ring/start settings follow from the coarse winner's stepping schedule, so it
  costs a few hundred keys rather than tens of thousands, and there is no
  keyspace where the stride costs more than it saves. Verified to recover
  everything an exhaustive refinement does, across wheels I–VIII, M4, `K` up to
  26, and messages long enough for the left wheel to step.
- **Five more scoring languages**: `swedish`, `finnish`, `icelandic`, `polish`,
  `spanish`.

### Changed

- **Settings that provably decode identically are no longer enumerated.** Two
  always-on keyspace reductions, both exact:
  - the **leftmost stepping wheel's** ring × start collapses totally (26×), in
    every mode, whenever both are wildcarded;
  - the **middle wheel's** collapses partially (3–5× at short lengths).

  Both are lossless — no key that decodes differently is dropped — and they
  apply to `--ring-stride`'s coarse pass as well.

- **The reported ring and start for those two wheels may differ from previous
  releases, on the same command line.** This is the one change that can surprise
  you, so read it before diffing output against 2.0.0:
  - the leftmost stepping wheel's ring is now always reported as `A`, since only
    its offset from the start position is recoverable at all;
  - the middle wheel's ring/start may be reported as a **class representative**
    rather than the true pair, because class members are indistinguishable from
    ciphertext alone. It is length-dependent: past L≈676 every class is a
    singleton and the true key is reported exactly.

  **The recovered plaintext is byte-identical either way** — an affected run
  decrypts exactly as before. Only the echoed key and the `Analysed N rotor
  combinations` count change. Set `--true-key` to disable the middle-wheel
  collapse.

- **N-gram loading is about twice as fast** (hand-rolled parser; the log value
  evaluated once per entry rather than twice). This is startup cost, not the hot
  path, but it is paid by every invocation.
- The resolved-settings echo reports `--ring-stride` and the middle-wheel
  collapse when they are active.

### Fixed

- **The `wehrmacht` table had an unbounded reweighting and a silent 32-bit
  overflow**, so some counts wrapped instead of saturating.

Bugs found and fixed in `--ring-stride` during its development are not listed:
the option is new in this release, so they were never in a shipped version.
