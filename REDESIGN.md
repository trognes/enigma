# REDESIGN — CLI consistency refactor

**Status: DRAFT PLAN — not yet implemented.** Execute one Part at a time, in order,
and only on an explicit "go" for that Part. Each Part must land (merged, suite green,
determinism preserved) before the next begins. This is a **consistency / correctness**
refactor, not a recovery or performance change — the bar for every Part is:

- full suite green under g++ **and** clang (`-Werror`), TSan-clean;
- behaviour **unchanged** for any invocation whose surface this Part does not explicitly
  change (byte-identical where determinism applies);
- the only intended behaviour changes are the ones spelled out per Part.

It does **not** aim to improve cracking quality. Exhaustion (`--exhaust`) remains a
measured-dominated exploration tool throughout.

---

## Resolved (the three review questions)

All confirmed in review — no open items:

1. **Ordering:** swap accepted. **Part A = long options (`getopt_long`)**, **Part B = seed
   pipeline** — because the seed pipeline's long-only options and the `-S`→`--score` rename
   need `getopt_long` first.
2. **Naming:** the exhaustion option is **`--exhaust`** (not `--brute-force`, which would
   read as brute-forcing the rotor key).
3. **`--restarts N` = Option A (kicked-only):** `--restarts 0` = one deterministic seed
   climb (no kick); `--restarts N≥1` = **N kicked climbs, and *only* those** (the seed climb
   is not additionally run). So `--restarts N` does exactly N climbs (or 1 when N=0). With
   `--exhaust`, `--restarts 0` gives *pure* exhaustion (one climb per combo) and
   `--restarts N` gives N *kicked* climbs per combo. (Chosen for the simple "restarts =
   number of random attempts" model; the tradeoff is non-monotonicity 0→1 and pure-vs-kicked
   being a per-run choice, both accepted.)

---

## Locked decisions (from review)

- **Long names additive; short forms kept.** Add long names for everything; **all** options
  (basic and advanced) keep their short alias permanently.
- **A1 before A2.** Exhaustion stays single-threaded first; parallelise it later (Part D).
- **`--exhaust` counts *forced* pairs**, not total (`-s` already states its own count;
  they add).
- **Model selectors stay as aliases.** `-i/-m/-b/-t/-q` are permanent thin aliases for
  `--score <model>` (C-lite); they are **not** removed.
- **`-S` long name is `--score`** (it selects the scan-ranking model too, so "schedule"
  would misread).
- **Unambiguous long-option prefixes are accepted** (native `getopt_long` behaviour, e.g.
  `--restart` ≡ `--restarts`, `--lang` ≡ `--language`).
- **Long-name preferences:** `--exhaust` (exhaustion), `--score` (was `-S`/schedule),
  `--ngrams` (was `-d`/data-dir), `--random` (the kick), `--start-position` (`-g`),
  `--language` (`-l`), `--plugboard` (`-s`).

### Restart / random-kick semantics (new)

- **`--restarts N` / `-R N`** = number of **randomised** restart attempts.
  - `--restarts 0` (**the new default**): no random kicks — one climb from the deterministic
    seed (`-s` pins + any `--exhaust` pins). Fully deterministic.
  - `--restarts N` (N≥1): exactly **N** climbs, each from the seed **plus a fresh
    `--random`-sized kick**; keep the best. The deterministic seed climb is **not** also run
    (Option A, kicked-only).
  - With `--exhaust`: `--restarts 0` = *pure* exhaustion (one climb per forced-pair combo);
    `--restarts N` = N *kicked* climbs per combo (work space = `combos × N`).
- **`--random K`** = kick size (random plug pairs injected per restart). **Default 10.**
  `--random 0` is legal (no perturbation — a control; N restarts then repeat the seed climb).
- **Warning (non-fatal):** if `--restarts N` exceeds the number of **distinct** `K`-pair
  kicks available among the free letters — `combos(free, K) = free!/(2^K·K!·(free−2K)!)`,
  where `free = 26 − 2·(‑s pairs + --exhaust pairs)` — then restarts must repeat by
  pigeonhole; warn (`stderr`). This mainly catches the small-`K`/high-`N` footgun.
