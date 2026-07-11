# Code Review — `enigma.cc` (current issues)

This is the **live** issue list: only what is still open. The original audit —
fixed correctness/memory-safety bugs, the C-style modernization, the `struct
machine` / threading refactor, M4 mode, the design rationale of every shipped
feature, and the experiments that were measured and **rejected** (so they are not
re-attempted) — is archived in **`archived/CODE_REVIEW_HISTORY.md`**. Section references of
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
out-scores what the climb reaches) — **re-confirmed under the shipped `-a` model**
(the scoring-failure floor is ~1% at L40–60 and ~0 beyond; `PERFORMANCE.md` §6.15).
And the residual is **compute-bound**: the exact-recovery curve is still climbing at
`-R 256` (~+15–25pp per 4× R), because the true basin is a rare *deep* target — 64
restarts give ~60 distinct exact boards but only ~15 distinct correct-plug states,
per-restart depth ~0.7/10, the truth assembled only in the union (~9/10). No
truth-free signal selects that union (per-plug consensus ~1.1/10; §3.10, §6.16), so
every *smart* search lever is blocked and **raw `-R` (via `-T`) is the reliable lever**.
The shipped search levers — random restarts
(`-R`), the staged schedule (`-S`), the key pre-filter (`-F`), and simulated
annealing (`-A`), and **first-improvement `-I`** — stack, with `-R 10 -S iq` a strong
baseline recipe and `-I -R <higher>` a measured throughput win on top of it. Remaining
ideas, in rough order of expected payoff (all with **diminishing returns** — SA
landed only as a *peer* of `-R -S iq`; 3-opt and **cross-restart consensus /
plug fixation** were built and measured and rejected — consensus loses to a higher
`-R` at equal compute and saturates as `-R` grows, `PERFORMANCE.md` §3.1). The
first-order lever remains **raising `-R`** (it never plateaus through 256), so the
best bets *buy more restarts*. The biggest such win shipped is **`-I` circular
first-improvement** (`PERFORMANCE.md` §7.2): ~2.8× cheaper per climb, so at matched
compute (paired with more `-R`) it recovers +8pp exact / +1–23pp mean at L40–60 — the
first idea to beat the baseline at the ~50-char target. **`-J` adds dynamic per-restart
best-first move ordering** on top of `-I` — a further matched-compute win (+2–6pp mean)
on the realistic ~10-plug regime, regime-dependent so also opt-in (`PERFORMANCE.md` §7.2).
Still nominally open on that axis: ILS with incumbent-walk acceptance (`PERFORMANCE.md`
§3.3) — but a **long shot**, since the basin analysis (§6.16) finds converged boards
*scattered*, not clustered near the truth, which is the structure ILS would need to
exploit. The **`max(greedy, SA)` portfolio was built and rejected** — greedy and SA are genuinely
complementary (+10–17pp union at double budget), but at matched compute the budget split
cancels it (~−3pp vs the best single solver; §3.2). **Don't-look bits were built and
rejected** too — exact only for a separable objective (TSP), but the plugboard's global n-gram
score makes them a heuristic that is neutral (`-I`) to a small loss (`-J`) at matched compute
(`PERFORMANCE.md` §7.2). *Static* (fixed-across-restarts) informed ordering was **rejected**
too (greedy + diversity-collapsing; the per-restart `-J` avoids the collapse — `PERFORMANCE.md` §7.2). A separate shipped
lever is **`-M` cap-as-target** (`PERFORMANCE.md` §7.8): at/over the `-S` plug cap the
climb may only *merge* or *remove* plugs (never add or reshuffle), so a big restart kick
is pruned cleanly back to the cap instead of leaving spurious plugs. It is neutral-to-+2.6pp
on realistic 10-plug boards and a large win (+3–20pp, biggest at the short/hard end) on
**known-few-plug** boards with a tight cap (`-S i4q6 -M`) — and cheaper per climb, so it
buys more restarts too:

- ✅ **`make crackquality` test additions — SHIPPED** (`CRACKQUALITY_TESTS.md`): the
  scoring-failure gate as `WILDCARD`, `-F` recall as `FILTERRECALL=1`, restart-diversity
  as `DIVERSITY=1`, plus the routine `SPLIT=1` scoring-vs-search split used throughout
  §6.15/§6.16. Original scope for reference:
  Three cheap, focused additions: (1) a **one-time scoring-failure gate** —
  wildcard only the start (`START` scope, ring pinned `AAA`, **unfiltered**) and
  check *once* whether a wrong (key, board) ever out-scores the true one; if not,
  the scoring work in §2 stays parked, if so it re-opens; (2) **`-F` prefilter
  validation** — a `recall@N` sweep + matched-`score_iter` filtered-vs-unfiltered
  A/B, the recall test `-F` lacks; (3) **restart-diversity diagnostics** —
  measure how often restarts collapse into the same optimum and rank the shipped
  knobs by basin coverage. The earlier "full-crack tier that gates everything"
  was **de-scoped**: its cross-key plug marginalization goal (`PERFORMANCE.md`
  §5.3) is dropped (correlated-noise argument, the §3.1 precedent). Build the
  gate before betting on scoring changes.
