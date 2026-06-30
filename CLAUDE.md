# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
this repository.

## Overview

This repository contains a single-file C++ command-line tool that simulates an
Enigma cipher machine and, more importantly, attempts to **break** Enigma
ciphertext by brute-forcing rotor/reflector/ring/start settings and
hill-climbing the plugboard (steckerbrett). Candidate decryptions are scored
against per-language n-gram statistics (monograms, bigrams, trigrams,
quadgrams) so that the configuration producing the most "language-like"
plaintext wins.

The tool supports the standard 3-wheel Wehrmacht Enigma (wheels I–VIII,
reflectors A/B/C), the special **Norway Enigma (Norenigma)** variant (reflector
N, wheels 1–5), and the four-rotor naval **M4** (`-4`): thin reflectors UKW-b/c
plus the static Beta/Gamma Greek wheel, which folds into an effective reflector so
the engine stays a 3-stepping-rotor machine (see "M4 mode" below).

- **Author:** Torbjørn Rognes
- **License:** GNU GPL v3 (see `LICENSE`)
- **Language/era:** C++ written in a predominantly C style (C stdio, raw
  arrays, global state).

## Repository layout

```
enigma.cc                 The entire program (single translation unit).
Makefile                  Builds the `enigma` binary with g++ -O3.
README.md                 User-facing description and usage.
LICENSE                   GNU GPL v3.
.gitignore                Ignores editor backups and cipher*.txt.
<lang>_monograms.txt       Single-letter frequencies.
<lang>_bigrams.txt         Two-letter frequencies.
<lang>_trigrams.txt        Three-letter frequencies.
<lang>_quadgrams.txt       Four-letter frequencies.
```

Languages provided: `english`, `german`, `danish`, `french` (no default — the
scoring language must be given with `-l` for the n-gram models).
N-gram files use the format `<LETTERS> <count>` per line (e.g. `TION 13168375`)
and were sourced from the Practical Cryptography website.

## Build & run

```sh
make                      # g++ -std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -Wshadow -O3 ...
make test                 # build, then run tests/run_tests.sh
make bench                # build, then run tests/bench.sh (performance)
make crackquality         # build, then run tests/crack_quality.py (cracking quality)
./enigma -h               # help / usage
```

`make bench` (`tests/bench.sh`) benchmarks the two hot paths **separately** —
`search` (brute-force scan, no plugboard) and `hillclimb` (the `-c` plugboard
loop) — because a change can regress one without touching the other. Each has a
`quick` tier (default, a few seconds) and an opt-in `long` tier (`make bench
LONG=1`, ≥15–30s each) for a stronger signal; `make bench SCALE=1` additionally
sweeps `-T` to show thread scaling. Timing is the min of several repetitions
(the per-tier benchmarks are single-threaded). The regression guard is a same-machine
A/B: `make bench BASE=<git-ref>` builds the binary at `<git-ref>` in a throwaway
git worktree and runs both, failing if any benchmark is >`THRESHOLD`% (default
10) slower than BASE — run this around the planned global-state/threading
refactor to confirm single-thread throughput hasn't regressed.

`make crackquality` (`tests/crack_quality.py`) measures something different again
— **cracking quality on hard (short) messages**, not speed. For each ciphertext
length it runs many random trials (random excerpt + rotor key + 10-pair
plugboard), recovers each with the true rotor key fixed and only the plugboard
hill-climbed (the cheap "plugboard-recovery" tier), and reports per length the
mean %-of-letters-correct (a graded signal) and the exact-recovery rate, plus
headline `L50`/`L90` (the shortest length reaching that recovery rate — lower is
better). A fixed `SEED` makes the trial set deterministic (Python's
`random.Random(seed)`, reproducible across machines), so `make crackquality
BASE=<git-ref>` is a same-machine A/B that solves identical problems with both
binaries. (It was rewritten from shell+awk to Python: awk's seeded `rand()` is not
reproducible in every awk, e.g. mawk, which silently broke the deterministic-A/B
premise.) Use this — not `make bench` — to tell whether a scoring/search change
actually helps short-message cracking. `make crackquality SPLIT=1` additionally
classifies each non-recovered trial as a **scoring** failure (the true plugboard
does not score highest) or a **search** failure (it does, but the climb stuck in
a local optimum) via an oracle run — telling you which lever to pull. (See
`CODE_REVIEW.md` §9 for the algorithmic ideas this is meant to measure; on the
v1.1.0 baseline every miss is a *search* failure.)

