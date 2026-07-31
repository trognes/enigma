# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/): the major number changes when
existing command lines can behave differently or stop working.

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

- **`--ring-stride`'s refinement is derived rather than searched.** The skipped
  positions' ring/start settings follow from the coarse winner's stepping
  schedule, so the refinement costs a few hundred keys instead of tens of
  thousands (84 500 candidates became 650–1 300). There is no longer a keyspace
  where the stride costs more than it saves, and the warning that used to say so
  is gone. Verified equivalent to the exhaustive refinement across wheels
  I–VIII, M4, K≥8, and messages long enough for the left wheel to step.
- **N-gram loading is about twice as fast** (hand-rolled parser; the log value
  evaluated once per entry rather than twice). This is startup cost, not the hot
  path, but it is paid by every invocation.
- The resolved-settings echo reports `--ring-stride` and the middle-wheel
  collapse when they are active.

### Fixed

- **The `wehrmacht` table had an unbounded reweighting and a silent 32-bit
  overflow**, so some counts wrapped instead of saturating.
- **`--ring-stride` searched the wrong rotors in Norway mode (`-n`)**, because a
  rotor index was translated twice. Invisible in standard and M4 mode, where the
  raw and translated indices coincide — so the whole test suite passed over a
  broken feature.
- **`--ring-stride` ran the plugboard finisher even with no `-c`**, adding plugs
  to a board supplied via `-s`. This also invalidated every accuracy measurement
  of the flag made before the fix: they were roughly an order of magnitude too
  pessimistic, which is why `--ring-stride` is now recommended at `K=2`/`K=3`
  rather than described as a poor trade.
- **The refinement clamped rather than wrapped at the ring 0/25 boundary**, so a
  coarse winner at `A` with the true ring at `Z` was never checked; and a second
  refinement segment could pick up stale, already-stepped machine state.
- The refinement no longer echoes duplicate progress lines or a misleading
  progress ladder, and no longer retests the coarse winner it already scored.

## 2.0.0

The first release since 1.1.0, and a large one: 418 commits. It adds the
four-rotor naval **M4** machine, a sharper **weighted all-order scoring model**,
a domain-matched **`wehrmacht`** scoring language for real telegraphic traffic,
and a family of plugboard-recovery search options (restarts, staged schedules,
simulated annealing, a key pre-filter). Every option also gained a long name.

### Breaking changes

**If you are upgrading from 1.1.0, read this section — one change is silent.**

- **The default scoring model is now the index of coincidence (`-i`), not
  quadgrams.** Previously `./enigma -l english < cipher.txt` scored candidate
  decrypts with quadgrams; it now scores them with the index of coincidence,
  which produces *different results with no error message*. Add an explicit
  model selector to restore the old behaviour:

  ```sh
  ./enigma -q -l english < cipher.txt     # 1.1.0's default behaviour
  ./enigma -a -l english < cipher.txt     # recommended in 2.0.0 (see below)
  ```

  The reason for the change: an n-gram default requires a language, and `-l` has
  no default, so the old default could not run without extra options. The index
  of coincidence needs no language, so the tool now works out of the box.

- **Conflicting scoring-model selectors are now a fatal error.** In 1.1.0 the
  last selector on the command line silently won; `-m -q` now exits with an
  error, because the intent is genuinely ambiguous. Agreement is still fine (`-q
  -q`, or `-q --score i4q10`). Pass exactly one model.

- No short option was removed or renamed. Every 1.1.0 flag (`-u -w -r -g -s -p
  -l -x -T -d -i -m -b -t -q -c -v -h -n`) still exists and still means the same
  thing.

> **Note for anyone tracking the `dev` branch rather than releases.** Several
> options that were added *and* removed between 1.1.0 and 2.0.0 never appeared
> in a release, so they are not listed above. If you were using them: `-I` is
> now `-J`, `--gainfix-best3` is now `--polish`, and `--gainfix` is now
> `--cascade`; `--infl-order`, `--repair3`, `--restart-tt`, `--score-tt` and
> `--dump-restarts` were removed as measured-dominated or subsumed (see
> `archived/PERFORMANCE.md`).

