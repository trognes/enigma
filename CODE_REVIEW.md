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

**Resolved.** Originally fixed by running the block loop over complete 16-byte
blocks (`textlength & ~15`) with a scalar remainder loop for the tail. The blocked
decoder has since been **removed entirely** — the scorers now fuse a scalar
decode into their loop (see §6), which has no block remainder to mishandle.
Output is unchanged (the 25-letter `BDZGO` KAT exercises a non-multiple-of-16
length).

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

The rotor stepping (inlined in `setup_mapping()`) implements the double-stepping
anomaly:

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
  function name. ✅ **Removed.** (`map16_direct`/`map16_step` were later removed
  too, when the blocked `decode_num` was dropped in favour of the fused scalar
  scorers — see §6.)
- 🟡 **`opt_threads`** and **`opt_logfilename`** were declared and initialized but
  never used (no `-T`/threading and no logging despite the names). ✅ **Removed.**
  (The "load triplet scores …" comment inside `quadgram_score_decode` still
  describes SIMD work that does not exist; git history shows SIMD code was added
  then removed.)
- 🟡 **`showit()`** is an entire function body wrapped in `#if 0` — a no-op.
  **Intentionally kept** as debug instrumentation (see note below).
- 🟢 **M4 (4-rotor naval) mode — ✅ IMPLEMENTED** (`-4`). The M4 thin reflectors
  (indices 4–5, UKW-b/c) and Beta/Gamma rotors (indices 13–14), previously present
  in the wiring tables but unreachable, are now selectable. The implementation
  follows the design below exactly; the as-built notes are summarised after it:
  - **Modelling (cheap — leaves the hot path untouched):** the M4's 4th "Greek"
    wheel (Beta/Gamma) is *static* — it does not step (note its empty notch), so
    it folds into the reflector. Build a composite **effective reflector**
    `greek ∘ thin ∘ greek⁻¹` (still an involution) from the chosen Greek wheel /
    position / ring and thin reflector, and use it at the single
    `reflector[m.ukw][x]` site in `subst_rotors`. The machine therefore stays a
    *3-stepping-rotor* engine (`wheels` stays 3); `subst_array`, `setup_mapping`,
    the stepping/double-step and the fused scorers are all unchanged.
  - **Search:** add outer loops over thin reflector (×2), Greek wheel (×2) and
    Greek position (×26, plus optional ring), recomputing the effective reflector
    and re-running `precompute()` per Greek config. The Greek position being
    wildcarded multiplies the 3-rotor search space by 26 — a strong motivator to
    land threading first.
  - **CLI (agreed):** an `-4` mode flag (mirroring `-n` for Norway); in M4 mode
    `-u` selects the thin reflector (`b`/`c`/`.`) and `-w`/`-r`/`-g` take **four**
    characters with the Greek wheel first. Add a validation branch alongside the
    Norway one, and print the 4th wheel in `showconfig`/`show_settings`.
  - **Testing:** anchor to a published M4 known-answer vector (e.g. a U-boat
    message), cross-checked against an independent reference, plus a round-trip.
  - **Memory / time cost (analysed).** The effective-reflector design leaves the
    hot engine untouched, so each precomputed table stays `26⁴ = 456 976 B`
    (≈0.457 MB) — and `struct machine` grows only by the 4th wheel position/ring
    plus a 26-byte effective-reflector buffer (negligible). What grows is the
    *number* of tables: wildcarding the thin reflector (×2), the Greek wheel (×2)
    and the Greek position−ring **offset** (×26 — only the offset matters, the same
    folding `subst_array` uses for start−ring, so 26 and not 676) adds up to ×104
    distinct effective reflectors. Whether that ×104 lands on memory or time is a
    design choice:
    - *Precompute-everything-up-front* (today's model, one `new`-ed block guarded
      at 8 GB): peak memory scales by the ×104. Fixed wheels + full Greek wildcard
      is ~48 MB (fine); also wildcarding the wheel order (336 orders of I–VIII)
      is `336 × 104 × 0.457 MB ≈ 16 GB`, which trips the 8 GB guard and aborts.
    - *Greek config as an outer loop* (recommended — re-run `precompute()` per
      Greek setting, reusing the buffer): peak memory stays essentially flat vs
      today (a full wheel-order search is ~82 MB today and stays ~82 MB), and the
      ×104 becomes recomputation **time** instead. This is the real reason to land
      threading first — to absorb the time cost, not a memory cost.
  - **As built.** Folded the Greek wheel into a per-task **effective reflector**
    `m.reflector_eff`, resolved once per task by `set_effective_reflector()` (so
    `subst_rotors` reads it and the hot `subst_array`/`setup_mapping`/scorer path
    is byte-for-byte unchanged — confirmed by `make bench BASE=dev` parity under
    g++ and clang). The search **folds the Greek config into the task list** (the
    chosen option): `wheel_task` carries the Greek wheel + offset, and
    `bruteforce()` enumerates thin × Greek wheel × *distinct* Greek offsets ×
    wheel orders (the pos/ring ranges collapse to ≤26 offsets). The precompute
    guard was raised to **16 GiB** to admit a full M4 wildcard (~15 GB). CLI: the
    Greek wheel is denoted **B/G** (Beta/Gamma) as the first character of `-w`;
    `-n` and `-4` are mutually exclusive. Correctness is anchored on the
    documented backward-compatibility equivalence (thin `b` + Beta@A ≡ reflector
    B, `c` + Gamma@A ≡ C) plus round-trip, offset/wheel-sensitivity, search
    recovery, and `-T` determinism tests (`tests/run_tests.sh`, 9 new checks).
    Verified clean under g++/clang `-Werror`, ASan/UBSan, TSan, valgrind, cppcheck,
    clang-tidy. Took ~the estimated half-day.
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
  -Wshadow -O3`, and the build is **warning-free** under them. `-Wshadow` has
  been enabled (see §5 — the `textlength` shadowing it flagged is fixed). The
  remaining stronger optional flags were surveyed and left off because they
  reflect the deliberate C-style design rather than bugs, and would be noisy
  without a larger refactor:
  - `-Wconversion` → ~38 (implicit `int`/`char`/`double` narrowings throughout
    the arithmetic).
  - `-Wold-style-cast` → ~12 (the remaining C-style `(int*)`/pointer casts; would
    want C++ `static_cast`/`reinterpret_cast` throughout).
  Enabling either would be a good ratchet once the corresponding cleanup
  (global-state refactor; C++-style casts) is done.

---

## 5. Structural / design issues

- 🟢 **Global mutable state encapsulated, and the search is multi-threaded** ✅.
  The per-search *mutable* state (`walzenlage`, `grundstellung`, `ringstellung`,
  `ukw`, `steckerbrett`, and the big working tables `subst_array`, `mapping`,
  plus the candidate `plaintext`) was gathered into a single `struct machine`,
  threaded through the search/scoring functions (`init_*`, `rotor_*`,
  `subst_rotors`, `precompute`, `setup_mapping`, `decode`, the `*_score_decode`
  scorers, `score_iter`, `hillclimb`, `search_worker`). The read-only data (the
  wiring tables built by `init()`, the n-gram statistics, and the `ciphertext` /
  `num_ciphertext` / `textlength` input) is intentionally left shared — safe to
  read from many threads. Several dead/obsolete functions were removed in the
  process (`step`, `substitute`, `step_precomputed`, `init_steckerbrett_direct`,
  and the 16-byte-blocked decode helpers).
  On top of that, `bruteforce()` runs `-T N` worker threads (default 1, max 256)
  over the **flat reflector × wheel-order × ring × start key space** (see §6 for
  the two-phase precompute-all-then-sweep structure); the global best is merged
  under a mutex (which also serialises the live progress line). Clean under
  **ThreadSanitizer**; the result is independent of thread count (determinism
  checks for the wheel-order and ring/start axes are in the suite). Scaling is
  ~3.3–3.5× on 4 cores for both axes (`make bench SCALE=1`). Getting the
  encapsulation perf-neutral first took three layout/codegen mitigations (see
  §6); the naive version had cost ~20–60% on clang/ARM and ~10–14% on the g++
  search path.
- 🟢 **`textlength` global/parameter shadowing** ✅ resolved. Nearly every
  function used to take an `int textlength` parameter while a file-scope global
  `textlength` also existed; every call site already passed exactly that global
  (likewise for the `ciphertext`/`plaintext` buffer parameters). The redundant
  parameters were removed so these functions (`setup_mapping`, `decode`, the five
  `*_score_decode` scorers, `score_iter`, `ciphertext_letterdist`, `hillclimb`)
  read the globals directly, and `-Wshadow` is now on in the build to keep it
  that way.
- 🟡 **`bruteforce()` is a single ~110-line function with six nested loops** and
  inline result reporting. The wheel/ring/start range setup, the search, and the
  reporting should be separated; the deep nesting plus the
  `if ((w1!=w2)&&(w1!=w3)&&(w2!=w3))` permutation guard make it hard to follow
  and easy to break.
- 🟢 **Duplicated scoring logic** ✅ resolved. The `char*`-based
  `quadgram_score`/`trigram_score`/`bigram_score`/`monogram_score`/`ic_score`
  family was a parallel, **unused** copy of the live `*_score_decode` family
  (which `score_iter` calls); the dead `char*` scorers have been removed. The
  four copy-paste n-gram *readers* were likewise unified into a single
  `ngrams_read(n, table, suffix)`.
- 🟢 **Magic numbers** ✅ named. Semantic ones: the scoring models are an `enum`
  (`SCORE_IC` … `SCORE_QUAD`); the Norway table offsets are
  `norway_reflector_index` / `norway_rotor_base` (used by both `init_walzen` and
  `showconfig`); and the search/hill-climb
  "−infinity" sentinel is a single `score_min` (hill-climb was converted to a
  `do`/`while` so it no longer needs two priming values). The mechanical sweep is
  also done: the pervasive literal `26` is now `asize`, wheel-count `3` is
  `wheels`, and `65` is the `'A'` character literal in `char2num`/`num2char`
  (only the `asize` definition and explanatory comments keep the literal 26).

---

## 6. Performance observations

The hot path is already thoughtfully optimized (precompute the rotor stack into
`subst_array`, fold stepping into a per-position `mapping`, score by table
lookup, 16-byte blocking).

> **Benchmark in place.** `tests/bench.sh` (`make bench`) now measures the two
> hot paths separately — `search` (brute-force scan, no plugboard) and
> `hillclimb` (the `-c` loop) — with a same-machine A/B mode
> (`make bench BASE=<ref>`) that fails on a >10% slowdown. Run it around the
> global-state/threading refactor below to guard single-thread throughput before
> chasing the parallel speedup.

Remaining opportunities:

- 🟢 **Parallelism** ✅ implemented, at the **flat key level** (reflector ×
  wheel-order × ring × start), not just wheel order. `bruteforce()` runs `-T N`
  worker threads (default 1, max 256) in two phases:
  1. **Precompute every wheel order's rotor table once, in parallel**, into one
     shared read-only block (a table depends only on reflector+wheel-order and
     serves all rings/starts via the start−ring offset; no early exit, so all are
     needed). Memory = `#wheel-orders × 457 KB` — bounded (~460 MB worst case for
     standard Enigma at `-u . -x 8`, ~82 MB for the `-x 5` default), and tiny in
     the fixed-wheels case. Guarded against absurd sizes.
  2. **Sweep the whole flat key space**: an atomic counter hands out
     adaptive-sized chunks (`≈ total/(threads·16)`); each worker decodes/scores
     its keys against the shared tables with its own small private `machine`
     (mixed-radix unflatten of the index → ring/start combo; the table pointer is
     swapped, never recomputed, when a chunk crosses a wheel-order boundary).
  This parallelises **rings and starts**, so a search with the wheels fixed but
  ring/start wildcarded now uses every thread — the previous wheel-order-only
  scheme left exactly that case single-threaded. Race-free (ThreadSanitizer);
  result independent of `-T` (determinism checks for both the wheel-order and the
  ring/start axis are in the suite). Scaling ~3.3–3.5× on 4 cores on a
  substantial job for **both** axes (`make bench SCALE=1`); a final stderr
  diagnostic reports wall-clock time, thread count, table count/size, and peak
  RSS.
