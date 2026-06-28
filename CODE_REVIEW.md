# Code Review — `enigma.cc`

A deep review of the Enigma cipher / code-breaking tool. The program is a
single ~1400-line C++ translation unit. It is functional and reasonably fast,
but it carries several real correctness bugs, a couple of memory-safety
hazards, a large amount of dead/experimental code, and structural issues that
make it hard to test and extend. This document is **analysis only** — no code
changes are proposed as patches here, only findings and recommendations.

Severity legend: 🔴 critical · 🟠 high · 🟡 medium · 🟢 low / nit.

---

## 1. Memory-safety bugs

### 1.1 🔴 Stack buffer overflow on long ciphertext (`best_plaintext[1025]`) — ✅ FIXED

> **Resolved:** the ciphertext is now capped at `maxlen = 1024` letters (the
> single length constant, reduced from the old 10240) and `readciphertext()`
> rejects longer input with a fatal error, so the `best_plaintext` buffer (sized
> `maxlen + 1`, like every other length buffer) can no longer overflow.



In `bruteforce()`:

```c
double best_score = -1e37;
char best_plaintext[1025];
...
strcpy(best_plaintext, plaintext);   // plaintext can be up to maxlen (10240) chars
...
strcpy(plaintext, best_plaintext);
```

`plaintext` is sized `maxlen + 1 = 10241`, and `readciphertext()` accepts up to
`maxlen = 10240` letters. But `best_plaintext` is only **1025 bytes**. Any
ciphertext longer than 1024 letters overflows the stack buffer on the very
first `strcpy`, corrupting the stack — a crash at best, exploitable memory
corruption at worst, on entirely ordinary input.

**Fix direction:** size `best_plaintext` as `maxlen + 1` (or make it a global of
the same type/size as `plaintext`), and prefer `memcpy(..., textlength + 1)` or
a bounded copy.

### 1.2 🟠 Fixed `filename[100]` buffer overflowable via `-l` — ✅ FIXED

In every `*_read()` function:

```c
char filename[100];
strcpy(filename, opt_language);          // opt_language is user-controlled (-l)
strcat(filename, "_quadgrams.txt");
```

`opt_language` came straight from `-l` with no length validation and no
allow-list check. A language argument longer than ~85 characters overflowed
`filename`.

**Resolved.** The four readers now build the filename with
`snprintf(filename, sizeof(filename), "%s_quadgrams.txt", opt_language)` (a
bounded write), and `main()` validates `-l` up front: 1–32 characters, letters
only — which also blocks path-traversal names like `../../etc/passwd`. Guarded
by an illegal-`-l` rejection test. (The four near-identical readers have since
been factored into one `ngrams_read()` — see §5.)

### 1.3 🟡 `readciphertext()` / `readplaintext()` do a single `read()` — ✅ FIXED

```c
len = read(STDIN_FILENO, buffer, maxlen);
```

`read()` may return fewer bytes than requested (pipes, terminals, large
inputs), so ciphertext could be silently truncated, and anything past the first
`maxlen` bytes was silently dropped.

**Resolved.** Both functions now loop on `read()` until EOF, filtering A–Z as
they go, and bound the letter count incrementally (fatal past `maxlen`).
A read error is reported instead of ignored, and the buffers are `unsigned char`
so `toupper()` is never handed a negative value. Guarded by a test that pipes
input larger than the read buffer (70 000 bytes before the letters) and checks
it is fully consumed.

### 1.4 🟡 Block decoder reads past `textlength` (uninitialized reads) — ✅ FIXED

`decode_num()` processed the message in fixed 16-byte blocks:

```c
for (int i = 0; i < textlength; i += 16) { ... map16_* over 16 elements ... }
```

When `textlength` was not a multiple of 16, the final block read
`num_ciphertext[]` and `mapping[]` rows beyond `textlength` (in-bounds of the
static arrays, but uninitialized) and wrote junk into `num_plaintext[]` past
`textlength`. Results were unaffected (scoring stops at `textlength - k`), but it
was undefined-ish and a trap for anyone reading the full `num_plaintext`.

**Resolved.** The block loop now runs over complete 16-byte blocks
(`textlength & ~15`) and a scalar remainder loop handles the tail, so nothing
past `textlength` is touched. Output is unchanged (the 25-letter `BDZGO` KAT
already exercises a non-multiple-of-16 length).

---

## 2. Correctness bugs

### 2.1 🔴 Index of coincidence is computed incorrectly — ✅ FIXED

Both `ic_score()` and `ic_score_decode()` computed:

```c
for (int j = 1; j < 26; j++)
    score += freq[j-1] * freq[j];   // product of ADJACENT letters' counts
```

