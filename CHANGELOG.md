# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/): the major number changes when
existing command lines can behave differently or stop working.

## Unreleased

### Added

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

### Changed

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