- 🟢 **`setup_mapping()` no longer copies a full row per position for the scan**
  ✅ (partial). It used to `memcpy` the 26-byte `subst_array` row into `mapping[i]`
  for every (ring, start) — but the scan only ever reads one entry of each row
  (the plugboard is fixed), so 25/26 of that copy was wasted. Now it stores a
  *pointer* `rows[i]` to the row: the scan points straight into the shared
  `subst_array` (no copy) and skips its per-key `decode()` pass (the plaintext is
  materialised only when a new best is recorded), while hill-climbing — which
  re-reads each row hundreds of times at varying indices as it permutes the
  plugboard — still copies into the contiguous `mapping[]` for locality. Measured
  ~12% faster scan on g++ / ~2–5% on clang, hill-climb neutral-to-faster, no
  regression on either compiler.
  **Optimisation "B" — measured and rejected.** The rotor *stepping* is still
  re-run per (ring, start). Stepping is independent of the ring, and the machine
  cycles through a fixed period of 26·25·26 = 16 900 states, so in principle the
  stepped sequence could be precomputed once per wheel order (really per
  middle/right wheel pair) and reused across every start/ring. This was prototyped
  and benchmarked: a per-(middle,right,start) trajectory table (`g1[i]` and the
  left-rotor cumulative advance `g0Δ[i]`, with the right rotor `g2[i]` still
  trivial), filled once and reused across all rings and left-start positions, so
  the hot loop reads the trajectory instead of running the notch lookups,
  double-step branch, and carries. The prototype recovered byte-identical
  plaintext (the stepping is provably equivalent), but it was **slower**: a
  ~11.9 M-key scan ran **+18% on g++** and **−1% (noise) on clang** versus the
  current inline stepping. The win never materialises because (1) the notch
  branches fire only ~1/26 and ~1/676 of the time, so they are extremely
  well-predicted and nearly free, while (2) the trajectory table adds two L1 loads
  per position that the register-resident inline stepping does not pay.

  > **Note on the earlier "16–24 % ceiling" estimate.** A first, cruder spike
  > bypassed stepping by *freezing* `g0`/`g1` to constants, which timed ~16 %
  > (g++) / ~24 % (clang) faster — but that was a measurement artifact: with
  > `g0`/`g1` constant the compiler hoists the 2-D address `sa[g0−r0][g1−r1]` out
  > of the loop into one loop-invariant base pointer, collapsing the per-position
  > gather to a 1-D lookup. Real B keeps `g0`/`g1` varying and cannot capture that.
  > The refined spike above (genuinely varying positions, read from the reuse
  > table) is the faithful measurement. Lesson: a bypass spike that changes the
  > loop's invariants measures the wrong thing. Not worth revisiting unless the
  > scan's branch/cache balance changes substantially.
