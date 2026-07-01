# Enigma cipher tool

A command-line tool that **simulates** an Enigma cipher machine and, more
usefully, attempts to **break** Enigma ciphertext when the settings are unknown —
by brute-forcing the rotor/reflector/ring/start settings and hill-climbing the
plugboard, scoring each candidate decryption against per-language letter
statistics so the most language-like plaintext wins.

It supports three machines:

- the common **three-wheel Enigma** (wheels I–VIII, reflectors A/B/C),
- the **Norway Enigma** (Norenigma) variant (reflector N, wheels 1–5), and
- the four-rotor naval **M4** (thin reflectors UKW-b/c plus the Beta/Gamma Greek
  wheel).

The settings are the reflector (*umkehrwalze*), the wheels (*walzen*) and their
order, the ring positions (*ringstellung*), the start positions
(*grundstellung*), and the plugboard (*steckerbrett*).

## Building

You need a C++17 compiler (g++ or clang++) and `make`. There are no external
dependencies; the n-gram data files ship in this repository.

```sh
make                 # builds the ./enigma binary (g++ -O3 -pthread)
make CXX=clang++     # build with clang instead
make test            # build, then run the test suite
make bench           # build, then run the performance benchmarks
make crackquality    # build, then measure short-message cracking quality
```

## Quick start

The ciphertext (or plaintext) is read from **standard input**; the result is
written to **standard output**. Only the letters A–Z are processed — spaces,
punctuation and case are stripped on input.

```sh
# Encrypt. A fully specified machine with no -c just enciphers its input -- nothing
# is searched -- so no scoring options are needed. (Give an explicit -r: the default
# ring is "AA.", whose wildcard would otherwise turn this into a search.)
echo "ATTACK AT DAWN" | ./enigma -u B -w 531 -r ABC -g XYZ
#   -> YYHISFEPIWUP

# Decrypt. Enigma is reciprocal: the SAME settings turn ciphertext back into
# plaintext.
echo "YYHISFEPIWUP" | ./enigma -u B -w 531 -r ABC -g XYZ
#   -> ATTACKATDAWN

# Encrypt with a plugboard (pairs A<->B and C<->D):
echo "THE QUICK BROWN FOX" | ./enigma -u B -w 123 -r AAA -g AAA -s "AB CD"
```

### Cracking

```sh
# You know the rotor key but not the plugboard: hill-climb the plugboard (-c),
# scoring English quadgrams (-q, the sharpest model; -l gives the language). The
# default model is the index of coincidence, so pass -q to use quadgrams.
./enigma -c -q -l english -u B -w 241 -r AAA -g QEW < cipher.txt

# You don't know the start positions either: wildcard them with '.' and the
# program brute-forces all 26x26x26 of them, on 4 threads, while still
# hill-climbing the plugboard.
./enigma -c -q -l english -u B -w 123 -r AAA -g ... -T 4 < cipher.txt

# You know almost nothing: wildcard the reflector, wheels, ring and start, and
# let it try everything. (This is a large search — use as many threads as you
# have cores, and see "Cracking strategy" below for the recommended options.)
./enigma -c -q -l english -u . -w ... -r ... -g ... -T 8 < cipher.txt
```

### Other machines

```sh
# Norway Enigma (reflector N, wheels 1-5):
echo "GODDAG" | ./enigma -n -u N -w 123 -r AAA -g AAA

# M4 (4-rotor naval): -u is the thin reflector b/c, and -w/-r/-g take FOUR
# characters with the Greek wheel (B=Beta / G=Gamma) / ring / start first.
echo "WETTERBERICHT" | ./enigma -4 -u b -w B123 -r AAAA -g AAAA
```

## How it works

For a fully specified machine the tool just enciphers the input. When some
settings are left unspecified (a dot `.` wildcard), it searches:

1. For every combination of the unspecified reflector / wheel order / ring /
   start positions, it decrypts the ciphertext.
