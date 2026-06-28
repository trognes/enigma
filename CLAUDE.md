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

Languages provided: `german` (default), `english`, `danish`, `french`.
N-gram files use the format `<LETTERS> <count>` per line (e.g. `TION 13168375`)
and were sourced from the Practical Cryptography website.

## Build & run

```sh
make                      # g++ -std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -O3 ...
make test                 # build, then run tests/run_tests.sh
./enigma -h               # help / usage
```

The program reads **ciphertext from stdin** and writes the best-scoring
**plaintext to stdout**; progress/diagnostics go to stderr. Only A–Z letters
are kept; everything else (spaces, punctuation, case) is stripped. The n-gram
files must be present **in the current working directory** at runtime because
filenames are built as `<language>_<ngram>.txt` and opened relative to the CWD.

### Common invocations

```sh
# Brute-force everything (all reflectors, wheels, ring & start positions),
# scoring with quadgrams (default), German language:
./enigma < cipher.txt

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
- `-l lang` scoring language
- `-i/-m/-b/-t/-q` scoring model: IC / mono / bi / tri / quad (quad default)
- `-p file` compare the recovered plaintext against a known plaintext file

## Architecture / how it works

A single pass through `main()`:

1. Parse and validate options (`getopt`).
2. `readciphertext()` reads stdin, uppercases, and keeps only A–Z.
3. Load the n-gram table matching the chosen scoring model and language; each
   count is stored as `log10(count + 1)` for additive scoring.
4. `init()` precomputes numeric forward/reverse rotor permutations, notch
   tables, and reflector permutations from the hard-coded wiring strings.
5. `bruteforce()` is the main search:
   - Iterates reflector × wheel-order combinations (skipping repeated wheels).
   - `precompute()` builds `subst_array[g1][g2][g3][x]` — the rotor-stack
     substitution for every (start-position-minus-ring-setting) triple, with
     ring fixed at 0 — once per wheel order.
   - For each ring/start combination, `setup_mapping()` steps the rotors over
     the message length and records, per character position, the full 26-letter
     substitution (`mapping[pos][letter]`), folding the stepping schedule in.
   - `decode()` / `decode_num()` apply plugboard → mapping → plugboard to
     produce candidate plaintext; `score_iter()` scores it.
   - With `-c`, `hillclimb()` greedily swaps plugboard pairs to maximize score
     before recording the result.
6. The best-scoring plaintext is printed; optionally compared to `-p` file.

### Core machine model

- `char2num`/`num2char` map A–Z ↔ 0–25.
- `rotor_l` / `rotor_r` apply a single rotor forward/reverse with the
  `grundstellung - ringstellung` offset.
- `step_rotors()` implements the stepping schedule including the Enigma
  double-stepping anomaly (checks the middle/right notches before advancing).
- `substitute()` = plugboard ∘ rotor-stack ∘ reflector ∘ rotor-stack ∘
  plugboard. The hot path replaces the rotor stack with precomputed
  `subst_array` / `mapping` lookups.

### Performance notes

The hill-climb decode-and-score loop (`quadgram_score_decode` →
`decode_num`) is where ~99% of runtime is spent (per the source comment).
That is why the rotor stack is precomputed into `subst_array` and folded into a
per-position `mapping` so the inner loop is just two plugboard lookups plus a
table lookup per character. `decode_num` processes the text in 16-byte blocks.

## Conventions & gotchas for contributors

- **Single translation unit, heavy global state.** Machine settings
  (`walzenlage`, `grundstellung`, `ringstellung`, `ukw`, `steckerbrett`),
  buffers (`ciphertext`, `plaintext`, `num_*`, `mapping`, `subst_array`), and
  the loaded n-gram tables are all file-scope globals. Most functions also take
  a `textlength` parameter even though a global of the same name exists.
- Debug instrumentation is intentionally retained: `showit`, `showconfig`,
  `showsteckerbrett`, the `#if 0` trace blocks, and the `SHOWHILLCLIMB`
  compile-time path. (The vestigial `all_subst_score`/`map`/`opt_threads`/
  `opt_logfilename` code has been removed; see `CODE_REVIEW.md` §3.)
- Index conventions: reflectors 0–2 = A/B/C, 3 = Norway, 4–5 = M4 thin;
  rotors 0–7 = I–VIII, 8–12 = Norway 1–5, 13–14 = Beta/Gamma. Norway mode
  applies a +3 / +8 offset (see `init_walzen`).
- Build is plain `make`. Tests live in `tests/run_tests.sh` and run via
  `make test` (known-answer vectors, round-trip properties, input-limit guards,
  and end-to-end cracking). CI runs `make test` on every push and pull request
  via `.github/workflows/ci.yml`.

## Known issues

A detailed audit lives in `CODE_REVIEW.md`. The most important things to know
before editing: there is a stack buffer overflow risk for long inputs
(`best_plaintext[1025]` vs `maxlen = 10240`), the index-of-coincidence scoring
is mathematically incorrect, and the `-l` language string can overflow a fixed
filename buffer. Read that document before changing the search or scoring code.