- 🟢 **n-gram tables are now `float`** ✅. The quadgram table was ~457 K×8 =
  3.6 MB of `double`s; storing the `log10` scores as `float` halves that to
  ~1.8 MB (and the trigram/bigram/monogram tables likewise), so it stays warmer
  in cache during the inner loop. The scorers accumulate the looked-up values
  into a `double`, so the score sum keeps full precision; cracking results are
  unchanged (the full `make test` cracking matrices still pass). The n-gram
  tables stay global; `subst_array`/`mapping` are per-`machine` (`subst_array`
  heap-allocated, `mapping` in the struct).
- 🟢 **Decode/score is now a single fused pass** ✅. The n-gram scorers decode
  each character once (`decode_at`) into a sliding window that indexes the n-gram
  table, instead of the old `decode_num` → `num_plaintext[]` scratch array →
  re-read two-pass. This removed `decode_num`, the `num_plaintext` member, and
  (earlier) the never-justified 16-byte-blocked decode (`map16_*`/`showit`/
  `blocksize`). Fusion is byte-identical to the two-pass version and ~3% faster
  on clang *search* and ~14% faster on clang *hill-climb* (less memory traffic;
  parity on g++).
- 🟡 **Struct encapsulation has an architecture-dependent hot-loop cost** worth
  remembering. Collapsing the separate global arrays into `struct machine`, done
  naively, cost ~20–60% under clang/Apple-silicon and ~10–14% on the g++ search
  path (large in-struct offsets past the 457 KB `subst_array`, lost no-alias
  assumptions, and a per-character read/modify/write of the rotor positions
  through the struct in the stepping loop). Three mitigations bring **both
  compilers back to parity** vs the pre-struct baseline and must be preserved:
  (1) `subst_array` is heap-allocated through its own pointer so the hot
  per-character tables keep small struct offsets; (2) the decode/score loops
  hoist member base pointers into `__restrict` locals; (3) `setup_mapping` holds
  the rotor positions in locals across its loop, writing back once. Always A/B
  with **both** compilers (`make bench BASE=<ref>` and
  `make bench CXX=clang++ BASE=<ref>`).