2. Each candidate plaintext is **scored** against the chosen statistical model
   (index of coincidence, or mono/bi/tri/quad-gram frequencies for a language).
   Real plaintext scores far higher than gibberish, so the highest-scoring
   settings are almost always the correct ones.
3. If `-c` is given, for each candidate key the plugboard is recovered by a
   **hill-climbing** search (greedily adding/swapping plug pairs to raise the
   score) before the key is scored.

The search is exhaustive over the rotor settings and heuristic over the
plugboard (whose ~150-trillion 10-pair configurations are far too many to
enumerate). The best plaintext found is printed to stdout; progress and a final
diagnostic (timing, threads, memory) go to stderr.

## Options

Defaults are shown in `[brackets]`. A dot `.` is the wildcard for the reflector,
wheels, ring and start positions — any position left as `.` is brute-forced.

### Machine settings

| Option | Meaning |
| --- | --- |
| `-u X` | Reflector (*umkehrwalze*): `A`/`B`/`C`, `N` for Norway, `b`/`c` for M4 thin, or `.` `[.]` |
| `-w XYZ` | Wheel order, left to right: digits `1`–`8` (`1`–`5` for Norway) or `.` per position `[...]` |
| `-x N` | Highest wheel number to consider when a wheel is wildcarded `[5]` |
| `-n` | Norway Enigma mode (reflector `N`, wheels 1–5) |
| `-4` | M4 mode; `-u` is the thin reflector `b`/`c`, and `-w`/`-r`/`-g` take **four** characters, Greek wheel (`B`/`G`) / ring / start first |
| `-r XYZ` | Ring positions (*ringstellung*), letters `A`–`Z` or `.` `[AA.]` |
| `-g XYZ` | Start positions (*grundstellung*), letters `A`–`Z` or `.` `[...]` |
| `-s AB…` | Fixed plugboard pairs, e.g. `-s "AB CD EF"` `[none]` |

`-n` and `-4` are mutually exclusive. In M4 mode only the Greek wheel's
`start − ring` offset is recoverable, so a full M4 wildcard search enumerates the
distinct offsets rather than every ring×start pair.

### Scoring (which plaintext "looks like a language")

| Option | Meaning |
| --- | --- |
| `-i` | Index of coincidence — language-independent, needs no `-l` (**default**) |
| `-m` / `-b` / `-t` / `-q` | Mono- / bi- / tri- / quad-gram statistics |
| `-l lang` | Scoring language: `english`, `german`, `danish`, `french`. **Required** for `-m`/`-b`/`-t`/`-q`; ignored by `-i` |

The **default model is the index of coincidence** (`-i`) — the only one that needs
no language, so the tool runs out of the box with no scoring options. Quadgrams
(`-q`) discriminate the correct key most sharply and are the recommended model when
you know the language; pass `-q -l <lang>` to use them. The n-gram tables are highly
language-specific — **`-l` must match the language of the plaintext**, especially
for `-q` (scoring an English message with `-l german` typically fails). Note that
`-l` on its own does nothing: it only takes effect with an n-gram model, so it is
`-q -l english`, not `-l english`, that scores with English quadgrams.

### Plugboard cracking

| Option | Meaning |
| --- | --- |
| `-c` | Hill-climb the plugboard for each candidate key |
| `-R N` | Random restarts of the plugboard climb (`1` = none) `[1]` |
| `-S sched` | Staged climb schedule (see below) |
| `-F N` / `-F N%` | Key pre-filter: full climb only the top `N` keys, or top `N%` of the keyspace (needs `-c`; `0` = off) `[0]` |

The plugboard climb gets stuck in local optima on short messages, so `-R N`
restarts it `N` times from perturbed boards and keeps the best, and `-S` runs the
climb in stages. When you are also brute-forcing rotor settings, `-F N` shortlists
the most promising keys with a cheap pass so the expensive climb runs only on
those. See **Cracking strategy** below.

### Data and performance