This multiplied the frequency of each letter by the frequency of the *next
letter in the alphabet* (A·B + B·C + …). The index of coincidence is

  IC = Σ_i f_i·(f_i − 1) / (N·(N − 1))

i.e. each letter's count times itself, summed, normalized. The implemented
formula was not IC at all and had no cryptanalytic meaning; the `-i` scoring mode
was effectively broken. (It also skipped the `j = 0` term and never normalized.)

**How bad it was — measured.** Comparing the old formula against a correct,
normalized IC on a 297-letter English passage versus 2000 random strings of the
same length:

| metric | English | random mean | random max | random samples ≥ English |
|--------|---------|-------------|------------|--------------------------|
| old (adjacent-product) | 3483 | 3252 | 3509 | **3 / 2000** |
| correct IC | 0.0624 | 0.0385 | 0.0425 | **0 / 2000** |

The old metric put English only ~7 % above random noise with the distributions
overlapping (a random string outscored the real English), whereas correct IC
separates them cleanly (English sits far beyond the random maximum). End to end:
with the old formula a brute-force start-position search under `-i` returned the
**wrong** key on a no-plugboard test; with the corrected formula it recovers the
plaintext. The corrected formula was applied to both functions, summed over all
26 letters and normalized by `N·(N−1)` (so the value is the standard ≈0.067 for
English), and is guarded by a new `-i` recovery test in the suite.

### 2.2 🟠 `fscanf` partial-match leaves variables uninitialized — ✅ FIXED

In all n-gram readers, e.g. monograms:

```c
int ret = fscanf(f, "%c %d\n", &a, &count);
if (ret > 0) {                       // true even when ret == 1
    if ((a >= 'A') && (a <= 'Z')) {
        monograms[char2num(a)] = count + 1.0;   // count may be uninitialized
```

`ret > 0` was taken whenever **any** field matched. If only `%c` matched
(`ret == 1`), `count` was read uninitialized; for bigrams/trigrams/quadgrams the
same applied to the later letters.

**Resolved.** Each reader now requires the **full** field count
(`ret != 2`/`3`/`4`/`5` → `break`) before using any parsed value, so a partial
match never feeds uninitialized data into the tables. The format strings also
gained a leading space (`" %c%c …"`) so the parser skips blank lines and stray
whitespace instead of misreading a newline as a letter, and the redundant
`if (ret < 1) break;` / `if (ret > 0)` pair in the trigram/quadgram readers is
gone. Guarded by a "messy file" parser test.

### 2.3 🟢 `total` is accumulated but never used (no normalization) — ✅ FIXED

Every reader maintained a `total` (the quadgram one as `unsigned int total`) and
incremented it, but never divided it into the counts. Scores are
`log10(count + 1)` of raw counts, not log-probabilities — which for ranking
candidate keys over a *fixed-length* ciphertext is only a constant offset, so
results are unaffected.

**Resolved.** The dead `total` variable was removed from all four readers. (The
ranking is unchanged; normalization was never needed for same-length
comparisons, so it was dropped rather than completed.)

### 2.4 🟢 Stepping model verified against a reference — ✅ ADDRESSED

`step_rotors()` implements the double-stepping anomaly:

```c
if (notch[middle]) { step left; step middle; }     // double step
else if (notch[right]) { step middle; }
step right;                                         // always
```

This is the standard model. It is now anchored externally by the test suite:

- A **double-stepping KAT** (`tests/run_tests.sh`) pinned to the documented
  rotor-position sequence `KDO KDP KDQ KER LFS LFT LFU` (wheel order III II I).
  Encrypting from `KDO` crosses the anomaly at the 4th letter, and the expected
  ciphertext (`AAAAAAAAAAAA → ULMHJCJJCWBY`) differs from the 4th letter on for
  any machine that omits the double step, so the test genuinely guards it.
- The authentic **1930 instruction-manual message** KAT (reflector A, wheels
  II I III, rings XMV, plugboard, start ABL) — a 90-letter external vector
  exercising reflector A, ring offsets and the plugboard.

Both were cross-checked with an independent reimplementation that reproduces the
canonical `AAAAA → BDZGO` vector, the 1930 message, and the published double-step
position sequence. (Note: only single notches are modeled; wheels VI–VIII
correctly carry `MZ` double notches in `notch_string`, which the position-based
table handles fine. The Norway-wheel notch tables (indices 8–12) reuse the
standard `Q/E/V/J/Z` turnover letters and remain covered only by round-trip
consistency, not an external KAT.)

### 2.5 🟢 Empty input causes division by zero / degenerate search — ✅ FIXED