---

## 7. Robustness, UX, and tooling

- 🟢 **Relative-path data files** ✅ resolved. N-gram files are now read from a
  data directory built as `<datadir>/<lang>_<ngram>.txt`, with `datadir` resolved
  by strict precedence `-d <dir>` → `$ENIGMA_DATA` → `.` (the historical CWD
  default, so existing usage is unchanged). The binary can be run from any working
  directory; a missing/mistyped dir fails fast (before stdin) with the full path
  it tried, and the resolved dir is echoed in the settings. (A compile-time
  install prefix + `make install`, and executable-relative resolution, were
  considered and deferred — not needed until the tool is packaged.)

  **Considered and declined — embedding the n-gram tables into the binary.**
  Baking the language statistics into the executable (a single self-contained
  binary, no data files to ship) was weighed and **not pursued**. If done, the
  thing to embed is the processed *dense `float` tables* (~7.25 MiB for all four
  languages × four orders) — smaller than the ~12 MB of source text and skipping
  the startup parse — which would grow the binary from ~62 KB to ~7.5 MB. It was
  declined because: (1) `-d`/`$ENIGMA_DATA` already removed the only practical
  pain (running from any directory), leaving only single-file *distribution* as a
  benefit, which is not a goal; (2) it would carry all four languages even though a
  run uses one, and would hard-code the language set, regressing the deliberately
  *open* `<lang>_<type>.txt` extension point (§ validation note) unless the file
  loader were kept as a `-d` override (a hybrid); and (3) every embedding mechanism
  has friction — C23 `#embed` needs bumping past the pinned `-std=c++17` and a very
  new compiler; a generated multi-MB array literal compiles slowly; and an
  `objcopy`/`ld -r -b binary` blob is toolchain/platform-specific and bakes in
  `float` endianness. If single-file distribution ever *does* become a goal, the
  hybrid (generated dense-`float` built-ins + retained `-d` override) is the form
  to revisit.
