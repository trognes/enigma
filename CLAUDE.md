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

The tool supports both the standard 3-wheel Wehrmacht Enigma (wheels I–VIII,
reflectors A/B/C) and the special **Norway Enigma (Norenigma)** variant
(reflector N, wheels 1–5). Tables for M4 thin reflectors (b/c) and the
Beta/Gamma rotors are present in the source but are not currently reachable
through the CLI.

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

The program reads **ciphertext from stdin** and writes the best-scoring
**plaintext to stdout**; progress/diagnostics go to stderr. Only A–Z letters
are kept; everything else (spaces, punctuation, case) is stripped. The n-gram
files must be present **in the current working directory** at runtime because
filenames are built as `<language>_<ngram>.txt` and opened relative to the CWD.

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
```

### Key CLI options (see `help()` in source for the full list)

- `-u X` reflector A/B/C or `.` wildcard (`N` forced by `-n`)
- `-w XYZ` wheels (digits, or `.` per position to brute-force)
- `-x N` highest wheel number to consider when wildcarding (default 5)
- `-n` Norway Enigma mode
- `-r XYZ` / `-g XYZ` ring / start positions (letters or `.`)
- `-s AB...` fixed plugboard pairs
- `-c` hill-climb the plugboard
- `-l lang` scoring language — **required** for `-m/-b/-t/-q` (no default), not
  used by `-i`
- `-i/-m/-b/-t/-q` scoring model: IC / mono / bi / tri / quad (quad is the
  default model)
- `-p file` compare the recovered plaintext against a known plaintext file

Every run echoes the resolved configuration (scoring model, language, machine
settings, plugboard, ciphertext length) to stderr.

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
5. `bruteforce()` is the main search:
   - Enumerates the reflector × wheel-order combinations (skipping repeated
     wheels) as a task list, then runs them across `opt_threads` worker threads
     (`-T N`, default 1) via an atomic task counter; each worker owns a private
     `machine` and the best result is merged under a mutex (which also serialises
     the live progress line). Per wheel-order task:
   - `precompute()` builds `subst_array[g1][g2][g3][x]` — the rotor-stack
     substitution for every (start-position-minus-ring-setting) triple, with
     ring fixed at 0 — once per wheel order.
   - For each ring/start combination, `setup_mapping()` steps the rotors over
     the message length and records, per character position, the full 26-letter
     substitution (`mapping[pos][letter]`), folding the stepping schedule in.
   - `decode()` applies plugboard → mapping → plugboard to produce the candidate
     plaintext; `score_iter()` scores it (the n-gram scorers fuse the same decode
     into their loop rather than materialising the decoded text).
   - With `-c`, `hillclimb()` greedily swaps plugboard pairs to maximize score
     before recording the result.
6. The best-scoring plaintext is printed; optionally compared to `-p` file.

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

### Performance notes

The n-gram score loop (`quadgram_score_decode`) is where ~99% of runtime is
spent when hill-climbing. That is why the rotor stack is precomputed into
`subst_array` and folded into a per-position `mapping` so each character costs
just two plugboard lookups plus a table lookup (`decode_at`). The four scorers
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
  `mapping`, the candidate `plaintext`) — is
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
ThreadSanitizer. Scaling is ~3× on 4 cores (`make bench SCALE=1`). The main
remaining feature is an **M4 (4-rotor naval) mode** — the wiring tables are
already present and a design (static Greek wheel folded into an effective
reflector; `-4` flag with `-u`/`-w`/`-r`/`-g` extended to the 4th wheel) is
recorded in `CODE_REVIEW.md` §5; deferred for now. Read `CODE_REVIEW.md` before
changing the search or scoring code.
