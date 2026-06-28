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

> **Resolved:** the ciphertext is now capped at `maxtextlen = 1024` letters and
> `readciphertext()` rejects longer input with a fatal error, so the
> `best_plaintext` buffer (sized `maxtextlen + 1`) can no longer overflow.



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

### 1.2 🟠 Fixed `filename[100]` buffer overflowable via `-l`

In every `*_read()` function:

```c
char filename[100];
strcpy(filename, opt_language);          // opt_language is user-controlled (-l)
strcat(filename, "_quadgrams.txt");
```

`opt_language` comes straight from `-l` with no length validation and no
allow-list check. A language argument longer than ~85 characters overflows
`filename`. Even short of overflow, an arbitrary `-l` value just produces a
"file not found" fatal error. The same unchecked pattern is duplicated in four
functions.

**Fix direction:** validate `-l` against the known set, use `snprintf` with the
buffer size, and factor the four near-identical readers into one.

### 1.3 🟡 `readciphertext()` / `readplaintext()` do a single `read()`

```c
len = read(STDIN_FILENO, buffer, maxlen);
```

`read()` may return fewer bytes than requested (pipes, terminals, large
inputs), so ciphertext can be silently truncated. There is also no loop to
consume input beyond `maxlen`; anything past the first `maxlen` bytes is
silently dropped with no warning. Prefer a read loop (or `fread`) and warn on
truncation.

### 1.4 🟡 Block decoder reads past `textlength` (uninitialized reads)

`decode_num()` processes the message in fixed 16-byte blocks:

```c
for (int i = 0; i < textlength; i += 16) { ... map16_* over 16 elements ... }
```

When `textlength` is not a multiple of 16, the final block reads
`num_ciphertext[]` and `mapping[]` rows beyond `textlength`. Those rows are
within the static arrays (so no segfault), but `mapping[]` is only initialized
up to `textlength - 1` by `setup_mapping()`, so this reads uninitialized data
and writes junk into `num_plaintext[]` past `textlength`. The scoring loops stop
at `textlength - k`, so the result is not corrupted, but this is undefined-ish
behavior and a trap for anyone who later reads the full `num_plaintext`. The
remainder should be handled explicitly (or the arrays zero-initialized and the
tail masked).

---

## 2. Correctness bugs

### 2.1 🔴 Index of coincidence is computed incorrectly

Both `ic_score()` and `ic_score_decode()` compute:

```c
for (int j = 1; j < 26; j++)
    score += freq[j-1] * freq[j];   // product of ADJACENT letters' counts
```

This multiplies the frequency of each letter by the frequency of the *next
letter in the alphabet* (A·B + B·C + …). The index of coincidence is

  IC = Σ_i f_i·(f_i − 1) / (N·(N − 1))

i.e. each letter's count times itself, summed, normalized. The implemented
formula is not IC at all and has no cryptanalytic meaning; the `-i` scoring mode
is effectively broken. (It also skips the `j = 0` term and never normalizes.)

**Fix direction:** `score += freq[j] * (freq[j] - 1);` over all 26 letters
(normalization is optional since comparisons are same-length).

### 2.2 🟠 `fscanf` partial-match leaves variables uninitialized

In all n-gram readers, e.g. monograms:

```c
int ret = fscanf(f, "%c %d\n", &a, &count);
if (ret > 0) {                       // true even when ret == 1
    if ((a >= 'A') && (a <= 'Z')) {
        monograms[char2num(a)] = count + 1.0;   // count may be uninitialized
```

`ret > 0` is taken whenever **any** field matched. If only `%c` matched
(`ret == 1`), `count` is read uninitialized. For bigrams/trigrams/quadgrams the
same applies to the later letters. The bigrams reader additionally treats
`ret == 0` and EOF the same as a partial match boundary only via the `else
break`. The correct guard is to require the **full** field count
(`ret == 2`/`3`/`4`/`5`). The trigram/quadgram readers redundantly test both
`if (ret < 1) break;` and `if (ret > 0)`.

This rarely bites because the bundled files are well-formed, but it is a latent
bug and makes the parser fragile to a trailing blank line or stray character.

### 2.3 🟡 `total` is accumulated but never used (no normalization)

