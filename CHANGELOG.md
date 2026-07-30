# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/): the major number changes when
existing command lines can behave differently or stop working.

## 2.0.0

The first release since 1.1.0, and a large one: 418 commits. It adds the
four-rotor naval **M4** machine, a sharper **weighted all-order scoring
model**, a domain-matched **`wehrmacht`** scoring language for real
telegraphic traffic, and a family of plugboard-recovery search options
(restarts, staged schedules, simulated annealing, a key pre-filter). Every
option also gained a long name.

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

  The reason for the change: an n-gram default requires a language, and `-l`
  has no default, so the old default could not run without extra options. The
  index of coincidence needs no language, so the tool now works out of the box.

- **Conflicting scoring-model selectors are now a fatal error.** In 1.1.0 the
  last selector on the command line silently won; `-m -q` now exits with an
  error, because the intent is genuinely ambiguous. Agreement is still fine
  (`-q -q`, or `-q --score i4q10`). Pass exactly one model.

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
  percentage points on a held-out set of 69 real messages. It is a *writing style*,
  not a separate language: on ordinary prose German it is a domain mismatch (−10.2 pp),
  so keep using `-l german` for prose.
- **Accented letters are folded to their A–Z base** (`é→E`, `ü→U`, `ø→O`,
  `ß→S`) in both the n-gram tables and the input text, instead of being
  discarded. This materially improved non-English scoring, which had been
  reading only a fraction of its tables.

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
  to English — German recovery on a 90-letter benchmark went from 24.7% to
  91.1% mean letters correct. An earlier recommendation to prefer lower-order
  models for German was an artifact of this bug and has been withdrawn.

## 1.1.0 and earlier

See the git history.