- 🟢 **Option validation hardened** ✅. The n-gram table for the chosen model is
  now loaded **before** standard input is read, so a missing/mistyped `-l` fails
  immediately with the offending filename instead of after consuming stdin.
  Explicitly named wheels are checked for duplicates (`-w 112`, `-w 11.`, …):
  previously a repeated wheel passed validation but made `bruteforce`'s
  permutation guard skip every combination, silently emitting garbage — it is now
  rejected at validation time, and `bruteforce` additionally fails loudly if it
  ever scores zero configurations. (Validating `-l` against a *fixed* language
  list was deliberately avoided — arbitrary `<lang>_<type>.txt` files are a
  supported, documented extension point and the test suite relies on it.)
- 🟢 **Consistent exit/usage** ✅. `version()`/`help()` take an output stream:
  explicit `-h`/`-v` print to **stdout** and exit `0`, while usage errors (no
  arguments, bad option) print to **stderr** and exit `1`. Other errors continue
  to go through `fatal()` (stderr, exit 1).
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
  **ASan + UBSan** (plus extra `-p` / scoring-mode coverage); the suite under
  **ThreadSanitizer** plus an explicit multi-threaded crack (guards the `-T`
  search); representative invocations under **valgrind** (catches
  uninitialised-memory use ASan misses);
  **cppcheck**; **clang-tidy** (curated `bugprone`/`clang-analyzer`/`performance`
  checks via `.clang-tidy`); and **shellcheck** on the test harness. A separate
  **CodeQL** workflow (`.github/workflows/codeql.yml`) runs on PRs and weekly.
- 🟢 **`Makefile`** has `test`, `bench` and `clean` targets and an
  `EXTRA_CXXFLAGS` hook (used by CI for `-Werror` and sanitizer builds; the base
  flags include `-pthread`). Still no `install` target and the data files are not
  listed as dependencies.

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
| 5/6 | 🟢 | ~~Pervasive global *mutable* state blocks threading; no parallelism~~ ✅ encapsulated in `struct machine` and the search is multi-threaded (`-T N`, TSan-clean, ~3× on 4 cores) |
| 1.3/1.4 | 🟢 | ~~Single `read()` truncation; 16-byte block over-read past `textlength`~~ ✅ fixed (read loop + scalar remainder) |
| 2.3/2.4 | 🟢 | ~~Unused `total`~~ ✅ removed; ~~stepping unverified~~ ✅ double-step KAT added |
| 4/5/6 | 🟢 | ~~Legacy `index()`, `char` returns, `const` literals; `textlength` shadowing; global mutable state; no parallelism~~ ✅ all fixed |
| 2.5/7 | 🟢 | ~~Empty-input div-by-zero; relative data paths~~ ✅ fixed (`-d`/`$ENIGMA_DATA`); weak Makefile remains |