### Added

**Machines**

- **M4 (four-rotor naval) mode, `-4`.** Thin reflectors UKW-b/c plus the static
  Beta/Gamma Greek wheel. `-u` selects the thin reflector `b`/`c`, and
  `-w`/`-r`/`-g` take **four** characters with the Greek wheel / ring / start
  first. The Greek wheel never steps, so it is folded into an effective
  reflector and the hot path is unchanged.

**Scoring**

- **`-a`, weighted all-order scoring** — a log-linear (geometric) mixture of
  quadgram/trigram/bigram/monogram statistics, and the recommended model when
  the language is known. Measurably better on short messages than plain
  quadgrams across every shipped language, and neutral on long ones.
- **`-l wehrmacht`**, a scoring language built from published statistics of
  ~20,000 letters of 1941 Enigma decrypts, for authentic telegraphic traffic
  (`X` as word separator, `Q` for *ch*, spelled-out numbers). Measured +20.9
  percentage points on a held-out set of 69 real messages. It is a *writing
  style*, not a separate language: on ordinary prose German it is a domain
  mismatch (−10.2 pp), so keep using `-l german` for prose.
- **Accented letters are folded to their A–Z base** (`é→E`, `ü→U`, `ø→O`, `ß→S`)
  in both the n-gram tables and the input text, instead of being discarded. This
  materially improved non-English scoring, which had been reading only a
  fraction of its tables.

**Plugboard recovery**

- **`-R N` / `--restarts N`** — random restarts, the primary quality lever for
  short messages, with **`--random K`** setting the kick size.
- **`-S` / `--score <schedule>`** — staged climb schedules with per-stage plug
  caps, e.g. `--score m4a10` (monogram pre-pass, then the weighted model capped
  at 10 plugs).
- **`-A N` / `--anneal N`** — plugboard recovery by simulated annealing, a
  matched-compute peer of the restart climb.
- **`-J`** — first-improvement climbing with dynamic per-restart move ordering;
  ~2.8× cheaper per climb, so pair it with a larger `-R`.
- **`-M`** — treat the plug cap as a strict descent target.
- **`--polish`** — the recommended finisher: one fixed-cost directed-repair pass
  over the best board after all restarts.
- **`-F N` / `-F N%`** — a two-tier key pre-filter for long messages.
- **`-e N` / `$ENIGMA_SEED`** — seed control. Runs now use a fresh random seed
  by default and echo it, so any run can be reproduced exactly.
- Opt-in, not recommended, kept for completeness: `--cascade`, `--exhaust`,
  `--no-repair`, `--crib-file`/`--crib-weight`.

**Interface**

- **Long-option names for every option** (`--reflector`, `--wheels`,
  `--restarts`, …), with unambiguous prefixes accepted (`--restart`, `--lang`).
- The resolved configuration is echoed to stderr at startup, and progress lines
  are fixed-width columns under a header, showing each improvement as the
  plugboard is built up.
- Diagnostic flags for measurement work: `--true-key`, `--dump-all`.

### Changed

- **The search is multi-threaded (`-T N`)** over the whole `keys × restarts`
  work space, so threads help even when the rotor key is fully specified and
  only the plugboard is being recovered. Results are independent of the thread
  count.
- **n-gram tables are stored as 8-bit fixed point**, shrinking the quadgram
  table to 0.45 MB so it stays cache-resident during the scan (~20% faster
  search). Recovery quality is unchanged.
- Unseen n-grams are floored at a hapax value, and model scores are reported as
  a per-symbol cross-entropy, so scores are negative and roughly
  length-independent.

### Fixed

- **Non-English n-gram tables were silently truncated at the first accented
  entry**, so German quadgram scoring ran on roughly 5% of its table. Combined
  with the accent folding above, German, Danish and French now crack comparably
  to English — German recovery on a 90-letter benchmark went from 24.7% to 91.1%
  mean letters correct. An earlier recommendation to prefer lower-order models
  for German was an artifact of this bug and has been withdrawn.

## 1.1.0 and earlier

See the git history.
