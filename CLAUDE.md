# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
this repository.

## Overview

This repository contains a C++ command-line tool that simulates an
Enigma cipher machine and, more importantly, attempts to **break** Enigma
ciphertext by brute-forcing rotor/reflector/ring/start settings and
hill-climbing the plugboard (steckerbrett). It was a single 9808-line
translation unit until PRs #205–#219 split it into the modules under `src/`
described below. Candidate decryptions are scored
against per-language n-gram statistics (monograms, bigrams, trigrams, quadgrams)
so that the configuration producing the most "language-like" plaintext wins.

The tool supports the standard 3-wheel Wehrmacht Enigma (wheels I–VIII,
reflectors A/B/C), the special **Norway Enigma (Norenigma)** variant (reflector
N, wheels 1–5), and the four-rotor naval **M4** (`-4`): thin reflectors UKW-b/c
plus the static Beta/Gamma Greek wheel, which folds into an effective reflector
so the engine stays a 3-stepping-rotor machine (see "M4 mode" below).

- **Author:** Torbjørn Rognes
- **License:** GNU GPL v3 (see `LICENSE`)
- **Language/era:** C++ written in a predominantly C style (C stdio, raw arrays,
  global state).
- **Development branch: `dev`.** All work happens on `dev`: branch from it, and
  open every pull request against it. `master` is the release branch and only
  ever receives `dev` by merge — never branch from `master` or target it. A
  branch cut from `master` will be missing whatever `dev` has gained since the
  last release, which is usually a lot (feature work lands on `dev` for several
  PRs before a release merge), so a stale base shows up as work re-deriving
  something already shipped or as conflicts against files the branch never saw.
- **Name working branches `claude/claude-dev-N`**, `N` counting up from the
  highest already on the remote — check with `git branch -r` rather than
  assuming, since merged branches are removed by hand and the numbering has
  gaps. One branch per PR, cut fresh from `dev`; do not reuse a branch whose PR
  has merged. If the harness assigns a different branch name, this convention
  wins.
- **Remote branches are deleted by the repository owner, manually.** There is no
  auto-delete on merge, so a merged branch lingering on the remote means nothing
  and must not be pruned or reused on that basis. `git fetch --prune` is fine
  when the owner has already deleted them; deleting them yourself is not.

## Repository layout