| Option | Meaning |
| --- | --- |
| `-d dir` | Directory holding the n-gram files (else `$ENIGMA_DATA`, else `.`) `[.]` |
| `-T N` | Worker threads for the search, 1–256 `[1]` |
| `-p file` | Compare the recovered plaintext against a known plaintext file |
| `-v` / `-h` | Version / help |

The full usage message (`./enigma -h`):

```
Usage: enigma [OPTIONS]
  -h           Show help information
  -v           Show version information
  -u X         Reflector (umkehrwalze) X (A-C, N, M4 b/c, or .) [.]
  -w XYZ       Wheels (walzen) XYZ (1-8 or .) [...]
  -x integer   Highest wheel number to use (3-8) [5]
  -n           Use the Norway Enigma reflector (N) and wheels (1-5)
  -4           M4 (4-rotor naval) mode: -u selects thin reflector b/c;
               -w/-r/-g take 4 chars, Greek wheel (B/G) / ring / start first
  -r XYZ       Ring positions (ringstellung) XYZ (A-Z or .) [AA.]
  -g XYZ       Start positions (grundstellung) XYZ (A-Z or .) [...]
  -s AB...     Plugboard (steckerbrett) letter pairs (A-Z pairs) [none]
  -c           Perform hill climbing to determine plugboard settings
  -R integer   Plugboard hill-climb random restarts (1 = none) [1]
  -S schedule  Staged plugboard climb: <letter><opt.number> tokens.
               Models i/m/b/t/q (number caps plug pairs; last = target),
               rN = per-restart random plugs (N pairs, default 8).
               E.g. -S r2i6q
  -l language  Scoring language (english, german, danish, french); required
               for -m/-b/-t/-q (no default), not used by -i
  -i           Use index of coincidence (IC) to score; needs no -l [default]
  -m           Use monogram statistics to determine plaintext score
  -b           Use bigram statistics to determine plaintext score
  -t           Use trigram statistics to determine plaintext score
  -q           Use quadgram statistics to determine plaintext score
  -p filename  Name of file containing plaintext to compare result with
  -F N[%]      Key pre-filter: rank keys by a cheap IC climb, then run
               the full -c climb on only the top N keys, or top N% of
               the keyspace (needs -c) [off]
  -d directory Directory holding the n-gram files (or $ENIGMA_DATA) [.]
  -T integer   Number of worker threads for the search (1-256) [1]
```

## Cracking strategy

A plain `-c` climb recovers the plugboard reliably on long messages but gets
stuck in local optima on short ones. Two options improve this and **compose**:

- **`-R N` — random restarts.** Runs the climb `N` times, each restart kicking
  the board with a few random plugs, and keeps the best result. This is the
  biggest lever for short messages, and it keeps paying as `N` grows (there is no
  practical plateau) — at the cost of roughly `N`× the work. The restart kick
  defaults to 8 random pairs (close to a typical plug count, which works best).

- **`-S <schedule>` — staged climb.** A schedule is a string of
  `<letter><optional number>` tokens:
  - model tokens `i`/`m`/`b`/`t`/`q` are climb stages run in order; an optional
    number caps how many plug pairs that stage may set (omitted = uncapped). The
    **last** model token is the target/ranking model.
  - an `rN` token sets the per-restart random kick to `N` pairs (omitted = the
    default 8).

  Climbing a low-order model first (its scoring surface is smoother when only a
  few plugs are set) steers the early plugs into a better basin. An **index-of-
  coincidence pre-pass works best**: `-S iq` climbs IC, then refines under
  quadgrams.