- Note vs today: default kick 8→**10**; `-R` renumbered (old `-R 1` un-kicked ≡ new
  `--restarts 0`; new `--restarts N` is all-kicked). Determinism-pinned tests will change
  output and must be updated in the Part that introduces this (Part B).

---

## End-state option table

`basic` = commonly-used, taught in short form. `advanced` = long-primary in docs, but keeps
its short alias permanently. New options (`--random`, `--exhaust`) are **long-only** from birth.

| Short | Long | Arg | Class | Notes |
|---|---|---|---|---|
| `-u` | `--reflector` | X | basic | |
| `-w` | `--wheels` | XYZ | basic | |
| `-r` | `--rings` | XYZ | basic | |
| `-g` | `--start-position` | XYZ | basic | |
| `-s` | `--plugboard` | AB… | basic | |
| `-n` | `--norway` | – | basic | |
| `-4` | `--m4` | – | basic | |
| `-c` | `--climb` | – | basic | |
| `-R` | `--restarts` | N | basic | new semantics (above) |
| `-S` | `--score` | str | basic | climb-only stages + scan model |
| `-l` | `--language` | name | basic | |
| `-d` | `--ngrams` | dir | basic | |
| `-T` | `--threads` | N | basic | |
| `-h` | `--help` | – | basic | |
| `-v` | `--version` | – | basic | |
| `-i/-m/-b/-t/-q` | `--ic/--mono/--bi/--tri/--quad` | – | basic | permanent aliases for `--score <m>` (C-lite) |
| `-x` | `--max-wheel` | N | advanced | |
| `-A` | `--anneal` | N | advanced | |
| `-F` | `--prefilter` | N[%] | advanced | |
| `-I` | `--first-improve` | – | advanced | |
| `-J` | `--dynamic-order` | – | advanced | |
| `-M` | `--cap-target` | – | advanced | |
| `-e` | `--seed` | N | advanced | |
| `-p` | `--compare` | file | advanced | |
| *(none)* | `--random` | K | advanced | kick size (was `-S rN`); long-only |
| *(none)* | `--exhaust` | E | advanced | forced-pair count (was `-S aN`); long-only |

(Long names not listed here — `--reflector`, `--wheels`, etc. — are open to bikeshedding.)

### Seed pipeline (end state)

```
seed  = (-s pins, fixed) + (--exhaust forced pins, fixed, enumerated) + (--random kick, volatile)
climb = --score  (ordered, capped i/m/b/t/q stages)
```

Work space = `--exhaust combos × restart attempts`. Per item: pin `-s` + one forced
combo, add this restart's `--random` kick (0 for `--restarts 0`), climb `--score`, keep the
global best. `-S`/`--score` no longer carries `r`/`a` tokens — only model stages with caps.

### `--score` without `-c` (rotor-only scan)

`--score`'s **target (last) stage's model is always the ranking model** — used by the rotor
scan to rank candidate decryptions *and* by the climb as its final model. The earlier stages
and their caps are **climb-only**: inert when there is no `-c`. So a pure rotor scan is
configured by `--score` alone:

- no `--score` → rank by **IC** (the default; `./enigma < cipher` works with no options);
- `--score q` (≡ `-q`) → rank by quadgrams;
- `--score iq` / `--score i4q10` (no `-c`) → rank by the target (`q`); the earlier stages and
  caps are ignored (there is no climb to apply them to).