```
src/                      The program, as 20 modules (20 .cc, 21 .h -- 19
                          module headers plus two that are header-only,
                          result.h and parallel.h). main.cc is only the run:
                          it calls parse_args() in args.cc and then sequences
                          read, sweep, report. Everything else lives in a
                          module with a header stating what it owns and, more
                          usefully, what it deliberately keeps private.
Makefile                  Builds the `enigma` binary from `src/*.cc` with
                          g++ -O3 (one object per source, `-MMD -MP` deps).
README.md                 User-facing description and usage.
CHANGELOG.md              Release history.
CLAUDE.md                 This file.
ENHANCEMENTS.md           The open issue list -- what is still worth doing.
LICENSE                   GNU GPL v3.
.gitignore                Editor backups, cipher*.txt, the built binary, the
                          `src/*.o` / `src/*.d` build products, and __pycache__.
ngrams/<lang>_monograms.txt   Single-letter frequencies.
ngrams/<lang>_bigrams.txt     Two-letter frequencies.
ngrams/<lang>_trigrams.txt    Three-letter frequencies.
ngrams/<lang>_quadgrams.txt   Four-letter frequencies.
cribs/                    Two different things, as its README explains:
                          german-hgnord.txt is a known-WORD list for the
                          --crib-rerank finisher; wehrmacht.cribs is a crib
                          LIBRARY (guessed phrases) for --crib-list.
tests/                    run_tests.sh (make test), bench.sh (make bench),
                          crack_quality.py (make crackquality), reflow_md.py,
                          plus older sweep harnesses and their captured data.
eval/                     Authentic-message database, eval harnesses, and the
                          Appendix-C source tables the wehrmacht language is built from.
archived/                 Frozen history: every past measurement and review.
                          READ-ONLY -- see "Status & remaining work" below.
```

Languages provided: `english`, `german`, `danish`, `french`, `swedish`,
`finnish`, `icelandic`, `polish`, `spanish`, and **`wehrmacht`** (no default —
the scoring language must be given with `-l` for the n-gram models). `wehrmacht`
is telegraphic military German (X as word separator, Q for *ch*, spelled-out
numbers), generated by `eval/build_telegraphic_ngrams.py` from the published
Appendix-C statistics; it is a *writing style*, not a separate language, so use
it for real WWII traffic and `german` for prose (measured +20.9pp on real
messages, −10.2pp on prose — `eval/MODERN_BREAKING_NOTES.md` §6). N-gram files
use the format `<LETTERS> <count>` per line (e.g. `TION 13168375`) and were
sourced from the Practical Cryptography website. Non-English letters (`Ä Å Ö Ñ Ø
Æ Ð Þ Ł Ą Ć Ę Ń Ś Ź Ż` etc.) are folded to a base A-Z letter at load time by
`fold_codepoint()` in `src/ngrams.cc` (diacritics stripped, e.g. `Ł`→`L`;
Icelandic `Þ`→`T`, pairing with `Ð`→`D` as the voiceless/voiced dental-fricative
counterpart) — this covers every accented letter appearing in the bundled
tables, verified by loading each with zero "non-mappable character" warnings
(`enigma -q -l <lang> ...`).

## Build & run

```sh
make                      # g++ -std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual
                          #     -Wshadow -Wold-style-cast
                          #     -Wmissing-declarations -O3 -pthread
make test                 # build, then run tests/run_tests.sh
make bench                # build, then run tests/bench.sh (performance)
make crackquality         # build, then run tests/crack_quality.py (cracking quality)
./enigma -h               # help / usage
```

`make bench` (`tests/bench.sh`) benchmarks the hot paths **separately** —
`search` (brute-force scan, no plugboard), `hillclimb` (the `-c` plugboard loop
under `-q`), **`fused`** (the same climb under `-f`, the recommended model) and
**`crib`** (a `--crib` sweep) — because a change can regress one without
touching the others. The last two exist because coverage gaps are where the
wins hide: `hillclimb` runs `-q`, which computes no index of coincidence, so it
cannot see anything inside `ngram_ic_decode` — and that function is **91.9% of
a `-f -c` run**, where a 10.5% item sat unnoticed until the three regimes were
profiled separately (PR #152). `fused` differs from `hillclimb` only in the
model, so a delta between the two rows is the scorer and nothing else;
verified by A/B-ing across the change it was built to catch, where `fused`
read −3.6/−4.1/−4.1% while `hillclimb` read +4.0/+5.3/−2.4%, i.e. noise.
`crib` is the only tier that exercises `crib_try` (63.6% of a crib sweep) at
all, and it earned its keep: a 26-letter prologue added to `crib_try` — to seed
the deduction from `-s`/`--no-plug` pins — cost **+48% quick and +51% long**
while the other three tiers stayed inside their floors. **In that tier the
PER-HYPOTHESIS FIXED COST is what matters**, not the closure: the sweep calls
`crib_try` 26 times per key and on a wrong key most hypotheses die within a few
edges, so 26 extra iterations roughly doubled the work. Anything added to the
top of `crib_try` should be behind a flag that is false in the common case
(`g_have_known_plugs`), not merely cheap per iteration.

Each has a `quick` tier (default, a few seconds) and an opt-in `long`
tier (`make bench LONG=1`, ≥15–30s each) for a stronger signal; `make bench
SCALE=1` additionally sweeps `-T` to show thread scaling. Timing is the min of
several repetitions (the per-tier benchmarks are single-threaded). The
regression guard is a
same-machine A/B: `make bench BASE=<git-ref>` builds the binary at `<git-ref>`
in a throwaway git worktree and runs both, failing if any benchmark
is >`THRESHOLD`% (default 10) slower than BASE — run this around the planned
global-state/threading refactor to confirm single-thread throughput hasn't
regressed. **CI REPORTS at the same 10%** — the `Bench` workflow sets no
`THRESHOLD` override, so there is one number and it cannot drift from this
sentence — **and BLOCKS at `FAIL_OVER=25%`.** Two levels, because one number
cannot serve both purposes: 10% is below the measured noise floor on some
tiers (see the control data below — up to ±10% on clang `hillclimb` in a
container), so a hard gate there would fail clean PRs, while 25% is 2.5× the
worst floor ever recorded here and cannot plausibly be scatter. Locally
`FAIL_OVER` defaults to `THRESHOLD`, so `make bench BASE=…` still exits
non-zero at 10% as it always did.

The split was added after a **+50% crib-sweep regression was flagged and merely
advisory** (the step used to be `continue-on-error`). The run that verified the
fix demonstrated the reason for two levels in one shot: with the regression
reintroduced on a busy box, `crib` read **+45.6%** (blocked, real) while
byte-identical `fused` read **+11.9%** (flagged only, pure noise). A single 10%
gate would have failed on the second.

**`BASE` can be a release tag, and a per-PR guard cannot see cumulative drift —
so check against the last release now and then.** `make bench BASE=v2.1.0` at
`dev` + 160 commits, long tier:

| tier | v2.1.0 | dev | |
|---|---:|---:|---:|
| `search` | 21.43 s | 8.11 s | **−62.1%** |
| `hillclimb` | 13.17 s | 13.32 s | +1.1% |
| `fused` | 18.00 s | 16.94 s | −5.9% |

No drift — `search` matches the −60.7% the `setup_mapping`/`mod26`/`precompute`
work claimed, and the climb tiers are flat.

**Re-run after the `src/` module split** (PRs #205–#219, 9808 lines in one
translation unit becoming 19 modules), same command and same tier:

| tier | v2.1.0 | dev | | vs the row above |
|---|---:|---:|---:|---:|
| `search` | 21.93 s | 8.57 s | **−60.9%** | 1.2pp |
| `hillclimb` | 13.20 s | 13.14 s | −0.4% | 1.5pp |
| `fused` | 18.07 s | 17.12 s | −5.3% | 0.6pp |

**The split cost nothing measurable.** Every cell is within ~1.5pp of the
pre-split reading, which is inside this machine's long-tier floor — so the
−62% is fully retained across nineteen modules, cross-unit calls and ~30
symbols that became `extern`. That is the answer to the question the split
opened with, and it took a release-tag comparison to get it: the per-PR guard
saw each step against its own base and could not have seen the sum.

The `crib` tier reports `n/a — base lacks these options` in both runs, which is
the documented graceful skip rather than a failure: `--crib` postdates v2.1.0.

**Also bench against the commit where a big win LANDED, not only against the
release** — that is the better-conditioned test and answers the more useful
question. Against `v2.1.0` the −62% on `search` dominates everything, so a 5%
regression elsewhere would hide inside a large victory; against the
post-optimisation commit both sides start from the same optimised baseline, the
expectation is **0% on every tier**, and real drift shows up as a number rather
than as a slightly smaller win. `make bench LONG=1 BASE=46d4999` (the merge of
PR #151, the first commit holding all four search optimisations), `dev` 58
commits later:

| tier | quick | long |
|---|---:|---:|
| `search` | −1.9% | **+0.2%** |
| `hillclimb` | −0.7% | **+0.7%** |
| `fused` | −7.9% | **−6.0%** |
| `crib` | −8.1% | **−10.3%** |

`search` and `hillclimb` flat: the −62% has been **fully retained** across the
two-notch collapse, `--tune-phase`, `--confidence` and the live progress line.
`fused` and `crib` are genuinely faster, and what makes those look real rather
than scatter is that **both reproduce across tiers** — an 8-point move that
holds at a different problem size and repetition count is not noise. The `crib`
gain is most likely `d6743b9` (BFS-ordered `crib_try` table, claiming −12%).
No base-vs-base control was run for this pair, so treat anything under ~2% as
unresolvable; that does not touch the conclusion, since the flat tiers are flat
and the moving ones moved by four times that.

**A tier whose options postdate the base is reported `n/a`, not as a
regression.** It used to be timed at 0.00 s and
printed as `crib +13268.6% REGRESSION`, because `--crib` did not exist in
v2.1.0; `min_time()` now returns empty on a non-zero exit and the row says
`base lacks these options`. Skipped tiers are counted and reported at the end,
so partial coverage is never silent, and a **head** binary that fails is still a
hard error (`RESULT: the benchmark itself is broken`, exit 1) — that one is a
broken benchmark, not a skippable row.

> **Run the bench on an otherwise IDLE box, and re-run before believing any
> flagged cell.** A build and a test suite left running alongside `make bench
> LONG=1` produced a **`crib` reading of +1096%** (11.5 s → 137.5 s) that
> re-measured at **−7.0%** the moment the box was quiet — while `search` and
> `hillclimb` in the same contaminated run sat at +0.2% and +1.5%, so the
> damage was *not* spread evenly enough to be obvious from the shape of the
> table. A single cell blowing up while its neighbours look sane is the
> signature of interference, not of a regression that happens to be
> tier-specific.
>
> **Measure the noise floor before believing any A/B number.** Check the base
> revision out into the working tree and run `make bench BASE=<that same ref>`;
> every non-zero number is then pure measurement noise. The floors also move
> day to day — the ±0.5% `search` figure below is the best case, and a control
> on a busy container measured **±8%** on the same tier, enough to make a
> quick-tier `search` number meaningless that session. The floors are
> **per-tier and differ by an order of magnitude** — measured here as **`search`
> ±0.5%** but **`hillclimb` ±4.5%** — and they are also **per-compiler**: the
> ±0.5% `search` figure is g++, while clang's own base-vs-base control swung
> −2.2%…+0.4% across three runs on the same box, so a clang `search` move under
> ~2% says nothing. Run the control on the compiler you are judging, not just on
> one of them.
>
> **The CI matrix has now been controlled directly, and the long tier is not
> the quiet one it looks like.** PR #205 moved `enigma.cc` to `src/` and split
> the build into compile-then-link; the resulting binary is **byte-identical**
> under both compilers (`cmp`, verified for the path change too), so its Bench
> run is a free four-cell base-vs-base control on the runners themselves —
> every number below is pure noise by construction:
>
> | cell | worst quick | worst long |
> |---|---:|---:|
> | g++ x86_64 | +0.9% | +0.2% |
> | g++ arm64 | +0.2% | +0.5% |
> | clang++ arm64 | +0.4% | +0.2% |
> | **clang++ x86_64** | +1.8% | **−9.0%** (`search`) |
>
> Three cells resolve to ~1%; the fourth threw a **−9.0% on `search` long** on
> code that cannot have changed. So the per-compiler warning above understates
> it — a clang `search` move is worth nothing under ~10% *on the long tier*,
> where the g++ figure is ±0.5%, and the long tier's extra repetitions do not
> buy immunity. It also lands just under the 10% `THRESHOLD`: one more point of
> scatter and a byte-identical build would have been flagged, which is exactly
> **REPRODUCING A NUMBER ON THE SAME BOX IS NOT EVIDENCE THAT IT IS REAL.**
> The advice above — re-run before believing a flagged cell — is necessary and
> not sufficient, because a container's bias is stable *within* a session, so a
> systematic error reproduces exactly as faithfully as a systematic effect. PR
> #209 (the `machine` module split) measured `search` long at **+1.9% twice**,
> against a base-vs-base control of +1.0% on byte-identical code, and it looked
> like a small real regression. The same commit on the CI runners read **−2.2%
> (g++ x86_64)** and **+0.3% (g++ arm64)** — the two cells whose own controls
> resolve 0.2–0.5%. There was nothing there. What a repeat rules out is a
> one-off disturbance; only a *different instrument* rules out a biased one, so
> for anything under a few percent take the CI matrix as the measurement and
> the local run as a smoke test.
>
> That gap decides real cases: on
> one set of runs a `search` +5% was a genuine regression (a hot-path struct
> grown from 48 to 156 bytes) while simultaneous `hillclimb` scatter of
> +4.5%/−1.3%/+5.1% was nothing at all. Both
> readings you would reach without the control — "both ~5%, so both noise" and
> "fix the hillclimb number" — are wrong. The 10% `THRESHOLD` is a
> coarse backstop, not a resolution limit: the `search` tier resolves ~1%, so a
> sub-threshold move there can still be real, and a `hillclimb` move under ~5%
> never is. Also don't trust a single compiler — clang reported `search`
> **−16.1%** (faster) on the very build g++ showed regressed. **`min_time` wraps
> the WHOLE invocation, so process startup is inside every benchmark — a change
> to startup shows up as a fake throughput move.** The n-gram load halving
> (`archived/PERFORMANCE.md` §7.13) A/B'd as `search` **−3.4%** and `hillclimb`
> **−10.1%** without touching a line of the hot path. The tell is that both
> deltas were the *same ~60 ms absolute* (2.14→2.07 s and 0.52→0.47 s): a
> constant saving inflates the **cheaper tier far more**, which is the opposite
> shape to a real per-iteration win, and it is not something the noise-floor
> control catches (base-vs-base has identical startup). Before believing a
> cross-commit bench number, check whether the tiers moved by the same *seconds*
> or the same *percent*, and whether the commit touched anything that runs once
> per process.

`make crackquality` (`tests/crack_quality.py`) measures something different
again — **cracking quality on hard (short) messages**, not speed. For each
ciphertext length it runs many random trials (random excerpt + rotor key +
10-pair plugboard), recovers each with the true rotor key fixed and only the
plugboard hill-climbed (the cheap "plugboard-recovery" tier), and reports per
length the mean %-of-letters-correct (a graded signal) and the exact-recovery
rate, plus headline `L50`/`L90` (the shortest length reaching that recovery rate
— lower is better). **When comparing search/scoring changes, judge on the mean
%-correct, not the exact-recovery rate.** The mean is the graded, lower-variance
signal: it moves smoothly with small quality changes and separates configs at
short lengths where the exact rate is near-zero and dominated by trial noise.
The exact rate (and `L50`/`L90`) is a coarse headline — use it as a secondary
check, not the metric a tuning decision turns on. A fixed `SEED` makes the trial
set deterministic (Python's `random.Random(seed)`, reproducible across
machines), so `make crackquality BASE=<git-ref>` is a same-machine A/B that
solves identical problems with both binaries. (It was rewritten from shell+awk
to Python: awk's seeded `rand()` is not reproducible in every awk, e.g. mawk,
which silently broke the deterministic-A/B premise.) Use this — not `make bench`
— to tell whether a scoring/search change actually helps short-message cracking.
`make crackquality SPLIT=1` additionally classifies each non-recovered trial as
a **scoring** failure (the true plugboard does not score highest) or a
**search** failure (it does, but the climb stuck in a local optimum) via an
oracle run — telling you which lever to pull. (See
`archived/CODE_REVIEW_HISTORY.md` §9 for the algorithmic ideas this is meant to
measure; on the v1.1.0 baseline every miss is a *search* failure.)

> **When evaluating a *search* change, exclude the scoring-failure cases
> first.** A scoring failure — the true board is not the top-scoring one
> (operationally: a non-exact trial with `recovered_score ≥ true_score`) — is an
> **information floor of the scoring model**, unrecoverable by *any* search, so
> leaving it in the stats only adds noise that no search improvement can move.
> The current work is improving **search** (restarts, `-F`, SA, the
> `--cascade`/`--polish` finishers, tabu/GA), not scoring, so measure search
> levers on the **search-failure + exact** population only (drop the scoring
> failures via the oracle `recovered_score`/`true_score`, as `SPLIT=1`
> classifies them). Judge on that filtered mean %-correct / exact rate; a change
> that only shuffles unfixable scoring failures is not a real search win. (A
> scoring change is the opposite case and *is* measured on the full population —
> its whole job is to shrink the scoring-failure floor.)

> **For matched-compute A/Bs, measure actual wall time — do not judge cost on
> `score_iter` alone.** The `score_iter` counter counts only calls through the
> fused n-gram score loop; it does **not** count the auxiliary per-symbol work
> some search moves do outside that loop — most notably the
> `--cascade`/`--polish` **gain scan** (`gainfix_candidates` does ≈`n·26·4`
> quad8 lookups per cascade call, ≈100 `score_iter`-equivalents, uncounted). So
> on gain-cascade changes `score_iter` **undercounts real cost by several×**,
> and the two axes can *disagree*: e.g. `--polish` at a large `K` can tie a
> plain 2-ply cascade+more-`-R` on `score_iter` while being **Pareto-dominated**
> on wall time (`archived/PERFORMANCE.md` §4.11 — K=169 at 131 ms lost to R200
> at 114 ms despite fewer counted iters). Take wall time as the min of a few
> reps (per problem) to damp noise, and treat `score_iter` as the *cheap
> deterministic proxy* it is — good for `-T`-independent A/Bs of moves that live
> **inside** the score loop (restarts, climb order, caps), misleading for
> anything that adds work outside it.

`crack_quality.py` also carries three opt-in test modes from
`archived/CRACKQUALITY_TESTS.md` (all off by default, the normal flow
unchanged): `WILDCARD` wildcards the rotor key for the **scoring-failure gate**
(§1); `FILTERRECALL=1` reports the true key's `-F` tier-1 **recall@N** (§2, via
the binary's `--true-key` diagnostic flag); and `DIVERSITY=1` reports **restart
basin-collapse** stats — distinct converged optima and best-board hit-count
across the `-R` restarts (§3, via `--dump-all`). `--true-key` and `--dump-all`
are off-by-default binary diagnostics (default paths byte-identical and
bench-neutral, ASan/TSan-clean). **`--dump-all`** prints the full setting of
each converged climb (`dumpall <refl+wheels> <ring> <start> <score>
<plugboard>`), so a *wildcarded* search can be inspected key-by-key (not just a
fixed key); it reuses `showconfig`'s `format_key`/`format_plugboard`, is
display-only under a mutex (so the dumped multiset is `-T`-invariant; only line
order is thread-timing dependent), and needs `-c`.

Two further diagnostics were **removed** once their questions were settled (the
findings survive in `archived/PERFORMANCE.md` / `archived/`): `--restart-tt`, a
Zobrist transposition table over converged restart boards, measured near-total
basin diversity at the standard `--random 10` kick — which is what took a tabu
visited-set and GA population dedup off the table; and `--score-tt`, a
plugboard→score cache, measured **rejected as a speedup** (only ~7–13% of scores
cacheable, flat in restarts and table size, a net wall-time loss —
`archived/PERFORMANCE.md` §7.9). `--dump-restarts` was removed as strictly
subsumed by `--dump-all`.

The program reads **ciphertext from stdin** and writes the best-scoring
**plaintext to stdout**; progress/diagnostics go to stderr. Only A–Z letters are
kept; everything else (spaces, punctuation, case) is stripped. The n-gram files
are read from a **data directory** (filenames built as
`<datadir>/<language>_<ngram>.txt`) resolved as `-d <dir>` → `$ENIGMA_DATA` →
`ngrams` (the bundled `ngrams/` subdirectory, found when run from the repo root)
— pass `-d`/`$ENIGMA_DATA` to run from any other working directory.

### Common invocations

```sh
# Brute-force everything with the DEFAULT model, the index of coincidence
# (language-independent, no -l needed):
./enigma < cipher.txt

# Same, but score with quadgrams against the English tables (-q selects quadgrams;
# -l gives the language -- -l on its own does nothing, the default stays IC):
./enigma -q -l english < cipher.txt

# Specify some settings, wildcard the rest with '.', and hill-climb plugboard:
./enigma -u B -w 123 -r AAA -g ... -c -q -l english < cipher.txt

# Norway Enigma:
./enigma -n -c -q -l english < cipher.txt

# M4 (4-rotor naval): thin reflector b, Greek Beta, wheels III-I-VII, wildcard
# the Greek position (first char of -g) and hill-climb the plugboard:
./enigma -4 -u b -w B317 -r AAAA -g .QXP -c -q -l english < cipher.txt
```

### Key CLI options (see `help()` in `src/cli.cc` for the full list)

> **Recommended vs. not.** The proven-good knobs are `-c` + `-R` restarts, the
> **`-f` fused scoring model** (`-a`'s weighted all-order mixture plus a
> weighted index of coincidence — the strongest measured scoring model, and the
> only one that does not depend on the writing style; see the `-f` entry below),
> `-S m4f10` staging (mono pre-pass then fused; **`i4f10` on telegraphic
> traffic at operational length** — measured, see `-S`), `-J` (dynamic move
> order, wins the realistic ~10-plug regime), `-M` (with a tight cap), and the
> best-board
> finisher `--polish` (the recommended finisher: one fixed-cost pass after all
> restarts, so it is negligible at a high `-R`). Several opt-in flags are **not
> recommended** — they are dominated, ablation/measurement tools, or only
> conditionally useful, and have not been proven to strictly dominate on the
> plain short-message sweep: `-F`, `--no-repair`, `--cascade` (superseded by
> `--polish`, kept because it is the only gain cascade that works with
> `-F`/`--exhaust`), `--crib-rerank` (measured-down) and `--exhaust`. Each is
> tagged **not recommended** in its entry below and in `--help`. `--ring-stride`
> was on this list until its accuracy numbers turned out to be contaminated by a
> `--polish` guard bug — at `K=2`/`K=3` it costs only ~0.5–2pp of exact recovery
> for 1.86×/2.61× fewer keys, so it is now **recommended when throughput
> matters** (K≥5 still is not); see its entry. `--tune-phase` is **measured and
> split, and the split is length-dependent**: at matched wall time against
> spending the same compute on `-R` over the full ring enumeration it **breaks
> more messages** — 63/80 exact against 51/80 at L=200 (McNemar p = 0.043) and
> 74/80 against 65/80 at L=300 (p = 0.049) — but scores **lower mean
> %-correct** at both (85.5 vs 91.0, then 94.7 vs 98.8; both CIs span 0):
> its failures are catastrophic where the exhaustive arm's are graceful. By
> **L=450 the two arms are indistinguishable** (100.0/100.0 mean, 39/40 against
> 38/40), so the choice only matters below roughly 400 letters. Use it there
> when a break is what you want and a partial answer is worth nothing; prefer
> the exhaustive sweep when you want the run to degrade gracefully.
> `archived/PERFORMANCE.md` §7.15 for L=200, "Tuning the rotor phase" below for
> the length sweep. **Removed options** (dominated or subsumed; the
> measurements survive in `archived/PERFORMANCE.md`): `-I` (bare
> first-improvement — `-J` supersedes it; the internal climb path remains, set
> by `-J`), `--infl-order`, `--repair3`, `--gainfix-best` (superseded by the
> finisher), `--dump-restarts` (subsumed by `--dump-all`), `--restart-tt` and
> `--score-tt`.

> **Search playbook — the measured priority (this is the whole game for
> search).** `-R` restarts are the **primary quality lever**; spend compute
> there first, via `-T` (which parallelises the `keys × restarts` space, so more
> cores buy more R at the same wall time). `--polish` is a **small bump on top**
> — turn it on and leave it on — but it is **not a substitute for restarts**: at
> matched wall time *every* finisher variant tried (the explicit cascade,
> `--polish`, higher-K, the depth-1 `1sac` and depth-3/4-ply probes) is
> **Pareto-dominated by more `-R`**, and the finisher's edge **fades as R
> grows** (restarts subsume the near-solution boards it targets — measured: its
> lift roughly halves R80→R160 and can vanish by R160). The reason is that the
> hard residual is **wrong-basin** failures — the climb converged somewhere not
> near the truth — which only a fresh *restart* can *land* near; local plug
> repair (any finisher) can complete an already-near board but cannot relocate a
> wrong basin. Raising R helps until the **scoring-failure floor** (boards where
> the true plugboard does not score highest — an information limit no search can
> cross; ~5% at L40); past that only a **sharper scoring model** moves the
> needle, not more search — which is exactly what the **`-a` weighted model**
> delivers (the first measured short-message *scoring* gain, +~1–2pp mean
> %-correct at L40–100 across all four languages; PR #106). Recipe: `-c -J
> --polish --score m4f10 --random 10 -R <as high as -T affords> -f -l <lang> -T
> <cores>` — swapping `m4f10` for **`i4f10` on telegraphic traffic at
> operational length** (+2.8pp, measured; see the `-S` entry).

- `-u X` reflector A/B/C or `.` wildcard (`N` forced by `-n`)
- `-w XYZ` wheels (digits, or `.` per position to brute-force)
- `-x N` highest wheel number to consider when wildcarding (default 5)
- `-n` Norway Enigma mode
- `-4` M4 (4-rotor) mode: `-u` selects thin reflector `b`/`c`; `-w`/`-r`/`-g`
  take **four** characters with the Greek wheel (`B`=Beta/`G`=Gamma) / ring /
  start first
- `-r XYZ` / `-g XYZ` ring / start positions (letters or `.`)
- `--ring-stride K` sparse ring sampling for the rightmost wheel (K=1..26,
  default 1 = off; needs both `-r` and `-g` to wildcard the rightmost wheel's
  position, else there is nothing to thin out; incompatible with
  `-F`/`--exhaust`, the same `best.idx`-encoding fragility `--polish` has). The
  coarse pass tests only ring2 ∈ {0, K, 2K, ...}, then one refinement pass
  re-checks **every** skipped ring2 (see "Sparse ring sampling for the rightmost
  wheel" below and `archived/PERFORMANCE.md` §7.11 for the measurement and the
  implementation gotchas). **`K=2`/`K=3` are recommended when throughput
  matters; K≥5 is not.** Measured end-to-end on authentic telegraphic German
  (200 paired trials/cell, `-f -l wehrmacht`, plugboard given via `-s`, no `-c`
  — this isolates the rotor-key question the flag is about), exact recovery:

  | L | K=1 | K=2 | K=3 | K=5 |
  |---:|---:|---:|---:|---:|
  | 40 | 50.0% | 48.0% | 49.0% | 42.0% |
  | 60 | 74.5% | 74.0% | 74.0% | 72.5% |
  | keys analysed | 1.00× | 1.86× | **2.61×** | 3.73× |

  with a stride-specific miss rate (failed where K=1 succeeded) of 2% (K=2),
  2-4% (K=3) and 5-12% (K=5). So **K=3 is the best operating point** — 2.61× for
  ~1pp — and K=5's 2-8pp is not worth its 3.73×.

  **Above 13 the cost curve is flat, so raising `K` there buys nothing.** The
  coarse set is `{v < 26 : v ≡ 0 mod K}`, which holds **two** values for every
  `K` in 13..25 (K=14 samples {0,14} and costs exactly what K=13's {0,13} costs)
  and **one** only at K=26. Measured on the bare-default keyspace: 79 092 keys
  at K=1, 42 003 at K=3, **20 709 flat across K=13..25**, 17 667 at K=26.
  Accuracy follows: at L=60 on authentic telegraphic German (120 paired trials)
  K=3 and K=13 both match the unstrided baseline (70.0% / 71.7% vs 70.0%), while
  **K=26 drops to 60.0%** — its single coarse anchor is a much worse starting
  point for the refinement. K≤13 is the whole useful range; 26 is legal but pays
  15% of cost for ~10pp of recovery.

  > ⚠️ **Every `--ring-stride` accuracy number predating the `--polish` guard
  > fix was contaminated and is roughly an order of magnitude too pessimistic.**
  > The finisher shared its enclosing `if` with the refinement and was not
  > guarded by `opt_polish`, so every strided run silently got a plugboard
  > hill-climb plus an unconditional gain cascade that the K=1 baseline never
  > got — *with no `-c` requested*, adding spurious plugs to a board supplied
  > via `-s`. The old tables read K=2 at −10pp and K=3 at −17pp; the real costs
  > are −0.5…−2pp and −0.5…−1pp. The "accuracy/throughput TRADE, not a free
  > reduction" verdict was an artefact of that bug. `archived/PERFORMANCE.md`
  > §7.11.

  The earlier "100% recoverable across 90 trials" result is *not* a
  contradiction — it measured a weaker *proximity* property (does the winner
  land within `⌊K/2⌋` of the truth?) on English prose, not exact recovery.

  **Whether the saved compute beats spending it on `-R` restarts is now
  measured: it is a wash.** At matched wall time on authentic telegraphic German
  (L=100, 10-pair board hidden, 40 paired trials/cell,
  `eval/results-ring-stride-vs-restarts.txt`), `--ring-stride K` plus the extra
  restarts its saving buys is indistinguishable from an exhaustive ring2 sweep:
  K=3 lands at **+0.2pp** mean %-correct (`-R 8`→`15`) and K=2 at **−4.2pp**,
  95% CI [−13.3, +4.9]. Read the interval before concluding more: ±9pp at n=40
  rules out a *large* effect either way and nothing smaller.

  **The trade only exists when `ring1` is wildcarded.** The refinement's cost is
  fixed — independent of `K` — and the coarse saving has to beat it. Its worst
  case is `25 × rc[1] × gc[1] × 26` keys, but **that bound is not the number to
  reason with**: the case it describes (ring1 *and* start1 both wildcarded) is
  exactly the case where the offset band applies, replacing the 26 × 26 (ring1,
  start1) pairs with 26 start1 × 5 offsets = 130. So the realistic figure is `25
  × 130 × 26` = **84 500** index keys, and the middle-wheel collapse then cuts
  what is actually *scored* to **~19 000** at L=100 — measured as 18 875 at both
  K=2 and K=3, confirming the K-independence.

  Those figures were the *enumerated* refinement's. With the derivation they
  invert: `-r AA. -g AA.` at K=2 now costs 363 keys against 676 unstrided (was
  988), and `-r A.. -g A..` 50 787 against 100 724 (was 68 987). There is no
  longer a keyspace where the stride costs more than it saves, which is why the
  warning was removed.
- `--tune-phase N` **hill-climb the rotor phase instead of enumerating it**
  (N=0..26, default 0 = off; needs `-c`, and both `-r` and `-g` wildcarding the
  middle *and* rightmost positions; rejected with `--ring-stride`, `-F`,
  `--exhaust`, `--crib` and `-A`). The middle and right wheels' **phase** (ring
  and start shifted together, so each wheel's *offset* — its whole contribution
  to the substitution — is unchanged and only the notch timing moves) stops
  being an enumerated key axis: the sweep enumerates **offsets only** (26³ per
  wheel order rather than 26⁵), and each converged plugboard climb is followed
  by a scan of all 26 × 26 phases **with that board frozen**, then a re-climb at
  the winner, alternating until neither improves. See "Tuning the rotor phase"
  below. **The order is load-bearing**: scoring a rotor key without a recovered
  board is noise (a rotor-only decrypt under a full board is ~95% scrambled —
  the same reason `-F`'s tier 1 is a climb and not a scan), so the plugboard is
  climbed first and only then frozen. An **approximation**, echoed in the
  settings as such: the scan has a capture radius of ~`0.4·L/26`, so it wants
  long messages, and `N` starting phases per wheel exist to put the worst case
  inside that radius. **Measured against the alternative use of the compute**
  (§7.15): at matched wall time it breaks more messages than an exhaustive ring
  sweep does (63/80 vs 51/80 exact, p = 0.043) but scores lower mean %-correct,
  because a wrong *offset* is unrecoverable — it fails less often and worse.
  **That holds at L=300 and is gone by L=450** — see "How the split moves with
  length" below.
- `-s AB...` fixed plugboard pairs — **held fixed during `-c`/`-A`**: the
  climb/SA never remove or rewire them (their letters are marked in
  `plug_fixed[]`, set once from `opt_steckerbrett` before the threaded search,
  and skipped by every switch/remove/ re-pair/toggle move), so `-s` supplies
  *known* plugs and the search recovers only the rest. They still seed a plain
  (no-climb) decrypt as before.
- `--no-plug LETTERS` letters **known to carry no cable** (needs `-c`; off by
  default). The other half of what `-s` expresses — `-s` says "these two are
  plugged to each other", `--no-plug` says "this one is plugged to nothing" —
  and until now the only way to say it was to invent a fake pair. Both end up in
  the same `plug_fixed[]` mark, so the climb, the SA toggle, the re-pair and the
  `--random` kick all skip the letter; the difference is only the board they
  start from (`-s` pairs its letters, `--no-plug` leaves its letters
  self-steckered). **The kick needed the extra guard**: it draws from
  self-steckered letters, which is exactly what a `--no-plug` letter looks like,
  so `perturb_steckerbrett()` tests `plug_fixed[]` as well as `steckerbrett[i]
  == i`. `--exhaust` excludes them from its forced pairs and from its
  free-letter bound the same way. A cable has two ends, so a marked letter
  removes **25 of the 325** candidate toggles, not one. Fatal on a letter `-s`
  also plugs (the two statements contradict), a repeated letter, a non-letter,
  or no `-c`. `-T`-deterministic.
- `--soft-plug AB...` plugboard pairs **guessed rather than known** (needs `-c`;
  off by default). Same shape as `-s`, opposite contract: the pairs are laid on
  the board each restart starts from and then left **free** — the climb may
  move, merge or remove them like any other plug. `-s` says *known* and marks
  `plug_fixed[]`; `--soft-plug` says *good guess* and marks nothing.
  - **The two failure modes are not comparable, which is the whole reason the
    option exists.** A wrong `-s` pin cannot be undone by anything downstream
    (the pins deliberately survive `--polish`), so a bad guess poisons the run;
    a wrong `--soft-plug` guess costs only the moves the climb spends walking
    back out of it. That is the trade a **deduced** seed needs — one that is
    right most of the time but not always, such as the terminal-signature
    self-crib (`ENHANCEMENTS.md` item 5), where pinning measured **worse than
    not seeding at all** on the ~28% of messages whose ranking picks the wrong
    seed.
  - **The kick needed no change, and that is not luck.**
    `perturb_steckerbrett()` draws only from *self-steckered* letters, so a
    soft-seeded pair is invisible to it: the kick adds pairs among the letters
    the seed left alone rather than scattering the seed. Seeding therefore
    happens **before** the kick, and every restart starts from seed + its own
    kick.
  - **The kick size barely matters, and an earlier claim here was WRONG.** This
    entry used to say a soft-seeded board wants a much smaller kick than the
    default, citing 72.7 → **79.0** mean %-correct at `--random 3`. That was
    measured on 60 trials and does not survive 300: the real spread across
    `--random` 10/5/3/2/1 is 73.0 / 74.3 / 74.6 / 74.1 / 74.1, i.e. ~1.6pp
    end to end, and on exact recovery `r3` against `r10` is 188 against 183
    of 300 (McNemar p = 0.40 — nothing). **No kick at all** (`-R 0`, one climb
    straight from the seed, since `--random 0` would make every restart
    identical) costs ~2pp of mean for **35% less compute** — 11 409
    `score_iter` against ~17 000 — so it is the cheapest sensible setting.
    Dropping the staged IC pre-pass (`--score f10`) is near-neutral too. None
    of these knobs rescues the option; see below.
  - Fatal on an odd number of letters, a repeated letter, a non-letter, no `-c`,
    a letter `-s` also pins or `--no-plug` also marks (each a contradiction),
    and on `--exhaust`/`--crib`/`--crib-list`/`-A`, which all install their own
    starting board at their own site. `-T`-deterministic.
- `--self-crib-seeds K` / `--self-crib-length L` / `--self-crib-signature`
  **self-crib seeding from a doubled word** (needs `-c`; `K = 0` = off, `L`
  default 6). Beats `-R` at matched compute **at ~100 letters with a 7+
  doubling, and not at 167** — see "When it pays" below; the unqualified claim
  that used to sit here was measured only at the shorter length. A doubled word
  is a *self*-crib: it says only that two positions
  carry the **same** plaintext letter, which cancels out of `p_i =
  steck[core_i[steck[c_i]]]` and leaves `steck[c_j] =
  core_j[core_i[steck[c_i]]]` — computable from the rotor key alone. As a
  *filter* this is worthless (0 of 160 wrong keys rejected); as a *seeder* it is
  decisive.
  - **Per key**: deduce under all 26 guesses for `steck[X]` over every
    hypothesis, keep the distinct surviving boards, rank them by the **index of
    coincidence** of their decrypt, and climb the top `K` with the deduced plugs
    pinned in `PLUG_FIXED_EX` (the same per-worker pin set `--crib`/`--exhaust`
    use). Letters deduced to carry *no* cable are pinned too — a finding, not an
    absence of one.
  - **IC is the ranking, and that is measured**: it ties the fused model
    (150/200 against 144/200 top-1) and beats every other model at p ≤ 0.005,
    while needing no language and no n-gram table.
  - **The default hypothesises the doubled word ANYWHERE;
    `--self-crib-signature` narrows it to one CLOSING the message.** That is the
    same shape as `--crib` (sweeps every alignment) and `--crib-at` (pins one) —
    the default assumes nothing and the flag adds knowledge, which is also the
    only place the word *signature* is true. Restricting is ~15× cheaper (20
    hypotheses against ~2 200) and wins only when the assumption holds: over
    every corpus message carrying a doubling anywhere it breaks **16/40**
    against the default's **26/40**, with a bare `-R 16` at 19/40.
  - **`K = 10` is the operating point.** Measured on 676-key sweeps (32 paired
    trials, the 16 messages with a 7+ doubling), `K` = 5/10/20/35/50 breaks
    21/23/23/23/24 of 32 at 1 457 / 1 650 / 2 113 / 2 838 / 3 626 µs per key,
    against `-R 16`'s **13/32 at 3 118 µs** — ten more messages at half the
    cost. Steep to `K=10`, a **plateau through `K=35`**, then one more break at
    `K=50` for +120% time. The plateau refutes the obvious reading of the recall
    curve: raising `K` lifts the best-of-`K` score of all the **wrong** keys
    too, so extra recall converts into discrimination slowly. (23 against 24 of
    32 is one trial and is not significant on its own.)
  - **`--self-crib-length` defaults to 6, and the ANYWHERE default dominates the
    signature restriction at every length.** Fixed population — all 20 corpus
    messages carrying a 4+ doubling anywhere, 40 trials, `K = 10`, 676-key
    sweeps (`eval/self_crib_length_grid.py`), exact recoveries of 40 and µs per
    key:
    | `L` | signature | | anywhere | |
    |---:|---:|---:|---:|---:|
    | 4 | 16/40 | 570 | **28/40** | 7 595 |
    | 5 | 14/40 | 495 | 27/40 | 4 212 |
    | **6** | 12/40 | 440 | **26/40** | **2 428** |
    | 7 | 11/40 | 379 | 24/40 | 1 597 |
    | 8 | 6/40 | 246 | 22/40 | 1 213 |
    | 9 | 4/40 | 202 | 19/40 | 1 065 |
    | *`-R 0`* | *5/40* | *265* | *`-R 1`* 5/40, 300 | *`-R 16`* 19/40, 2 901 |
  - **The gap widens as the floor rises** — 28/40 against 16/40 at `L=4`, 19/40
    against 4/40 at `L=9` — because a long doubling *closing* the message is
    rare (only 4 of the 66 corpus messages have an 8+ terminal one) while long
    doublings *somewhere* stay common. Every anywhere cell beats `-R 16` except
    `L=9`, which ties it at 2.7× less time; the signature restriction is **worse
    than `-R 16` at every length**, which is why it is the flag and not the
    default.
  - **Two knees.** The cheapest thing that beats `-R 16` is anywhere `L=8` —
    22/40 against 19/40 at 1 213 µs per key against 2 901, i.e. 2.4× *cheaper*
    for three more breaks. The best value overall is `L=6`, the default: 26/40
    still under `-R 16`'s per-key cost, for seven more breaks. `L=5` and `L=4`
    buy one and two more breaks for 1.7× and 3.1× the time.
  - **`-R 0` and `-R 1` are indistinguishable** (5/40 each, 18.3 against 17.0
    mean — noise at 40 trials): both are a *single* climb and the kick only
    changes which basin it starts from. Reliability tracks the doubled word's
    length because that is what sets the number of equality edges — a 6-letter
    mid-message doubling was not recovered even at `K=100` on one fixture, while
    9 and 13 letters recovered at `K=1`.
  - **`score_iter` is the WRONG axis for these flags and says the opposite.** A
    swept `K=1` run scores *fewer* plugboards than a signature-restricted one
    (5.2 M against 8.0 M) while taking 15× the wall time: its seeds are more
    constrained, so the climbs are cheap and the uncounted deduction dominates.
    Judge on wall time.
  - **Recall is perfect and that is structural**: at the true key a correct seed
    exists in 48/48 trials swept at `L≥7`, because sweeping enumerates a
    superset of the terminal alignments. Requiring the seed to pin at least one
    *cable* drops `L≥9` to 32/33 — that trial is `ROMANOVKA`, flanked by `N` and
    `G` rather than `X`, so its only true variant is the separator alone, one
    anchor edge, and on that key the closure proved four letters unplugged
    without forcing a pair.
  - **`-R 0` is right and the kick should stay off**: a seeded climb starts near
    the answer, and `-R 0` measured 201 of 204 exact recoveries at half the
    compute of `-R 8`. `-R N` still asks for N kicked passes.
  - **`--self-crib-tandem` adds the doubled word with NO separator**
    (`SIEGFRIEDSIEGFRIED`), which the default cannot see at all: the 26 guesses
    are on `steck[X]`, and the separator anchor is what carries that guess into
    the message, so a tandem repeat forms no hypothesis. **Opt-in on cost, not
    on whether it works.** Recall barely moves — a correct hypothesis exists in
    195 of 200 trials against the separated case's 197 — but gap 0 has as many
    alignments as gap 1, so enumerating both roughly **doubles the hypothesis
    count** (+101% over the corpus). Per-key cost tracks that count almost
    linearly (2196 hypotheses ↔ 2428 µs, 1328 ↔ 1065), so on by default it
    would take the seeder to ~4900 µs per key — past the 2901 µs of the `-R 16`
    baseline it is measured against, i.e. it would cost the feature its
    headline. What it buys is **3 of 66 corpus messages, +4.5pp**
    (`SIEGFRIED`, `OSTROW`, `ROSENOW`). Four messages carry a tandem doubling,
    but one of them also carries a separated `ZANDERS`, so the default already
    seeds it and the flag can add nothing there — the payoff population is the
    **tandem-only** three.
  - **Measured end to end, and on those three messages it is decisive.** 60
    paired 676-key sweeps per pool, board hidden, `--self-crib-seeds 10`,
    `-R 0`, the arms differing only by the flag
    (`eval/selfcrib_tandem_ab.py`, `eval/results-selfcrib-tandem.txt`):

    | pool | off | on | discordant | wall |
    |---|---:|---:|---|---:|
    | tandem-only | 3/60 | **22/60** | 19 only-on, 0 only-off | 2.64× |
    | separated-only | 38/60 | 36/60 | 0 only-on, 2 only-off | 2.60× |

    The payoff arm is `p = 3.8e-6` (McNemar), +31.7pp with a 95% CI of
    [+19.8, +43.5]. The risk arm — where every tandem hypothesis is wrong by
    construction and competes for the same `K` seed slots — shows **no
    measurable loss** (p = 0.5, −3.3pp, CI [−7.9, +1.2]), though the sign is
    the one crowding-out predicts and the interval rules out only a *large*
    loss. **Corpus-weighted that is ~+0.6pp for 2.6× the time**, which is the
    arithmetic that keeps it opt-in.
  - **The cost is 2.6× wall while plugboards SCORED go DOWN** (2.42 M → 2.31 M)
    — the `score_iter`-is-the-wrong-axis note again, in its sharpest form yet:
    the whole added cost is the uncounted deduction, and the counter moves the
    other way because more-constrained seeds make the climbs cheaper.
  - **A tandem repeat is not anchorless, and that is what makes it usable.** It
    has no separator but nearly always has an X *before* it — 4 of 4 in the
    corpus, matching the 96% left-flank rate for the separated case — so the
    left flank is asserted instead and the closure runs unchanged. That
    recovers most of the sharpness: top-5 168 → **182** of 200, level with a
    separated word whose own anchor is withheld (192 fully anchored). A
    hypothesis with no anchor at all would deduce nothing, which is why the
    left flank is always asserted at gap 0 and only the right one is a variant.
    Refused with `--self-crib-signature`, which says the copies *are* separated
    by an X closing the message — a contradiction, not a narrowing.
  - **WHEN IT PAYS — length-dependent, and the failure mode is severe.**
    Synthetic telegraphic German with the doubling length controlled and the
    message length fixed, 40 paired trials per cell, 676-key sweeps, 10-pair
    board hidden (`eval/selfcrib_vs_restarts.py`,
    `eval/make_doubling_messages.py`). Exact recoveries of 40:

    | doubling | | L=100 | | | | L=167 | |
    |---:|---:|---:|---:|---:|---:|---:|---:|
    | | seeder | `R16` | `R128` | | seeder | `R16` | `R128` |
    | 4 | **1** | 4 | 19 | | **1** | 30 | 39 |
    | 5 | **0** | 6 | 16 | | **1** | 30 | 36 |
    | 6 | 15 | 13 | 24 | | 19 | 27 | 36 |
    | 7 | 15 | 8 | 15 | | 28 | 29 | 37 |
    | 9 | **24** | 11 | 21 | | 32 | 33 | 40 |
    | 13 | **33** | 11 | 22 | | 38 | 34 | 39 |

    At **L=100** the seeder ties or beats `-R 128` from a 7-letter doubling up,
    at 1.05 s against 9.8 s — **~9× cheaper**, and cheaper than `-R 16`
    (McNemar vs `R16`: 22-vs-0 at doubling 13, p = 0.000; 16-vs-3 at 9,
    p = 0.004). At **L=167 it is not significantly ahead in any bucket**,
    because `-R 128` already reaches 36–40 of 40 and there is no headroom left
    to convert. So the operative question is **"is `-R 128` still failing at
    this length?"**, not "does the message have a doubling?".
  - **A doubling SHORTER than `--self-crib-length` is a near-total loss, not a
    wash**: 1/40 and 0/40 at doubling 4 and 5 at L=100, against `-R 128`'s 19
    and 16 (p = 0.000). The seeder hypothesises doublings of the configured
    length *anywhere* whether or not one exists — 3520 hypotheses on such a
    message — pins the deduced (wrong) plugs, and those pins survive the climb.
    It does **not** degrade to an ordinary climb.
  - **So do not use it blind**: only ~27% of the 66 authentic messages carry a
    6+ doubling (30.3% at 4+, 24.2% at 7+, 9.1% at 10+) and nothing tells you
    which yours is beforehand. Exploit the cost asymmetry instead — **run the
    seeder first, then fall back to `-R`**. At 1.05 s against 9.8 s that is
    ~11% over the restart run alone and takes the union, which beats either arm
    at every doubling length (33 vs 22 at doubling 13; 19 vs 1 at 4).
  - Rejected with `--crib`/`--crib-list`, `--exhaust`, `-A`, `--soft-plug`, `-F`
    and `--tune-phase` — each installs its own starting board or moves the key
    the deduction was computed for. `-T`-deterministic; the dedupe key is the
    (board, pinned-letter set) PAIR, since two hypotheses can agree on the
    cables while one additionally proves a letter carries none.
- `-c` hill-climb the plugboard. The climb rule is **steepest ascent** by
  default: each step scans the whole 325-pair toggle operator and applies the
  single best improving move, to convergence. `-J` swaps that rule for
  first-improvement (below); every other climb option (`-R`, `-S`, `-M`,
  `--random`, `--polish`, …) needs `-c`.
- `-J` **first-improvement climb with dynamic best-first move ordering** (needs
  `-c`; off by default). Instead of steepest ascent (full-scan all 325 toggles,
  take the single best), it applies the **first** improving move and sweeps the
  move list circularly, ~2.8× cheaper per climb. Each climb first scores *every*
  move once against its starting (perturbed) board, sorts, and runs the circular
  first-improvement in that order — the order is rebuilt **per restart**, so it
  front-loads good moves *without* collapsing the restart diversity that a
  *static* (fixed-across-restarts) informed order destroys. Costs +24%
  `score_iter`/climb (the extra scan), so it is compared at matched compute.
  Measured a **robust win on the realistic ~10-plug regime** (+2–6pp mean
  %-correct at L40–60, two seeds) and a **loss at 6 plugs** (best-first
  over-commits when few plugs are truly needed), hence opt-in. 10 plugs is the
  `crackquality` default and standard Wehrmacht, so the win lands on the
  hard/realistic case. The 6-plug loss is over-plugging: capping at the true
  count (`-J -S iKqK`, the same known-plug-count prior as `-A -S qK`) turns it
  into a **+~30pp win vs uncapped** at matched compute — so the recipe is
  count-dependent (`~10 plugs → -J` uncapped; `known-few → -J --score iKqK`).
  Static frequency-ordering was measured and **rejected**
  (`archived/PERFORMANCE.md` §7.2).
- `-M` **cap-as-target** climb rule (needs `-c`; off by default). Changes what
  the plug cap means during the climb: by default the cap is only a *growth
  ceiling* (at/over the cap, a brand-new **add** is blocked but count-preserving
  reshuffles are allowed), so an over-cap board — the common case when a big
  `--random` kick lands on a small stage cap — can converge still holding more
  plugs than the cap, merely reshuffled. `-M` makes the cap a strict **descent
  target**: at/over the cap only **count-reducing** moves are allowed (**merge**
  — both ends already plugged to different partners → −1 — and **remove**),
  blocking adds *and* count-preserving endpoint-moves, so the climb must shed
  plugs down to the cap while keeping the strongest descent move (the merge).
  Measured a **matched-compute win that grows as the true plug count falls below
  the cap**: neutral-to -**+2.6pp** mean %-correct on realistic 10-plug boards
  (`--score i4q10 --random N`, best at the true-count kick `N≈10`), and
  **+3…+20pp** on **known-few-plug** boards (`--score i4q6 --random N`, largest
  at the short/hard end — +20pp at L40) — because forcing a clean descent stops
  the baseline wasting its climb reshuffling an over-cap board. It is also
  **cheaper per climb** (up to ~2.7× fewer `score_iter` in the `q6` regime: quad
  converges from a tidy ≤cap basin), so at matched compute it earns many more
  restarts. Most useful with a **tight `--score` target cap**; near-inert
  (harmless) with no cap set. `-T`-deterministic. (The reason the IC-pre-pass
  cap in `--score i4q…` is a *flat plateau* by default is that without `-M` the
  cap can't pull an over-cap board down; `-M` is what makes a tight cap bite —
  see `archived/PERFORMANCE.md` §7.3.)
- `--no-repair` **disable the default 2-plug `try_repair` barrier cross** (**not
  recommended** — ablation/measurement flag; needs `-c`; off by default). An
  ablation/measurement flag: the 2-plug re-pair is normally always-on (it earns
  its keep at long lengths — `archived/CODE_REVIEW_HISTORY.md` §9 item 7), and
  this turns it off so its value can be A/B'd (e.g. at short lengths where its
  convergence scan is a larger fraction of a fast climb). Default off keeps the
  climb byte-identical; the flag only skips the `try_repair` call at each
  convergence.
- `--cascade[=GATE]` **quadgram-gain directed-repair cascade** (**not
  recommended** — prefer `--polish` on the plain sweep; kept as the
  `-F`/`--exhaust`-compatible variant; needs `-c`; quad-only; off by default).
  At each quad convergence, uses per-position quad **gain** to propose plug
  corrections on *both* plugboard contacts — the exit re-plug `{S[pt[j]], bx}`
  and the reciprocal entry re-plug `{ct[j], core_j(S[bx])}` (machine-exact via
  the precomputed rotor core; self-encryption pruned since Enigma never maps a
  letter to itself) — ranks them by the full re-decode score, and runs a **2-ply
  cascade**: apply the best plug *even if downhill* (which un-masks a masked
  second plug), keep the pair only if the net beats the converged score, then
  let the cheap climb resume and finish it (the reclimb amplification is free
  from the `do/while` loop). **Gated** by a near-solution per-symbol score
  threshold (`GATE`, default `-4.9` English-quad-calibrated; tune per language)
  so it fires only on promising boards and skips the ~76% junk — which is what
  makes it a small **matched-compute win** (+0.2–0.3pp mean / +0.5–0.6pp exact
  on short English, ~zero added `score_iter`); *ungated it is dominated*.
  Default off (baseline byte-identical); `-T`-deterministic; `template<bool
  EX>`/`plug_fixed` like `try_repair`. See `archived/PERFORMANCE.md` §4.10.
- `--polish` **best-board finisher with a deeper 3-plug-tangle escalation**
  (**recommended** — the finisher, a fixed-cost pass so it is negligible at a
  high `-R`; needs `-c`; mutually exclusive with `--cascade`; off by default).
  Runs the gain cascade **once, unconditionally (no score gate)**, on the single
  best board after all restarts (its key + stecker recorded at the merge), then
  one finishing climb; a fixed ~6.5k `score_iter` independent of `-R` *and* of
  message length (measured L40–L190, `-R` 160/640: a flat ~6500, i.e. 2.8–3.3%
  of the run at `-R 160` but only ~0.7% at `-R 640`), so it is ~free at high
  `-R` and NOT free at low `-R`. Note `score_iter` undercounts it — the gain
  scan runs outside the counted loop (see the matched-compute note above). The
  once-only best-board finisher also runs a **"sacrifice + reclimb"** step when
  the 2-ply cascade finds nothing: it ranks the `(plug1,plug2)` sacrifice pairs
  by 2-plug score and, for the top-`K` (K=8), commits the sacrifice (both plugs,
  possibly downhill) and runs a full plain reclimb — letting the ordinary climb
  find the completing plug(s) and shed spurious ones — keeping the best. No
  explicit plug3 search (the completing plug is the top move the reclimb finds
  anyway; a full climb per sacrifice recovers *more* than committing one fixed
  plug — it matches the explicit 3-ply at K=6 and beats it at K=12). Targets
  3-plug tangles the 2-ply pair can't cross: real-tool capped, it solves **56%
  of fixable ≥80%-base boards vs the 2-ply cascade's 41%**, and beats 2-ply by
  **+~1.3pp mean / +2…5pp exact** at matched compute (english+german L40), for
  ~+2–3 ms wall (<1% for `-R`≥640). Simple sweep only. `-T`-deterministic
  (internal reclimb reuses `hillclimb` with the cascade off — no recursion). See
  `archived/PERFORMANCE.md` §4.11.
- `-A N` recover the plugboard by **simulated annealing** instead of the greedy
  climb (needs `-c`; `0` = off, use the greedy climb). `N` is the move budget —
  SA's cost/quality knob, the analogue of `-R`. One geometric cool-down per key:
  an IC pre-pass seeds the board (mirrors `-S iq`), acceptance-ratio calibration
  sets the temperature from a warm-up sample (`anneal_once()`), the walk accepts
  worsening `toggle-connect` moves with probability `exp(Δ/T)`, and a final
  greedy quench lands on a local optimum. `χ0 = 0.12` (a *cool* start) was tuned
  by a quality-per-climb-time sweep — the surface is greedy-friendly, so a
  mostly-downhill walk with occasional escapes matches or beats greedy `-R
  --score iq` at equal compute (the guessed `χ0 = 0.8` lost ~2×; reheating and
  chain-length sweeps didn't help). All randomness is from the per-key RNG
  stream (same `opt_seed + key_index` mix as `-R`), so `-A` is `-T`-independent.
  It composes with `-R` (each restart is an independent SA trajectory) and `-F`
  (SA runs in tier 2). SA is a *peer* of the greedy restart climb **on prose**,
  not a strict win — see `archived/CODE_REVIEW_HISTORY.md` §9 item 5 and
  `archived/SIMULATED_ANNEALING.md` §15. **That parity depends on the writing
  style: on telegraphic traffic (`-l wehrmacht`) greedy wins outright** — every
  length in L50–90, −10.4pp mean over 3000 paired trials, no crossover
  (`archived/PERFORMANCE.md` §3.11). Part of that is structural: SA reads only
  the *last* `--score` stage's cap and seeds itself with a built-in IC pre-pass,
  so it cannot use the mono pre-pass greedy benefits from. **SA honours the
  `--score` target-stage plug cap** (`opt_stages[last].cap`): `-A N --score qK`
  caps the whole trajectory (IC pre-pass, the cap-aware `apply_toggle`, and the
  quench) at `K` pairs. When the true plug count is known and below 13, that
  prior is a *measured win on short messages at modest budgets* (it stops SA
  adding spurious plugs a noisy short-message quad score would reward), neutral
  once the message/budget is large enough to recover the board unaided, and a
  loss if set below the true count — `archived/SIMULATED_ANNEALING.md` §16. With
  no `--score` the cap defaults to uncapped (13), so plain `-A` is unchanged.
- `-R N` / `--restarts N` plugboard hill-climb random restarts (**REDESIGN Part
  B, Option A — kicked-only**): `-R 0` (**the default**) is one deterministic
  climb from the seed, **no kick** (fully reproducible); `-R N` (N≥1) is exactly
  **N kicked climbs** (each from the seed plus a fresh `--random` kick, best
  kept) — the un-kicked seed climb is *not* additionally run. Per-restart RNG
  seeded from `opt_seed + flat key index + restart`, so each climb is an
  independent unit and the result stays independent of `-T`. ~`N`× the `-c`
  cost. The restart count is separate from the schedule string (`--score`). A
  non-fatal **pigeonhole warning** fires when `-R N` exceeds the distinct
  `K`-pair kicks among the free letters (they must then repeat).
- `--random K` (long-only) **kick size**: `K` random plug pairs injected per
  restart (0–13; default **10**, near the typical plug count). `--random 0` is a
  legal control (no perturbation — N restarts then repeat the seed climb). Needs
  `-c` (errors otherwise, since a kick does nothing in a bare rotor scan).
  Replaces the old `-S rN` token.
- `--exhaust E` (long-only) **partial plugboard exhaustion** (**not
  recommended** — measured, dominated; §3.6 in `archived/PERFORMANCE.md`): force
  `E` **extra** plug pairs among the free letters (on top of any `-s` pairs) —
  `E` counts *forced* pairs, not total. It tries *every* set of `E` disjoint
  pairs (pinned like `-s`), runs the staged climb from that seed, and keeps the
  best. It **composes** with the kick and restarts (fixing the earlier silent
  no-op): for each forced combo, `-R N` runs N kicked climbs (the kick perturbs
  only the still-free letters). `--exhaust 1` (no `-s`) = the 325 first pairs;
  larger `E` explodes as `free!/(2^E E! (free−2E)!)` (~45k for 2, ~3.5M for 3).
  **Parallel** (REDESIGN Part D): the *first forced pair* is the work unit —
  ≤325 units per key, spread across threads like restarts, each running its own
  sub-exhaustion × restarts against a per-worker pin set — so `--exhaust` scales
  with `-T` and stays `-T`-independent. Validation forbids `-A`, requires `-c`,
  and bounds `E` by the free plug pairs (13 − `-s` pairs). Deterministic.
  **Measured, dominated** — at matched `score_iter` a high-`-R` greedy climb
  beats it by 10–40pp exact (§3.6); an exploration tool, not recommended.
  Replaces the old `-S aN` token (note the semantics change: `aN` counted
  *total* pinned pairs, `--exhaust E` counts *forced* pairs).
- `-e N` random seed for the restart perturbation. Resolved as `-e` >
  `$ENIGMA_SEED`
  > a fresh `std::random_device` draw, so **by default every run explores
  > different
  restarts**; the chosen seed is echoed by `show_settings()` (when restarts are
  active) so a random run can be reproduced with `-e`. `opt_seed == 0`
  reproduces the historical pre-seed behaviour exactly (the RNG mixes
  `opt_seed + key_index`), which is why `tests/` and the `crackquality`/`bench`
  harnesses pin `ENIGMA_SEED=0` for deterministic, cross-ref-comparable runs.
- `-S <schedule>` / `--score <schedule>` staged plugboard climb — a string of
  `<letter><optional cap>` **model tokens** `i`/`m`/`b`/`t`/`q`/`a`/`f` parsed
  by `parse_schedule()` into `opt_stages[]`. Each is a climb stage run in order;
  the number caps the **plug pairs** that stage may set (1–13; omitted =
  uncapped, 13). The **last** model token is the target/ranking model (sets
  `opt_scoring`), so the target lives *in* the string — e.g. `--score i6q` = IC
  capped at 6 pairs, then quad uncapped. A lower-order early stage steers the
  first plugs into a better basin (its surface is smoother when few plugs are
  set); **`--score i…q` (IC pre-pass) is the best measured** for a quad target —
  much better than bigram, extra stages after IC add little.

  > ⚠️ **NEVER use the TARGET model as the pre-pass — `-S f4f10` is a trap.**
  > The syntax permits it and it reads like a reasonable thing to write ("cap
  > the target for the first four plugs, then let it run"). Measured against
  > `i4f10` at L=167, 2000 paired trials, `score_iter` matched within 3%, it
  > costs **−17.84pp** of mean %-correct (63.57 against 81.41; exact recovery
  > 1191/2000 against 1569), 95% CI [−20.0, −15.7]. That is **six times** the
  > 2.81pp the whole IC-versus-mono question is worth at that length — so
  > *which* low-order pre-pass barely matters beside *using one at all*. The
  > mechanism is §6.10's, taken to its limit: sharper models "over-commit and
  > lose at low R", and `-f` is the sharpest the tool has. **The trap is
  > invisible at short lengths** — the same swap is −0.31pp at L=60, because
  > sixty letters is too little for the n-gram half of `-f` to say anything at
  > four plugs, so `f4` degenerates to an IC pre-pass. It is severe exactly at
  > operational length. `eval/results-prepass-model-vs-cap.txt`.
  >
  > The same run prices the **cap** separately (`f10` against `f4f10`, same
  > model both sides): −4.36pp at L=167 and nothing measurable at L=60. So the
  > pre-pass's value is **in the model, not the cap, by 4×** — and note the
  > cap's own length dependence runs opposite to the naive expectation that
  > capping should matter most where the least text is available.

  **The recommended target is now `a` (weighted), staged as
  `--score m4a10`** (mono pre-pass then
  weighted, both capped) — the `a` stage reads the log-linear `all8` table, so
  `-S m4a10` is byte-identical to the winning tuning recipe. **The mono-vs-IC
  pre-pass choice depends mildly on the writing style**
  (`archived/PERFORMANCE.md` §6.10 follow-up; paired A/B, matched compute,
  n=1800 per corpus): mono wins on telegraphic traffic (−2.2pp for IC), ties on
  English prose (−1.4pp, CI spans 0), and **loses on German prose** (+2.2pp for
  IC). `m4a10` stays the general recommendation — the gap is ~2pp either way —
  but `i4a10` is the better pick for German prose specifically. **That A/B used
  `-a` as the TARGET, and against `-f` at operational length the ordering
  REVERSES**: `-f` differs from `-a` precisely by folding IC into the target
  score, so the recommendation had never been checked against the model the tool
  actually recommends. Measured on authentic HG Nord telegraphic German at
  L=167, 2000 paired trials in five independent seeds
  (`eval/prepass_ab.py`), **`i4f10` beats `m4f10` by 2.81pp** mean %-correct
  (95% CI [−4.80, −0.82], z = 2.76) and 3.1pp of exact recovery (72.2% →
  **75.2%**; McNemar over the 1800 trials with logged discordants p = 0.021).
  All five seeds favour `i4f10`, heterogeneity is Q = 1.65 on 4 df (so they
  scatter around one effect rather than disagreeing), and `score_iter` matched
  within 2% every run. **Use `-S i4f10` for telegraphic traffic at operational
  length**; `m4f10` remains the default elsewhere.

  **The full `{m4,i4} × {a,f}` square is measured at L=167** (1000 paired trials
  per cell, two seeds), and it says two things:

  | | `m4` pre-pass | `i4` pre-pass |
  |---|---:|---:|
  | **`-f` target** | 75.6 / 75.1 | **81.5 / 78.1** |
  | **`-a` target** | 69.0 / 68.5 | 75.4 / 73.7 |

  - **The target matters about twice as much as the pre-pass.** Fused over
    weighted is **+6.56pp** with a mono pre-pass (95% CI [+4.79, +8.32]) and
    **+5.20pp** with an IC one ([+3.29, +7.12]) — both above the +3.0…+4.4pp
    recorded for `-f` over `-a` below.
  - **There is NO measurable interaction.** The difference between those two
    target effects is +1.35pp, SE 1.33, 95% CI [−1.25, +3.95], **z = 1.02**. An
    earlier writeup here claimed an interaction on three cells; the fourth cell
    refutes it.

  So the IC pre-pass wins under **both** targets at this length — measured
  directly for `-a` too (`--arms m4a10 i4a10`: −6.40pp, i.e. IC ahead, McNemar
  p = 0.009). The documented "mono beats IC by 2.2pp on telegraphic" therefore
  **does not reproduce at L=167 under either target**, which points back at
  **LENGTH** (or some other difference in that original setup) rather than at an
  interaction. A single run at L=60 did lean mono under `-f` (+1.77pp, CI spans
  0), consistent with a crossover somewhere between.

  **The midpoint is now measured, and the answer is that it does not matter
  there.** Same five-seed design at **L=107** (2000 paired trials,
  `eval/prepass_ab.py --length 107`): pooled `m4f10 − i4f10` = **−1.28pp**, 95%
  CI [−3.28, +0.73], z = −1.25, exact 33.5% against 34.8% (McNemar p = 0.278),
  heterogeneity Q = 4.40 on 4 df — so the seeds agree with each other and there
  simply is no resolvable effect. Lined up by length the pre-pass effect is
  monotone and crosses zero below 107:

  | L | `m4f10 − i4f10` | reading |
  |---:|---:|---|
  | 60 | +1.77pp | leans mono, CI spans 0 |
  | 107 | **−1.28pp** | **indistinguishable**, CI spans 0 |
  | 167 | −2.81pp | IC wins, z = 2.76 |

  So `i4f10` is never behind at or above ~107 and is the safe pick across the
  operational range; the `m4f10` default only has a case below that. Note also
  that this — like
  every other tuning result here — measures the **plugboard-recovery**
  sub-problem with the rotor key given. The schedule
  carries **only** model stages: the per-restart kick and the exhaustion are
  their own options (`--random` / `--exhaust`), not schedule tokens (REDESIGN
  Part B moved the old `rN`/`aN` tokens out). Per-`machine` `scoring` field
  (never a global → race-free); deterministic; the `--score` stages, `--random`
  kick and `-R` count compose. Without `-c`, a `--score` schedule that carries
  climb-only detail (more than one stage, or any cap) emits a non-fatal warning
  and the run ranks by the target model (there is no climb to apply the stages
  to). (Replaces the earlier separate `-L` cap, which was folded into the
  per-stage numbers — see `archived/CODE_REVIEW_HISTORY.md` §9.)
- `-l lang` scoring language — **required** for `-m/-b/-t/-q/-a` (no default),
  not used by `-i`. `-l` alone does nothing: it takes effect only with an n-gram
  model, so it is `-q -l english`, not `-l english`, that scores with English
  quadgrams.
- `-i/-m/-b/-t/-q/-a/-f` scoring model: IC / mono / bi / tri / quad /
  weighted-all / fused (weighted-all + IC). **IC is the default** — the only
  model needing no `-l`, so the tool runs with no scoring options (an n-gram
  default would be inconsistent: it requires a language, which has no default).
  **`-a` (weighted) is the sharpest and the recommended model when the language
  is known** (see its entry below); quad (`-q`) is the plain single-order
  alternative. Each selector is a **thin alias for a single uncapped `--score
  <model>` stage** (REDESIGN Part C); it sets the scan **ranking** model and the
  climb **target** model. Setting the model to *conflicting* values is a **fatal
  error** — two disagreeing selectors (`-m -q`, `-q -a`) or a selector vs a
  different `--score` target (`-m --score q`) — since the intent is genuinely
  ambiguous; agreement is silent (`-q -q`, `-a --score m4a10`, `-q --score
  i4q10`).
- `-a` **weighted all-order scoring** (**recommended** when the language is
  known; needs `-l`; a schedule token too — `-S m4a10`). Scores each quadgram
  window as a **log-linear mixture** of all four orders — `a·log p(ABCD) + b·log
  p(BCD) + c·log p(CD) + d·log p(D)` with baked weights `(1, 0.6, 0.3, 0.15)`
  and the **symmetric folding** (every sub-gram a window contains, divided by
  its window-multiplicity 2/3/4, so leading edge grams are included). It is a
  **geometric (Product-of-Experts) mixture** that stays in joint log-prob space
  (weights `(1,0,0,0)` = plain quad), folded once into a quad-shaped `all8`
  table at load — so the per-character scoring **hot path and the gain cascade
  are unchanged** (they treat `all8` exactly like `quad8`). Measured the **first
  short-message scoring win** in the tuning history: +~1–2pp mean %-correct at
  L40–100 across all four languages (2000-trial German confirms +1.3pp avg, all
  lengths positive), neutral by L≥190 where quad already saturates. The linear
  (Jelinek-Mercer) form was tried and **lost** (the conditional reframing it
  forces is the cost); log-linear wins because it is *conjunctive* — a candidate
  must look plausible at every order at once. See `archived/PERFORMANCE.md` /
  PR #106. Because `-a` is the sharpest single-family model, the recipe built on
  it is `-c -S m4a10 -J --polish -a -l <lang>` -- but see `-f` below, which
  supersedes it.
- `-f` **fused: weighted all-order + index of coincidence** (**recommended**
  when the language is known; needs `-l`; a schedule token too -- `-S m4f10`).
  Takes `-a`'s `all8` table unchanged and adds `lambda * IC` to the
  **per-symbol** score, with `lambda = 30` baked in (`ENIGMA_IC_BLEND` overrides
  it). **`ENIGMA_LOGLIN` does NOT override `-a`'s weights** — an earlier version
  of this sentence said it mirrored them, and it does not: `load_table()` passes
  the baked vector as `force_ll`, and that branch ignores the environment
  entirely. The override reshapes the plain **quad** table instead, so
  `ENIGMA_LOGLIN=0,0,0,1 -q` is a monogram-shaped `-q` while `-a`/`-f` are
  byte-identical to an unset run (verified: `-a` reads −14.1491 either way while
  `-q` moves −8.9547 → −1.5214). IC cannot be folded into
  the table the way `-a`'s four orders are -- they are additive over positions,
  IC is quadratic in the whole-message letter histogram -- so it is accumulated
  in the same decode pass and added after normalisation. Measured **+3.0 to
  +4.4pp** mean %-correct over `-a` on english, german AND wehrmacht (n=1800
  each), the first scoring change in this codebase that is **not dependent on
  the writing style** -- expected, since IC is language-independent. Wall-time
  neutral (the histogram is cheap beside the gather-bound decode). **It is a
  better CLIMB, not better discrimination**: a decomposition
  (`archived/PERFORMANCE.md` 6.4) puts the whole gain in surface reshaping
  (+3.4pp) with selection contributing -0.0pp, so it does *not* move the
  scoring-failure floor. Recommended recipe: `-c -S m4f10 -J --polish -f -l
  <lang>` — but on **telegraphic traffic at operational length** use `i4f10`
  instead of `m4f10` (+2.8pp over 2000 paired trials; see `-S`).
- `--confidence N` **is the winner better than chance?** (N = null samples, 0 =
  off). A raw score answers nothing on its own: each model has a distribution on
  text with no signal, and a search reports the **maximum** over the keys it
  analysed, which drifts upward as the keyspace grows. This samples `N` keys
  uniformly from the resolved key space **before the sweep**, scores each
  **exactly as the search will**, and then **replaces the `Score` column of
  every progress line with a `Margin`** — `(s − μ)/σ − √(2 ln K)`, the distance
  above the null minus what the best of `K` keys reaches by chance. A summary
  line after the search gives the pieces behind it (μ, σ, the raw σ-count and a
  p-value). **Zero is the meaningful line**: negative means the board is no
  better than luck over the whole sweep. On real English every line before the
  true key reads negative and the winner jumps to `+17.04`; on signal-free
  ciphertext the run tops out at `+0.51`.
  - **`K` is the TOTAL key count, never keys-so-far.** That keeps the margin a
    constant offset from the score — monotone, so the merge order and the
    display high-water mark are untouched — and keeps the printed number
    independent of thread timing. A running count would make the *number* itself
    `-T`-dependent, which is worse than the existing "which lines appear".
  - **`--ring-stride` is handled, and correctly**: the sampling draws from the
    coarse space (`range.r2_vals` already holds the strided set), and `K` is the
    coarse key count — so a strided sweep gets a **lower** bar, measured 5.34σ
    against 5.53σ unstrided on the same message (1 528 334 keys against
    4 411 576). The null itself is unchanged, as it must be: stride changes
    which wrong keys are visited, not what a wrong key scores (measured
    −7.9994 ± 0.1686 unstrided against −7.9911 ± 0.1770 at K=3).
  - **The refinement's keys are NOT in the bar, and cannot be.** `g_null_zk` has
    to exist before the sweep — that is what makes the margin a constant offset
    — while `extra_keys_analysed` is not known until the refinement finishes.
    They should not be in it anyway: the refinement's candidates are chosen
    *conditional on the coarse winner*, so they are not the independent draws
    `√(2 ln K)` describes. `g_null_keys` records the `K` the bar was built from
    so the summary reports that rather than being handed a count; passing it in
    let the two drift, and under `--ring-stride` they did — the line read
    "chance best of 1528334 keys is 5.3 sd" with 5.3 computed for 1 527 084.
    Worth **0.00015σ** (zk grows as `√(ln K)`, so it barely moves; even a
    5 859-key coarse sweep with a full 1 300-key refinement reaches only
    0.048σ), so this removed a bug class below the printed precision rather than
    a visible error. The inclusive total is still reported, by the
    `Analysed N rotor combinations` diagnostic.
  - **A bare z would flatter the early lines.** A progress line is a running
    maximum, so z reads 3–5 σ well before anything is found — which looks like
    p < 1e-5 and is exactly what chance delivers over a few thousand keys.
  - **`--dump-all` keeps raw scores**, as the machine-readable form. The
    calibration suppresses it for its own climbs too: `hillclimb_one()` dumps
    unconditionally, so the samples landed in the diagnostic until that was
    fixed (16 spurious rows at `--confidence 16`).
  - **The settings echo reports `N`**, and says when the samples are climbed.
    The flag changes what the first column *means*, and the two readings differ
    by ~20 on the same run, so a log that did not say so up front could not be
    read at all by someone joining at the progress lines.
  - **The sampling shows a live `\r` progress line**, TTY-only like `-F`'s
    tier 1, because under `-c` a sample is a whole plugboard climb and `N` =
    1024 is a couple of seconds before the search prints anything. It is
    **erased** rather than left at 100% — the settings echo already gave `N`
    and the summary gives the result. Single-threaded (the workers have not
    started), so no atomic or mutex, unlike `-F`'s.
  - **A one-key space has no null, and that needed a RELATIVE guard.** With the
    rotor key fully specified every sample climbs the same key to the same
    score, so σ̂ came out as float noise (~1e-15) rather than 0 — a bare
    `sd > 0.0` test passed it, the margin became `score/1e-15` ≈ 1e13, and the
    8-wide first column blew out to **87 characters**. The test is now
    `sd > 1e-9·(|μ| + 1)`: scores are per-symbol log10 probabilities, so that
    sits nine orders below any real null (~0.17) and six above the noise. On a
    degenerate null the run says there is nothing to measure against and falls
    back to raw scores — `g_null_sd` staying 0 already routes `showconfig` and
    `showconfig_header` there. `showconfig` additionally falls back to `%+.1e`
    (exactly 8 characters) if a margin ever fails to fit, so no arithmetic
    surprise can shift the columns.
  - **Do not anchor a test on `^Confidence`.** Both the settings echo and the
    summary carry that label, and the echo's following lines include
    `Threads: N` — so the pre-existing `-T`-independence check, which captured
    `grep -A2 '^Confidence'`, started reading a thread count as a thread
    dependency the moment the echo was added. Anchor on `^Confidence: null`.

  Three more things worth knowing:
  - **Samples are climbed when `-c` is on.** A climbed key is drawn from a much
    higher distribution than a scanned one (measured −6.84 against −8.01 on
    the same ciphertext), so calibrating a climbed search against a scanned
    null would make every run look significant.
  - **And they are climbed by the SAME UNIT the search runs**, which is a
    sharper version of the same rule. `--crib` and `--self-crib-seeds` replace
    the plain climb with a deduction-seeded one, whose scores sit somewhere else
    entirely: measured on one ciphertext, the seeded null is −11.7399 ± 0.4244
    against the plain climb's −10.7938 ± 0.2505 — nearly a point lower and
    **1.7× wider**. Both the sweep and `calibrate_null()` therefore go through
    one `climb_unit()` helper so they cannot drift. **The bias this removes was
    not one-directional**, which is why it mattered: on a score of −10.4908 the
    plain null read 1.21 σ against the correct 2.94, but the two lines cross at
    s ≈ −9.43 and **above that — where a real break sits — the plain null
    overstates**. Near zero it undersold a run; far out it would have oversold
    one.
  - **A key the unit REJECTS must be dropped, not sampled.** `crib_unit()` /
    `self_crib_unit()` return `unit_no_score` (`-1e300`) when no hypothesis
    survives — a sentinel so the unit never wins the merge, not a score. It
    used to go straight into the null, and since a crib worth using rejects
    99%+ of keys, nearly every sample was `-1e300`: μ ≈ `-1e300`, the variance
    **overflowed to `+inf`**, and `(s − μ)/σ` was exactly `0.0` for every
    board — so every progress line printed the identical margin `−z_k` and the
    summary printed a 300-digit null. The search was unaffected throughout
    (the run that surfaced it recovered its plaintext), which is exactly why it
    went unnoticed. Rejected keys are not in the null the search draws from,
    since it never scores them. The attempt budget is `want·256 + 4096` rather
    than the plain path's `·64`, because a rejected draw costs only the
    deduction while an accepted one costs a climb; if too few still survive,
    the run names the crib and falls back to raw scores.
  - **`K` is still the TOTAL key count under a crib, which is conservative.**
    The best-of-`K` bar should strictly use the number of keys actually
    *scored*, which a crib cuts by ~100×; using the total overstates the bar by
    ~0.5–1.1σ, so a crib run's margin reads low. That is the safe direction for
    a "is this a find?" test, and the accepted count is not known before the
    sweep — the same reason the `--ring-stride` refinement's keys are excluded.
  - **Keys are sampled, not random text.** The null a search actually draws
    from is "this ciphertext under a wrong key", and `key_to_machine()` already
    builds exactly that in every machine mode.
  - **IC does not follow the Gaussian tail** — 6.1σ observed against 4.4
    predicted for its best-of-K, because its null is a quadratic form in the
    letter histogram rather than a sum over positions, and so right-skewed. The
    printed p-value says so under `-i`.
  - **The p-value is optimistic near zero for EVERY model, not only IC.**
    Measured on 2000 signal-free ciphertexts (L=200, K=17 576,
    `--confidence 1000`, `eval/confidence_false_positive.py`): a margin of
    **+0.54 came up 2.35% of the time against the 0.70%** the Gaussian tail
    implies — a factor of 3.4. The real null's best-of-K sits **+0.21σ above** a
    Gaussian of the same μ/σ and its upper tail is fatter:

    | percentile of the margin on pure noise | measured | Gaussian |
    |---|---:|---:|
    | 50th | −0.20 | −0.47 |
    | 95th | **+0.40** | +0.11 |
    | 99th | **+0.80** | +0.44 |

    The score is a sum over positions, so the CLT delivers the *centre* of the
    null quickly and the *tail* slowly — and a best-of-K statistic reads only
    the tail, at ~4.4σ. **The rate rises with the keyspace**: at K = 3 163 680
    the same +0.54 came up **4.83%** (29/600). So `--confidence` now flags the
    p-value as optimistic for every model, and prints an explicit
    *"below +2 sd is not a find"* note when the margin is under 2σ — the
    measured 99th percentile of noise, rounded up. **`N` does not fix this**:
    at N=1000 the estimation error is only ~0.10σ, and nearly all the spread is
    the genuine fluctuation of the best of K, which no amount of sampling
    removes. None of it matters far out, where a real break reads +15 to +17σ
    and a factor of three on 1e-98 changes nothing — which is why the note
    fires only near zero.

  It also **ranks the scoring language on a single message**, which is its
  second use: on telegraphic German the margin measured +15.4σ for `wehrmacht`,
  +8.6σ for `german` and +2.5σ for `english` — the order `CLAUDE.md` recommends,
  recovered from one ciphertext.

  **Recommended `N` = 256; 128 is the floor.** `N` buys precision in μ̂ and σ̂
  and nothing else, and too small an `N` makes the flag report the one thing it
  exists to rule out. Measured over 12 seeds per cell on a signal and a
  signal-free ciphertext (L=200, `-q -l english`, K = 17 576;
  `eval/confidence_sample_size.py` reproduces it):

  | N | noise-arm sd | worst noise margin | signal-arm sd | cost under `-c` |
  |---:|---:|---:|---:|---:|
  | 16 | 0.96σ | **+1.7σ** | 4.23σ | 0.02 s |
  | 64 | 0.56σ | **+1.2σ** | 1.77σ | 0.10 s |
  | 128 | 0.27σ | +0.4σ | 1.50σ | 0.19 s |
  | **256** | **0.17σ** | **+0.1σ** | 1.01σ | **0.38 s** |
  | 512 | 0.07σ | −0.2σ | 0.75σ | 0.77 s |
  | 1024 | 0.04σ | −0.3σ | 0.37σ | 1.5 s |

  **The noise column is what decides it**: at `N` ≤ 64 a ciphertext with random
  letters behind it reports a *positive* margin on some seeds — a false
  "significant", which is worse than no answer. 128 is the first row that never
  crosses zero and 256 the first with headroom. Past 512 there is nothing left
  to buy: the residual is the null's departure from Gaussian (the −0.3σ floor
  the noise arm settles on, and IC's skew above), not sampling error.

  The spread follows `SE(margin) ≈ √((1 + z²/2)/N)` — `1/N` from μ̂, `z²/2N`
  from σ̂'s own relative error — which predicted 0.82/0.41/0.21σ at N=16/64/256
  against the 0.96/0.56/0.17σ observed. Two things fall out of that formula:

  - **`N` does not scale with the keyspace.** `K` enters only through `√(2 ln
    K)`, which is arithmetic, not sampling. The decision always sits near `z ≈
    √(2 ln K)`, so `SE ≈ 3.3/√N` whatever the sweep size.
  - **`N` does not scale with message length** either. A long message inflates
    the *signal* arm's error (z is large, so σ̂'s error is amplified — 1.0σ at
    N=256), but +16 ± 1 is no less decisive than +16 ± 0.2.

  **Cost is only ever a question under `-c`.** In scan mode the calibration is
  free — N=1024 still fits inside the ~0.05 s startup floor, since a scan
  samples one score per key. Under `-c` each sample is a plugboard climb,
  measured **1.5–1.7 ms at L=200**; that is ~1% of a real run, but it is
  **single-threaded**, so its share grows with `-T` — the one case worth
  trimming `N` for is a short climb on many cores.

  **Measuring that cost needs a pinned rotor key.** The obvious harness — time
  the same sweep at several `N` — reads *zero* for all of them: 512 calibration
  climbs disappear under a 31 s `-c` sweep of 676 keys × 4 restarts × 4 threads.
  The signal is there, it is just 2% of the total. The cost arms therefore fix
  `-g` so the search itself is one climb and the calibration is nearly the whole
  run.

  **No line of the summary may look like a progress line either** — the rule
  the pre-flight block carries, and this one broke it. The documented way to
  read a run's margin off stderr is `grep '^ *[+-][0-9]'`, and the near-zero
  note wrapped as `"… a margin of\n            +0.5 sd came up …"`, so the
  **continuation was such a line** and an extractor read the caveat back as the
  result — silently, because `+0.5` is a plausible margin. Found when a sweep
  of 33 known 1941 day keys against BYQMZ reported *"+0.5 sd came up in 2-5% of
  runs"* as the margin for every one of them. Re-wrapped so no line begins with
  a signed number, and `tests/run_tests.sh` now asserts it of the summary as it
  already did of the pre-flight lines (verified by injecting the old wording).
  The lesson generalises: **any new stderr line is a candidate for this bug**,
  and the two guards are cheap.
- `-p file` compare the recovered plaintext against a known plaintext file
- `-F N` / `-F N%` key pre-filter (**not recommended** — situational: a
  long-message throughput tool, unreliable on the short/hard end and
  proxy-measured; needs `-c`; `0` = off): a two-tier search — tier 1 ranks
  *every* key by a single **cheap IC climb** and keeps the top `N` (or top `N%`
  of the resolved keyspace); tier 2 runs the full `-R`/`-S` climb on only those.
  The big *throughput* win (~8–20× over climbing every key), so more restarts
  are affordable per surviving key. Both tiers are parallel and
  `-T`-deterministic (per-thread top-N min-heap, deterministic tie-break).
  Details worth knowing:
  - **Tier 1 is a *climb*, not a plugboard-free scan.** A raw IC *scan* fails
    (rotor-only decrypt is ~95% scrambled under a full board, ~0% top-1 recall);
    an IC *climb* partially recovers the stecker and discriminates.
  - **Tier 1 climb is capped at `filter_climb_cap = 5` plug pairs.** Capping
    both speeds tier 1 up and *improves* recall — an uncapped climb lets wrong
    keys overfit IC and bury the true key. Measured both-axes win (+~16pp
    recall, ~1.4× faster; harmless on easy keyspaces). `cap≈5` (near the true
    plug count) is the optimum.
  - **`N%` scales with the keyspace** (recall tracks the *fraction* kept, not
    the absolute count); absolute `N` bounds tier-2 cost. Both forms are
    supported.
  - **Recall is strongly length-dependent** (measured via `--true-key` /
    `FILTERRECALL=1`; `archived/CRACKQUALITY_TESTS.md` §2): on a 6-order proxy
    at 10 plugs the true key's median tier-1 rank is ~12k/105k at L120
    (effectively unrecoverable), ~27 at L200 (bimodal), and 1 at L300. So `-F`
    is sound for realistic-length traffic (~L300) and unreliable on the
    short/hard end — and the real 60-order keyspace is worse than the proxy.
  - **Chi-squared was benched as the tier-1 model and lost to IC** (χ² is
    gameable by the plugboard permutation) — IC stays. See
    `archived/CODE_REVIEW_HISTORY.md` §9 item 2.
  - **Tier 1 shows a live `\r` progress line** (`ranking NN% (done / total
    keys)`) while it ranks, but only when stderr is a terminal (`isatty`) so
    redirected logs and the tests stay clean. A shared atomic counter drives it,
    and because each atomic add owns a disjoint slice, exactly one thread prints
    each 1% step — no races, `-T`-safe.
- `--crib TEXT` / `--crib-at N` **known-plaintext key filter** (off by default;
  `archived/cribs.md` §12 step 3). A crib is a guess at part of the plaintext
  *together with where it sits*. Decryption is `p = steck[core_i[steck[c]]]` and
  the rotor core is an involution, so it rearranges to `steck[p] =
  core_i[steck[c]]` — one
  lookup on the `rows[]` table `setup_mapping()` already builds. Guess a single
  plug, chain that along every crib position, and add reciprocity (`steck[x]=y`
  ⇒ `steck[y]=x`, and no two letters share a partner — **Welchman's diagonal
  board**, free because the board is an involution). A contradiction kills the
  guess; all 26 dead means the rotor setting cannot have produced the crib, so
  the search skips it **without scoring anything** — measured 99.9% of keys on a
  12-letter crib. Runs after `setup_mapping()`, reads only `rows[]`, so it is a
  pure per-key test and `-T`-deterministic; zero cost when the option is off.
  **The diagonal board is what does the work**, not menu loops: a loop-free
  12-letter menu still rejects 88% of settings against 0% without it
  (`archived/cribs.md` §4.1). `--crib-at` is **1-based** and optional: given,
  the crib is pinned there; omitted,
  every alignment the self-encryption filter leaves is tried and a key is
  rejected only if **every** alignment rejects it. **That compounding sets the
  usable crib length.** Rejections multiply, so what matters is `∏ p_i` over the
  alignments, not `p`: measured on a 125-letter message, a 12-letter crib
  rejects 99.9% pinned but only **5.3%** swept, while 16 letters holds at 99.9%
  either way — and the product of the 70 measured per-alignment rates (63.4% to
  100%) predicts 5.2% against 5.3% observed, so the weakest alignments dominate.
  16 letters is the swept floor; below it a crib can only seed a climb
  (`archived/cribs.md` §4.2a). The crib mode is rejected against `-F`,
  `--exhaust`, `--ring-stride` and `-A`. A crib that matches the ciphertext
  anywhere is fatal — an Enigma never encrypts a letter to itself, so that
  alignment is
  impossible. `--crib-dump` prints each surviving hypothesis, its alignment and
  the plugs it deduces (diagnostic, needs `--crib`); the progress line gains an
  **`A` column** for the alignment, its width taken from the preview so the
  80-column budget holds; `eval/crib_vectors_check.py` checks those against
  `eval/crib_menu.py`'s vectors, which carry the true board — 40/40 exact. **The
  rejection count is reported per key, counted at the key's first work item**: a
  key's restarts can straddle a chunk boundary and be seen as new by two
  workers, so counting at the deduction would make the total depend on `-T`.
- **`-s` pins now CONSTRAIN the deduction, and that is worth orders of
  magnitude.** `crib_try()` starts the closure from whatever `-s` and
  `--no-plug` already fix, so a hypothesis contradicting them dies at
  `crib_set` instead of being carried through and silently overwriting the
  pinned plug at the seeding site. That overwrite was the stack smash (a
  non-involution board overflowing `format_plugboard`), but the crash was the
  cheap half of the bug: the expensive half is that every contradicting
  hypothesis used to survive and get a **full plugboard climb**. Measured on a
  90-letter message with a 12-letter swept crib over 17 576 keys, plugboards
  scored before → after, plaintext recovered in every arm:

  | `-s` pins | before | after | |
  |---|---:|---:|---:|
  | `AB` | 20 736 444 | 211 588 | 98× |
  | `AB CD` | 15 555 774 | 1 851 | 8 404× |
  | `AB CD EF` | 11 425 564 | 36 | 317 377× |
  | `AB CD EF GH IJ` | 6 057 585 | 2 | 3 028 790× |

  Key rejection goes 9.3% → **100.0%** at three pins: the deduction kills every
  wrong key and only the true one is ever climbed. So `--crib` with **any**
  known plugs is a different proposition from `--crib` alone — the two kinds of
  knowledge compound, where before they fought. The default path (no `-s`, no
  `--no-plug`) is untouched: `plug_fixed` is all false, so the seeding loop
  sets nothing.
  - **The pins are now LOAD-BEARING, which is the other half of the same
    change.** Being wrong about one used to cost nothing — the contradiction
    was silently overwritten and the search still found the key. Now it kills
    every hypothesis at every alignment, so the run ends *"Fatal error: No
    machine configuration produced a score"* (the same way a `--crib` that
    rejects every key already did). Demonstrated on a board of `AB CD EF GH IJ
    KL MN OP QR ST`: `--no-plug UVWXYZ` is true and recovers the plaintext in
    1 839 plugboards, `--no-plug XYZ` likewise in 58 063, while `--no-plug
    QWERTYU` — false, since Q, E, R and T are all plugged — now returns
    nothing where the released code returned the correct plaintext. Failing
    loudly on contradictory input is the right behaviour, but it does mean a
    guessed pin belongs in `--soft-plug`, not in `-s`.
- **The menu is walked BREADTH-FIRST FROM THE ANCHOR, not in crib order.** An
  edge deduces nothing until one endpoint is known, and at the start only the
  anchor is; in crib order the loop visits edges whose endpoints are both
  unknown, does nothing, and leaves the enclosing `while (changed)` to come back
  for them, so a long menu is re-scanned repeatedly. Visiting edges as the
  frontier reaches them makes the work track the **component** rather than
  the edge count. Measured on wrong keys — the case a sweep spends its time on —
  total edge steps for the 26 hypotheses fall 226 → 97 at a 16-letter crib and
  253 → 93 at 50 letters, because the BFS cost stays flat (~95) while crib order
  grows with length; **wall time 1.61× at 16 letters rising to 1.72× at 40**, so
  the longest cribs gain most. It is a pure **reordering**: the closure is
  order-independent, so exactly the same keys are rejected and the same plugs
  deduced — verified by `crib_vectors_check.py` (40/40) and by unchanged
  rejection counts. The order is built once per alignment in `init_crib()`, so
  nothing in the per-key deduction changes shape.
- **With `-c` the crib also SEEDS the climb** (the hybrid,
  `archived/cribs.md` §7): instead of starting from an empty board, each
  surviving hypothesis's deduced plugs are pinned in `PLUG_FIXED_EX` — the same
  per-worker pin set `--exhaust`
  uses, since `plug_fixed` is a read-only global no worker may touch — and the
  climb finds the rest. Letters the deduction settles as carrying **no** cable
  are pinned too (that is a finding, not an absence of one, and it stops the
  climb spending moves on them). The pins stay fixed through `--polish`: a
  deduced plug is arithmetic on the machine equation and the cascade is
  score-driven local repair, so releasing them would let weaker evidence
  overwrite stronger, and a wrong hypothesis needs no rescue — it loses on score
  to the other 25. Measured on an 88-letter message with the board hidden and a
  12-letter crib: **92% of letters recovered against 8% unseeded** (10% at
  `-R 64`), and the same 92% swept as pinned, so seeding does not need the
  alignment to be known. **That was measured with the rotor key given**, one
  key; with the key unknown the sweep is not free — see `--crib-list`.
- `--crib-list FILE` **a whole crib library**, one crib per line (`#` comments,
  duplicates dropped, **file order preserved** — the generator emits
  most-likely-to-match first and re-sorting would throw away the early exit).
  Runs **one complete rotor sweep per crib** — crib-outer, because the shared
  `setup_mapping`/`precompute` a rotor-outer loop would save is 0.6% of the run
  while early exit is worth up to 50× (`archived/cribs.md` §6.7) — and keeps
  the best board across all of them. Three things that are fatal for a single
  `--crib` merely skip the crib here: longer than the ciphertext, matching the
  ciphertext
  at every alignment, or rejecting every key. A library is written against a
  network's vocabulary, not one message, so most of its cribs not fitting is the
  normal case. Rejected with `--crib` and with `--crib-at` (which pins *one*
  alignment, and the cribs differ in length). The progress-line column header is
  printed once for the run, and the echo high-water mark carries across cribs so
  a later crib cannot re-print boards worse than the best already found.
- `--crib-seeds K` **IC-rank the crib's surviving hypotheses and climb only the
  best `K`** (needs `-c`; `0` = off, the historical climb-every-survivor path,
  kept byte-identical). The same lever as `--self-crib-seeds`, and it applies
  because `crib_unit()` had the same shape: a swept crib leaves a *set* of
  surviving (alignment, hypothesis) pairs and every one used to get a full
  plugboard climb.
  - **Why there is anything to rank.** Survivors per key at the true key:
    **438.6 at an 8-letter crib, 90.7 at 10, 8.3 at 12, 1.5 at 14**. Long cribs
    reject nearly everything and leave nothing to cut; short ones leave hundreds
    of climbs per key, which is exactly what puts them beyond reach — the swept
    floor is documented as 16 letters.
  - **Why IC works.** A correct hypothesis pins several correct plugs, and that
    lifts the index of coincidence of its decrypt before any climbing. No
    language and no n-gram table needed, the same reason `--self-crib-seeds`
    ranks on it.
  - **The window is narrow and bounded on BOTH sides** (`eval/crib_ic_rank.py`,
    40 trials/length, true key): at 12+ ranking is perfect and pointless, at 8
    the top 10 keeps only 57% of correct hypotheses, and only at ~10 letters are
    both true at once — 91 survivors, top-10 keeps 92.5%.
  - **Measured on the sweep, which is the number that decides it**
    (`eval/crib_seeds_ab.py`, 20 trials, 10-letter crib, board hidden, 676-key
    sweep): `K=10` recovers **19/20 against the unseeded run's 19/20 with zero
    discordant trials, for 12.1× fewer plugboards**. `K=3` gives up 3 breaks for
    43.6× and `K=1` gives up 4 for 138×. **Use `K=10`** — the same operating
    point `--self-crib-seeds` settled on, reached independently.
  - `-T`-deterministic; the dedupe key is the (board, pinned-letter-set)
    **pair**, as in `--self-crib-seeds`, since two hypotheses can agree on
    every cable
    while one additionally proves a letter carries none.
- `--no-crib-reorder` **keep a `--crib-list` in file order** (off by default,
  i.e. cribs run cheapest-measured-cost first). Reverses `archived/cribs.md`
  §5 step 5, which priced cribs with
  `build_cribs.py`'s *modelled* cost — flat-ish by length. The measured curve is
  a **cliff**: relative to a no-crib sweep, 8 letters costs **52×**, 12 costs
  0.67×, 25 costs 0.02× — a ~2 600× spread against the model's 13×, with the
  16-letter point cross-checked on a 5× larger key space (0.074× vs 0.085×).
  Since how often a crib is *present* spans only ~26× (§4.2), the cost term
  dominates: the whole long tail of a library costs less than one short crib.
  Ordering is a **preference, not a filter**: nothing is discarded, so the
  worst case is a later win, never a lost one. **A `--crib-max-hyps` flag that
  *discarded* costly cribs was built and removed**: cost is anti-correlated with
  the chance of a hit — short cribs are the most expensive *and* the most likely
  to be present (93% of messages carry an 8-letter crib, 3% a 20-letter one) —
  so on the shipped library it skipped all four cribs actually in the message
  and recovered nothing, where not skipping recovered it in 8 s. Reordering
  captures the throughput without that risk, which is why it replaced it.
- **The crib list reports an expected gain per crib**, printed as a table before
  each sweep: `# crib len algn hyp/key gain`. `gain` is what a key costs
  *without* the crib over what it costs *with* it — above 1 it saves work, below
  1 it costs more than using no crib at all. **Measured, not modelled**:
  `crib_unit()` and `hillclimb_one()` are both run on the same eight sampled
  keys and their plugboards-scored counters compared, so it already contains
  both opposing effects (keys rejected for free, extra climbs where they are
  not). Boards rather than wall time, so a printed number stays reproducible.
  It omits the deduction's own cost (outside the score loop), so a crib
  rejecting nearly everything is flattered — hence `>1000x` rather than a
  figure. `<` marks a crib that hit the work budget: a bound, not a measurement.
  The table is also the standing argument against acting on it automatically —
  on the shipped library the cribs actually present in the message are the ones
  scoring ~0.03–0.07×, so the column guides the reader rather than gating a
  crib.
- `--doubling-report L` / `--doubling-z Z` / `--doubling-mismatches N`
  **report a
  converged climb whose decrypt carries a doubled word**
  of `L`+ letters around an X — `ENGELMANN X
  ENGELMANN`, telegraphic German's own error correction (off by default; needs
  `-c` and `--confidence`, which is what defines z). Fires after each converged
  climb and once after `--polish`, on any key clearing **z ≥ `Z`** (default 3)
  — the raw sigma count over the null, *not* the margin the lines print.
  `--doubling-z` alone is refused, since it would silently do nothing. Prints
  the ordinary progress line with the text preview replaced by
  `>> <len> <WORD>`, so the columns stay aligned and the marker is greppable in
  an overnight log.
  - **A CONFIRMATION SIGNAL, never a score term, and that is the whole point.**
    It enters no ranking, so a false positive costs a second look and cannot
    promote a wrong key. The *score-bonus* form of the same evidence
    (`ENHANCEMENTS.md` 5(e)) was swept over 140 genuine 17 576-key sweeps and
    **measured down**: a bonus applied after the climb needs a trial where the
    climb recovered the plaintext and the score still lost, and there were
    **zero** — in 35 of 35 recovered trials the true key was already top,
    because the climb is steered by the same score the bonus would adjust.
    Reporting has no such dependency: it fires on the key that *is* right.
  - **The z gate comes first, and that is what makes it free.** Computing z is
    two flops on a value already in hand; only ~0.56% of keys clear z > 3, and
    only those pay the decode and the scan. Ordering it the other way would
    decode every converged climb. Measured `make bench BASE=origin/dev`: no
    regression, and the `crib`/`search` tiers cannot be affected at all since
    they run without `-c`.
  - **`L` is the cheap lever; `--doubling-z` is not — raise `L` first.** Chance
    reports fall ~16× per
    extra letter (the null falls by `B/A ≈ 16.4`), so a full 230 M-key rotor
    sweep expects **~6 spurious reports at `L = 7`** against ~90 at `L = 6`.
    Loosening the gate to z > 2 quadruples them; tightening to z > 4 throws the
    true key out with the chaff, since a true key whose climb has recovered the
    plaintext sits at **z = 7…16** — nowhere near the gate — while the keys
    below z = 3 are the ones whose climb failed, where there is no doubling to
    find anyway. See `ENHANCEMENTS.md` item 5(e) for the table.
  - **`--doubling-mismatches N` (default 1) is a knob you should almost never
    turn, and the numbers say why.** Measured on 2 M synthetic texts drawn from
    the climbed-wrong-key letter statistics (X-rate 2.41%, IC 0.0514 — the
    generator validates by reading **6.0e-6** at `L=6, N=1` where the
    operational null is 4.9e-6):

    | L | N | false-positive rate | vs N=1 | real doublings found (of 46) |
    |---:|---:|---:|---:|---:|
    | 6 | 0 | 0 | — | 8 |
    | 6 | **1** | **6.0e-06** | 1× | **13** |
    | 6 | 2 | 2.9e-04 | **49×** | 13 |
    | 6 | 3 | 7.3e-03 | 1212× | 14 |
    | 7 | 2 | 2.0e-05 | ~53× | 11 (same as N=1) |
    | 8 | 2 | 2.0e-06 | — | 7 (N=1 finds 6) |

    **`N=2` multiplies false reports ~50× and finds nothing extra** at `L`=6 or
    7, one more at 8 — which matches the corpus, where 18 of the 25 real
    doublings have no mismatch, 7 have exactly one, and **none has two**. `N`
    and `L` are not interchangeable: a letter divides the rate by ~16 and a
    mismatch multiplies it by ~50, so a step in `N` costs what **1.4 letters**
    buy back. If you want `N=2`, add 2 to `L` and you are back where you
    started. `N ≥ L` is refused as vacuous (every equal-length X-free pair would
    match).
  - **One SUBSTITUTION is allowed by default, and it buys exactly the error
    the channel makes.** Enigma has no diffusion, so one corrupted ciphertext
    letter damages
    exactly one plaintext letter — in one copy of the doubling and not the
    other. An **indel is a different matter and is missed by design**: a dropped
    or added letter misaligns the copies, so every position after it differs and
    `|W| ≠ |V|` besides. Real traffic does contain those — the Nr 173 form
    doubles a surname as `SCUHNACHER` (10) against `SCHUHMACHER` (11), which
    this rule does **not** catch (`ENHANCEMENTS.md` item 5(d)). Widening to
    indels would mean an edit distance and a null far thinner than the
    16×-per-letter one `L` is priced against.
  - **The scan is capped at 30 letters, and the cap is load-bearing — but it is
    set by COST, not coverage.** `W` and `V` may not contain an X, so each is a
    single X-delimited token — a *word* — and the length distribution over the
    54 authentic decrypts is known: of the 25 carrying a doubling, the longest
    per message runs 6–13 and **nothing reaches 14** (the maximum is
    `STUERZBAECHER`; the probes' own `MAXLEN` of 16 was measured to saturate).
    So 30 is 2.3× anything observed. **20 was tried and reverted**: 1.7× fewer
    passes on paper, but at the default gate only ~0.56% of keys reach the scan
    at all and the difference did not resolve against a base-vs-base control, so
    the wider cap is free insurance — and 54 messages is a thin basis for ruling
    out 14–30 outright. What the cap must not be is *absent*: without one the
    scan runs every length from `(n-1)/2` down to `L`, which is **O(n²) and
    grows with the message** (193 linear passes at 400 letters against 24, and a
    measured **+7.6%** of a run ungated at L=200 against noise with the cap in).
    A doubling *longer* than the cap is **missed rather than truncated** — a
    long repeat does not decompose into a shorter matching one, since sliding
    the window puts the copies out of alignment — so `--doubling-report` is
    validated against the same constant and a larger `L` is refused rather than
    silently searching nothing. One effect argues mildly the other way and is
    recorded for completeness: the rule tolerates one mismatch across `2L`
    letters, and 18 of the 25 real doublings have none while 7 have exactly one,
    putting the per-letter rate near 2% — at which `P(≤1 mismatch)` falls from
    90% at `L=13` to 81% at 20 and 66% at 30. It only bites at lengths that do
    not occur.
  - **A doubling is a TRANSLATION by `len+1`, not a reflection.** `W[i]` sits at
    `pt[j-len+i]` and `V[i]` at `pt[j+1+i]`, so the pair is `(y, y+len+1)`.
    Extending *outward* from the separator compares `W` reversed against `V` and
    matches only palindromes — the first implementation did exactly that and
    reported nothing on `ENGELMANN X ENGELMANN`. `find_doubling()` slides a
    window over the shift instead: longest length first, so the first hit is the
    answer, O(n²) worst case rather than the O(n³) of testing every length at
    every X. Cross-checked against `eval/`'s Python reference on 4 000 random
    strings, 0 mismatches, offsets included.
  - **A report is NOT a new best**, and the settings echo says so: these lines
    fire on any key past the gate whatever the search's high-water mark, which
    is exactly the point — the true key can be reported while another board
    still leads on score. Identical consecutive repeats are collapsed (one call
    per converged restart plus one after `--polish` would otherwise print the
    same row `-R`+1 times); display-only, under the progress mutex, so which
    candidate WINS is untouched. **`--full-text` expands a report** as it does
    a progress line: the report says a doubling is present and the whole
    decrypt is what lets the reader judge it, so skipping the one line most
    worth expanding would be the wrong default.
  - **A one-key space has no null**, so `--confidence` falls back to raw scores
    and the report stays silent. Give it a keyspace bigger than the sample.
- `--full-text` print the **whole decrypted message** with each progress line
  instead of the 19-character preview (16 under `-4`), on its own wrapped,
  indented lines *below* the line rather than by widening it — the columns are
  budgeted to land exactly on 80 and must keep lining up whether it is on or
  off. The continuation wraps at **`80 − indent`**, so it reaches the same right
  margin as the preview it replaces and the two read as one block; it was 2
  columns short for a long time, from a period when the target was a 79-column
  terminal, and a one-sided "stays within 80" test could not see it. The test
  now compares the widest continuation against the progress line itself rather
  than against a literal.
    Not a hot-path concern: a progress line is emitted only when a board beats
  everything echoed so far, so this prints once per improvement, not once per
  board scored. Off by default.
- `--no-preflight` **is this ciphertext even Enigma?** (the check is **on by
  default**; this turns it off). Enigma is a permutation
  cipher, so its output is near-flat; a ciphertext carrying residual language
  structure was not produced by one and has **no key to find**. Two free
  statistics — the index of coincidence, and how many letters of A–Z never
  occur — are computed once from the ciphertext and compared against a
  length-dependent null, and the verdict is printed before the sweep.
    **It reports only for a SEARCH — a wildcarded key — and that gate is part
  of the design, not a limitation.** With a fully-specified key the tool is
  encrypting or decrypting, and on encryption the input is *plaintext*, which
  is language-like by definition; reporting there would print an alarming
  line about a ciphertext that is not one, on every encryption (including the
  hundreds `tests/run_tests.sh` performs). It is also the only run for which
  the question is meaningful: a search is what risks looking for a key that
  does not exist.
  - **It was measured the expensive way.** A 28-hour, 75.2M-key sweep of the
    QTXMA challenge message returned nothing (best margin +0.81 sd against a
    6.0 bar). The reason was in the ciphertext before the search started: IC
    **0.0577** against the 0.0385 ± 0.0018 that 3000 simulated Enigma
    encryptions give at that length (**z = +10.9**, the largest of the 3000
    being 0.0468),
    and **4 letters unused** where 0.06 are expected (P = 8.5e-08; none of the
    3000 lacked more than 2). `eval/MODERN_BREAKING_NOTES.md` §5l.
  - **The null MUST be length-dependent, and the collection proves it.** IC
    variance goes as `1/C(n,2)`, so short messages reach a high IC routinely:
    two of the four *broken* — i.e. genuinely Enigma — 1941 messages sit at
    **z = +4.2**, at 47 and 74 letters, and one has **9 of 26 letters unused**
    in 47. A fixed IC threshold condemns them and leaves QTXMA looking
    ordinary.
  - **No tables are needed**, which is what makes it free. Both statistics have
    closed forms under a uniform multinomial that match simulated Enigma within
    1–2% at every length from 40 to 600: `IC = P/C(n,2)` over same-letter
    position pairs, whose indicators are pairwise **uncorrelated** under
    uniform `p` (the shared-index covariance is `Σp³ − (Σp²)² = 0`), giving
    `E[IC] = 1/A` and `Var[IC] = q(1−q)/C(n,2)` with **no dependence on the
    plaintext**; and `E[X] = A(1−1/A)^n` for the unused count, whose tail is
    quoted as the first Bonferroni term `C(A,k)(1−k/A)^n` because `X` is small
    and skewed enough that a z-score misleads.
  - **Thresholds come from the measured tail, not a nominal p-value** — the
    same discipline `--confidence` documents for its own Gaussian tail. Warns
    at **z(IC) > 6.0** or **P(unused) < 1e-4**; across one sample of **18 000
    simulated Enigma ciphertexts** at n = 40…600 the largest z seen was 5.89
    and neither test fired once. *Simulated*, not real traffic: authentic
    1941 German under random keys and 10-pair boards, and the corpus tops out
    at 214 letters so the n ≥ 300 cells draw from it concatenated. A false
    positive is expensive in trust (it would tell someone to abandon a
    breakable message) and a false negative only costs what it costs today,
    hence the wide margin. `eval/preflight_null.py` reproduces it.
  - **No pre-flight line may look like a progress line.** The margin extractor
    for `--confidence` greps stderr for `^ *[+-][0-9]`, and a continuation
    line reading `  +10.95 sd; …` was picked up as the run's last margin —
    breaking a `--confidence` check in a way that pointed at `--confidence`
    rather than here. The statistics line now opens with `(`, no line begins
    with a digit, and a check asserts it.
  - Runs once per process on ≤1024 letters, so it is nowhere near the hot path
    and far below the n-gram load in the startup budget.
- **Live sweep progress** — a `\r` line under the main sweep carrying
  percentage, key rate and ETA, e.g.
  `Progress:   50% (5.94M / 11.88M keys) 10.12M/s, 1s left`. No flag: on
  whenever stderr is a terminal, like `-F`'s tier-1 line. The score lines say
  how *well* the search is doing and nothing about how far it has come — and
  they thin out to nothing precisely when a run is longest, so without this
  there is no way to tell a slow sweep from a stuck one.
  - **Counts are in KEYS, the percentage is over WORK ITEMS.** The counter runs
    over `keys × restarts` (what the sweep actually hands out), but that unit
    would read `8×` high against the `Analysed N rotor combinations` diagnostic
    under `-R 8`, so the display divides it back out. The percentage is
    identical either way, since the two differ by a constant factor.
  - **Redrawn on a CLOCK — every 5 s — not on a percentage boundary.** A 1%
    boundary is the wrong clock: 1% of the work takes longer the bigger the
    sweep, so the line updated most rarely on exactly the runs that need it.
    Measured at the climb rate of ~1800 keys/s, that was one update every 5.8 s
    over 1.05M keys but **2.5 minutes** over 27.4M and **21 minutes** over 230M
    — long enough that the first line looks like a hang. The only draw exempt
    from the interval is the last, so the line always finishes at 100%. One
    worker claims each slot by `compare_exchange` on the timestamp, so the
    others do not queue on the mutex to redraw the same line.
  - **Ticked per item-block, not per chunk, and the BLOCK FOLLOWS THE REGIME.**
    A chunk is `total/(threads·16)`, so per-chunk ticking gives *sixteen*
    updates for a whole `-T 1` run. A worker-local counter fixes that, but its
    size cannot be a constant: an item costs four orders of magnitude more under
    `-c` than in a scan. A scanned key is ~0.3 µs, so 4096 of them is ~1 ms;
    a climbed key is ~1–2 ms, so 4096 of them is **a thread reporting once every
    nine seconds** — the other half of why the line appeared to hang. Hence
    `tick_block = opt_hillclimb ? 64 : 4096`: ~100 ms of climb work per tick,
    which the 5 s gate then paces. When the line is off, `g_sweep_total == 0`
    short-circuits the whole thing to a single predictable branch per key.
  - **Throughput.** `make bench LONG=1 BASE=origin/dev` on a quiet box: `search`
    −1.4%, `hillclimb` −0.4%, `fused` +4.7%, `crib` −7.0% — no regression. The
    quick tier that day had a base-vs-base floor of **±8% on `search`** (−7.7%
    measured on byte-identical code), so only the long tier says anything at
    all; what it establishes is the absence of a large regression, and the
    argument that the cost is negligible rests on the code shape rather than on
    a number this coarse.
  - **`progress_line()` is the choke point that makes the two streams safe.**
    Every score line — the key-level merge and `report_climb_progress` alike —
    goes through it, and every caller already holds `best.mutex`, so
    `sweep_progress_clear()` sits at its top and the `\r` line is erased before
    anything is printed over it. The tick takes the same mutex to draw.
  - **The line is padded to the widest drawn and erased at that width**, not at
    a blanket 79. It can *shrink* (`999k/s` → `1.0M/s`, `10m00s` → `9m59s`) and
    a bare `\r` plus a shorter string leaves the tail of the previous one on
    screen; and an over-wide erase wraps on a narrow terminal, after which the
    `\r` returns to the start of the *second* line and leaves the first dirty.
  - **Armed for the main sweep only.** `bruteforce()` sets `g_sweep_total`
    around that one `run_parallel` and clears it straight after, because the
    `--ring-stride` refinement reuses `search_worker` over its own key space and
    would otherwise push the percentage past 100. `--crib-list` runs one sweep
    per crib, so each gets its own 0–100%.
  - **Suppressed under `--dump-all`**, whose rows are the machine-readable form
    the harnesses parse and which print under a *different* mutex — so a `\r`
    line could not be sequenced against them even in principle.
  - **Nothing appears in under 0.5 s of sweeping.** A short run should not flash
    a line up and wipe it again, and an ETA off the first few milliseconds is
    noise.
- `-d dir` directory holding the n-gram files (else `$ENIGMA_DATA`, else
  `ngrams`)
- `-T N` worker threads for the search (default 1, max 256). Parallelises over
  the `keys × restarts` work space, so it scales even a **fully-specified rotor
  key** with `-c -R N` (the restarts are spread across threads) — not just
  wildcarded keyspaces. `-T`-independent (deterministic regardless of thread
  count).

Every run echoes the resolved configuration (scoring model, language, n-gram
data directory, machine settings, plugboard, ciphertext length) to stderr.

> **Gotcha — match `-l` to the plaintext language, especially for `-q`.** There
> is no default language: `-m/-b/-t/-q` require `-l`, and the n-gram tables are
> highly language-specific (most of all quadgrams). Scoring an English message
> with, say, `-l german` typically fails — the german table scores ~0 for
> English quadgrams and the correct key does not stand out. Lower-order models
> (`-m/-b/-t`) tolerate a mismatch better, and `-i` (index of coincidence) is
> language-independent and needs no `-l`. Use `-l` matching the plaintext.
> **Quad works for every language (after the table-loading fix); `-q` is the
> default recommendation.** An earlier eval round suggested German needed a
> lower order (`-b`/`-t`), but that was a **bug**: `load_counts` truncated the
> non-English tables at the first accented gram, so the "german quad" scorer ran
> on only its 29 most frequent quadgrams (4.9% of the table). Fixed by folding
> each accented gram to its A-Z base and accumulating counts (`ä→A`, `ø→O`,
> `ç→C`, …; `load_counts`, and the ciphertext/plaintext readers fold input the
> same way and warn on non-mappable characters — see `archived/PERFORMANCE.md`
> §6.9). With the full table German quad is search-bound and fully solvable like
> English (German L90 mean %-correct 24.7 → **91.1** before → after the fix); on
> an orthogonal four-language grid german/danish/french all crack comparably to
> English. Model order is *not* meaningfully language-dependent once the tables
> load — use `-q`. The residual gap is *genuine telegraphic* German (operational
> orthography off-distribution for the prose tables — §6.6), not the model
> order.

## Architecture / how it works

A single pass through `main()` (which is only the sequencing — the command line
is resolved by `parse_args()` in `src/args.cc`, and each numbered step below is
one call into a module):

1. `parse_args()` parses and validates the options (`getopt_long`).
2. Still inside `parse_args()`, and deliberately **before stdin is touched**:
   `load_table()` loads the n-gram table for the target model and for every
   `--score` stage that needs a different one (target first), and the
   `--crib-rerank` word list and `--crib-list` library are read. A missing `-l`,
   a mistyped language or an unreadable crib file therefore fails immediately,
   naming the offending filename, rather than after the ciphertext has been
   consumed from a pipe that cannot be rewound.

   In the loaded table, each
   count is stored as the log10 probability `log10(count / total)` (unseen grams
   floored at `log10(1 / total)` — scored as a single occurrence, a hapax), so
   the additive scorers sum a log-likelihood and `score_iter`'s per-symbol
   average is a cross-entropy (dits/char). IC is a separate normalised ratio and
   is left untouched. `ngrams_read()` quantises each table directly into a
   **uint8 fixed-point** copy (`mono8`/`bi8`/`tri8`/`quad8`/`all8`) that the
   scorer reads: `q = round((v − bias)·scale)`, where **both the bias and the
   scale are per-table adaptive** — `bias` = the table's minimum log10 value
   `vmin` and `scale` = `255/(vmax − vmin)`, so all 256 levels land on that
   table's actual `[vmin, vmax]` span and nothing clips by construction
   (`ngram_bias[SCORE_*]` / `ngram_scale[SCORE_*]`). The scale was a fixed 32
   for a long time, which left the narrow tables short — danish quad reached
   only byte 172 — and it has to adapt anyway because graded smoothing and
   interpolation move the span. A zero span (an empty table) falls back to 32.
   This matters for **quad** — the hot, largest table (26⁴ entries): uint8
   shrinks it to 0.45 MB (vs 1.8 MB float / 0.9 MB int16) so it stays
   cache-resident during the scan (measured faster; see Performance notes).
   mono/bi/tri are tiny and already cache-resident — same representation only
   for consistency. Scorers sum uint8 into a `long` and recover the log-prob sum
   as `isum/scale + n·bias`. Recovery quality is identical to the wider types
   for every model, measured across all four languages.
3. `readciphertext()` reads stdin, uppercases, and keeps only A–Z.
4. `show_settings()` echoes the resolved configuration, then
   `report_preflight()`
   says whether the ciphertext looks like Enigma output at all — in that order,
   so the configuration is on stderr before any verdict about the input, and
   after the empty-ciphertext check so the statistics have something to
   describe. `--self-crib-seeds` builds its hypothesis list just *before* the
   echo, because the echo reports how many there are and read 0 for a while
   when it ran first.
5. `init()` precomputes numeric forward/reverse rotor permutations, notch
   tables, and reflector permutations from the hard-coded wiring strings;
   `init_plug_fixed()` turns `-s` and `--no-plug` into the pin marks, and
   `init_crib()` builds the `--crib` menu — here rather than during option
   validation, since it depends on the ciphertext (as do the checks that the
   crib fits and can sit somewhere an Enigma could have produced).
6. `bruteforce()` is the main search, run across `opt_threads` worker threads
   (`-T N`, default 1, max 256) in two parallel phases over the flat reflector ×
   wheel-order × ring × start key space:
   - **Phase 1 (`precompute_worker`)**: build `subst_array[g1][g2][g3][x]` — the
     rotor-stack substitution for every (start-minus-ring) triple, ring fixed at
     0 — once **per reflector × wheel-order**, for *all* of them, into one
     shared read-only block (`#wheel-orders × 457 KB`). A table serves every
     ring/start of its wheel order via the start−ring offset.
   - **Phase 2 (`search_worker`)**: an atomic counter hands out adaptive chunks
     of the flat work space; each worker decodes a flat index → (wheel-order,
     ring, start) by mixed radix, points its private `machine` at that wheel
     order's shared table (swapped, never recomputed, on a boundary), and:
     - **The work space is `keys × restarts`, not just keys** (`restarts` = `-R`
       under `-c`, else 1). The `-R` plugboard restarts of a key are independent
       — each draws from its own `(key,restart)` RNG seed (`restart_seed`)
       rather than one stream advanced sequentially — so they are spread across
       threads too. **Restart is the OUTER dimension**: the sweep does every key
       at restart 0, then every key at restart 1, and so on.
     - **Why restart-major, when restart-innermost is cheaper.** Innermost lets
       consecutive items share a key and reuse its `setup_mapping`, which is
       why it was built that way. That saving is **under 1%** — `setup_mapping`
       is <0.1% of a `-c` run by callgrind, and a direct `-R 1` vs `-R 8`
       timing cannot resolve it above thread jitter — while the ordering
       decides **when** an answer appears. There is no early exit, so this does
       not shorten a run; it front-loads the probability, which is what lets a
       watcher kill a 28-hour sweep early. On the measured climb curve (87% at
       `R = 16`, ~11.9% per restart): found by the quarter mark **40% against
       22%**, by halfway **64% against 44%**, the same 87% at the end.
     - **`best.idx` decodes as `idx % keys`, NOT `idx / restarts`.** Two sites
       reconstruct the winning rotor key from the merged work index —
       `--polish` and the `--ring-stride` refinement — and both go through
       `work_key()` so they cannot diverge. Getting it wrong prints a key that
       does not decrypt to the plaintext the run just wrote to stdout, which is
       the failure the `--tune-phase` notes record; `tests/run_tests.sh`
       re-encrypts the reported plaintext under the reported key and compares,
       for `--polish`, `--ring-stride` and both together.
     - **The live progress line reports per PASS** (`pass 7/16, 8.1M / 17.6M
       keys`). Dividing the item count by the restarts — what it did while
       restarts were innermost — would report 6% of keys covered at the point
       where every key has been visited once, i.e. exactly the fact the reader
       is watching for. The rate is key-*visits* per second and the ETA runs to
       the end of the whole sweep, both unchanged in meaning from the
       single-restart case, where the pass field is omitted entirely.
     - The ordering changes which of several **exactly-tied** boards wins (the
       `better_cand` tie-break is on the work index, which now enumerates in a
       different order). Still deterministic and still `-T`-independent; simply
       not byte-identical to the pre-change output on a tie.
     - **This is what lets a *fully-specified rotor key* (one key) still use
       every thread** — the old key-only scheme left that case single-threaded
       (`-T` a no-op). The `-F` tiers and the plain scan keep one item per key.
       Determinism is preserved by a lowest-work-index tie-break in the
       best-merge (`better_cand`), since parallel restarts of one key often
       converge to the same score.
   - `setup_mapping()` steps the rotors over the message length and records, per
     position, a pointer `rows[pos]` to that position's rotor-stack substitution
     row (folding in the stepping). The scan points `rows[pos]` straight into
     the shared `subst_array` (no copy); hill-climb copies the row into a
     contiguous `mapping[]` first (it re-reads each row many times);
   - `decode()` + `score_iter()` produce and score the candidate (the n-gram
     scorers fuse the decode into their loop). The best is merged under a mutex
     (which also serialises the live progress line). Parallelising the flat key
     space means rings/starts scale even when the wheels are fixed.
   - With `-c`, `hillclimb()` greedily improves the plugboard: each pass scans a
     single **"toggle a–b"** operator over all 325 letter pairs — one operator
     that, by the current state of a and b, *adds* a new plug (both ends free),
     *moves* an endpoint (one end plugged), *merges* two plugs into one (both
     plugged, different partners), or *removes* the a–b plug (a and b already
     paired) — and takes the single best improving toggle, run to convergence.
     (Folding removal in as the already-paired case is what lets one scan
     replace the former separate switch-scan + removal-loop; a switch still wins
     ties over a removal, so it stays byte-identical.) Then one "re-pair" move
     (`try_repair()`, re-match two plugs into the other pairing — the one
     count-neutral two-plug move a single toggle can't express) is tried as a
     barrier-cross, and if it improves the cheap climb resumes; re-pair is a
     general local-optima escape gated to fire only at convergence (~zero cost).
     The plug cap gates the toggle by count-effect: an *add* is blocked at/over
     the cap, and with `-M` a count-preserving *move* too, so only the
     count-reducing *merge*/*remove* remain (cap as a strict descent target).
     See `archived/CODE_REVIEW_HISTORY.md` §9 item 7.