Every reader maintains a `total` (and the quadgram one uses `unsigned int
total`) and increments it, but it is never divided into the counts. Scores are
therefore `log10(count + 1)` of raw counts, not log-probabilities. For ranking
candidate keys over a *fixed-length* ciphertext this is only a constant offset,
so results are unaffected — but the variable is dead and misleading, and the
code reads as if it intended Laplace-smoothed log-probabilities (the `+ 1` /
seeding tables with `1.0` confirms that intent). Either finish the
normalization or delete `total`.

### 2.4 🟡 Stepping model worth verifying against a reference

`step_rotors()` implements the double-stepping anomaly:

```c
if (notch[middle]) { step left; step middle; }     // double step
else if (notch[right]) { step middle; }
step right;                                         // always
```

This is the standard model and looks correct, but notch detection keys on the
*current* `grundstellung` before stepping, and the Norway-wheel notch tables
(indices 8–12) reuse the standard `Q/E/V/J/Z` turnover letters. There is no test
verifying encrypt/decrypt round-trips against a known reference (e.g. a known
Enigma message or another simulator). Given how subtle stepping is, this should
be pinned down with tests rather than read-by-eye. (Note also: only single
notches are modeled; wheels VI–VIII correctly carry `MZ` double notches in
`notch_string`, which the position-based table handles fine.)

### 2.5 🟢 Empty input causes division by zero / degenerate search

If stdin yields zero A–Z letters, `textlength == 0`: scoring loops are empty,
`bruteforce()` reports a meaningless "best", and `readplaintext()` (with `-p`)
divides by `textlength` → `100.0 * identical / 0`. There is no guard for empty
ciphertext.

---

## 3. Dead, experimental, and misleading code

The file carries a lot of half-finished or abandoned code that obscures the
working path:

- 🟠 **`all_subst_score()`** computes plug "scores" as `random() % 10000` — pure
  random numbers. It is never called (its call site in `hillclimb` is commented
  out). The real scoring lines are all `#if 0`'d out. This function is entirely
  vestigial and actively misleading.
- 🟡 **`best_steckerbrett[26]`** in `hillclimb()` is filled via `memcpy` but
  never read again — dead.
- 🟡 **`map()`** (line ~559) is unused, and its parameter is named `map`,
  shadowing the function name. **`map16_direct`/`map16_step`** are used only
  inside `decode_num`; the simpler scalar paths next to them are `#if 0`'d.
- 🟡 **`showit()`** is an entire function body wrapped in `#if 0` — a no-op.
- 🟡 **`opt_threads`** and **`opt_logfilename`** are declared and initialized but
  never used; there is no `-T`/threading and no logging despite the names. The
  header comment block and the "load triplet scores …" comment inside
  `quadgram_score_decode` describe SIMD/threading work that does not exist
  (git history shows SIMD code was added then removed).
- 🟡 **Unreachable tables:** M4 thin reflectors (indices 4–5) and Beta/Gamma
  rotors (indices 13–14) exist in the wiring tables but cannot be selected via
  any CLI option. Either wire up an M4/4-rotor mode or remove them and the
  associated comments to avoid implying support that isn't there.
- 🟢 Numerous `#if 0` / `#if 1` blocks (`showsteckerbrett`, debug prints,
  `SHOWHILLCLIMB`) scattered through the search and hill-climb code.
- 🟢 `score_iter(int iter, ...)` ignores its `iter` argument entirely; callers
  pass `0` or `iter` inconsistently.

Recommendation: delete the vestigial code (or move genuinely useful debug
output behind a real `-d/--verbose` flag) so the ~30% of the file that is noise
stops competing with the parts that matter.

---

## 4. API misuse, portability, and modern-C++ concerns

- 🟡 **Legacy `index()` from `<strings.h>`** is used in `init()` instead of the
  standard `strchr`. `index` is removed in POSIX-2008 deprecation tracks and is
  non-standard C++. Also `<sys/uio.h>`/`<sys/types.h>` are included but unused.
- 🟡 **`rotor_l`/`rotor_r` return `char`** but compute and are consumed as
  `int`s in 0–25. `char` may be signed; the values fit, but returning `int`
  would be clearer and avoids any narrowing surprises. `substitute` chains these
  through `steckerbrett[...]` indexing, so a stray negative would index out of
  bounds.