**Progress:** nearly every finding is resolved — (1.1) the stack overflow,
(1.2) the `-l`/filename overflow, (1.3/1.4) the read-loop and block over-read,
(2.1) the IC formula, (2.2) the `fscanf` partial-match bug, (2.3) the unused
`total`, (2.4) stepping verification, (2.5) the empty-input guard, (3) the
dead/misleading-code cleanup, the §4 modernization (legacy `index()` → `strchr`,
`int` rotor returns, `const`-correct options, stray includes), the four n-gram
readers unified into one and the dead `char*` scorer family removed (§5/§6),
the `textlength` global/parameter shadowing removed and `-Wshadow` enabled (§5),
the per-search state encapsulated into `struct machine`, the search
**multi-threaded** (`-T N`, default 1, max 256; TSan-clean; ~3× on 4 cores), and
(7) the test suite + CI. The build is warning-free under
`-std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -Wshadow` (g++ and clang++), and
the suite has 86 checks. **M4 (4-rotor naval) mode is now implemented** (`-4`;
§5) — the Greek wheel folds into an effective reflector, leaving the hot path
untouched. The open direction now is short-message **cracking quality**: the
`make crackquality` harness (with `SPLIT=1`) shows every miss is a *search*
failure, so the next lever is the plugboard search (§9), not scoring. Sharing the
rotor *stepping* across start positions (§6, optimisation
"B") was prototyped and **rejected** — it is byte-identical but ~18 % slower on
g++ / neutral on clang, because the notch branches are already near-free and the
trajectory table only adds memory traffic. The per-key row copy is already gone,
and the relative-path data-file issue is fixed via `-d`/`$ENIGMA_DATA`.

---

## 9. Plugboard hill-climb — algorithm and possible improvements

This section documents how plugboard recovery currently works and the algorithmic
alternatives, as reference for future cracking-quality work. Nothing here is a bug
— the current climb is a correct, textbook method — so these are **deferred /
optional** improvements, not findings.

> **Measuring stick in place.** `tests/crack_quality.py` (`make crackquality`)
> measures recovery quality vs ciphertext length on the hard (short) regime, so
> any change below can be evaluated before/after on the same problems
> (`BASE=<ref>` same-machine A/B). It implements the cheap *plugboard-recovery*
> tier (true rotor key fixed, only the plugboard climbed) and reports mean
> %-correct (graded) + exact-recovery rate + `L50`/`L90`. `SPLIT=1` adds the
> **failure-mode split**: an oracle scores the decrypt under the *known* true
> plugboard and compares it to the climb's score, labelling each non-recovered
> trial **scoring** failure (true plugboard does not score highest — only better
> scoring helps) vs **search** failure (true scores higher, the climb stuck in a
> local optimum — only better search helps).
>
> **What it found (v1.1.0 baseline, english/quad, 10 pairs).** Scoring failure is
> **0 % at every length, down to 40 characters** — every miss is a *search*
> failure: the true plugboard always out-scores what the single-start
> steepest-ascent climb reaches. So for plugboard recovery the quadgram score is
> *not* the bottleneck; the lever is the **search** (random restarts, better
> seeding, simulated annealing — items 1/3/4/5 below), not a better scoring
> schedule. (This overturned the initial guess that the flat short-text quadgram
> surface was the problem — hence building the split before changing any code.)
>
> Not yet built: the *full-crack* tier (wildcard the rotor key too), where
> rotor-key discrimination may surface genuine *scoring* failures the
> plugboard-only tier cannot.

### How it works today (`hillclimb()`, `enigma.cc:643`)

The plugboard (steckerbrett) is an **involution** on the 26 letters — disjoint
swapped pairs, applied before and after the rotor stack — stored as
`m.steckerbrett[26]` with `steck[steck[x]] == x`. It cannot be brute-forced (~1.5
× 10¹⁴ ten-pair involutions), so for each rotor/ring/start key the tool
hill-climbs a good plugboard. The climb is **steepest-ascent** over a single move:

- **A pass** scores the current steckerbrett, then tries all `C(26,2) = 325`
  letter pairs `(a,b)`. For each it applies one move, scores the whole message
  (`score_iter`, default quadgrams), records the delta, and **restores** before
  the next (`enigma.cc:678`–`:708`).
- **The move** (`:681`–`:688`): force `a`–`b` to be a plug; if either endpoint was
  already plugged, its old partner is ejected to self-steckered. So one move can
  dissolve up to two existing plugs to form one new — the standard
  Weierud/Gillogly plugboard move.
- After all 325 pairs, only the **single best** improving move is committed
  (`:714`–`:742`); passes repeat until one yields no improvement
  (`do … while (best_score > last_best)`, `:746`).