If stdin yielded zero A–Z letters, `textlength == 0`: scoring loops were empty,
`bruteforce()` reported a meaningless "best", and `readplaintext()` (with `-p`)
divided by `textlength`. **Resolved:** `main()` now fails with
"Ciphertext is empty …" when `textlength < 1` (after echoing the settings), so
no downstream code runs on empty input. Guarded by a test.

---

## 3. Dead, experimental, and misleading code

The file carries a lot of half-finished or abandoned code that obscures the
working path:

- 🟠 **`all_subst_score()`** computed plug "scores" as `random() % 10000` — pure
  random numbers. Never called (its call site in `hillclimb` was commented out),
  entirely vestigial and actively misleading. ✅ **Removed** (with its
  `subst_score_s` struct, `subst_scores` global and `subst_score_comp`
  comparator, the file's only use of `random()`).
- 🟡 **`best_steckerbrett[26]`** in `hillclimb()` was filled via `memcpy` but
  never read again — and the copy read `26*sizeof(int)` (104) bytes from the
  26-byte `steckerbrett`, an out-of-bounds read the compiler warned about.
  ✅ **Removed** (clears the only `-Wall` warning).
- 🟡 **`map()`** was unused and its parameter was named `map`, shadowing the
  function name. ✅ **Removed.** (`map16_direct`/`map16_step` are kept — they are
  used inside `decode_num`.)
- 🟡 **`opt_threads`** and **`opt_logfilename`** were declared and initialized but
  never used (no `-T`/threading and no logging despite the names). ✅ **Removed.**
  (The "load triplet scores …" comment inside `quadgram_score_decode` still
  describes SIMD work that does not exist; git history shows SIMD code was added
  then removed.)
- 🟡 **`showit()`** is an entire function body wrapped in `#if 0` — a no-op.
  **Intentionally kept** as debug instrumentation (see note below).
- 🟡 **Unreachable tables:** M4 thin reflectors (indices 4–5) and Beta/Gamma
  rotors (indices 13–14) exist in the wiring tables but cannot be selected via
  any CLI option. **Kept** (reserved for a future M4/4-rotor mode; the index
  conventions are documented in `CLAUDE.md`). Wire them up or remove them later.
- 🟢 Numerous `#if 0` / `#if 1` blocks (`showsteckerbrett`, debug prints,
  `SHOWHILLCLIMB`) scattered through the search and hill-climb code.
  **Intentionally kept** as debug instrumentation.
- 🟢 `score_iter(int iter, ...)` ignores its `iter` argument entirely; callers
  pass `0` or `iter` inconsistently. **Kept** (the `iter` plumbing feeds the
  `SHOWHILLCLIMB` debug output).

The purely vestigial / buggy items above have been removed; the remaining
`#if 0` / `SHOWHILLCLIMB` / `showit` blocks are deliberately retained as debug
instrumentation rather than deleted.

---

## 4. API misuse, portability, and modern-C++ concerns

- 🟢 **Legacy `index()` from `<strings.h>`** ✅ replaced with the standard
  `strchr` in `init()`; `<strings.h>` and the unused `<sys/uio.h>` includes were
  dropped. (`<sys/types.h>` is now genuinely used for `ssize_t` in the read
  loops, so it stays.)
- 🟢 **`rotor_l`/`rotor_r` return `char`** but compute and are consumed as
  `int`s in 0–25. ✅ Changed to return `int`, avoiding any signed-`char`
  narrowing surprise on the values that later index `steckerbrett[...]`.
- 🟡 **String-literal-to-`char*`** assignments (`opt_ukw = (char*) ".";` etc.)
  cast away `const`, then `alltoupper`/`removespaces` mutate `optarg` (i.e.
  `argv`) in place. ✅ **Fixed:** the option globals are now `const char *` with
  uncast string-literal defaults, and the getopt cases call
  `alltoupper`/`removespaces` on the mutable `optarg` before assigning it. The
  `compare` qsort callback also casts `const void*` to `const int*` now. (Some
  `argv` mutation remains, but no `const` is cast away.)
- 🟢 **C++ written as C:** C stdio, raw global arrays, `qsort` with
  `void*` comparators, `struct subst_score_s { ... } subst_scores[...];`
  declared with a trailing global instance. No use of `std::` containers,
  `std::array`, RAII, or `constexpr`. This is a style/maintainability point, not
  a bug, but it forfeits a lot of compiler help.
- 🟢 **Build flags** ✅ now `-std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual
  -O3`, and the build is **warning-free** under them. Stronger optional flags
  were surveyed and left off because they reflect the deliberate C-style /
  global-state design rather than bugs, and would be noisy without a larger
  refactor:
  - `-Wshadow` → ~14 (mostly the global `textlength` shadowed by the same-named
    function parameters; see §5).
  - `-Wconversion` → ~38 (implicit `int`/`char`/`double` narrowings throughout
    the arithmetic).
  - `-Wold-style-cast` → ~12 (the remaining C-style `(int*)`/pointer casts; would
    want C++ `static_cast`/`reinterpret_cast` throughout).
  Enabling either would be a good ratchet once the corresponding cleanup
  (global-state refactor; C++-style casts) is done.

---

## 5. Structural / design issues

- 🟠 **Pervasive global mutable state.** Machine configuration
  (`walzenlage`, `grundstellung`, `ringstellung`, `ukw`, `steckerbrett`), the
  big lookup tables (`subst_array`, `mapping`), I/O buffers, and the n-gram
  tables are all file-scope globals. Consequences: the code is not reentrant or
  thread-safe (so the dangling `opt_threads` could never have worked without a
  rewrite), functions silently depend on global setup order
  (`precompute()` clobbers `ringstellung`/`grundstellung`, relying on
  `bruteforce` to reset them), and unit testing any piece in isolation is
  impractical.
- 🟡 **`textlength` is both a global and a parameter.** Nearly every function
  takes `int textlength` while a global `textlength` also exists. This shadowing
  is confusing and invites bugs where the wrong one is used.
- 🟡 **`bruteforce()` is a single ~110-line function with six nested loops** and
  inline result reporting. The wheel/ring/start range setup, the search, and the
  reporting should be separated; the deep nesting plus the
  `if ((w1!=w2)&&(w1!=w3)&&(w2!=w3))` permutation guard make it hard to follow
  and easy to break.
- 🟡 **Duplicated scoring logic.** `quadgram_score`/`trigram_score`/… (operating
  on `char*`) and `*_score_decode` (operating on `num_plaintext`) are parallel
  implementations of the same math; the non-`decode` variants appear unused in
  the hot path. (The four copy-paste n-gram *readers* have been unified into a
  single `ngrams_read(n, table, suffix)` ✅; the parallel *scorers* remain.)
- 🟡 **Magic numbers.** The high-value semantic ones have been named ✅: the
  scoring models are an `enum` (`SCORE_IC` … `SCORE_QUAD`), the Norway table
  offsets are `norway_reflector_index` / `norway_rotor_base` (used by both
  `init_walzen` and `showconfig`), the decode block width is `blocksize`, and
  the search/hill-climb "−infinity" sentinel is a single `score_min` (hill-climb
  was converted to a `do`/`while` so it no longer needs two priming values).
  Still pending (a mechanical sweep): the pervasive literal `26` → `asize`,
  wheel-count `3` → `wheels`, and `65` → the `'A'` character literal in
  `char2num`/`num2char`.

---

## 6. Performance observations

The hot path is already thoughtfully optimized (precompute the rotor stack into
`subst_array`, fold stepping into a per-position `mapping`, score by table
lookup, 16-byte blocking). Remaining opportunities:

- 🟡 **No parallelism.** `bruteforce()` is embarrassingly parallel over
  reflector × wheel-order (and ring/start). The scaffolding (`opt_threads`)
  hints this was intended but the global state blocks it. Encapsulating machine
  state into a struct would unlock multi-threading for a near-linear speedup.
- 🟡 **`setup_mapping()` rebuilds the full per-position mapping for every
  ring/start combination**, including re-stepping the rotors from scratch. For
  long messages this is a large repeated cost; some of it could be shared across
  start positions.
- 🟢 **Quadgram table is ~457 KB×8 = 3.6 MB of `double`s** (`[26]^4 * 8`),
  plus `subst_array` (~457 KB) and `mapping` (~266 KB) as zero-initialized BSS.
  Using `float` for n-gram scores would halve the largest table and improve
  cache behavior in the inner loop with negligible accuracy loss.
- 🟢 The `decode_num()` "scalar vs blocked" choice is locked at compile time via
  `#if 0`; there is no measured justification in-tree that the 16-byte blocking
  actually wins for the relevant message lengths.

---

## 7. Robustness, UX, and tooling

- 🟡 **Relative-path data files.** N-gram files are opened relative to the CWD
  (`fopen("german_quadgrams.txt")`). Running the binary from anywhere but the
  repo root fails. Consider a data-directory option/env var or installing the
  tables to a known prefix.
- 🟡 **No validation of `-l`** against the known languages (see 1.2), and no
  validation that `-x` is consistent with the selected wheel set beyond the
  range check.
- 🟢 **Inconsistent exit/usage:** running with no args prints help and exits `1`;
  `-h` exits `0`. Errors go through `fatal()` (exit 1) but some validation
  messages are printed inline. Acceptable, but worth standardizing.
- 🟢 **Tests and CI added.** For a cryptographic tool whose correctness is
  subtle (stepping anomaly, ring/start offset arithmetic, plugboard
  involution), the absence of any round-trip or known-answer tests was the
  single biggest risk to long-term correctness. A test suite now exists
  (`tests/run_tests.sh`, run via `make test`) covering the canonical
  `AAAAA → BDZGO` known-answer vector, reciprocity, plugboard, ring/start
  offsets, the double-stepping anomaly, the Norway variant, input filtering and
  the 1024-character limit, plus end-to-end cracking — brute-force start-position
  and plugboard hill-climb matrices over every scoring model (IC/mono/bi/tri/quad)
  in every language (german/english/danish/french); the hill-climb matrix uses
  long plaintexts and a small (2-pair) plugboard so every model converges. The
  double-stepping anomaly is covered by
  an externally-anchored known-answer test (see §2.4), alongside the authentic
  1930 instruction-manual message vector.

  CI (`.github/workflows/ci.yml`) runs, on every push and pull request: the
  suite built `-Werror` under **both g++ and clang++**; the suite under
  **ASan + UBSan** (plus extra `-p` / scoring-mode coverage); representative
  invocations under **valgrind** (catches uninitialised-memory use ASan misses);
  **cppcheck**; **clang-tidy** (curated `bugprone`/`clang-analyzer`/`performance`
  checks via `.clang-tidy`); and **shellcheck** on the test harness. A separate
  **CodeQL** workflow (`.github/workflows/codeql.yml`) runs on PRs and weekly.
- 🟢 **`Makefile`** has `test` and `clean` targets and an `EXTRA_CXXFLAGS` hook
  (used by CI for `-Werror` and sanitizer builds). Still no `install` target and
  the data files are not listed as dependencies.

---

## 8. Prioritized summary

| # | Severity | Issue |
|---|----------|-------|
| 1.1 | 🔴 | ~~`best_plaintext[1025]` overflow for ciphertext > 1024 letters~~ ✅ fixed (input capped at 1024 + validated) |
| 2.1 | 🔴 | ~~Index of coincidence formula is wrong (`-i` broken)~~ ✅ fixed (Σ f·(f−1)/N(N−1) + recovery test) |
| 7 | 🟢 | ~~No tests / CI~~ ✅ test suite (`make test`) + GitHub Actions CI added |
| 1.2 | 🟢 | ~~`-l` can overflow `filename[100]`; no language allow-list~~ ✅ fixed (snprintf + `-l` validation) |
| 2.2 | 🟢 | ~~`fscanf` partial matches use uninitialized variables~~ ✅ fixed (require full field count) |
| 3 | 🟢 | ~~Dead/misleading code (`all_subst_score` = random, OOB `memcpy`, etc.)~~ ✅ vestigial/buggy code removed; debug kept |
| 5 | 🟠 | Pervasive global state blocks testing and threading |
| 1.3/1.4 | 🟢 | ~~Single `read()` truncation; 16-byte block over-read past `textlength`~~ ✅ fixed (read loop + scalar remainder) |
| 2.3/2.4 | 🟢 | ~~Unused `total`~~ ✅ removed; ~~stepping unverified~~ ✅ double-step KAT added |
| 4/5/6 | 🟡 | ~~Legacy `index()`, `char` returns, `const` literals~~ ✅ fixed; duplicated readers/scorers, no parallelism remain |
| 2.5/7 | 🟢 | ~~Empty-input div-by-zero~~ ✅ fixed; relative data paths; weak Makefile |

**Progress:** nearly every finding is resolved — (1.1) the stack overflow,
(1.2) the `-l`/filename overflow, (1.3/1.4) the read-loop and block over-read,
(2.1) the IC formula, (2.2) the `fscanf` partial-match bug, (2.3) the unused
`total`, (2.4) stepping verification, (2.5) the empty-input guard, (3) the
dead/misleading-code cleanup, the §4 modernization (legacy `index()` → `strchr`,
`int` rotor returns, `const`-correct options, stray includes), the four n-gram
readers unified into one (§5/§6), and (7) the test suite + CI. The build is
warning-free under `-std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual` (g++ and
clang++), and the suite has 63 checks. Remaining items — the parallel
`*_score`/`*_score_decode` scorers, and the larger global-state refactor that
would unlock threading and clear the `-Wshadow` noise (§5).
