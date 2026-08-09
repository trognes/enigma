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

### Added

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
  or message length. Calibration is free without `-c` and costs ~1.5 ms per
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
