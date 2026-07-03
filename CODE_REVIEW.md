# Code Review — `enigma.cc` (current issues)

This is the **live** issue list: only what is still open. The original audit —
fixed correctness/memory-safety bugs, the C-style modernization, the `struct
machine` / threading refactor, M4 mode, the design rationale of every shipped
feature, and the experiments that were measured and **rejected** (so they are not
re-attempted) — is archived in **`CODE_REVIEW_HISTORY.md`**. Section references of
the form "§9 item N" elsewhere in the codebase point into that archive.

Status in one line: the tool is correct, warning-free under
`-std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -Wshadow` on g++ and clang,
clean under ASan/UBSan/TSan/valgrind/cppcheck/clang-tidy, multi-threaded (`-T`),
and supports standard / Norway / M4 machines. Nothing below is a known bug; these
are enhancements, deferred hardening, and standing cautions.

Severity legend: 🔴 critical · 🟠 high · 🟡 medium · 🟢 low / nit.

Measure before shipping any of these: `make crackquality BASE=<ref>` (recovery
quality on short messages, `SPLIT=1` for the scoring-vs-search failure split) and
`make bench BASE=<ref>` under **both** g++ and clang (hot-path throughput).

---

## 1. Cracking quality — search (the main open direction)

`make crackquality SPLIT=1` shows that on the plugboard-recovery tier every miss
is a **search** failure, not a scoring failure (the true plugboard always
out-scores what the climb reaches). The shipped search levers — random restarts
(`-R`), the staged schedule (`-S`), the key pre-filter (`-F`), and simulated
annealing (`-A`), and **first-improvement `-I`** — stack, with `-R 10 -S iq` a strong
baseline recipe and `-I -R <higher>` a measured throughput win on top of it. Remaining
ideas, in rough order of expected payoff (all with **diminishing returns** — SA
landed only as a *peer* of `-R -S iq`; 3-opt and **cross-restart consensus /
plug fixation** were built and measured and rejected — consensus loses to a higher
`-R` at equal compute and saturates as `-R` grows, `performance.md` §3.1). The
first-order lever remains **raising `-R`** (it never plateaus through 256), so the
best bets *buy more restarts*. The biggest such win shipped is **`-I` circular
first-improvement** (`performance.md` §7.2): ~2.8× cheaper per climb, so at matched
compute (paired with more `-R`) it recovers +8pp exact / +1–23pp mean at L40–60 — the
first idea to beat the baseline at the ~50-char target. **`-J` adds dynamic per-restart
best-first move ordering** on top of `-I` — a further matched-compute win (+2–6pp mean)
on the realistic ~10-plug regime, regime-dependent so also opt-in (`performance.md` §7.2).
Still open on that axis: the `max(greedy, SA)` portfolio. **Don't-look bits were built and
rejected** — exact only for a separable objective (TSP), but the plugboard's global n-gram
score makes them a heuristic that is neutral (`-I`) to a small loss (`-J`) at matched compute
(`performance.md` §7.2). *Static* (fixed-across-restarts) informed ordering was **rejected**
too (greedy + diversity-collapsing; the per-restart `-J` avoids the collapse — `performance.md` §7.2). A separate shipped
lever is **`-M` cap-as-target** (`performance.md` §7.8): at/over the `-S` plug cap the
climb may only *merge* or *remove* plugs (never add or reshuffle), so a big restart kick
is pruned cleanly back to the cap instead of leaving spurious plugs. It is neutral-to-+2.6pp
on realistic 10-plug boards and a large win (+3–20pp, biggest at the short/hard end) on
**known-few-plug** boards with a tight cap (`-S i4q6 -M`) — and cheaper per climb, so it
buys more restarts too:

- 🟢 **Full-crack tier for `make crackquality`** (measurement gap, and a
  prerequisite). The harness only exercises the plugboard-recovery tier (true
  rotor key fixed). A tier that wildcards the rotor key too would reveal whether
  genuine *scoring* failures exist there — which is what would justify the scoring
  work in §2. Build this before betting on scoring changes.
- 🟢 **Greedy plug-by-plug seed** (history §9 item 4). Pick the best single plug,
  fix it, repeat to a small budget, then refine with the swap climb — a better
  start than the identity board. Cheap; modest expected gain.
- 🟢 **Tabu search** (history §9 item 6). A short list of recently-reversed moves
  to avoid cycling and cross plateaus; modest deterministic robustness.
- 🟢 **Genetic / evolutionary** (history §9 item 8). Population + crossover; the
  archive's assessment is that it rarely beats restart hill-climbing or SA for
  plugboard recovery — likely overkill, lowest priority.

## 2. Cracking quality — scoring

The plugboard tier is search-bound, so better scoring is unlikely to move it;
these mostly pay off in the (unbuilt) full-crack tier of §1.