7. The best-scoring plaintext is printed; optionally compared to `-p` file. A
   final stderr diagnostic reports the number of rotor combinations analysed (`=
   total_keys`, brute force has no early exit) and plugboards scored (total
   `score_iter` calls, summed from a per-`machine` counter so the hot path stays
   lock-free; `-T`-independent), then wall-clock time, thread count, the
   number/size of precomputed rotor tables, and peak RSS (via `getrusage`).

### Core machine model

- `char2num`/`num2char` map A–Z ↔ 0–25.
- `rotor_l` / `rotor_r` apply a single rotor forward/reverse with the
  `grundstellung - ringstellung` offset.
- The rotor stepping schedule — including the Enigma double-stepping anomaly
  (the middle rotor advances on its own notch as well as the right rotor's
  carry) — is implemented inline in `setup_mapping()`, which holds the rotor
  positions in locals across the per-character loop (see the performance note).
- The full substitution is plugboard ∘ rotor-stack ∘ reflector ∘ rotor-stack ∘
  plugboard. The hot path computes it as precomputed `subst_array` / `mapping`
  lookups wrapped in two plugboard lookups (`decode_at`, shared by `decode()`
  and the fused scorers). `precompute()`'s middle loop is where that
  composition is written out — wheels 1 and 0 in, reflector, wheels 0 and 1
  out — with the right wheel factored out of it, which is what makes one table
  serve all 26 of its offsets. A `subst_rotors()` that spelled the whole stack
  out survived until PR #218 with **no callers**: `precompute()` had replaced
  it, and being an `inline` with external linkage hid that for years, until
  making it `static` during the module split drew a warning. **That is why
  `-Wmissing-declarations` is in `CXXFLAGS`** — external linkage with no header
  declaration is exactly the state that hides a dead function from every gate
  the repo has, and nothing else looks for it. It found three more the day it
  went in (`precompute_worker`, `filter_worker`, `finish_worker`, each with one
  same-file caller); a clean tree reports none, and both compilers accept the
  flag.