- 🟡 **String-literal-to-`char*`** assignments (`opt_ukw = (char*) ".";` etc.)
  cast away `const`, then `alltoupper`/`removespaces` mutate `optarg` (i.e.
  `argv`) in place. Mutating `argv` and casting away `const` on literals is
  legal-but-smelly; with `-Wwrite-strings` these would warn. Prefer `const
  char*` defaults and copy before mutating.
- 🟢 **C++ written as C:** C stdio, raw global arrays, `qsort` with
  `void*` comparators, `struct subst_score_s { ... } subst_scores[...];`
  declared with a trailing global instance. No use of `std::` containers,
  `std::array`, RAII, or `constexpr`. This is a style/maintainability point, not
  a bug, but it forfeits a lot of compiler help.
- 🟢 **Build flags** are only `-Wall -O3`. Adding `-Wextra -Wshadow
  -Wconversion -Wwrite-strings` would surface several of the issues above
  (shadowed `map`, signed/`char` returns, `const` literals, unused vars).

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
  the hot path. Likewise the four n-gram readers are copy-paste of one another.
- 🟢 **Magic numbers** throughout: `65`, `26`, `1025`, `100`, `10000`,
  offsets `+3`/`+8`/`+10`/`-7` for Norway indexing in `init_walzen` and
  `showconfig`. The Norway offset logic in particular is duplicated and easy to
  get out of sync.

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
- 🟠 **No CI (tests added).** For a cryptographic tool whose correctness is
  subtle (stepping anomaly, ring/start offset arithmetic, plugboard
  involution), the absence of any round-trip or known-answer tests was the
  single biggest risk to long-term correctness. A test suite now exists
  (`tests/run_tests.sh`, run via `make test`) covering the canonical
  `AAAAA → BDZGO` known-answer vector, reciprocity, plugboard, ring/start
  offsets, the double-stepping anomaly, the Norway variant, input filtering and
  the 1024-character limit, plus end-to-end cracking (hill-climb and
  brute-force recovery). **Still outstanding:** there is no CI to run it
  automatically, and the suite does not yet include an externally-anchored
  double-step KAT (it relies on round-trip consistency for that case, which
  cannot catch a symmetric stepping bug — see §2.4).
- 🟢 **`Makefile`** has no `clean`, `install`, or `debug` target and does not
  list the data files as dependencies. A debug build (`-O0 -g
  -fsanitize=address,undefined`) target would immediately flag §1.1 and §1.4.

---

## 8. Prioritized summary

| # | Severity | Issue |
|---|----------|-------|
| 1.1 | 🔴 | ~~`best_plaintext[1025]` overflow for ciphertext > 1024 letters~~ ✅ fixed (input capped at 1024 + validated) |
| 2.1 | 🔴 | Index of coincidence formula is wrong (`-i` broken) |
| 7 | 🟠 | ~~No tests~~ ✅ test suite added (`make test`); CI still missing |
| 1.2 | 🟠 | `-l` can overflow `filename[100]`; no language allow-list |
| 2.2 | 🟠 | `fscanf` partial matches use uninitialized variables |
| 3 | 🟠 | Large amount of dead/misleading code (`all_subst_score` = random, etc.) |
| 5 | 🟠 | Pervasive global state blocks testing and threading |
| 1.3/1.4 | 🟡 | Single `read()` truncation; 16-byte block over-read past `textlength` |
| 2.3/2.4 | 🟡 | Unused `total` (no normalization); stepping unverified |
| 4/5/6 | 🟡 | Legacy `index()`, `char` returns, duplicated readers/scorers, no parallelism |
| 2.5/7 | 🟢 | Empty-input div-by-zero; relative data paths; weak Makefile |

**Progress:** (1.1) the stack overflow and (7) the test suite are now done; the
tests give confidence to safely tackle the remaining high-value items — (2.1)
the IC formula, the dead-code cleanup (§3, including the `best_steckerbrett`
out-of-bounds `memcpy` that the compiler already warns about), and eventually
the global-state refactor that would unlock threading.