- 🟢 **Crib / known-plaintext objective** (history scoring item 4) — score by
  match to a suspected word/phrase instead of n-gram fitness. This is the most
  genuinely *new capability* here, and it composes cleanly with the shipped `-s`
  plug-freeze (known plugs + known crib). Highest-value scoring item.
- 🟢 **Domain-matched corpus** — tables built from telegraphic/period German
  (X-for-space, spelled-out numbers) rather than generic prose would model real
  Wehrmacht traffic better. Cheap to try (`-d` loads any `<lang>_<type>.txt`);
  value expected mainly in the full-crack tier.
- 🟢 **Interpolated / back-off n-gram** (blend orders, Kneser-Ney) and
  **PPM/compression perplexity** / **z-score normalization** — documented in the
  archive as marginal or overkill; only if a measured need appears.

## 3. Maintainability

- ✅ **`bruteforce()` decomposed.** The 306-line function was split into
  `build_key_space()` (the reflector/wheel/ring/start ranges and the task list),
  `allocate_subst_tables()` (the guarded table allocation), and a `run_parallel()`
  template that replaced the four copy-pasted spawn/join blocks — `bruteforce()`
  itself is now ~116 lines that read as phases. Behaviour-preserving (byte-identical
  output vs the prior version, TSan-clean, no bench regression).
- ✅ **`-Wold-style-cast` cleaned and enabled.** The four remaining C-style casts
  were converted to `static_cast` and the flag is now in the base `CXXFLAGS`.
- 🟢 **`-Wconversion` (~52) deliberately deferred.** 43 of the warnings are
  `int → unsigned char` narrowings in the hottest loops (steckerbrett writes,
  decode). Adding that many `static_cast`s clutters the hot path for a low-value
  nit on deliberately C-style code, so it stays off — a documented future ratchet,
  not a bug.

## 4. Tooling & packaging

- 🟢 **`Makefile` has no `install` target**, and the n-gram data files are not
  listed as build/run dependencies. Fine for development; add if the tool is ever
  packaged.
- 🟢 **Single-file distribution (embedded tables).** Baking the n-gram tables into
  the binary was previously declined (see history §7), but it is more attractive
  now: the shipped **uint8** tables are ~4× smaller than the float tables that
  analysis assumed, so an embedded blob (one language, or all four) is much
  cheaper. Revisit only if a self-contained binary becomes a goal; keep `-d` /
  `$ENIGMA_DATA` as the override.

## 5. Minor known limitations

- 🟢 **Status lines and a long `-d` path.** All status lines are kept within 79
  columns *except* the `Scoring:` line, which can exceed it if the `-d`
  data-directory path is unusually long. The path length is unbounded and can't be
  shortened without hiding it; everything else is guaranteed to fit.

---

## Standing cautions (not issues — preserve these when editing)

- **Hot-path layout is load-bearing.** Collapsing per-search state into `struct
  machine` cost 20–60% on clang/ARM until three mitigations fixed it (heap-separated
  `subst_array`; `__restrict` base-pointer hoisting; rotor positions held in locals
  across `setup_mapping`). The uint8 n-gram tables + per-table bias, and the fused
  decode-score loop, are likewise tuned. Any hot-path edit must be A/B'd with
  `make bench BASE=<ref>` under **both** g++ and clang. Details in history §6.
- **Measure; don't ship losers.** The following were built/prototyped, measured,
  and **rejected** — don't re-attempt without a materially different regime:
  cross-restart consensus / plug fixation (freeze the plugs that a majority of the
  restart boards agree on, then climb the residual — compute-neutral-to-negative vs
  simply raising `-R`, and a no-op at high `-R` because best-of-`R` saturates; swept
  over vote threshold × elite-set size × `-R` × plug count × length × seed;
  `performance.md` §3.1), §7.1a surrogate-ranked ascent (rank switch moves by a cheap
  monogram surrogate, full-quad only a top-K — ~1.5× *slower* at the ~50-char target
  because warm short-message quad decodes are too cheap to skip; only wins ≥150 chars;
  the exact mono/IC delta remnant was briefly shipped as `-D` then removed — a
  long-message-only speedup, net-negative for this short-message tool and a maintenance
  tax on the hottest loop; `performance.md` §7.1), incremental **quad** delta-scoring (~2× slower;
  `SIMULATED_ANNEALING.md` §6.2), χ² as the scoring/`-F` model (gameable by the
  plugboard), 3-opt / 3-plug re-pair (cost > gain), rotor-stepping reuse across
  starts (history §6 "optimisation B"), `-march=native` / SIMD gathers and GPU (the
  scorer is gather-latency-bound, not throughput-bound), and 5-grams / 4-bit scores
  (too sparse / too coarse).
- **Determinism is a contract.** Results must be independent of `-T`; the per-key
  RNG is seeded from the flat key index. Keep new randomness on that stream.