The program reads **ciphertext from stdin** and writes the best-scoring
**plaintext to stdout**; progress/diagnostics go to stderr. Only A–Z letters
are kept; everything else (spaces, punctuation, case) is stripped. The n-gram
files are read from a **data directory** (filenames built as
`<datadir>/<language>_<ngram>.txt`) resolved as `-d <dir>` → `$ENIGMA_DATA` →
`.` (the current directory, the historical default) — so the tool can run from
any working directory.

### Common invocations

```sh
# Brute-force everything (all reflectors, wheels, ring & start positions),
# scoring with quadgrams (default model) against the English tables:
./enigma -l english < cipher.txt

# Language-independent: search with the index of coincidence (no -l needed):
./enigma -i < cipher.txt

# Specify some settings, wildcard the rest with '.', and hill-climb plugboard:
./enigma -u B -w 123 -r AAA -g ... -c -l english < cipher.txt

# Norway Enigma:
./enigma -n -c -l english < cipher.txt

# M4 (4-rotor naval): thin reflector b, Greek Beta, wheels III-I-VII, wildcard
# the Greek position (first char of -g) and hill-climb the plugboard:
./enigma -4 -u b -w B317 -r AAAA -g .QXP -c -l english < cipher.txt
```

### Key CLI options (see `help()` in source for the full list)

- `-u X` reflector A/B/C or `.` wildcard (`N` forced by `-n`)
- `-w XYZ` wheels (digits, or `.` per position to brute-force)
- `-x N` highest wheel number to consider when wildcarding (default 5)
- `-n` Norway Enigma mode
- `-4` M4 (4-rotor) mode: `-u` selects thin reflector `b`/`c`; `-w`/`-r`/`-g` take
  **four** characters with the Greek wheel (`B`=Beta/`G`=Gamma) / ring / start first
- `-r XYZ` / `-g XYZ` ring / start positions (letters or `.`)
- `-s AB...` fixed plugboard pairs
- `-c` hill-climb the plugboard
- `-R N` plugboard hill-climb random restarts (1 = none; restart 0 is the seed,
  the rest start from random involutions, best kept). Per-key RNG seeded from the
  flat key index, so the result stays independent of `-T`. ~`N`× the `-c` cost.
- `-S <schedule>` staged plugboard climb — a string of pre-pass model letters
  (`i`/`m`/`b`/`t`/`q`) climbed in order before the target model, to steer the
  early plugs into a better basin (the lower-order surface is smoother when few
  plugs are set). **`-S i` (IC pre-pass) is the best measured** — much better than
  bigram, and extra stages after IC add nothing. Per-`machine` `scoring` field
  (never a global → race-free); deterministic; stacks with `-R`.