- 🟢 **Greedy plug-by-plug seed** (history §9 item 4). Pick the best single plug,
  fix it, repeat to a small budget, then refine with the swap climb — a better
  start than the identity board. Cheap; modest expected gain. The last search idea
  not yet measured down, but temper expectations against the compute-bound /
  rare-deep-basin frontier above. (**Tabu** and **genetic/GA** used to sit here and
  have since been **measured down** — see the rejected list under Standing cautions.)

## 2. Cracking quality — scoring

Scoring is now **near-optimal and effectively resolved** (`PERFORMANCE.md` §6.15). The
one measured win is the shipped **`-a` weighted all-order model** (a log-linear /
Product-of-Experts mixture of the four n-gram orders; PR #106) — +~1–2pp mean %-correct
at L40–100 across all four languages, the first short-message scoring gain. Two ceiling
probes show there is essentially no headroom left for a *further* scoring model:
discrimination floor ~1% (SPLIT under `-a`) and a **flat** surface-smoothness sweep (an
8× weight change moves search-fail% <1pp). So the remaining scoring items below are for
the (unbuilt) full-crack tier or real operational traffic, not the plugboard-recovery
benchmark — do not expect them to move it.

- 🟢 **Crib / known-plaintext objective** (history scoring item 4) — score by
  match to a suspected word/phrase instead of n-gram fitness. This is the most
  genuinely *new capability* here, and it composes cleanly with the shipped `-s`
  plug-freeze (known plugs + known crib). **Highest-value scoring item** — it attacks
  from outside the statistical floor entirely (a crib is hard evidence, not a prior).
- 🟢 **Domain-matched corpus** — tables built from telegraphic/period German
  (X-for-space, spelled-out numbers) rather than generic prose would model real
  Wehrmacht traffic better. Cheap to try (`-d` loads any `<lang>_<type>.txt`);
  value expected mainly for real traffic (invisible to the prose-excerpt benchmark).
- ✅/❌ **Multi-order & smoothing — RESOLVED.** Log-linear interpolation shipped as `-a`
  (above). The rest of the family was **measured down**: linear (Jelinek-Mercer)
  interpolation loses (the conditional reframing it forces), and back-off / graded-floor
  smoothing (Kneser-Ney-style, `background`/`overlap`) is neutral-to-harmful (PR #105,
  §6.15). **PPM/compression perplexity** / **z-score normalization** remain archive-only
  (marginal/overkill); revisit only on a measured need.

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
  `PERFORMANCE.md` §3.1), §7.1a surrogate-ranked ascent (rank switch moves by a cheap
  monogram surrogate, full-quad only a top-K — ~1.5× *slower* at the ~50-char target
  because warm short-message quad decodes are too cheap to skip; only wins ≥150 chars;
  the exact mono/IC delta remnant was briefly shipped as `-D` then removed — a
  long-message-only speedup, net-negative for this short-message tool and a maintenance
  tax on the hottest loop; `PERFORMANCE.md` §7.1), incremental **quad** delta-scoring (~2× slower;
  `archived/SIMULATED_ANNEALING.md` §6.2), χ² as the scoring/`-F` model (gameable by the
  plugboard), 3-opt / 3-plug re-pair (cost > gain), rotor-stepping reuse across
  starts (history §6 "optimisation B"), `-march=native` / SIMD gathers and GPU (the
  scorer is gather-latency-bound, not throughput-bound), and 5-grams / 4-bit scores
  (too sparse / too coarse). Added this cycle (`PERFORMANCE.md` §6.15/§6.16): **tabu**
  (restarts already almost never revisit a basin at `--random 10` — near-total
  exact-board diversity, so a visited-set has nothing to forbid; `--restart-tt`, §6.14);
  **genetic / GA** (the crossover *material* exists — correct plugs union to ~9/10 across
  restarts — but is **unselectable**: board-fitness picks ~2.5/10, per-plug consensus
  ~1.1/10; §3.10); **linear (Jelinek-Mercer) interpolation** and **graded-floor / back-off
  smoothing** (`background`/`overlap`) as scoring models (the log-linear form shipped as
  `-a`, the rest lose or are neutral — §6.15); and, by extension, any **truth-targeted kick
  or coarse basin-repel**, which need the same absent selection signal.
- **Determinism is a contract.** Results must be independent of `-T`; the per-key
  RNG is seeded from the flat key index. Keep new randomness on that stream.
