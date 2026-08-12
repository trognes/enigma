# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/): the major number changes when
existing command lines can behave differently or stop working.

## Unreleased

### Changed

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

### Changed

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

### Added

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

### Fixed

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

### Changed

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