**Decision — warn (option b):** when `--score` carries climb-only detail (more than one stage,
or any cap) but `-c` is absent, emit a **non-fatal** warning ("climb schedule ignored without
`-c`; ranking by *<model>*") and proceed, ranking by the target. This flags a forgotten `-c`
or a pasted climb recipe without breaking recipe reuse — same spirit as the pigeonhole and
`N < fixed` warnings. (By contrast, `--random`/`--exhaust` are plugboard operations and
**error** without `-c`, since they can do nothing in a scan.)

---

## The plan

### Part A — long options (`getopt_long`, additive)  [was B1]

Switch `getopt`→`getopt_long`. Add every long name in the table; **keep all existing short
forms**. New options (`--random`, `--exhaust`) are **not** added here (Part B). Rewrite
`help()` grouped basic/advanced showing `-x, --long`. Unambiguous prefixes work natively.
- **Behaviour: unchanged.** Pure surface addition.
- **Done:** every short flag still works; every long flag and unambiguous prefix works;
  byte-identical outputs; suite green (g++/clang), plus a few long-option smoke tests.

### Part B — seed pipeline (`--score` climb-only; `--random`/`--exhaust`)  [was A / A1]

Move `r`/`a` out of `-S`; `-S` becomes `--score` (climb stages only). Add `--random K`
(kick, default 10) and `--exhaust E` (forced pairs, long-only, single-threaded — A1).
Implement the seed pipeline so exhaustion and the kick **compose** (fixing the current
silent no-op). Adopt the new `--restarts`/`--random` semantics + the pigeonhole warning, and
the **climb-schedule-without-`-c` warning** (see "`--score` without `-c`" above); `--random`/
`--exhaust` **error** without `-c`.
- **Behaviour changes (intended):** `-R` renumbered; default kick 8→10; `-S r…`/`-S a…`
  strings no longer parse (moved to `--random`/`--exhaust`); exhaustion now respects the
  kick and restarts.
- **Files:** `parse_schedule()` (drop `r`/`a`), `exhaust_first_pairs()` (compose kick +
  restart loop), validation, help; migrate every `-S r…`/`-S a…` and affected `-R` usage in
  tests/harness/README/CLAUDE/performance; update the recommended recipes
  (`-S r10i4q10` → `--score i4q10 --random 10`; `-S a1i4q10` → `--score i4q10 --exhaust 1`).
- **Done:** exhaustion+kick+restarts compose (test it); determinism preserved (`-T`-indep);
  suite green; recipes updated.

### Part C — model selectors as aliases (C-lite)

Redefine `-i/-m/-b/-t/-q` precisely as aliases for `--score <model>` (single uncapped
stage). Make it a **fatal error** when the scoring model is set to *conflicting* values by
different options — a selector vs an `--score` target (e.g. `-m --score q`), or two
disagreeing selectors (`-m -q`) — since the intended model is genuinely ambiguous; fail fast
and make the user pick. No error when they agree (`-q --score q`, `-q -q`). Confirm the
scan-model rule holds (see "`--score` without `-c`" above): `--score`/selectors set the
**scan** ranking model with no `-c`, target model wins.
- **Done:** selector ≡ `--score <m>`; **conflicting scoring models rejected** with a clear
  message; scan ranking works via `--score` alone; the no-`-c` staged-`--score` warning fires.

### Part D — parallel exhaustion (A2)

Move `plug_fixed[]` into `struct machine` (per-worker). Make `--exhaust` combos a
parallel work dimension (like restarts); drop the `-T 1` guard. Re-verify determinism and
TSan.
- **Done:** `--exhaust` runs on `-T>1`, `-T`-independent, TSan-clean; no hot-path bench
  regression (check under g++ and clang).

Part D is the last Part. (Two further steps were considered and **dropped**: making advanced
options long-only, and removing `-i/-m/-b/-t/-q` entirely — advanced options keep their short
aliases, and the model selectors stay as permanent aliases for `--score <model>`.)

---

## Migration surface (touched across Parts)

- `enigma.cc` — option table, parse loop, `help()`, `version()`, `parse_schedule()`,
  `exhaust_*`, validation, `struct machine` (Part D).
- `tests/run_tests.sh`, `tests/bench.sh`, `tests/crack_quality.py` — the `-S r…`/`-S a…`
  strings → `--random`/`--exhaust`, and any `-R` usage affected by the new semantics.
- `README.md`, `CLAUDE.md`, `performance.md` — option tables, `-S` grammar section, §3.6,
  recommended recipes.
- CI runs via `make test/bench`, so covered by the harness edits (no direct option strings).

## Per-part git discipline

One Part = one PR (or one reviewable commit), off the latest default branch, suite green
before merge. Because container resets have lost local state before, **push early**; this
plan doc is committed so it survives resets.