- `-L N` cap each staged pre-pass at `N` committed moves (≈ first `N` plugs);
  `0` = uncapped (default). A tuning knob for the staged climb — a preliminary
  sweep found capping the *IC* pre-pass only hurts (IC doesn't over-fit), but it is
  retained to investigate capping an over-fitting pre-pass (bigram) thoroughly.
- `-l lang` scoring language — **required** for `-m/-b/-t/-q` (no default), not
  used by `-i`
- `-i/-m/-b/-t/-q` scoring model: IC / mono / bi / tri / quad (quad is the
  default model)
- `-p file` compare the recovered plaintext against a known plaintext file
- `-d dir` directory holding the n-gram files (else `$ENIGMA_DATA`, else `.`)
- `-T N` worker threads for the search (default 1, max 256)

Every run echoes the resolved configuration (scoring model, language, n-gram data
directory, machine settings, plugboard, ciphertext length) to stderr.

> **Gotcha — match `-l` to the plaintext language, especially for `-q`.** There
> is no default language: `-m/-b/-t/-q` require `-l`, and the n-gram tables are
> highly language-specific (most of all quadgrams). Scoring an English message
> with, say, `-l german` typically fails — the german table scores ~0 for
> English quadgrams and the correct key does not stand out. Lower-order models
> (`-m/-b/-t`) tolerate a mismatch better, and `-i` (index of coincidence) is
> language-independent and needs no `-l`. Use `-l` matching the plaintext.

## Architecture / how it works

A single pass through `main()`:

1. Parse and validate options (`getopt`).
2. `readciphertext()` reads stdin, uppercases, and keeps only A–Z.
3. Load the n-gram table matching the chosen scoring model and language; each
   count is stored as `log10(count + 1)` for additive scoring.
4. `init()` precomputes numeric forward/reverse rotor permutations, notch
   tables, and reflector permutations from the hard-coded wiring strings.
5. `bruteforce()` is the main search, run across `opt_threads` worker threads
   (`-T N`, default 1, max 256) in two parallel phases over the flat
   reflector × wheel-order × ring × start key space:
   - **Phase 1 (`precompute_worker`)**: build `subst_array[g1][g2][g3][x]` — the
     rotor-stack substitution for every (start-minus-ring) triple, ring fixed at
     0 — once **per reflector × wheel-order**, for *all* of them, into one shared
     read-only block (`#wheel-orders × 457 KB`). A table serves every ring/start
     of its wheel order via the start−ring offset.
   - **Phase 2 (`search_worker`)**: an atomic counter hands out adaptive chunks
     of the flat key space; each worker decodes a flat index → (wheel-order,
     ring, start) by mixed radix, points its private `machine` at that wheel
     order's shared table (swapped, never recomputed, on a boundary), and:
   - `setup_mapping()` steps the rotors over the message length and records, per
     position, a pointer `rows[pos]` to that position's rotor-stack substitution
     row (folding in the stepping). The scan points `rows[pos]` straight into the
     shared `subst_array` (no copy); hill-climb copies the row into a contiguous
     `mapping[]` first (it re-reads each row many times);
   - `decode()` + `score_iter()` produce and score the candidate (the n-gram
     scorers fuse the decode into their loop). The best is merged under a mutex
     (which also serialises the live progress line). Parallelising the flat key
     space means rings/starts scale even when the wheels are fixed.
   - With `-c`, `hillclimb()` greedily swaps plugboard pairs to maximize score
     before recording the result.
6. The best-scoring plaintext is printed; optionally compared to `-p` file. A
   final stderr diagnostic reports wall-clock time, thread count, the number/size
   of precomputed rotor tables, and peak RSS (via `getrusage`).

### Core machine model

- `char2num`/`num2char` map A–Z ↔ 0–25.
- `rotor_l` / `rotor_r` apply a single rotor forward/reverse with the
  `grundstellung - ringstellung` offset.
- The rotor stepping schedule — including the Enigma double-stepping anomaly
  (the middle rotor advances on its own notch as well as the right rotor's
  carry) — is implemented inline in `setup_mapping()`, which holds the rotor
  positions in locals across the per-character loop (see the performance note).
- The full substitution is plugboard ∘ rotor-stack ∘ reflector ∘ rotor-stack ∘
  plugboard. `subst_rotors()` is the rotor-stack-and-reflector core; the hot path
  replaces it with precomputed `subst_array` / `mapping` lookups wrapped in two
  plugboard lookups (`decode_at`, shared by `decode()` and the fused scorers).
- The reflector applied in `subst_rotors()` is `m.reflector_eff`, resolved once
  per task by `set_effective_reflector()` (never per character). Standard/Norway
  just copy the wired reflector; **M4** composes `greek ∘ thin ∘ greek⁻¹`.

### M4 mode (4-rotor naval)

The M4's 4th "Greek" wheel (Beta/Gamma) is **static** — it never steps — so it
folds into the reflector: `set_effective_reflector()` builds an effective
reflector `greek ∘ thin ∘ greek⁻¹` at the Greek wheel's fixed offset
`(start − ring) mod 26` (an involution, since conjugation preserves it), used at
the single reflector site. The machine therefore stays a **3-stepping-rotor**
engine (`wheels` stays 3) and the entire hot path (`subst_array`, `setup_mapping`,
stepping, scorers) is unchanged — the fold is paid only in `precompute()`.

- CLI: `-4` mode flag; `-u` is the thin reflector `b`/`c`/`.`; `-w`/`-r`/`-g` take
  **four** characters with the Greek wheel (`B`/`G`/`.`) / ring / start first. The
  Greek char is split off in validation so the shared 3-char checks and the
  3-rotor search run unchanged on the tail. `-n` and `-4` are mutually exclusive.
- Search: `wheel_task` carries the Greek wheel + offset; `bruteforce()` enumerates
  thin × Greek wheel × **distinct Greek offsets** (only the `start − ring` offset
  is identifiable, so the pos/ring ranges collapse to ≤26 offsets, not 26×26) ×
  wheel orders. Each task precomputes its own effective reflector, then the
  existing two-phase precompute + flat ring/start sweep runs unmodified
  (threading/determinism preserved). A full M4 wildcard is ~15 GB of tables, hence
  the precompute guard is **16 GiB**.
- Correctness is anchored (KAT) on the documented backward-compatibility
  equivalence: thin `b` + Beta at ring/pos A ≡ standard reflector B, and `c` +
  Gamma@A ≡ C (`tests/run_tests.sh`), plus round-trip and search-recovery checks.
- Reflector indices 4–5 = UKW-b/c, rotor indices 13–14 = Beta/Gamma (already in
  the wiring tables). Only the `(start − ring)` offset of the Greek wheel is
  recoverable, so `showconfig` reports it as start = offset, ring = A.

### Performance notes

The n-gram score loop (`quadgram_score_decode`) is where ~99% of runtime is
spent when hill-climbing. That is why the rotor stack is precomputed into
`subst_array` and reached per position through `rows[pos]` so each character
costs just two plugboard lookups plus a table lookup (`decode_at`). For the scan
`rows[pos]` points straight into `subst_array` (no per-position copy, and the
scan skips its `decode()` pass, materialising the plaintext only for a new best);
hill-climb copies each row into a contiguous `mapping[]` for locality across its
many re-reads. The four scorers
**fuse decoding into the score loop**: each character is decoded once into a
small sliding window of the last *n* decoded letters that indexes the n-gram
table, so the decoded message is never written to / read back from a scratch
array (this is faster than the previous `decode_num` → `num_plaintext` → score
two-pass on both compilers, markedly so on clang/ARM). An even earlier
16-byte-blocked decode was never shown to win and was removed too; the scalar
fused loop is the current form.

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
> holds the
> rotor positions (`grundstellung` etc.) in locals across its per-character loop
> — stepping them through the struct member each character could not be proven
> not to alias the `mapping[]` store and cost ~10–14% on the search path (worst
> on ARM). With all three, both paths are at parity vs the pre-struct baseline on
> g++ and clang. Always re-check `make bench BASE=<ref>` under **both** g++ and
> clang (`make bench CXX=clang++ BASE=<ref>`) after touching the hot path or the
> struct.

## Conventions & gotchas for contributors

- **Code style.** Allman braces (every `{` and `}` on its own line),
  2-space indentation, and no tabs anywhere in `enigma.cc`. Continuation lines
  (e.g. wrapped parameter lists or `if` conditions) are aligned under the
  opening `(`. The only tabs in the repo are the recipe lines in the `Makefile`,
  which `make` requires.
- **Single translation unit; per-search state in `struct machine`.** The
  mutable per-search state — machine settings (`walzenlage`, `grundstellung`,
  `ringstellung`, `ukw`, `steckerbrett`) and the working buffers (`subst_array`,
  the per-position row pointers `rows`, the contiguous `mapping`, the candidate
  `plaintext`) — is
  bundled into `struct machine`, threaded through the search/scoring functions as
  `machine & m`; `main()` owns one heap instance. This makes the search
  reentrant (the precondition for multi-threading — see `CODE_REVIEW.md` §5/§6).
  The read-only data stays file-scope global and shared: the wiring tables
  (`rotor_fwd/rev`, `notch`, `reflector`) built by `init()`, the n-gram tables,
  and the `ciphertext` / `num_ciphertext` / `textlength` input. (`-Wshadow` is on;
  the redundant `textlength`/`ciphertext`/`plaintext` parameters that used to
  shadow the globals were removed earlier.)
- Debug instrumentation is intentionally retained: `showit`, `showconfig`,
  `showsteckerbrett`, the `#if 0` trace blocks, and the `SHOWHILLCLIMB`
  compile-time path. (The vestigial `all_subst_score`/`map`/`opt_threads`/
  `opt_logfilename` code has been removed; see `CODE_REVIEW.md` §3.)
- Index conventions: reflectors 0–2 = A/B/C, 3 = Norway, 4–5 = M4 thin;
  rotors 0–7 = I–VIII, 8–12 = Norway 1–5, 13–14 = Beta/Gamma. Norway mode
  applies a +3 / +8 offset (see `init_walzen`).
- Build is plain `make` (override `CXX`, or append `EXTRA_CXXFLAGS=` for e.g.
  `-Werror`/sanitizers). Tests live in `tests/run_tests.sh` and run via
  `make test` (known-answer vectors, round-trip properties, input-limit guards,
  and end-to-end cracking — brute-force start-position and plugboard hill-climb
  matrices over every scoring model × language). Performance is benchmarked
  separately by `tests/bench.sh` (`make bench`; see "Build & run"). CI
  (`.github/workflows/ci.yml`) runs on every push
  and PR: the suite `-Werror` under g++ and clang++, ASan+UBSan, valgrind,
  cppcheck, clang-tidy (config in `.clang-tidy`), and shellcheck; a separate
  CodeQL workflow runs on PRs and weekly. Keep all of these green.

## Status & remaining work

A detailed audit lives in `CODE_REVIEW.md`. Most findings have been fixed —
the stack buffer overflow, the index-of-coincidence formula, the `-l`/filename
overflow, the `fscanf`/read-handling bugs, dead code, the C-style
modernization, the `textlength` global/parameter shadowing, the encapsulation of
the per-search state into `struct machine`, and **multi-threading** the search
over reflector × wheel-order (`-T N`, default 1, max 256; each worker owns its
own `machine`, results merged under a mutex) — and the build is warning-free
under `-std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -Wshadow`, and clean under
ThreadSanitizer. Scaling is ~3× on 4 cores (`make bench SCALE=1`). **M4 (4-rotor
naval) mode** is now implemented (`-4`; static Greek wheel folded into an
effective reflector, so the hot path is untouched — see "M4 mode" above and
`CODE_REVIEW.md` §5). The remaining open direction is **cracking quality on short
messages**: the `make crackquality` harness shows every miss is a *search*
failure (the plugboard hill-climb sticking in local optima), so the next lever is
the search — random restarts / better seeding / annealing (`CODE_REVIEW.md` §9).
Read `CODE_REVIEW.md` before changing the search or scoring code.