**Cost / placement:** with `-c`, `search_worker` runs a *full* climb on **every**
key (`enigma.cc:892`), so the climb dominates runtime (~400× the bare scan per
key per the bench notes); per-key cost ≈ passes × 325 × one full-message scoring.

### Weaknesses

- **Local optima** — single-move steepest ascent from one fixed start stops at the
  nearest optimum; no restarts, no worsening moves.
- **Scoring schedule** — quadgrams are used from the first plug, but with the wrong
  key or zero plugs in, the quadgram surface is nearly flat, so early moves are
  poorly guided.
- **Climbs every key** — the dominant cost is structural: a full climb runs on the
  ~99.99 % of keys that are wrong, not just promising ones.
- **Steepest vs first-improvement** — best-of-325 per pass re-scans all pairs
  between commits; first-improvement often reaches the same optimum with less work.

### Alternatives (roughly by expected payoff for this tool)

1. **Staged scoring schedule — ✅ IMPLEMENTED (`-S`).** A bigram pre-pass climbs to
   convergence, then the target (quad) model refines from there. The bigram surface
   is far smoother when only a plug or two are set, so it steers the early plugs
   into a better basin (Gillogly/Weierud). Note this is a *search* lever, not a
   scoring one — the SPLIT metric (final-model ranking of the truth) does not see
   it; the recovery curve does. It is per-`machine` (`m.scoring`, never a global →
   race-free) and `-T`-deterministic. **Measured** (english/quad, 10 pairs, 25
   trials) it helps both alone and stacked on restarts (table below). A naive full
   bigram climb *can* over-fit and hurt an individual easy case (bigrams don't
   constrain the board tightly), but on average it wins, and restarts absorb the
   variance.
2. **Pre-filter keys, climb only the top-N** (biggest *throughput* win) — a fast
   plugboard-free scan (IC/unigram) over the whole key space shortlists the best
   few hundred keys; the expensive climb runs only on those. Attacks the real cost
   driver and fits the existing parallel architecture; climb internals unchanged.
   Still open.
3. **Random restarts — ✅ IMPLEMENTED (`-R N`).** Restart 0 is the configured seed
   (= the old behaviour); restarts 1..N-1 start from random involutions (per-key
   splitmix64, so `-T`-deterministic), best kept. The simplest defence against the
   local optima the single-start climb got stuck in — and the diagnosis said every
   miss was a *search* failure, so this was the first lever pulled. Roughly doubles
   short-message exact-recovery (table below).
4. **Greedy plug-by-plug seed** — pick the best single plug, fix it, pick the best
   next given that, up to a budget, then refine with the swap climb. Better start
   than identity.
5. **Simulated annealing** — accept worsening moves with a decaying probability to
   escape local optima (Weierud used it for hard/short messages). More robust but
   needs a cooling schedule and more evaluations; best as a fallback, not default.
6. **Tabu search** — short list of recently reversed moves to avoid cycling and
   cross plateaus; modest deterministic robustness gain.
7. **Richer move set** — add explicit "remove a plug" / "re-pair endpoint" moves as
   distinct candidates; larger neighbourhood, reaches optima the single move misses.
8. **Genetic / evolutionary** — population + crossover + mutation; generally overkill
   here, rarely beats random-restart hill climbing or SA for plugboard recovery.

**Measured (exact-recovery %, english/quad, 10-pair plugboard, 25 trials):**

| config | L140 | L190 | L250 |
|--------|-----:|-----:|-----:|
| plain (`-R 1`)        | 16 | 32 | 56 |
| staged (`-S`)         | 28 | 56 | 72 |
| restarts (`-R 10`)    | 36 | 76 | 88 |
| **both** (`-R 10 -S`) | **64** | **88** | **96** |

**Bottom line:** **(3)** random restarts and **(1)** the staged schedule are both
**shipped** and **stack** — `-R 10 -S` lifts L140 from 16 % to 64 %. Still open:
**(2)** a key pre-filter (the big *throughput* win, so more restarts are
affordable per surviving key), and the heavier metaheuristics (SA/tabu/GA, items
5–8) as fallbacks for the hardest cases. The default (`-R 1`, no `-S`) is the
unchanged single-start climb.

### Scoring data and smoothing (the other lever — and what it can/can't help)

The n-gram tables come from the Practical Cryptography site. Two ways the *scoring
side* could in principle be improved, distinct from the search items above:

- **Better smoothing (model, not data) — tried, measured NEUTRAL.** Counts are
  stored as `log10(count + 1)`, so an *unseen* n-gram scores 0 (neutral). The
  community-standard "quadgram fitness" instead uses `log10(count/total)` with a
  small floor for unseen n-grams (`log10(0.01/total)`), which *penalises* gibberish
  carrying many unseen quadgrams rather than ignoring it. Because all candidates
  share the table, the `+1`-vs-`/total` difference is ~a constant offset; the real
  change is the unseen-n-gram floor. This was **implemented and A/B'd** against the
  current scheme (`make crackquality BASE=dev`, identical seeded problems,
  english/quad, 10 pairs): **statistically neutral** — 80-trial aggregate
  exact-recovery 36.0 % (floor) vs 35.2 % (current), within noise, lengths swinging
  both ways; the scorers and hot path are untouched so there is no runtime cost.
  `SPLIT=1` explains it: scoring failures stay **0 %** at every length under *both*
  schemes (the true plugboard already scores highest), so the plugboard tier is
  entirely search-bound and a better scoring *shape* cannot move it. **Dropped**
  for now (no measured win, same bar that rejected optimisation B). Its plausible
  payoff is the not-yet-built *full-crack* tier (rotor-key discrimination, where
  wrong keys generate many unseen quadgrams and the floor would bite) — revisit and
  re-run this exact A/B once that tier exists.
- **Different / domain-matched data (source).** Alternatives to Practical
  Cryptography: the Leipzig Corpora Collection, or build tables from a large public
  corpus (Gutenberg, a Wikipedia dump, news-crawl, OpenSubtitles). The plausible
  *win* is not a bigger corpus but a **domain-matched** one: authentic Wehrmacht
  traffic is telegraphic German (`X` for spaces/punctuation, spelled-out numbers,
  abbreviations), so tables built from period/telegraphic text — or from text
  preprocessed the way the cipher input is — model real messages better than
  generic prose. Higher-order 5-grams are not worth it (26⁵ ≈ 12 M entries, too
  sparse for short text).

Both are **cheap to try and measurable**: the tool already loads any
`<lang>_<type>.txt` via `-d`, and `make crackquality` (`BASE=…` / `SPLIT=1`) A/Bs
two table sets on identical problems. **But temper expectations:** the failure-mode
split shows plugboard-recovery misses are *search* failures, not scoring failures,
so better data/smoothing is unlikely to move that tier much — its value would show
up in the not-yet-built *full-crack* tier (recovering the rotor key on short
messages). Net: the search items above remain the bigger short-message lever;
revisit scoring data/smoothing alongside the full-crack tier.

#### Where a German telegraphic corpus would come from (assessment)

If domain-matched German is ever pursued, the findings from looking into it:

- **No large, clean, download-ready "telegraphic Enigma-style" German corpus is
  known.** The closest *authentic* sources are decrypted-message archives, not a
  corpus: **Frode Weierud's CryptoCellar** (cryptocellar.org), message sets
  published in *Cryptologia* (Erskine / Marks / Weierud), and the **TNA HW series**
  (Bletchley Park / GC&CS). These are domain-perfect but only hundreds–thousands of
  messages — enough to populate **bi/trigrams**, **too sparse for quadgrams**
  (26⁴ ≈ 457 K cells). (Provenance/availability here is from memory, not verified.)
- **The biggest surface-statistics mismatch is `X`, not vocabulary.** Real
  Wehrmacht plaintext uses **`X` as the word/sentence separator** (and spells out
  digits: `EINS`, `ZWO`, …), so genuine decrypts are full of word-boundary
  quadgrams containing `X` (`…XEIN`, `GXNA`). The current tables come from prose
  with spaces *removed* (words concatenated), so they never see `X`-as-separator —
  a real mismatch for cracking authentic intercepts. (It does *not* affect the
  `crack_quality` harness, which encrypts space-stripped prose and is
  self-consistent; testing the `X` effect would need the harness to encrypt
  `X`-substituted text.)
- **Pragmatic path — synthesise the register.** Rather than find a scarce corpus,
  take a large general German corpus (Leipzig, Wikipedia dump, Gutenberg) and apply
  the cipher's own preprocessing: uppercase, A–Z only, **substitute `X` for
  spaces/punctuation, spell out digits**. That yields quadgram-scale data with the
  right orthography even if vocabulary stays generic; optionally blend in the
  authentic archives to nudge phrasing. A synthesis pipeline (general corpus →
  `X`-substituted/number-spelled A–Z text → n-gram tables) is the form to build if
  real-traffic cracking is ever tackled.