- The reflector applied by the rotor core is `m.reflector_eff`, resolved once
  per task by `set_effective_reflector()` (never per character). Standard/Norway
  just copy the wired reflector; **M4** composes `greek ∘ thin ∘ greek⁻¹`.

### M4 mode (4-rotor naval)

The M4's 4th "Greek" wheel (Beta/Gamma) is **static** — it never steps — so it
folds into the reflector: `set_effective_reflector()` builds an effective
reflector `greek ∘ thin ∘ greek⁻¹` at the Greek wheel's fixed offset `(start −
ring) mod 26` (an involution, since conjugation preserves it), used at the
single reflector site. The machine therefore stays a **3-stepping-rotor** engine
(`wheels` stays 3) and the entire hot path (`subst_array`, `setup_mapping`,
stepping, scorers) is unchanged — the fold is paid only in `precompute()`.

- CLI: `-4` mode flag; `-u` is the thin reflector `b`/`c`/`.`; `-w`/`-r`/`-g`
  take **four** characters with the Greek wheel (`B`/`G`/`.`) / ring / start
  first. The Greek char is split off in validation so the shared 3-char checks
  and the 3-rotor search run unchanged on the tail. `-n` and `-4` are mutually
  exclusive.
- Search: `wheel_task` carries the Greek wheel + offset; `bruteforce()`
  enumerates thin × Greek wheel × **distinct Greek offsets** (only the `start −
  ring` offset is identifiable, so the pos/ring ranges collapse to ≤26 offsets,
  not 26×26) × wheel orders. Each task precomputes its own effective reflector,
  then the existing two-phase precompute + flat ring/start sweep runs unmodified
  (threading/determinism preserved). A full M4 wildcard is ~15 GB of tables,
  hence the precompute guard is **16 GiB**.
- Correctness is anchored (KAT) on the documented backward-compatibility
  equivalence: thin `b` + Beta at ring/pos A ≡ standard reflector B, and `c` +
  Gamma@A ≡ C (`tests/run_tests.sh`), plus round-trip and search-recovery
  checks.
- Reflector indices 4–5 = UKW-b/c, rotor indices 13–14 = Beta/Gamma (already in
  the wiring tables). Only the `(start − ring)` offset of the Greek wheel is
  recoverable, so `showconfig` reports it as start = offset, ring = A.

### Leftmost stepping wheel's ring is unidentifiable — offset-only search

The Greek wheel isn't the only place a ring × start pair collapses to a pure
offset: `walzenlage[0]` (the leftmost of the 3 *stepping* wheels, in every mode
— standard, Norway, and M4's 3-wheel core alike) has the same property, and for
a stronger reason than "it happens not to step on this message."
`setup_mapping()`'s notch checks — `notch[w1][g1]`, `notch[w2][g2]` — test each
wheel's raw window position, never the ring-adjusted offset; wheel 0 has no
notch check of its own (nothing sits further left for it to step), so nothing
downstream ever depends on its *absolute* ring/start, only their difference.
Shifting `ringstellung[0]` and `grundstellung[0]` by the same amount therefore
reproduces the identical decode **unconditionally** — verified empirically
across every shift 1–25 at 127 characters, not just "usually" the way wheels 1/2
are (their own window value gates further stepping, so shifting them is a real,
measurable approximation — see `archived/PERFORMANCE.md` §7.10 for the
mismatch-vs-shift data). When both `-r`'s and `-g`'s first character are
wildcarded together, `build_key_space()` collapses `ring0`'s range to the single
sentinel `0` (leaving `start0` to enumerate the 26 offsets directly) — the same
`offset_list` pattern as the Greek wheel above, a lossless 26× reduction.
Reported ring position for wheel 0 is therefore always `A`. Only fires when
*both* are wildcarded together — either alone enumerates genuinely distinct,
necessary offsets. Full derivation, measurements, and the still-open (riskier,
approximate) extension to wheels 1/2: `archived/PERFORMANCE.md` §7.10.

This is not a niche precondition to arrange deliberately: the non-M4 default (no
`-r` given at all) is `opt_ringstellung = "AA."` and the default `-g` is `"..."`
— ring0/ring1 default to `A`, ring2 (rightmost) and every start default to
wildcarded. So "ring2 + start2 both wildcarded" is live in the tool's bare
default invocation whenever a caller doesn't explicitly pin ring, which is
exactly the precondition the `--ring-stride` sparse-sampling option needs — see
`archived/PERFORMANCE.md` §7.11.

### Middle wheel's ring × start is partially redundant — always-on collapse

The three stepping wheels each behave differently, and the tool now exploits all
three:

| wheel | ring × start collapse | factor |
|---|---|---|
| 0 (left) | total, unconditional, exact | 26× |
| 1 (middle) | **partial, exact** — this section | **3–5× at short lengths** |
| 2 (right) | **exact for VI–VIII**, else `--ring-stride` | **2×** / 1.7× |

That table is about *position*. There is a fourth collapse that depends on the
*wheel* instead — the two-notch VI–VIII, exact and unconditional — see
"Two-notch wheels" below. It is now always-on for the right wheel; the middle
wheel's share of it was already inside row 1, because §7.12 derives its classes
by simulation.

Shifting `ring1` and `start1` together leaves `mod26(g1-r1)` — the middle
wheel's entire contribution to the substitution — invariant, so two such pairs
can differ *only* through `notch[w1][g1]`, the middle notch that gates the left
wheel and the double step. The middle wheel steps only ~once per 26 characters,
so in a short message it visits ~L/26 positions: most `start1` values never
reach the notch at all and every one of those decodes byte-identically. Measured
**182 distinct of 676** at L=140, 130 at L=100.

Exploited by **skipping** keys whose `start1` is not its class's canonical
member — no reparameterisation, because for a representative `start1`, `ring1`
ranging over all 26 already yields every offset. `g_mid_rep_mask` holds a 26-bit
mask per (middle rotor, right rotor, start2); it is indexed by the *rotor pair*,
not the task, since the reflector, the left rotor and every ring setting are
irrelevant to stepping. Built in `build_key_space()` via `mid_first_fire()`,
gated on ring1 **and** start1 both being fully wildcarded (with ring1 pinned,
each start1 carries a distinct offset, so dropping any would lose real keys) —
hence it is inert under the default `-r AA.` and active under `-r A..` / `-r
...`.

**Do not derive the class count from a formula.** `⌈L/26⌉+1` fits only the
single-notch case: a two-notch *right* rotor (VI–VIII) steps the middle twice
per revolution giving `⌈L/13⌉+1`, double-step extras add one for some wheel
orders, and the count also varies with `start2`. The masks come from simulating
the stepping and deduping on the first-firing index — exact for every
combination. The failure mode of getting this wrong is **silent key loss**,
which is why `tests/run_tests.sh` covers `126`/`168` (two-notch right) and `132`
(double-step) specifically.

**The reported ring1/start1 may be a class representative, not the true key.**
Class members are indistinguishable from ciphertext alone, so `showconfig` can
print a different ring1/start1 while the plaintext is byte-identical — the same
contract already documented for wheel 0 ("reported ring position for wheel 0 is
therefore always `A`") and the M4 Greek wheel. It is length-dependent: past
L≈676 every class is a singleton and the true key is reported exactly.
`--true-key` disables the collapse, since that diagnostic ranks a specific key
and a collapsed one would simply be absent.

Full derivation, measurements and the shipped results: `archived/PERFORMANCE.md`
§7.12.

### Two-notch wheels collapse ring × start by 13 — exact, always-on

Wheels **VI, VII and VIII** all carry notches at `M` (12) and `Z` (25) — exactly
`26/2 = 13` apart, so their notch *set* is invariant under a shift of 13. That
makes a shift of 13 an **exact, unconditional** decode equivalence for such a
wheel in the **middle or right** position:

    ring_w += 13, start_w += 13 ⇒ byte-identical decode

Two things reach the machine from a stepping wheel's (ring, start): the offset
`(start − ring)`, which the joint shift preserves, and the **absolute**
position, which is read only by the notch test — and that test cannot tell `g`
from `g+13` when the notch set is `{12, 25}`. Nothing else downstream sees the
wheel's absolute position, so every derived quantity (the middle-step schedule
`S`, the left-step schedule `D` including double steps) is identical step for
step, at every message length.

In the **left** position there is no additional effect: that wheel's ring ×
start already collapses for *every* shift under §7.10, so 13 is not special
there.

**Measured** (random keys, wheels I–VIII, L ∈ {40, 110, 400, 900}): identical in
**152/152** middle-position and **138/138** right-position trials. The
single-notch control is the useful contrast, because it separates this from
§7.12's collapse — in the right position it differs in **0/262**, and in the
middle position it is length-dependent and decays away (62% identical at L=60,
18% at L=200, **0% at L=900**), while the two-notch case stays at **100% at
every length**. Unconditional versus conditional is the whole distinction.

Size: with a two-notch wheel on the right the distinct `(start1, start2)`
classes drop from **651 to 326 of 676** — a clean factor of 2, or **1 bit**, per
affected wheel, and 2 bits when VI–VIII sit in both the middle and the right
positions. Averaged over all 336 ordered triples from I–VIII this is a **34.8%
keyspace reduction**; under the default `-x 5` (wheels I–V) it is **0%**, since
none of I–V has two notches. So it is worth exactly as much as `-x 8` is used.

**Shipped, for the RIGHT wheel only — the middle wheel was already covered.**
§7.12 derives its classes by *simulating* the stepping rather than from a
formula, so it picks the two-notch case up for free: measured at L=700, a
two-notch middle wheel gives **13.0 start1 classes against 26.0** for a
single-notch one, exactly the factor of 2. That is the discipline paying off —
a formula-based §7.12 would have missed it. The right wheel had no collapse at
all, and now has this one.

`notch_halfperiod[w]` is derived in `init()` from the notch table (invariant
under a shift of 13, and the wheel has a notch at all — a *notchless* wheel is
trivially period-13 but must not count, since its absolute position is unread
and the equivalence is the whole ring, a different fact). `search_worker()` then
**skips `ring2 ≥ 13`** when the task's right wheel qualifies: exactly one
representative per class, because every dropped `(r2, g2)` has its twin
`(r2−13, g2−13)` still in the sweep.

The precondition is `rc[2] == 26 && gc[2] == 26` — ring2 *and* start2 both fully
wildcarded, or the twin may be absent and the skip would lose a real key. That
one test also excludes `--ring-stride` and `--tune-phase` for free, since both
leave `rc[2]` short of 26, and `--true-key` opts out for the same reason §7.12
does. Reported ring2/start2 may be either member of the pair — the same
class-representative contract wheel 0 and §7.12 already carry, and harmless
because the decode is identical.

**It composes with §7.12 multiplicatively**: measured 2× with a two-notch wheel
in either position and **4×** with one in both (`-w 176`, L=700, `-r A.. -g
...`). Against today's baseline the increment is ~20% of the keyspace averaged
over the 336 ordered triples — the 34.8% above assumed *neither* half was
banked. Unlike §7.12's, this collapse has **no length term**: 2× at L=40 and 2×
at L=900 alike, where §7.12's is worth 7.4× at L=40 and 1.00× past L≈676.

**The equivalence itself is tested by encryption, not by recovery.** A wrong
class drops keys silently, and a recovery test does not reliably catch that —
so `tests/run_tests.sh` first asserts that shifting ring2/start2 by 13 gives an
identical ciphertext for VI/VII/VIII and a *different* one for a single-notch
wheel, then checks the key counts, both halves of the precondition, recovery
with the true ring2 in the dropped half, `-T` independence, and that Norway
(single-notch wheels only) and M4 (which reaches VI–VIII) behave correctly —
the translated-versus-raw rotor index being where this kind of bug hides.

### Sparse ring sampling for the rightmost wheel — `--ring-stride`

Unlike wheel 0, the rightmost wheel (`walzenlage[2]`) has a real notch that
gates further stepping, so a ring+start shift there is only an *approximation* —
small and smoothly growing with the shift, not an unconditional equivalence
(`archived/PERFORMANCE.md` §7.10's mismatch table). `--ring-stride K` (K=1..26,
default 1 = off) exploits this: the coarse search tests only ring2 ∈ {0, K, 2K,
...} (`build_key_space()` shrinks `rc[2]`; `search_worker()`/`key_to_machine()`
scale the decoded index back up by `K`), then `refine_ring_stride()` in
**`src/refine.cc`** runs one small refinement pass — reusing `search_worker` on
a self-contained mini key-space — over the ring2 values the coarse pass skipped,
keeping the improvement only if it beats the coarse result. That module's header
carries the four bugs the code's shape is a response to; read it before touching
any of this.

**The refinement covers *every* skipped ring2, not a `±⌊K/2⌋` window around the
coarse winner.** It runs **once**, at the end of the whole search (next to
`--polish`, before it), over a *single pinned* wheel order/reflector with
ring0/start0 fixed — so a ring2 value costs it `tasks.size() × rc[0] × gc[0]`
times less than the same value cost the coarse pass: 26× on a single-wheel-order
key with start0 open, ~26 000× with the wheels wildcarded. §7.11's original
`26/K + K` accounting priced refinement values at coarse-pass cost and so
concluded "≈2.5× at best"; at the true price the extra is a **constant** (25
values on one task, independent of `K`) and the saving approaches `K`×. **All 25
ring2 values, unconditionally — no window, no budget, no dependence on `K`.** An
earlier version grew the ring2 window under a "25% of the coarse pass" budget;
that was a *ratio* masquerading as a cost and it made the same command do
different work depending on an unrelated part of the keyspace, so it was
removed.

> **SUPERSEDED: the refinement is now DERIVED, not enumerated — see
> `archived/refinement.md`.** `start2` follows from the coarse winner's
> offset2, and `ring1`/`ring0` from the step-count difference between the
> winner's schedule and the candidate's — computable from the two keys, so the
> `±2` band and its
> `mid_ring_window` constant are gone. The set went from `25 × 130 × 26` = 84
> 500 to `25 ×` the start1 range (650–1 300), measured equivalent to the
> enumerated refinement on the stride-specific miss rate (0 in both, 360 paired
> trials). The "not paying for itself" warning is **removed**: the keyspace it
> warned about is now a 1.98× win and the warning is provably unreachable. The
> width numbers below are the history that led there. **The derivation is
> equivalence-clean across machine variants too** — 1200 paired trials,
> `lost:derived` = 0 in all twelve cells (standard wheels I–VIII and M4, `K` =
> 2/3/5, L = 60/110), 480 of them with a two-notch wheel in the *right*
> position. Two features had to be separated to test this: two-notch right
> wheels change the **stepping** (the part the derivation is sensitive to),
> while M4's folded Greek wheel changes the **substitution** and not the
> stepping (the part it is indifferent to). The run also scores the shape that
> actually ships for the first time — the earlier tables scored only the shapes
> that *led to* the derivation, so the claim rested on the derived set being a
> subset of a measured-clean superset, which bounds it the wrong way.
> `archived/PERFORMANCE.md` §7.11 and `archived/refinement.md` §12. **The three
> conditions those runs did not cover are now measured too** — K≥8, a hidden
> plugboard, and messages long enough for the left wheel to step
> (`archived/PERFORMANCE.md` §7.11, `eval/ring_stride_scope_probe.py`).
> K=8/10/13 cost the same ~1% stride-specific miss as the documented K=2/3,
> which closes a gap in the flat-cost-curve claim below: the flatness above K=13
> was measured on *keys analysed*, never on accuracy. Left-wheel stepping is
> clean — 0.0% at every stride through K=26 on random keys at L=600, with the
> wheel verified to have stepped in 54 of 60 trials. The one place anything
> moved is a **hidden plugboard at K=13**: 4 losses in 69 trials against 0 in 72
> for a paired given-board control, consistent in direction across two seeds but
> **p ≈ 0.13 — suggestive, not established**, and only at a stride already
> outside the recommended K≤3.

> **The width was re-measured, and then the question was superseded — see
> `archived/refinement.md`.** A `w=3` cap matches the full sweep at K≤5 but
> costs 20pp of exact recovery at K=10 and 40pp at K=13, so any cap would have
> to be `K`-dependent (`⌈K/2⌉+3` is the narrowest measured-clean rule; `⌈K/2⌉+2`
> is
> 2pp light at K=10–11). It is not worth building, because the whole width is
> only 1–2.4% of a run — and because the refinement's *shape* has far more slack
> than its width: the coarse winner's `start2` offset can be pinned (0 losses in
> 600 trials) and its `ring1` **derived** rather than banded, taking the set
> from 84 500 candidates to 650. `archived/refinement.md` has the algebra, the
> design and the verification plan; the measurement-only `ENIGMA_REFINE_WINDOW`
> env override (default off, byte-identical) reproduces the width columns.

**The MIDDLE wheel is banded, and the band is derived rather than enumerated.**
Shifting ring2 and start2 together leaves the right wheel's substitution
untouched (only `(start2 − ring2) mod 26` enters it) and moves nothing but the
notch *timing*, so the middle wheel's step schedule is time-shifted rather than
lengthened. Its position can therefore run at most **2** steps from the coarse
winner's — established by *enumeration*, not sampling: simulating the real
schedule (double step included) over every rotor pair × 26 start1 × 26 start2 ×
every shift 1–25 at L=600 gives max divergence 2, never 3 (the original run
covered 1–13, the old `K` ceiling; it was re-run over the full range when the
ceiling rose to 26 and the bound is unchanged). One step is the ordinary time
shift; the second is **double stepping**, when the two schedules straddle the
middle wheel's own notch. Two-notch right rotors (VI–VIII) change how *often*
that happens, not how far it reaches — the worst case is rotor II, single-notch.

The band is on the **offset** `(start1 − ring1) mod 26`, **not on raw `ring1`**
— under §7.12 the reported ring1/start1 are class representatives, so raw ring1
wanders freely while the offset barely moves (measured: two misses where the
winning ring1 sat 5 and 6 away from the coarse one while the offset was 0 and 1
away). Banding raw ring1 clips a meaningless axis and silently loses keys; a
first attempt did exactly that, losing 2 in 120. Since `ring1 = start1 − offset`
the band is a **diagonal** in (ring1, start1) space, so it is enumerated as
explicit pinned pairs — `search_range` holds rectangles only, and growing it is
measurably bad for the hot path. Verified by **equivalence**: 390 paired trials
over L=60/80/110, K=2/3/5, four seeds, byte-identical to searching all 26
offsets.

This is what makes the flag pay off where it used to lose. On `-r A.. -g A..` at
`K=2`: 110 864 keys unstrided, 162 032 with the old full enumeration (a net
**loss**), 75 932 with the band — a **1.46× win**. On `-r A.. -g ...` the saving
goes 1.86× → **1.97×**. A non-fatal warning fires in the one case the band
cannot rescue — `ring1` pinned by the caller, so there is no band to apply — and
it compares the **whole strided run against not striding**, not the refinement
against the coarse pass. Those are different questions: the coarse pass shrinks
as `K` grows, so at a large `K` a refinement bigger than it is the normal case,
not a problem. Requires both `-r` and `-g` to wildcard the rightmost wheel's
position (else every ring2 value is distinct and necessary, same precondition as
the leftmost wheel's exact collapse above); rejected together with
`-F`/`--exhaust` (the refinement's `key_to_machine(best.idx / restarts_par,
...)` reconstruction shares `--polish`'s dependency on the "simple sweep"
`best.idx` encoding).

> **`wheel_task` holds RAW wheel/reflector numbers — never rebuild one from a
> `machine`.** `init_walzen()` *translates* on the way in: under `-n` it adds
> `norway_rotor_base` / `norway_reflector_index`, so `m.walzenlage[]`/`m.ukw`
> are already-translated values. Constructing a `wheel_task` from them hands
> `search_worker()` values it translates a **second** time. That is exactly how
> the `--ring-stride` refinement came to search the wrong rotors under `-n`, and
> once the §7.12 collapse landed (mask keyed on raw index) the doubled values
> hit an unbuilt row and skipped every key, so the refinement silently found
> nothing. Pass `tasks[cur_wo]` through verbatim instead. **It is invisible in
> standard and M4 mode**, where raw == translated — so anything touching
> `wheel_task` needs a Norway test, or the whole suite will pass over a broken
> feature (it did).

**The refinement must re-open ring1/start1, not pin them to the coarse winner**
— the one place the initial design (and the measurement harness, which
independently re-optimizes ring1/start1 per candidate ring2) got it wrong. The
coarse winner's ring1/start1 are only optimal for *its own* possibly-corrupted
ring2 row; a nearby ring2 — including the true one — can need a different
ring1/start1 entirely. Confirmed by a concrete miss during implementation
testing: a manually-constructed key whose true ring2 fell inside the refinement
window still failed to recover, because a completely different
(also-approximated) ring1 had outscored the truth's row in the coarse pass. The
refinement now re-opens ring1/start1 to the *original* search's bounds
(`range.r_min/ max[1]`, `range.g_min/max[1]` — this collapses back to a pin
automatically if the caller had explicitly pinned ring1 rather than wildcarding
it), matching the measurement harness's actual per-candidate re-search rather
than the cheaper "just check `K` neighbours" reading of the total-cost
accounting. Full measurement, the K=2..13 sweep, and this implementation gotcha:
`archived/PERFORMANCE.md` §7.11.

**Two further implementation-only bugs, caught by a targeted boundary sweep
(true ring2 pinned to A or Z) rather than the original spot checks:** the
refinement window must *wrap* at the 0/25 ring2 boundary, not clamp — ring2 is
circular, so a coarse winner at A(0) with the true ring2 at Z(25) is a
documented-"recoverable" case that a clamped window silently never checks; fixed
by splitting into up to two contiguous segments when the window would wrap
(`search_range` can only express one contiguous interval per wheel position, so
a genuinely modular range needs two sub-searches). Separately, the refinement
was reading `m.ringstellung[0]`/`m.grundstellung[0]` fresh from the live machine
to pin each segment — but on the plain-scan path `search_worker()` deliberately
leaves those fields in whatever state the *last scanned key* stepped them to (a
documented "lazy restore" perf optimisation; only the hillclimb path restores
per key), so a second segment picked up a stale, stepped pin instead of the
coarse winner's actual value. Fixed by snapshotting everything each segment pins
(ring0/start0/wheel order/reflector/Greek wheel) into locals once, before any
segment runs, instead of re-reading the live machine between segments. Both
confirmed with concrete failing keys before the fix and a 0/100 targeted sweep
after (cross-checked against the K=1 baseline to rule out the separate,
pre-existing scoring-floor cases, which neither fix touches). Full writeup:
`archived/PERFORMANCE.md` §7.11.

**The refinement skips the coarse winner itself** — phase 1 already scored that
exact ring2 over a *superset* of what phase 2 searches there (phase 2
additionally pins ring0/start0 to the winner's values), so retesting it can only
reproduce the same score.

**ring2 is carried as an explicit value list, not a range.** `search_range`
holds `r2_vals[26]`/`r2_n` (filled by `set_ring2()` from a 26-bit mask, bit *v*
= "test ring2 *v*"), and `rc[2]` is just its length. It is the one ring position
that can be a *non-contiguous* set — `--ring-stride`'s coarse pass samples `{0,
K, 2K, …}` and its refinement tests a wrapped window minus the coarse winner —
so a plain `[min,max]` interval cannot express it. `r_min[2]`/`r_max[2]` still
describe the caller's requested *bounds* (`build_key_space()` derives the list
from them by stepping `opt_ring_stride`); every decode site reads the list,
never the bounds:

```c
r3 = range.r2_vals[rr % rc[2]];      // was: range.r_min[2] + (rr % rc[2]) * opt_ring_stride
```

This is what makes the sparse set uniform rather than special-cased. Three
things fall out of it: the coarse set, the boundary wrap, and the excluded
centre are all *just different masks*; the refinement is **one** search rather
than up to three (the excluded centre can split the window into three contiguous
pieces — centre=1, half=2 → {25}, {0}, {2,3} — which a value list absorbs at no
cost); and the decode no longer consults a global, so the
`save_stride`/`opt_ring_stride = 1` save/restore around the nested refinement is
**gone**.

**`unsigned char`, not `int` — this is load-bearing.** `search_range` is read by
`search_worker()`'s per-key decode, so its size matters: an `int[26]` list grew
the struct 48 → ~156 bytes and cost a *measured* ~5% on the `search` benchmark
under g++; bytes bring it back to ~80 and the regression disappears. See the
noise-floor note under "Build & run" — that regression was only distinguishable
from jitter because the `search` tier's floor is ±0.5% while `hillclimb`'s is
±4.5%.

**Do not shrink it further to just the mask.** Storing the 26-bit mask alone (52
bytes, decoding via a `m &= m-1` select loop + `__builtin_ctz`) was built and
measured against a same-session control: clang neutral, g++ ~1–2% *slower* on
`search`. The returns are not linear in struct size — 156→80 was worth ~5%,
80→52 is worth nothing — so the O(1) indexed load stays. Full numbers and the
arm64 caveat: `archived/PERFORMANCE.md` §7.11. That save/restore was the direct
cause of the first corruption bug here, so this removes the bug class, not just
the instance. `opt_ring_stride` now survives only in `build_key_space()`
(building the mask), the `> 1` guards, and option parsing — never in a decode.
Verified byte-identical to the previous implementation across 200 comparisons
(40 random keys × K=1/2/3/5/13). K=1 yields the full contiguous list, i.e. the
unstrided search exactly. The mask-then-expand idiom mirrors
`build_key_space()`'s existing `seen[]`→`offset_list` handling of the M4 Greek
wheel.

### Tuning the rotor phase instead of enumerating it — `--tune-phase`

Every reduction above removes keys the machine cannot tell apart.
`--tune-phase N` is a different move: it takes an axis the machine *can* tell
apart — the middle and right wheels' **phase** — out of the enumeration and
**optimises** it per key instead.

Split each stepping wheel's `(ring, start)` into two coordinates:

| coordinate | definition | what it reaches |
|---|---|---|
| **offset** | `(start − ring) mod 26` | the whole substitution |
| **phase** | `ring`, `start` shifted to hold it | *only* the notch timing |

Wheel 0's phase reaches nothing at all, which is why §7.10 collapses it
outright. Wheel 1's reaches only the notch, rarely, which is why §7.12's
collapse is partial and length-dependent. Wheels 1 and 2 keep a real phase
axis — and it is a **smooth** one: a wrong phase mistimes the notch, so the
decode is right until the first mistimed step and drifts afterwards, corrupting
roughly `26·δ/L` of the message for a phase error `δ`. Score against phase
therefore has a peak at the truth rather than the flat noise a wrong *offset*
gives, and can be searched.

**The order is load-bearing, and it is the whole design.** Scoring a rotor key
with no plugboard is noise — a rotor-only decrypt under a full board is ~95%
scrambled, the same reason `-F`'s tier 1 is a climb and not a scan. So each work
item runs:

1. climb the plugboard at the starting phase (the ordinary `-c` climb);
2. **freeze that board** and scan all 26 × 26 phases, keeping the best;
3. re-climb the plugboard at the winning phase;
4. repeat 2–3 until neither improves (capped at 4 rounds; it converges sooner).

Step 2 is **scanned, not climbed**: the axis is only 26 × 26 and the score along
it is not reliably monotone (the peak is correct far more often than the path to
it is uphill), so steepest ascent can stall short of the truth. The scan costs
about half a plugboard climb — against the 676 plugboard climbs it *replaces*,
since the outer sweep no longer enumerates the phase subspace at all.

**Measured** (L=439, 10 plugs hidden, board frozen): the score peaks at the true
phase in **8/8** trials when the starting phase error is ≤ 5, with a wide margin
(−4.98 against −7.1…−7.6). It degrades with distance — 67% at 7, 33% at 9, 25%
at 13 — and the failures score badly *even at the true phase*, i.e. the climb
started from too much corruption to recover a usable board at all. So there is a
**capture radius**, and it grows with length: distance 13 recovers 25% at L=439
but 67% at L=900, i.e. roughly `0.4·L/26`. That is why `N` is a knob rather than
1 — `N` starting phases per wheel put the worst case `13/N` away.

Implementation notes worth knowing before touching it:

- **`build_key_space()` reparameterises rather than adding a special case.**
  `ring_i` is pinned to the `N` starting phases (`r_min = 0`, `r_max =
  (N−1)·step`, `step = 26/N`) and `start_i` is left over all 26, so `start_i`
  *is* the offset — the same reparameterisation wheel 0 already uses.
  `search_range` carries the stride as `unsigned char r_phase_step`, placed in
  existing padding so the struct stays 80 bytes (see the `--ring-stride` note
  above on why that size is load-bearing).
- **The winning key is no longer derivable from the work index.** `tune_phase()`
  moves ring1/start1 and ring2/start2, so `best.idx` identifies only where the
  climb *started*. `best_result` therefore records the ring/start alongside the
  plugboard at the merge, the merge does **not** restore the index-derived key
  before echoing, and `--polish` overwrites `key_to_machine()`'s reconstruction
  with the recorded one. Getting any of the three wrong prints a key that does
  not decrypt to the plaintext on stdout.
- **The restarts of one key can no longer share the machine.** `search_worker()`
  normally builds the rotor stack once per key and reuses it across that key's
  `-R` restarts; `tune_phase()` leaves it on the phase *it* found, so restart 1
  would start from restart 0's phase — losing the independence `-R` relies on
  and, worse, making the result depend on which thread ran which restart. With
  the option on, the key is rebuilt per work item.
- **`setup_mapping()` steps `grundstellung`**, and `tune_phase()` calls it
  outside `search_worker()`'s usual restore, so every call is followed by a
  fresh `init_ring_grund()`. The first version did not, and reported `AGD RET`
  for a key whose true value was `AGD QMW` — the left wheel had stepped once
  over 439 characters.
- **A rejected phase move must restore the board too.** The tentative re-climb
  in step 3 is not guaranteed to end above the current score (a staged `--score`
  schedule optimises an earlier model first), so the board is snapshotted before
  it and restored if the move loses; otherwise the returned score and the
  machine the caller merges describe different things.
- Rejected with `--ring-stride` (both reparameterise the ring positions), with
  `-F`/`--exhaust` (both re-encode the work index) and with `--crib`/`-A`.

#### How the split moves with length

§7.15 measured the flag at **one** length, and the obvious question it left is
whether the trade survives at operational lengths, where the capture radius is
wide and the wrong-offset failures should thin out. Measured at L=300 (80 paired
trials) and L=450 (40), same design as §7.15 — same corpus, same recipe
(`-c -f -l english -J -S m4f10 --polish`, reflector B, wheels 231, ring0/start0
pinned, `-T 4`), 10-pair board hidden, and the budget **re-calibrated at each
length** as §7.15 requires, since §7.12 collapses more of the exhaustive arm's
keyspace as `L` falls:

| L | arm B keys | arm A `-R` | wall B / A | B mean / exact | A mean / exact |
|---:|---:|---:|---|---|---|
| 200 | 169 676 | 24 | 28 s / 28 s | 91.0, 51/80 | 85.5, **63/80** |
| 300 | 237 276 | 32 | 78 s / 79 s | **98.8**, 65/80 | 94.7, **74/80** |
| 450 | 338 676 | 42 | 168 s / 166 s | 100.0, 38/40 | 100.0, 39/40 |

**The split holds at L=300 and dissolves by L=450.** At 300 letters
`--tune-phase` still breaks more messages (74 against 65, McNemar p = 0.049, 13
only-A against 4 only-B) and still scores lower on the graded metric (−4.2pp,
95% CI [−8.9, +0.6]) — the same shape as L=200 with both arms shifted up. At 450
the arms are **indistinguishable**: identical means, one discordant pair,
p = 1.0.

**It dissolves because the problem stops being hard, not because the flag pulls
ahead.** That distinction is the whole result, and it is visible in the failure
shapes rather than the headline:

| | L=200 | L=300 | L=450 |
|---|---:|---:|---:|
| A misses / of which catastrophic | 17 / **12** | 6 / **4** | 1 / **0** |
| B misses / of which catastrophic | 29 / 6 | 15 / **0** | 2 / 0 |

A's catastrophic rate (a miss under 20% correct — a wrong *offset*) falls
12/80 → 4/80 → 0/40, which is the direction the capture radius predicts: the
radius `≈0.4·L/26` passes the largest possible starting-phase distance (6.5,
since `--tune-phase 2` starts from ring ∈ {A, N}) at about **L=420**. But B's
catastrophic misses hit zero *first*, at L=300 — past that length the exhaustive
arm cannot fail badly at all, because the true key is always in its keyspace and
a plugboard miss still returns ~94% of the letters. So the reason to prefer the
exhaustive sweep weakens with length at the same time as the reason to prefer
the flag does.

**The mechanism explains the RATE but not WHICH trials fail.** Bucketing trials
by the cyclic distance from the true ring to the nearest starting phase — the
quantity the capture radius is about — does not separate them: the catastrophes
sit at distances 3, 4 and 5 at both L=200 and L=300, and at L=300 the *worst*
bucket (distance 5–6) recovers 29/32. Distance 0–1 is the one clean signal (no
catastrophe at either length, but only 3 and 5 trials). So these behave like
climb failures at the starting phase rather than pure capture failures, and
raising `N` is not obviously the fix that reading would suggest.

**And below matched compute it pays outright.** Once both arms saturate the
matched-wall-time question stops discriminating, and the useful one is how far
*below* the exhaustive sweep's cost `--tune-phase` can go — which is where a
125× smaller keyspace should show up. Swept over the same 40 instances at L=450
(`eval/tune_phase_budget.py`, results `...-L450-budget.jsonl`):

| `-R` | mean %-correct | exact | wall/trial |
|---:|---:|---:|---:|
| 2 | 95.0 | 36/40 | **5.8 s** |
| 4 | 98.7 | 37/40 | 11.8 s |
| **8** | 98.9 | **38/40** | **23.4 s** |
| 16 | 98.9 | 38/40 | 46.6 s |
| *exhaustive arm B* | *100.0* | *38/40* | *171.5 s* |

**`-R 8` matches the exhaustive arm's 38/40 for 23.4 s against 171.5 s — 7.3×
cheaper — and saturates there**, `-R 16` being identical outcome for double the
time. `-R 4` gives up one break for 14.5×. So at operational lengths the flag is
not a trade at all: it is the same result for a seventh of the compute, and the
right operating point is a *low* restart count, nowhere near the `-R 42` matched
compute forced.

The residual is not budget-limited. Of the three non-exact trials, two sit at
the same %-correct at every `-R` (58.0% and 99.56%) and so are untouched by more
restarts; only one moves (5.78% at `-R 2`, exact from `-R 4`). That is what
saturation looks like from the inside.

Raw data: `eval/results-tune-phase-L300.jsonl` / `-L450.jsonl` with `.txt`
summaries; `eval/tune_phase_vs_restarts.py` and its report script, which prints
the failure-shape table above.

### Performance notes

> **The profile is completely different in the two regimes, and only the
> hill-climb one was ever measured.** Under `-c` the score loop really is
> everything (callgrind, `-R 2`: **92.8%** scorer, `setup_mapping` under 0.1%).
> On a **plain scan** it is not even the largest item — at 300 characters it is
> **53.8% `setup_mapping`**, 20.9% scorer, 12.5% `precompute`; at 88 characters,
> 33.7% / 12.9% / 26.4% with a further ~21% in the one-off n-gram load. That is
> the regime `--ring-stride`, the crib sweeps and the `search` bench tier live
> in, so "optimise the scorer" was the wrong instinct there for years. Four
> things came out of it, worth **`search` −60.7% against the pre-fix `dev`** in
> total, with the hill-climb untouched throughout:
>
> - **`mod26()` was a magic-constant division**, and `setup_mapping()` called it
>   six times per character while `rotor_l`/`rotor_r` did twice each per rotor
>   stage. Every input was already in `[0, 25]`, so `step26`/`diff26`/`add26`
>   (compare + cmov) replaced them. **Do not fold those into a general
>   `mod26()`** — that took `x >= -26` with *no* upper bound and the
>   `--ring-stride` refinement passes values reaching 50, where one conditional
>   subtract is silently wrong. It has since been deleted; `mod26_full()` is the
>   only general form left and has exactly three callers.
> - **`setup_mapping()` recomputed what only moved by one.** The ring offsets
>   advance in lockstep with the positions, and the row address moves a constant
>   26 bytes, so both are carried incrementally instead of re-derived (the 3-D
>   index cost two multiplies per character). −31%.
> - **`precompute()`'s stack factors** as `R2(g3) ∘ [ … (g1,g2) only … ] ∘
>   L2(g3)`, so the middle is built 676 times rather than 17 576 and the right
>   wheel's permutations are tabulated once: three table lookups per letter
>   instead of seven rotor applications. **10.8×** — where counting only the
>   lookups predicts 2.2×, the rest being the arithmetic around them.
> - **`setup_mapping()` tested both notches once per character** to learn
>   something almost always false. On most characters only the right wheel
>   moves, and while it moves alone the row just walks 26 bytes, so the loop
>   jumps event to event instead: `notch_gap[w2][g2]` gives the characters
>   before the right notch fires, and the middle wheel's own notch cannot fire
>   mid-run because `g1` does not change there. A run is a branch-free pointer
>   fill (split at the ring-offset wrap so even that test leaves the inner
>   loop). A further **−65%**.
>
> The profile that leaves is **52.0% scorer / 19.7% `setup_mapping`** at 300
> characters and **30.3% / 13.1%** at 88 (plus ~33% one-off n-gram load there,
> now dominated by the *text parse* rather than the log10s, which a memo on the
> integer count cut by 90%);
> `precompute` is down to **1.0%**. So the plain scan now looks like the
> hill-climb — scorer-dominated — and "optimise the scorer" has become the
> right instinct here for the first time. The `precompute` figure also retired
> a queued idea: its inner loop is the only `vpshufb`-shaped kernel in the
> program, but
> there is no longer 1% in it to win, so SIMD cannot repay a NEON path for the
> arm64 CI. Profile the regime you are actually changing before believing any
> share-of-runtime figure below.

The n-gram score loop (`quadgram_score_decode`) is where ~99% of runtime is
spent when hill-climbing. That is why the rotor stack is precomputed into
`subst_array` and reached per position through `rows[pos]` so each character
costs just two plugboard lookups plus a table lookup (`decode_at`). For the scan
`rows[pos]` points straight into `subst_array` (no per-position copy, and the
scan skips its `decode()` pass, materialising the plaintext only for a new
best); hill-climb copies each row into a contiguous `mapping[]` for locality
across its many re-reads. The four scorers **fuse decoding into the score
loop**: each character is decoded once into a small sliding window of the last
*n* decoded letters that indexes the n-gram table, so the decoded message is
never written to / read back from a scratch array (this is faster than the
previous `decode_num` → `num_plaintext` → score two-pass on both compilers,
markedly so on clang/ARM). **Quantified once** (Bench CI, `make bench LONG=1`
A/B storing-into-`m.plaintext` vs fused, hillclimb hot path): g++ **+6.8%**
(x86_64) / **+7.0%** (arm64), clang **+28.4%** (x86_64) / **+45.6%** (arm64)
slower with the store; the scan is +0.6–2.6% under g++, +8.8–9.9% under clang.
So the store is small-but-negative on g++ and a large regression on clang — the
reason the fused form is kept (measurement PR #77, store-variant commit
`71d8633`; the plaintext is materialised lazily only on a new best, so nothing
needs it per-scoring). An even earlier 16-byte-blocked decode was never shown to
win and was removed too; the scalar fused loop is the current form.

The tables the scorers read are **uint8 fixed-point**
(`mono8`/`bi8`/`tri8`/`quad8`/`all8`, per-table `bias` *and* per-table `scale`),
not float,
and each scorer sums uint8 into a `long` (exact and order-independent, a small
determinism bonus) then recovers the log-prob sum as `isum/scale + n·bias`. This
is
a cache win for **quad** — the hot, largest table: shrinking it to 0.45 MB (from
1.8 MB float / 0.9 MB int16) keeps it cache-resident during the **scan**, where
every key decodes a fresh message and hits cold cells. The narrowing happened in
three shipped steps, each measured on `make bench`: float→int16 (0.9 MB) gave
**search −16/−17%**, then int16→uint8 (0.45 MB) a further **~−4/−6%** (~−20%
total vs float). It is machine-dependent (the win is the cache-level latency
gap, so it shrinks or vanishes on CPUs where the boundary sits elsewhere). The
**hill-climb re-scores one message** and keeps its few cells warm, so it gains
less. mono/bi/tri are tiny and already L1/L2-resident: 8-bit gives them **no
measurable speed-up**; they carry it only for representational consistency.
Nothing clips, because the scale is fitted to each table
(`scale = 255/(vmax − vmin)`) rather than fixed — the span is bounded in the
first place by the hapax floor on unseen grams (`log10(1/total)`), which caps
each table at `log10(max_count)`, but the fitted scale is what spends all 256
levels on it whatever that span turns out to be (a fixed 32 left danish quad
reaching only byte 172). Recovery is **neutral** vs int16
(`make crackquality`, 160 trials/length, all four languages). Rejected
precision/SIMD alternatives on record: 16-bit and lower resolutions were the
step *before* 8-bit (8-bit won); `-march=native` (no win — the gather-bound loop
does not auto-vectorise), hardware SIMD gathers (latency-bound, not
throughput-bound), and the delta-scorer (`archived/SIMULATED_ANNEALING.md`
§6.2). 4-bit would need <16 levels over a ~8-unit range and is not viable.

> **Struct layout matters for the hot loop.** When the per-search state moved
> into `struct machine`, collapsing the formerly separate global arrays into one
> object cost real throughput, and the size of the hit is compiler/arch
> dependent (negligible for g++, but ~20–60% under clang/Apple-silicon if done
> naively). Three things keep it in check and must be preserved: (1) the 457 KB
> `subst_array` is **heap-allocated separately** and reached through its own
> pointer, so it never pushes the hot per-character tables (`mapping`,
> `steckerbrett`) out to large struct offsets (large immediate offsets are
> expensive on ARM); (2) the decode/score loops hoist the member base pointers
> into `__restrict` locals so the compiler keeps the no-alias guarantees the
> separate globals used to give, and the scorers fuse the decode so no
> `num_plaintext` scratch array round-trips through memory; (3) `setup_mapping`
> holds the rotor positions (`grundstellung` etc.) in locals across its
> per-character loop — stepping them through the struct member each character
> could not be proven not to alias the `mapping[]` store and cost ~10–14% on the
> search path (worst on ARM). With all three, both paths are at parity vs the
> pre-struct baseline on g++ and clang. Always re-check `make bench BASE=<ref>`
> under **both** g++ and clang (`make bench CXX=clang++ BASE=<ref>`) after
> touching the hot path or the struct. The **climb move loop** is
> aliasing-sensitive the same way (Part D finding): its `plug_fixed` reads must
> stay a **plain-global** access — routing them through a struct member, a
> `thread_local`, or an opaque pointer parameter cost ~18% on one compiler or
> the other. Hence the `template<bool EX>` climb chain (the common `EX=false`
> instantiation folds to the plain global; only `--exhaust` uses `EX=true`) and
> the compiler-conditional storage for the exhaust scratch (`PLUG_FIXED_EX`:
> `thread_local` under clang, a `machine` member under g++ — each compiler's
> measured-neutral form; verified byte-identical to the clean build under
> clang).
>
> **Only the two instantiations another unit CALLS are exported**, and the
> narrowing is the case study for proving a codegen-neutral change with
> `objdump` rather than with a bench. `hillclimb<false>` (search.cc) and
> `run_stages<true>` (`--exhaust`, the crib hybrid, the self-crib seeder) are
> named in `plugboard.cc`; `hillclimb<true>` and `run_stages<false>` were too,
> with **no caller outside the file** — the first is reached from
> `run_stages<true>`, the second from `optimize_once`. Dropping the two dead
> exports leaves **every remaining function instruction-for-instruction
> identical on both compilers**, `hillclimb<false>` included: the only change is
> that `run_stages<false>`'s standalone body disappears while `optimize_once`
> stays at its exact 550 (g++) / 508 (clang) instructions — i.e. it had always
> been inlined there and the explicit instantiation was forcing a second,
> unreachable copy to be emitted. A bench could not have established that (the
> effect is far below the floor); the disassembly settles it outright, and the
> bench run then doubles as a free base-vs-base control — it read −2.5%…+2.9%
> across the quick tiers on byte-identical hot code. Also beware: the climb/scan
> benches on a shared box can be **bimodal**
> — before trusting a regression, re-run and check base-vs-base; disassembly
> comparison (`objdump -d`) settles whether codegen actually changed. The
> shipped form was verified neutral **end-to-end on Apple-silicon clang** (M2,
> the layout-sensitive target): `make bench LONG=1` vs a pre-REDESIGN base, all
> four benchmarks within ±0.7%.

## Conventions & gotchas for contributors

- **Code style.** Allman braces (every `{` and `}` on its own line), 2-space
  indentation, and no tabs anywhere in `src/`. Continuation lines (e.g.
  wrapped parameter lists or `if` conditions) are aligned under the opening `(`.
  The only tabs in the repo are the recipe lines in the `Makefile`, which `make`
  requires.
- **Parse every numeric input with `parse_opt_int`/`parse_opt_double`/
  `parse_opt_u64` (`common.h`), never `atoi`/`atof`/bare `strtol`.** They
  cannot report failure — `atoi("abc")` is 0 — and **0 is the *off* value for
  most options here**, so a typo did not fail, it silently disabled what was
  asked for and the settings echo (keyed on the same value) then printed
  nothing to show it. `-R 64O` ran with no restarts and left no trace in the
  log. The helpers reject non-numbers, trailing text and range errors, naming
  the option in the message; callers still apply their own bounds afterwards.
  The rule extends to `$ENIGMA_*` overrides, where a silently-zeroed
  `ENIGMA_IC_BLEND` turns `-f` into `-a` and makes a probe measure the
  baseline it was meant to be compared against. Empty means *unset* for every
  value-carrying override, so `FOO=` still turns a probe off. **CI would not
  have caught this, and the reason is a version skew worth knowing about**: the
  check is `bugprone-unchecked-string-to-number-conversion`, which *is* inside
  the enabled `bugprone-*` group with `WarningsAsErrors: '*'` — but the runner's
  clang-tidy does not report it, so `dev` was green while a local LLVM 22
  clang-tidy reported **26 errors, 24 of them this check**. So a green
  clang-tidy job is evidence about the runner's version, not about the
  enabled check list; run it locally on a recent LLVM before assuming a group
  is being enforced.
- **Always brace the body of an `if`/`else`/`for`/`while`, even when it is a
  single statement.** The bare form is a live hazard here, not a matter of
  taste: adding a second statement to an unbraced body silently leaves it
  *outside* the condition, and the resulting bug is invisible to every gate
  this repo has except the compiler. That happened during the `src/` split —
  rewriting

      if (opt_crib_text)
        crib_stop_at = g_crib_stop_shown = crib_first_stop(m);

  into two statements (the second becoming an accessor call) left the accessor
  running on **every** key. `make test` stayed 541/541 and all 49 cases of a
  byte-comparison harness stayed identical, because none of them distinguishes
  the two; only `-Wmisleading-indentation` caught it, on both compilers. Braces
  make that edit impossible to get wrong, which is worth more than the line
  they cost.

  **This applies to new and edited code only — the existing bodies are not
  being rebraced.** There are ~517 of them across `src/`, and touching them all
  would be a large diff that makes every later `git blame` and bisect worse for
  no reader benefit, exactly as with the 80-column rule below. A file staying
  mixed for a long time is expected, not a defect to tidy up in bulk.
- **Line width: 80 columns.** From now on every file written by hand here —
  source, scripts, and documentation including Markdown — wraps at **80
  columns**, so it reads in an 80-column terminal without wrapping or horizontal
  scrolling. Measure **display width, not bytes**: multi-byte characters are
  fine when they occupy one column (`—`, `×`, `≈`, `§` are three bytes each but
  one column wide), and these files use them constantly. **`awk 'length'`
  silently counts BYTES when the locale is not UTF-8**, and `LANG` is unset in
  this environment, so bare `awk` over-reports by a few percent and `wc -c` is
  worse. Either set the locale or count in Python:

      python3 -c "import sys for i, l in enumerate(open(sys.argv[1],
      encoding='utf-8'), 1): if len(l.rstrip(chr(10))) > 80: print(i, len(l))"
      FILE

  Exempt, because breaking them would do more harm than the overflow:
  - long URLs, file paths, and other unbreakable tokens;
  - Markdown tables and fenced code blocks whose content cannot wrap;
  - literal data — n-gram lines, sample ciphertext, captured tool output;
  - imported and generated files, which are left exactly as they arrive.

  **Documentation is now compliant; code is not, and is not being reflowed.**
  `CLAUDE.md`, `ENHANCEMENTS.md`, `README.md` and `CHANGELOG.md` all hold to
  80 (bar a handful of unwrappable lines inside fenced blocks). The code is
  ~21% over and `tests/run_tests.sh` ~36%; reflowing those would touch thousands
  of lines and make every later diff noisier for no reader benefit, so apply the
  rule to new and edited code and leave the rest. A source file staying mixed
  for a long time is expected, not a defect to tidy up in bulk. The files under
  `archived/` are frozen history and exempt outright.

  `tests/reflow_md.py` does both jobs: `--check FILE...` reports violations and
  exits non-zero, `--write FILE...` rewraps in place. One of the two is
  required, so it cannot edit a file by accident. Reflowing prose safely is
  harder than it looks, and that tool exists because both of the traps below bit
  during this repo's own reflow. Two traps hit in practice: a naive paragraph
  reflow **merges consecutive list items** into one run-on block, and wrapping
  can place a token like `>` or `-` at the **start** of a line, where Markdown
  silently reads it as a blockquote or list marker rather than as prose. Guard
  with a token-identity assertion — the word sequence must be unchanged, only
  the line breaks may move.

- **One module per concern; per-search state in `struct machine`.** The program
  was a single 9808-line translation unit until PRs #205-#219 split it into
  `src/` (`args`, `cli`, `common`, `confidence`, `crib`, `exhaust`, `keyspace`,
  `machine`, `main`, `ngrams`, `options`, `plugboard`, `preflight`, `progress`,
  `refine`, `schedule`, `scoring`, `search`, `text`, `wiring`, plus two
  header-only ones: `result.h`, which holds the shared best because the workers
  write it and the display reads it and neither owns it, and `parallel.h`,
  which is a template and so has nothing to compile once).

  **The module boundaries were drawn by what must stay PRIVATE, not by
  subject.** Three cases decided their own shape and are worth knowing before
  moving anything between modules: `plug_fixed`/`plug_fixed_ex` are read in the
  climb's move loop and their storage form is measured to matter by ~18%, so
  simulated annealing lives in `plugboard.cc` rather than its own module purely
  because `apply_toggle()` reads them; the crib menu tables are read inside
  `crib_try`'s loop, so the units that climb from a deduced board are in
  `crib.cc` with them rather than in the search; and `ngrams.cc` never names
  `quad8`/`all8`, because the loader taking a destination pointer is what keeps
  the hottest table out of a shared header. Splitting any of those "by subject"
  would export exactly the state the measurements say to keep local.

  **`refine.cc` is the one boundary drawn the other way, and the distinction is
  CALL FREQUENCY.** The `--ring-stride` refinement was 354 lines inside
  `bruteforce()` — the largest single thing in the file, with a contract
  separable from the sweep and a design document of its own
  (`archived/refinement.md`). Extracting it exports `search_worker()`, which
  sounds like exactly what the rule above forbids until you count: it is called
  once per **chunk** (~16 × threads per sweep) with the per-key loop inside it,
  where `plug_fixed` is read per **move**. A cross-unit call at chunk frequency
  is free, and the long-tier bench across the extraction reads ±1.3% on every
  tier against a base-vs-base floor of the same size. So the rule is not "never
  export a search symbol" — it is "never export one that a hot loop reads", and
  the two are told apart by measuring, not by which module the name looks like
  it belongs to.

  The mutable
  per-search state — machine settings (`walzenlage`, `grundstellung`,
  `ringstellung`, `ukw`, `steckerbrett`) and the working buffers (`subst_array`,
  the per-position row pointers `rows`, the contiguous `mapping`, the candidate
  `plaintext`) — is bundled into `struct machine`, threaded through the
  search/scoring functions as `machine & m`; `bruteforce()` in `src/search.cc`
  heap-allocates one per worker thread (it is far too big for a stack frame).
  This makes the search reentrant (the precondition for multi-threading — see
  `archived/CODE_REVIEW_HISTORY.md` §5/§6). The read-only data stays file-scope
  global and shared: the numeric wiring tables (`rotor_fwd/rev`, `notch`,
  `notch_gap`, `reflector`) that `init()` derives from `wiring.h`'s alphabets —
  private to `machine.cc` since the split, the one exception being
  `notch_halfperiod`, which the two-notch collapse reads per key on the scan
  path — the n-gram tables, and the `ciphertext` / `num_ciphertext`
  / `textlength` input. (`-Wshadow` is on; the redundant
  `textlength`/`ciphertext`/`plaintext` parameters that used to shadow the
  globals were removed earlier.)
- The live diagnostics are `showconfig` (echo the winning key + plugboard on a
  new best) and `show_settings` (echo the resolved config at startup). Progress
  lines are fixed-width columns under a one-time header (`Score W R G S Text`,
  printed by `showconfig_header` before the first line —
  `best_result.header_shown`): score, reflector+wheels, ring, start, plugboard
  (room for all 13 pairs) and a preview of the decoded text — 19 characters for
  3 wheels, 16 under `-4`, 15 and 12 with the crib column, each chosen so the
  line lands on **exactly 80** whatever the mode. The preview is decoded on the
  fly from the machine's *current* board (`m.plaintext` can be stale
  mid-climb). With `-c` the echo is per plugboard IMPROVEMENT, not per
  finished climb: every accepted climb/SA move whose (target-model) score beats
  everything echoed so far prints a progress line (`report_climb_progress`,
  called on accepted moves only — nothing on the 325-move scoring scans, so the
  hot path is untouched). Display state lives in `best_result.shown` (atomic),
  never read by the merge logic, so which candidate WINS stays
  `-T`-deterministic; which lines appear is thread-timing dependent, as before.
  Staged pre-pass stages and the `-F` tier-1 filter score in a different model
  and stay silent (`m.report` + target-model gate). The unused debug scaffolding
  has been removed: the `SHOWHILLCLIMB` compile-time climb-trace path (and its
  vestigial per-climb `iter` counter), the `#if 0` blocks and the dead
  `ciphertext_letterdist`/`compare`/`count`/`order` cluster that only fed one,
  and the earlier `all_subst_score`/`map`/`opt_threads`/`opt_logfilename` dead
  code (see `archived/CODE_REVIEW_HISTORY.md` §3).
- Index conventions: reflectors 0–2 = A/B/C, 3 = Norway, 4–5 = M4 thin; rotors
  0–7 = I–VIII, 8–12 = Norway 1–5, 13–14 = Beta/Gamma. Norway mode applies a +3
  / +8 offset (see `init_walzen`).
- Build is plain `make` (override `CXX`, or append `EXTRA_CXXFLAGS=` for e.g.
  `-Werror`/sanitizers). Tests live in `tests/run_tests.sh` and run via `make
  test` (known-answer vectors, round-trip properties, input-limit guards, and
  end-to-end cracking — brute-force start-position and plugboard hill-climb
  matrices over every scoring model × language). Performance is benchmarked
  separately by `tests/bench.sh` (`make bench`; see "Build & run"). CI
  (`.github/workflows/ci.yml`) runs on **every PR, and on pushes to `dev` and
  `master` only** — a push to a branch with an open PR is both events, so a bare
  `push:` ran the whole matrix twice per PR; a concurrency group additionally
  cancels a superseded PR run but never a `dev`/`master` one, where each push is
  a different landed commit rather than a revision of the same one. The jobs are
  the suite `-Werror`
  under g++, **g++-14** and clang++, ASan+UBSan, ThreadSanitizer, valgrind,
  cppcheck, clang-tidy (config in
  `.clang-tidy`), and shellcheck plus a `py_compile` of the Python harness; a
  separate CodeQL workflow runs on PRs and
  weekly. Keep all of these green. **The `g++-14` cell exists because `-Werror`
  turns any warning a NEWER gcc adds into a build failure for users on current
  distros, while the runners stay quiet** — a `-Wformat-truncation` warning
  reached a Debian 13 user (gcc 14) that CI could not see on its gcc 13.3. It
  is installed by name rather than taken from the image, so a compiler upgrade
  is a commit here rather than something GitHub does on its own schedule; the
  default `g++` cell stays alongside it so the version most users have is still
  covered. **The TSan job is a handful of hand-picked invocations, not the
  suite, so a race is only caught where someone thought to point it** — it ran
  a scan, a climb and an SA climb, and a real race in the `--ring-stride`
  refinement (which fans out its own parallel sub-search) sat there unreported
  because not one of the three passed `--ring-stride`. Adding a code path that
  spawns threads means adding a case here; a fourth now covers the refinement,
  and it was verified by reinstating the race and watching it fail (exit 66).
  Note the fan-out is capped at the restart count, so such a case needs a
  real `-R` — at `-R 2` only two threads enter the sub-search.
  **The VALGRIND job has the same shape and had the same gap**, and it matters
  more than it looks because valgrind is the *only* gate for **uninitialised**
  reads — ASan does not catch them. It ran three invocations (a scan, a climb, a
  `-p` compare), so every option that DERIVES state and then reuses it went
  unchecked: the crib deduction and its seeded climbs, the self-crib closure,
  `--exhaust`'s pin sets, the `--ring-stride` refinement's derived candidates,
  `--confidence`'s sampled null, `--tune-phase`'s re-climbs, and both
  non-standard
  machines. A second block now covers all nine; each was verified clean when
  added, so a failure there is new. Two things learned building it:
  - **Use `--error-exitcode=66`, not `1`.** The program's own fatal exit is 1,
    so
    at `--error-exitcode=1` a run that dies of *"No machine configuration
    produced a score"* is indistinguishable from a valgrind finding — and a
    mis-specified case can then look like it is passing the check it no longer
    performs. (The TSan job already used 66; the valgrind job did not.)
  - **Inject on the HEAP, not the stack, when testing such a case.** An
    uninitialised *stack* read is folded away at `-O1` and the compiler warns
    (`-Wuninitialized`), so it never reaches valgrind — the injection reads as a
    passing run. A `malloc`'d read whose value reaches a branch does reproduce:
    with one in `refine.cc`, the three old cases still passed (which is the
    proof the gap was real) and the new `--ring-stride` case failed with 66.
  **The two lint jobs are the ones easiest to
  meet for the first time in CI rather than locally, and both run in seconds** —
  `clang-tidy src/*.cc -- -std=c++17` and `shellcheck tests/run_tests.sh
  tests/bench.sh` (if `shellcheck` is missing, `pip install shellcheck-py`
  installs the binary). Run them before pushing, and grep clang-tidy's output
  for `error:` rather than for the names you touched: it suppresses ~24 000
  warnings from system headers and reports its own findings at file scope, so a
  name-filtered grep looks clean when it is not. **A clean tree reports ZERO
  `error:` lines, so any count above zero is a red CI job** — an earlier version
  of this sentence said clang-tidy "prints one file-scope error", which reads as
  a standing benign one; it is not, and a `bugprone-implicit-widening-of-
  multiplication-result` error was pushed on the strength of that reading
  (`2*i` used as a pointer offset — index with a `const char *` walked two at a
  time instead). Do not stop at the count; read the line. A `Bench` workflow
  (`.github/workflows/bench.yml`) additionally runs `make bench LONG=1` on a
  {g++, clang++} × {x86_64, arm64-Linux} matrix — on PRs as a same-machine A/B
  vs the PR base at the script's default **10%**, and **advisory only**
  (`continue-on-error`), so treat a flagged cell as "re-check and compare
  disassembly", never as an automatic block (or a pass as proof). **Do not
  assume CI is the noisy end.** The GitHub runners have been observed *quieter*
  than a local dev container: a base-vs-base control in one container measured
  a **±10% floor on the clang `hillclimb` tier**, which is where an
  intermediate revision's flagged +11–19% cell came from — `objdump` showed the
  score loop was 118 instructions in both builds with only addresses differing,
  i.e. pure code placement. Measure the floor wherever you are judging, and do
  not import a number measured somewhere else.
- **Every check in `tests/run_tests.sh` must be QUICK — size the keyspace to the
  property under test, not to realism.** The sanitizer job runs the *whole*
  suite at roughly a 10× slowdown, so a check that costs 2 s locally costs ~20 s
  there, and the cost compounds across ~290 binary invocations. The suite had
  drifted to 15+ minutes under ASan because four checks did a large search
  purely as a side effect of how they were written: the worst wildcarded
  ring1/start1 (456 976 keys through a `-c` climb, ~244 s each) merely to assert
  that a *settings-echo line* was absent, and another wildcarded them for a test
  about *progress output*, which the refinement produces identically over 338
  keys. Trimming those four — with no loss of coverage; each was re-verified to
  still catch its bug — cut the job to ~162 s, and halving the n-gram load
  (`archived/PERFORMANCE.md` §7.13) took it to **~139 s**. The suite has since
  grown to 437 checks, and the **plain g++/clang job was cut 232 s → 64 s** by
  splitting the shared start-position fixture (below). It then drifted straight
  back to **245 s**: three checks added with the restart-major work space swept
  456 976 keys through a `-c` climb — 189 s, **75% of the whole suite** — to
  assert that the echoed key re-encrypts to the ciphertext, and were cut to
  1.3 s once measurement showed they caught nothing (see "a check that cannot
  fail" below). They were **full price under `TEST_QUICK` too**, since they
  spelled their own keyspace out rather than using `$rg`, so they cost ~6× as
  much again under ASan — which is how that job reached **24 minutes** while
  still passing. Current baselines to watch for drift: **~65 s plain** (541
  checks) and **~190 s under ASan** (499 with `TEST_QUICK`, of which the ASan
  build runs 498 — the M4 oversized-allocation check detects a sanitizer
  runtime and skips itself, by design).
  **Re-measure these when a PR adds checks** — nothing compares against them
  automatically, which is why the drift above went unnoticed for a day.
  Rules of thumb when adding a check:
  - **Ask what the assertion actually reads.** A settings/echo line is printed
    by `show_settings()` *before* the search, so any legal keyspace works — use
    the smallest one the option accepts. A `-T`-independence or display check
    needs the code path, not a big search.
  - **A check that asserts two runs AGREE almost never needs a big keyspace,
    and this was 72% of the plain suite's runtime.** The harness had one shared
    start-position fixture, `-g $rg` (676 keys, 26 under `TEST_QUICK`), and 48
    checks used it — but only about eight were *recovery* checks, where breadth
    is the point because the true key has to beat decoys. The rest were
    `-T`-independence and equality checks (`-R 0` equals the default, `-F 0` is
    off, the seed echoes), which establish exactly as much at 26 keys as at 676
    — and the sanitizer job had always run them at 26 via `TEST_QUICK`, so the
    plain job was paying 26× for a duplicate of an assertion already covered.
    Splitting the fixture in two (`$rg` broad for recovery and for `-F`, which
    needs more keys than it keeps; `$rgd` = `AA.` unconditionally for the rest)
    took 232 s → 64 s with all 437 checks intact.
  - **A check that cannot fail is worse than no check, and size is not what
    makes it fail — INJECT THE BUG AND SEE.** Three checks re-encrypted the
    reported plaintext under the reported key to catch a `best.idx` misdecode,
    and swept 456 976 keys to do it. Injecting the historical bug at the two
    reconstruction sites (`best.idx / 2` in place of `work_key(best.idx,
    total_keys)`, at `-R 2` so the two decodes differ) left all three
    **passing, byte-identical, at 676 keys and at 456 976 alike**. The reason
    is structural rather than statistical: the assertion is
    *self-consistency*, and the line it reads is the last progress line, which
    the ordinary climb emits — both finishers re-echo only when they
    **improve** on the climb's best, so a wrong reconstruction scores worse,
    never echoes, and the climb's own correct line stands. No keyspace fixes
    that. What does is a fixture where the finisher's reconstruction
    **determines the result**: 10 plugs so `--polish` has something to complete
    (asserted as a strict score gain over the same run without it), and a true
    ring2 inside the half `K=3` skips so only the refinement can reach it. Both
    then fail under injection, at 676 keys, for 1.3 s. **The injection also
    tells you which half was really uncovered**, and it was not the expected
    one: 21 of the 23 checks that fail are pre-existing `crack: --ring-stride
    N` recovery checks, so the refinement was already covered — while
    `--polish`'s reconstruction had **no coverage at all** until the new
    improvement check.
  - **A mis-sized fixture can mean the check does not test its property at
    all.** The three `restart-parallel` checks are about the *one-key* case —
    their own comment says "with a fully-specified rotor key the search has
    exactly ONE key, so `-T` can only speed things up by spreading the `-R`
    restarts across threads" — yet they passed `-g $rg` and so swept 676. They
    were **56 s, a fifth of the whole suite**, and never exercised the path they
    were written for. Pinning `-g AAA` made them both correct and ~500× cheaper.
    Read a slow check's comment before trimming it: the comment often already
    says what the fixture should have been.
  - **Wildcard only the positions the property needs.** A gate keyed on "ring1
    and start1 both wildcarded" still fires with ring2/start2 pinned (676 keys,
    not 457k). Pin plugs with `-s` to shrink `--exhaust` (E=2 over 26 letters is
    44 850 forced-pair combinations; over 10 free letters it is 45 × 28 and
    still spreads work across threads).
  - **Beware the options that disable a reduction.** `--true-key` turns off the
    §7.12 middle-wheel collapse and `--ring-stride`/`-F` interact with the key
    space, so a check using them pays full price where a neighbouring check does
    not.
  - **Profile before guessing.** Run an instrumented copy of the harness (patch
    `check()` to print the elapsed time since the previous `ok` line) against
    the sanitizer build with `TEST_QUICK=1`. Costs are wildly skewed — 4 of 221
    checks were most of the runtime — and the most expensive one sat *last* in
    the file, so profilers that were interrupted never showed it at all.
  - **There is a floor you cannot trim.** Each invocation loads its n-gram
    tables before doing any work, so the suite's ~290 invocations pay it ~290
    times. That load was **halved** (`archived/PERFORMANCE.md` §7.13 —
    hand-rolled parse instead of `sscanf`, and `logval` evaluated once instead
    of twice per entry): under ASan `-q` went 223 → 155 ms and `-f` 370 → 241
    ms, worth a measured **23 s** off the sanitizer job and **44 s** off the
    plain one. What remains is genuine work; below that only a cached binary
    table would help, not more test trimming.
  - `TEST_QUICK=1` (set by the sanitizer job) already shrinks the recovery
    wildcard and the language matrix. It is not a licence to write a slow check:
    it does not touch `-R`/`-A` budgets or any keyspace a check spells out
    itself.

## Status & remaining work

The still-open issues live in `ENHANCEMENTS.md`, which numbers them and points
at the detail. Everything behind them is history and sits in `archived/` —
including the documents that used to carry them (`archived/IMPROVEMENTS.md`,
`archived/cribs.md`, `archived/refinement.md`):
`archived/PERFORMANCE.md` (every measurement), `archived/CODE_REVIEW.md` (the
previous issue list), `archived/CRACKQUALITY_TESTS.md` (harness design) and
`archived/CODE_REVIEW_HISTORY.md` (the original audit, plus the design rationale
and rejected experiments). Read the archive to check a number, not to find work.

> **`archived/` is READ-ONLY. Never edit a file there, and never append to one
> — not a new section, not a correction, not a link.** It is the frozen record
> of what was measured at the time, and its value is that it does not move.
> New measurements go in `ENHANCEMENTS.md` (or here); they may *cite* an
> archived section, and the archived section stays exactly as written. The
> pattern that leads to breaking this is plausible: every past measurement sits
> in `archived/PERFORMANCE.md`, so adding §7.N+1 beside §7.N looks like
> following the convention rather than violating it. It is not — the numbering
> stopped where the archive was frozen. "Read the archive to check a number,
> not to find work" above is about not *mining* it for tasks; this is the
> stronger rule about not writing to it at all.

Most findings have been fixed — the stack buffer overflow, the
index-of-coincidence formula, the `-l`/filename overflow, the
`fscanf`/read-handling bugs, dead code, the C-style modernization, the
`textlength` global/parameter shadowing, the encapsulation of the per-search
state into `struct machine`, and **multi-threading** the search over reflector ×
wheel-order (`-T N`, default 1, max 256; each worker owns its own `machine`,
results merged under a mutex) — and the build is warning-free under `-std=c++17
-Wall -Wextra -Wpedantic -Wcast-qual -Wshadow -Wold-style-cast
-Wmissing-declarations`, and clean under ThreadSanitizer.
Scaling is ~3× on 4 cores (`make bench SCALE=1`). **M4 (4-rotor naval) mode** is
now implemented (`-4`; static Greek wheel folded into an effective reflector, so
the hot path is untouched — see "M4 mode" above and
`archived/CODE_REVIEW_HISTORY.md` §5). The **rotor keyspace** has since been cut
on identifiability grounds — settings that provably decode identically are no
longer enumerated: wheel 0's ring × start collapses totally and exactly (26×,
`archived/PERFORMANCE.md` §7.10) and wheel 1's partially and exactly (3–5× at
short lengths, §7.12), both always-on when the relevant positions are
wildcarded; wheel 2 admits no exact collapse, but its approximate
`--ring-stride` costs only ~0.5–2pp of exact recovery at `K=2`/`K=3` for
1.86×/2.61× fewer keys, so it is opt-in-and-recommended rather than the trade it
was long documented as (§7.11 — the earlier ~10–17pp figures were a measurement
artefact). These are throughput reductions, not quality levers — the recovery
frontier below is unchanged by them. On **cracking quality for short messages**
the `make crackquality` harness shows every miss is a *search* failure (the
plugboard hill-climb sticking in local optima); the search levers shipped so far
are random restarts (`-R N`), the staged climb (`-S`), the **key pre-filter**
(`-F N`, a cheap-IC-climb tier that shortlists keys so the full climb runs only
on the top `N` — ~8–20× throughput, see `archived/CODE_REVIEW_HISTORY.md` §9
item 2), and **simulated annealing** (`-A N`, tuned `χ0 = 0.12`; a peer of the
greedy restart climb at equal compute **on prose** —
`archived/SIMULATED_ANNEALING.md` §15 — but *beaten outright* on telegraphic
traffic, `archived/PERFORMANCE.md` §3.11). The heavier metaheuristics once
listed as open — tabu and **GA** — have since been **measured down**:
`--restart-tt` (PR #100) found restarts already almost never revisit a basin
(near-total exact-board diversity at `--random 10`), so a tabu visited-set has
nothing to forbid — **but that is an `-R` ≲ 64 result and it does not hold at
high `-R`.** The distinct-converged-board count per restart falls steadily with
budget: measured over 350 trials at each of four lengths
(`eval/restart_ladder.py`), **0.97 at `-R 8`, 0.79 at 100, 0.49 at 1000 and
0.28 at 5000** — so at five thousand restarts **72% of them rediscover a basin
already found**, which is exactly what a visited-set would forbid. The premise
"nothing to forbid" is therefore false in the regime where restarts are the
recommended lever; whether forbidding helps is still open, since the basins a
tabu set would push the climb into may simply be worse. Do not cite this
sentence as closing tabu at high `-R`. And an oracle probe of the GA
precondition
(`archived/PERFORMANCE.md` §3.10) found the crossover *material* exists (correct
plugs union to ~8/10 across restarts) but is **unselectable** — board-fitness
picks only ~2.5/10 and per-plug consensus is worse (~1.1/10, amplifying the
climb's decoy attractors). So the search frontier was no longer a heavier search
metaheuristic; it was **a sharper scoring model** and **restart diversity**.
Both have since been resolved, and the result closes the short-message frontier
to *smarter* methods (`archived/PERFORMANCE.md` §6.15):

- **Scoring — resolved, a win: the weighted all-order model `-a`** (PR #106). A
  log-linear (geometric / Product-of-Experts) mixture of all four n-gram orders,
  folded once into a quad-shaped table so the hot path is untouched — the
  **first measured short-message scoring gain** (+~1–2pp mean %-correct at
  L40–100, all four languages; 2000-trial German confirmed). It is now
  near-optimal on **both** scoring axes **as measured on English prose**: the
  **scoring-failure floor is ~1%** (SPLIT under `-a`: with the correct rotor key
  the true plugboard already scores highest ~99% of the time — so
  *discrimination* has essentially no ceiling left to recover), and the
  **climb-surface smoothness is flat** (an 8×
  sweep of the order weights moves search-fail% by <1pp). `-a` in fact won by
  *smoothing the climb surface* (fewer search failures), not by lifting an
  information floor. Scoring is tapped **on prose**.

  > **On AUTHENTIC TELEGRAPHIC GERMAN it is not.** The two bullets here rest on
  > `make crackquality`, whose corpus is 477 characters of English prose. Swap
  > in real HG Nord decrypts and the split inverts: same harness, same `-q`
  > model, L = 40, scoring failures go **0.0% → 56.0%**, and under the
  > recommended `-f -S i4f10 -J --polish` recipe **90%**. A stronger search
  > makes it worse rather than better, because it reaches boards a weak climb
  > never found and those overfit 40 characters with ten free plug pairs.
  > Telegraphic German is flatter to score than prose — X separators,
  > spelled-out numbers, small vocabulary — so the truth stands less far above
  > a wrong decrypt. Neither "~1%" nor "~99% search failure" below is a
  > statement about real traffic. `ENHANCEMENTS.md` item 2 and §3a;
  > `eval/joint_score_gain.py`.

- **Search — resolved, compute-bound with no selectable shortcut.** At short
  lengths **on the English-prose corpus** the residual is ~99% *search* failure
  (see the note above — on telegraphic German at L = 40 it is the reverse), and
  the coverage curve is
  **still climbing at R=256** (~+15–25pp per 4× R — the true basin is reachable,
  just a rare deep target). **It is still climbing at `-R 5000` too, on
  telegraphic German** — measured 350 paired trials per cell, judged at ≥50% of
  the plaintext recovered: the 1000 → 5000 step alone is worth +4.9pp at L=60,
  +6.0pp at L=80 and +8.9pp at L=100, reaching 30.3/58.6/84.0%. Only L=40
  saturates (flat at 1.4% exact from `-R 1000`), which is what its 98%
  scoring-failure share predicts. `eval/results-restart-ladder.txt`. The
  apparent restart diversity is an illusion: 64
  restarts give ~60 distinct *exact* boards but only **~15 distinct correct-plug
  states** (a 4× overcount — the rest is spurious-plug noise on a few basins),
  with per-restart depth ~0.7/10 and the truth assembled only in the **union
  (~9/10)**. Every *smart* lever to exploit that — recombination (GA), a
  truth-targeted kick, coarse basin-repelling — needs a truth-free way to tell
  the ~9/10 real plugs from the noise, and **per-plug consensus is only ~1.1/10
  correct** (the frequent plugs are decoys). No such signal exists in the
  converged-board population, so the **only reliable lever is raw compute** —
  more restarts via `-T`, which scales predictably.

### The unknown-key break rate — measured, and the keyspace barely matters

Every other tuning result here measures the **plugboard-recovery sub-problem
with the rotor key given**. The full problem — where the true key must also
outscore millions of competitors — was unmeasured, so a negative sweep could
never be read as "this message is probably not breakable".
`eval/unknown_key_headroom.py` closes that, without sweeping: a break needs the
climb to work **at the true key** (independent of `K`) *and* the true key's
score to clear `μ + σ·√(2 ln K)`. The second is arithmetic once you know the
true key's z, so one pinned climb plus `--confidence`'s sampled null gives every
keyspace at once — **3 s a trial instead of the ~10 h a real 80M-key sweep
costs**.

Measured, 60 trials, L=167, 10-pair board hidden, `-R 8`, authentic HG Nord
plaintext:

| keyspace | bar | z > bar | × climb | = break |
|---|---:|---:|---:|---:|
| start only (17 576) | 4.42 | 75% | 73% | **55%** |
| wheels+ring2 (27.4 M) | 5.85 | 72% | 73% | 53% |
| `-r A..` exact (230 M) | 6.21 | 72% | 73% | **53%** |

**Four orders of magnitude of `K` cost two points of break rate.** The bar grows
as `√(2 ln K)` — 4.42 → 6.21 — while the true key's z has a **median of 11.5**.
Discrimination is not the bottleneck and never was. The two real limits are both
`K`-independent: **climb failure at the true key**, and a **scoring floor**
where the true key's z is genuinely too low (minimum observed **1.1**).

**Separating those two needs care, and the obvious reading is circular.** At a
single `-R` a failed climb also produces a low z, so "low z" and "climb failed"
are the same trials and the split cannot be read off one run — doing that gave a
"~25% scoring floor" that is wrong. Judge breakability at a **high** `-R`
instead, where the climb is nearly always succeeding: at `-R 64`, **95%** of
messages are intrinsically breakable at L=167. The floor is **5%**, not 25%, and
almost all of the `-R 8` residual is climb failure — which `-R` moves.

The measured climb curve, over messages that *are* breakable:

| `-R` | 2 | 4 | 8 | 16 | 32 | 64 |
|---|---:|---:|---:|---:|---:|---:|
| wins | 50% | 68% | 79% | 87% | 95% | 100% |

**At matched wall time the middle option wins**, because the climb curve
flattens before the coverage penalty does. Per 24 h at ~11k items/s:

| option | affordable `-R` | coverage | break |
|---|---:|---:|---:|
| `-r A..` exact | 4.1 | 100% | 66% |
| **`-r A.. --ring-stride 3`** | **11.9** | **99%** | **80%** |
| `-r AA.` | 34.5 | 72% | 65% |

Same ordering at 12 h and 48 h. Exact coverage costs 2.89× for the ~1pp the
stride gives up; `-r AA.` buys restarts long past the point they help while
excluding 28% of keys outright. **Spend on `-R` until the curve flattens
(~`-R 16`), then buy coverage with what is left.**

It also gives a stopping rule, and the runs are **not** independent: the scoring
floor is common mode, so repeated attempts converge on 95%, not on 100%. Update
the posterior instead — a failed `-r AA. -R 2` (36% win) and a failed stride-3
`-R 6` (74%) take a message from a 95% prior to **76%**, and a further stride-3
`-R 12` would leave 35%.

> **These are SYNTHETIC trials, and real messages come out lower — treat the
> figures above as an upper bound on a specific message.** Every number in this
> section draws a random excerpt of authentic plaintext, a random key and a
> fresh 10-pair board. Run on the actual messages instead — real key, real
> board, real garbles — the true key's z reads **9.41 at 174 letters** against
> the median 11.5 recorded here for L=167, and **2.20 at 69 letters**, far below
> the bar of 6.15 that a 160M-key sweep sets. The crossover between "breakable"
> and "not" sits between **70 and 110 letters** and is sharp: 113 letters
> measures 7.59. `eval/real_traffic_z.py`; `eval/MODERN_BREAKING_NOTES.md` §5h.
>
> The **plugboard tier is harder on real traffic too**: at 174 letters with the
> true rotor key *given* and only the board hidden, `-R 32` fails to recover it
> (−10.1261 against the true board's −9.1212) and needs `-R 64`. So size `-R`
> against real traffic, not against the `crackquality` curve, when the target is
> an actual message. §5i.

> **How much LENGTH costs on the plugboard tier alone — the number to size a
> real attempt with.** Rotor key *given*, 10-pair board hidden, `-f -l
> wehrmacht -S i4f10`, 200 paired trials per cell on authentic HG Nord
> decrypts (`eval/prepass_ab.py --length L --restarts R`), exact recovery:
>
> | L | `-R 8` | `-R 64` | an unbroken message at that length |
> |---:|---:|---:|---|
> | 82 | 12.0% | 32.0% | MVUEH (Nr 172) |
> | 107 | 30.5% | 54.0% | RXPSB (Nr 53) |
> | 167 | 73.0% | 91.0% | BYQMZ (Nr 8-C) |
>
> **This is the sub-problem with the key already known**, so a real sweep is
> strictly worse — the true key must additionally outscore millions of
> competitors. Two things follow. The curve is **still climbing steeply at
> `-R 64` below ~110 letters** (30.5 → 54.0 at L=107, no sign of saturation),
> where at L=167 it is already flattening — so the shorter the target, the more
> of the budget belongs in `-R` rather than in coverage, reversing the
> §"unknown-key break rate" advice tuned at L=167. And a 60-letter difference
> in target length is worth **more than a 8× difference in restarts**: L=167 at
> `-R 8` beats L=107 at `-R 64`. When choosing which unbroken message to
> attack, length dominates every other consideration.

Read `ENHANCEMENTS.md` and then `archived/IMPROVEMENTS.md` before changing the
search or scoring code — in
particular its "Measured down" table, which lists what has already been built
and lost. The supporting measurements are in `archived/PERFORMANCE.md` and
`archived/CODE_REVIEW_HISTORY.md`.