- **`-F N` (or `-F N%`) — key pre-filter.** With `-c` the full `-R`/`-S` climb is
  paid on *every* candidate key, which dominates runtime when you wildcard rotor
  settings. `-F` instead runs a single **cheap index-of-coincidence climb** on every
  key, keeps the best `N` (or the best `N%` of the keyspace), and pays the full climb
  on only those — a ~8–20× throughput win, so you can afford more restarts per
  surviving key. (A plugboard-free IC *scan* does not work here: under a full
  plugboard the rotor-only decrypt is almost entirely scrambled, so it cannot rank
  keys; a cheap IC *climb* partially recovers the plugboard and does.) Prefer the
  `N%` form — recall depends on the *fraction* of the keyspace you keep, so a
  percentage stays meaningful as the wildcard grows; keep a generous slice (≥ ~10%),
  since too tight a shortlist on a large keyspace or a weakly-discriminated wildcard
  can drop the true key. It is a throughput tool, not lossless: on a large keyspace
  with a full plugboard even a good `N` recovers only around half of the hardest
  keys.

A good general recipe for a hard (short) message with a known rotor key:

```sh
./enigma -c -R 20 -S iq -l english -u B -w 241 -r AAA -g QEW < cipher.txt
```

When you also brute-force the rotor key, add `-F` to shortlist keys and `-T` for
threads:

```sh
./enigma -c -R 20 -S iq -F 10% -T 4 -l english -u . -w ... -r AAA -g ... < cipher.txt
```

Increase `-R` for harder messages.

## Input, output and diagnostics

- **Input** comes from stdin; only A–Z are kept (case, spaces and punctuation are
  stripped).
- **Output** is the single best-scoring plaintext, on stdout.
- **Diagnostics** go to stderr: the resolved configuration is echoed at the
  start, the running best is shown during the search, and two final lines report
  the number of rotor combinations analysed and plugboards scored, then wall-clock
  time, thread count, the precomputed-table memory and peak memory.
  With `-F`, the pre-filter's ranking phase shows a live progress line (percentage
  of keys ranked) when stderr is a terminal.
- A scoring model is needed only when the run actually scores — a wildcard search
  or a plugboard hill-climb (`-c`). Those require `-l` (or `-i`). Pure
  encryption/decryption — a fully specified machine with no `-c` — needs no scoring
  options at all. (The default ring is `AA.`, so give an explicit `-r` to encrypt;
  otherwise the wildcard makes it a search and `-l`/`-i` is required again.) A fully
  specified run still prints a single-candidate score on stderr, and it honours the
  model you ask for: `-q -l english` scores that one decrypt with quadgrams; with no
  scoring options (or an n-gram model but no `-l`) it falls back to the index of
  coincidence, which needs no language.

## n-gram data files

Scoring uses letter-frequency tables read from `<datadir>/<language>_<ngram>.txt`,
where `<ngram>` is `monograms`/`bigrams`/`trigrams`/`quadgrams`. Each line is
`<LETTERS> <count>`, e.g. `TION 13168375`. The data directory is resolved as
`-d <dir>` → `$ENIGMA_DATA` → `.` (the current directory), so the tool can run
from any working directory.

At load time each count is converted to a log10 probability `log10(count/total)`,
with n-grams never seen in the corpus floored at `log10(0.01/total)` so that
gibberish (which contains impossible n-grams) is penalised rather than ignored. The
model score is the per-symbol average of these — a cross-entropy in dits/char, so
scores are negative and roughly length-independent (a more language-like decrypt
scores closer to zero). The index of coincidence (`-i`) is a separate normalised
statistic and is unaffected.

Tables for `english`, `german`, `danish` and `french` ship in this repository.
They were obtained from the
[Practical Cryptography](http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/)
website, where additional languages are available in the same format.

## Performance

The search is parallelised over the whole key space — reflectors, wheel orders,
ring settings and start positions — so `-T N` uses N worker threads even when the
wheels are fixed and only the rings/starts are being searched. The default is a
single thread; on a 4-core machine a search runs about 3× faster with `-T 4`, and
scaling can be measured with `make bench SCALE=1`. Results are independent of the
thread count (`-T` does not change which plaintext is found).

## References

The hill-climbing strategy is based on the algorithms described in the
[publications by Frode Weierud et al.](http://cryptocellar.org/Enigma/)

The software is available under the GNU GPL version 3 license.
Copyright (C) 2017–2026 Torbjørn Rognes.
