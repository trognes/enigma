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
ngrams/<lang>_monograms.txt   Single-letter frequencies.
ngrams/<lang>_bigrams.txt     Two-letter frequencies.
ngrams/<lang>_trigrams.txt    Three-letter frequencies.
ngrams/<lang>_quadgrams.txt   Four-letter frequencies.
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
better). **When comparing search/scoring changes, judge on the mean %-correct,
not the exact-recovery rate.** The mean is the graded, lower-variance signal:
it moves smoothly with small quality changes and separates configs at short
lengths where the exact rate is near-zero and dominated by trial noise. The
exact rate (and `L50`/`L90`) is a coarse headline — use it as a secondary check,
not the metric a tuning decision turns on. A fixed `SEED` makes the trial set deterministic (Python's
`random.Random(seed)`, reproducible across machines), so `make crackquality
BASE=<git-ref>` is a same-machine A/B that solves identical problems with both
binaries. (It was rewritten from shell+awk to Python: awk's seeded `rand()` is not
reproducible in every awk, e.g. mawk, which silently broke the deterministic-A/B
premise.) Use this — not `make bench` — to tell whether a scoring/search change
actually helps short-message cracking. `make crackquality SPLIT=1` additionally
classifies each non-recovered trial as a **scoring** failure (the true plugboard
does not score highest) or a **search** failure (it does, but the climb stuck in
a local optimum) via an oracle run — telling you which lever to pull. (See
`archived/CODE_REVIEW_HISTORY.md` §9 for the algorithmic ideas this is meant to measure; on the
v1.1.0 baseline every miss is a *search* failure.)

> **When evaluating a *search* change, exclude the scoring-failure cases first.**
> A scoring failure — the true board is not the top-scoring one (operationally:
> a non-exact trial with `recovered_score ≥ true_score`) — is an **information
> floor of the scoring model**, unrecoverable by *any* search, so leaving it in
> the stats only adds noise that no search improvement can move. The current work
> is improving **search** (restarts, `-F`, SA, the `--gainfix*` finishers, tabu/GA),
> not scoring, so measure search levers on the **search-failure + exact** population
> only (drop the scoring failures via the oracle `recovered_score`/`true_score`, as
> `SPLIT=1` classifies them). Judge on that filtered mean %-correct / exact rate;
> a change that only shuffles unfixable scoring failures is not a real search win.
> (A scoring change is the opposite case and *is* measured on the full population —
> its whole job is to shrink the scoring-failure floor.)

> **For matched-compute A/Bs, measure actual wall time — do not judge cost on
> `score_iter` alone.** The `score_iter` counter counts only calls through the fused
> n-gram score loop; it does **not** count the auxiliary per-symbol work some search
> moves do outside that loop — most notably the `--gainfix*` **gain scan**
> (`gainfix_candidates` does ≈`n·26·4` quad8 lookups per cascade call, ≈100
> `score_iter`-equivalents, uncounted). So on gain-cascade changes `score_iter`
> **undercounts real cost by several×**, and the two axes can *disagree*: e.g.
> `--gainfix-best3` at a large `K` can tie `--gainfix-best`+more-`-R` on `score_iter`
> while being **Pareto-dominated** on wall time (`PERFORMANCE.md` §4.11 — K=169 at 131 ms
> lost to R200 at 114 ms despite fewer counted iters). Take wall time as the min of a few
> reps (per problem) to damp noise, and treat `score_iter` as the *cheap deterministic
> proxy* it is — good for `-T`-independent A/Bs of moves that live **inside** the score
> loop (restarts, climb order, caps), misleading for anything that adds work outside it.

`crack_quality.py` also carries three opt-in test modes from `CRACKQUALITY_TESTS.md`
(all off by default, the normal flow unchanged): `WILDCARD` wildcards the rotor key
for the **scoring-failure gate** (§1); `FILTERRECALL=1` reports the true key's `-F`
tier-1 **recall@N** (§2, via the binary's `--true-key` diagnostic flag); and
`DIVERSITY=1` reports **restart basin-collapse** stats — distinct converged optima and
best-board hit-count across the `-R` restarts (§3, via `--dump-restarts`). The two
`--true-key`/`--dump-restarts` flags are off-by-default binary diagnostics (default
paths byte-identical and bench-neutral, ASan/TSan-clean). **`--dump-all`** is the
`--dump-restarts` companion that additionally prints the **rotor key** of each converged
climb (`dumpall <refl+wheels> <ring> <start> <score> <plugboard>`), so a *wildcarded* search
can be inspected key-by-key (not just a fixed key); it reuses `showconfig`'s
`format_key`/`format_plugboard`, is display-only under the same mutex (so the dumped multiset
is `-T`-invariant; only line order is thread-timing dependent), and needs `-c`.

A third restart-diversity diagnostic, **`--restart-tt`**, does the basin dedup *in-binary*
instead of via the external `--dump-restarts` harness: it hashes each converged restart
board into a **Zobrist transposition table** (325 fixed pseudorandom 64-bit words, one per
unordered letter pair, XORed over the set plugs; a deterministic splitmix64 seed, so the hash
is reproducible and `-T`-invariant) and, at the end, prints a one-line **basin-collapse
summary** — distinct optima, total climbs, the heaviest basin's hit count, and the Shannon
entropy of the hit distribution (`log2 N` = maximally diverse, `0` = full collapse). Each
bucket stores the **exact board** (so a hash collision is a probe-on, never a false merge),
its score, and its hit count; open-addressing, linear-probe, sized to the work items. It is
diagnostic-only — it never gates which candidate wins, and because the multiset of converged
boards is a deterministic function of the work items, the reported stats are `-T`-invariant
(verified T1 ≡ T4). Off by default (needs `-c`; default path byte-identical, all tests pass).
Measured aside: at the standard `--random 10` kick, exact-board collapse is near-zero (restarts
land on distinct boards, differing by at least a spurious plug), so meaningful collapse shows
only as the kick shrinks (`--random 0` → 1 basin, entropy 0). The TT is the intended scaffold
for the diversity-driven search ideas (a tabu visited-set, GA population dedup).

A fourth diagnostic, **`--score-tt`**, reuses that Zobrist board hash to answer a different
question — *how much of the plugboard climb's scoring is repeated work a cache could recover?*
It memoises `score_iter` in a per-worker **plugboard→score transposition table**: within one
rotor key and scoring model `score_iter` is a pure function of the plugboard, so a board scored
again returns the stored value instead of re-decoding. A hit requires the exact board, the
current rotor-key generation (bumped once per key in `setup_mapping`, invalidating the cache in
O(1)), and the scoring model all to match, so it is **semantically transparent** — byte-identical
results, `-T`-invariant, on or off. It prints the fraction of `score_iter` served from cache at
the end. **Measured result (PERFORMANCE.md §7.9): rejected as a speedup** — only ~7–13% of scores
are cacheable, the rate is *flat in restarts and in table size* (so it is almost all intra-climb
repeats; cross-restart score reuse is essentially nil, the board-probe-level echo of the §6.14 /
`--restart-tt` basin-diversity finding), and the per-call hash+compare+copy against a cache-cold
multi-MB table makes it a **net wall-time loss** at every config. Kept as an off-by-default
diagnostic (needs `-c`; default path byte-identical) for the diversity-search line — the negative
answer *is* the artifact.

The program reads **ciphertext from stdin** and writes the best-scoring
**plaintext to stdout**; progress/diagnostics go to stderr. Only A–Z letters
are kept; everything else (spaces, punctuation, case) is stripped. The n-gram
files are read from a **data directory** (filenames built as
`<datadir>/<language>_<ngram>.txt`) resolved as `-d <dir>` → `$ENIGMA_DATA` →
`ngrams` (the bundled `ngrams/` subdirectory, found when run from the repo root) —
pass `-d`/`$ENIGMA_DATA` to run from any other working directory.

### Common invocations

```sh
# Brute-force everything with the DEFAULT model, the index of coincidence
# (language-independent, no -l needed):
./enigma < cipher.txt

# Same, but score with quadgrams against the English tables (-q selects quadgrams;
# -l gives the language -- -l on its own does nothing, the default stays IC):
./enigma -q -l english < cipher.txt

# Specify some settings, wildcard the rest with '.', and hill-climb plugboard:
./enigma -u B -w 123 -r AAA -g ... -c -q -l english < cipher.txt

# Norway Enigma:
./enigma -n -c -q -l english < cipher.txt

# M4 (4-rotor naval): thin reflector b, Greek Beta, wheels III-I-VII, wildcard
# the Greek position (first char of -g) and hill-climb the plugboard:
./enigma -4 -u b -w B317 -r AAAA -g .QXP -c -q -l english < cipher.txt
```

### Key CLI options (see `help()` in source for the full list)

> **Recommended vs. not.** The proven-good knobs are `-c` + `-R` restarts, the **`-a` weighted
> all-order scoring** (log-linear mixture of quad/tri/bi/mono — the sharpest scoring model and the
> one measured *scoring* win on short messages; strictly recommended over `-q` when the language is
> known, see the `-a` entry below), `-S m4a10` staging (mono pre-pass then weighted), `-J` (dynamic
> move order, wins the realistic ~10-plug regime), `-M` (with a tight cap), and the best-board finisher
> `--gainfix-best3` (the recommended finisher — strictly ≥ `--gainfix-best` at its near-free default
> K=8, so there is no short-message case where `--gainfix-best` is preferred over it).
> Several opt-in flags are **not recommended** — they are dominated, ablation/measurement
> tools, or only conditionally useful, and have not been proven to strictly dominate on the
> plain short-message sweep: `-I`, `--infl-order`, `-F`, `--repair3`, `--no-repair`,
> `--gainfix` (superseded by `--gainfix-best3`, kept for `-F`/`--exhaust` compatibility),
> `--gainfix-best` (superseded by `--gainfix-best3`, which is strictly ≥ it at near-free cost;
> kept as the simpler/lighter variant and for ablation), and `--exhaust`. Each is tagged
> **not recommended** in its entry below and in `--help`.

> **Search playbook — the measured priority (this is the whole game for search).** `-R` restarts are
> the **primary quality lever**; spend compute there first, via `-T` (which parallelises the
> `keys × restarts` space, so more cores buy more R at the same wall time). `--gainfix-best3` is a
> **near-free bump on top** — turn it on and leave it on — but it is **not a substitute for restarts**:
> at matched wall time *every* finisher variant tried (the explicit cascade, `--gainfix-best3`,
> higher-K, the depth-1 `1sac` and depth-3/4-ply probes) is **Pareto-dominated by more `-R`**, and the
> finisher's edge **fades as R grows** (restarts subsume the near-solution boards it targets — measured:
> its lift roughly halves R80→R160 and can vanish by R160). The reason is that the hard residual is
> **wrong-basin** failures — the climb converged somewhere not near the truth — which only a fresh
> *restart* can *land* near; local plug repair (any finisher) can complete an already-near board but
> cannot relocate a wrong basin. Raising R helps until the **scoring-failure floor** (boards where the
> true plugboard does not score highest — an information limit no search can cross; ~5% at L40); past
> that only a **sharper scoring model** moves the needle, not more search — which is exactly what the
> **`-a` weighted model** delivers (the first measured short-message *scoring* gain, +~1–2pp mean
> %-correct at L40–100 across all four languages; PR #106). Recipe:
> `-c -J --gainfix-best3 --score m4a10 --random 10 -R <as high as -T affords> -a -l <lang> -T <cores>`.

- `-u X` reflector A/B/C or `.` wildcard (`N` forced by `-n`)
- `-w XYZ` wheels (digits, or `.` per position to brute-force)
- `-x N` highest wheel number to consider when wildcarding (default 5)
- `-n` Norway Enigma mode
- `-4` M4 (4-rotor) mode: `-u` selects thin reflector `b`/`c`; `-w`/`-r`/`-g` take
  **four** characters with the Greek wheel (`B`=Beta/`G`=Gamma) / ring / start first
- `-r XYZ` / `-g XYZ` ring / start positions (letters or `.`)
- `-s AB...` fixed plugboard pairs — **held fixed during `-c`/`-A`**: the climb/SA
  never remove or rewire them (their letters are marked in `plug_fixed[]`, set once from
  `opt_steckerbrett` before the threaded search, and skipped by every switch/remove/
  re-pair/toggle move), so `-s` supplies *known* plugs and the search recovers only the
  rest. They still seed a plain (no-climb) decrypt as before.
- `-c` hill-climb the plugboard
- `-I` **circular first-improvement** climb (**not recommended** — worse per restart; prefer `-J`) instead of steepest ascent (needs `-c`; off by
  default). `hillclimb()` normally full-scans all 325 toggle moves per accepted move and takes
  the single best; `-I` sweeps the same fixed 325-pair toggle list (each pair a `toggle a-b`:
  already-paired → remove, else add/move/merge) with a cursor, applies the **first** improving
  move, and **continues from where it accepted** (circular — so each move is examined ~once per
  sweep and attention rotates evenly instead of favouring low letters). ~2.8× fewer `score_iter` per climb, no data structure (which is why
  it wins at 50 chars where the surrogate/delta ideas lost). Deterministic (fixed order +
  acceptance, no RNG) → `-T`-independent; **not** byte-identical (different trajectory). It
  recovers **worse per restart**, so it is a *throughput multiplier*, not a free win: pair it
  with more `-R` and it wins at **matched compute** (+8pp exact / +1–23pp mean %-correct at
  L40–60, scaling with how much signal the message has — `PERFORMANCE.md` §7.2). Because a
  user at the default `-R 0` (a single deterministic climb) would get worse results, it is opt-in.
- `-J` **first-improvement with dynamic best-first move ordering** (implies `-I`; needs
  `-c`; off by default). Each climb first scores *every* move once against its starting
  (perturbed) board, sorts, and runs the circular first-improvement in that order — the
  order is rebuilt **per restart**, so it front-loads good moves *without* collapsing the
  restart diversity that a *static* (fixed-across-restarts) informed order destroys. Costs
  +24% `score_iter`/climb (the extra scan), so it is compared at matched compute
  (`-J -R 18` ≈ `-I -R 22`). Measured a **robust win on the realistic ~10-plug regime**
  (+2–6pp mean %-correct at L40–60, two seeds) and a **loss at 6 plugs** (best-first
  over-commits when few plugs are truly needed), hence opt-in. 10 plugs is the
  `crackquality` default and standard Wehrmacht, so the win lands on the hard/realistic
  case. The 6-plug loss is over-plugging: capping at the true count (`-J -S iKqK`, the same
  known-plug-count prior as `-A -S qK`) turns it into a **+~30pp win vs uncapped** at matched
  compute — so the recipe is count-dependent (`~10 plugs → -J` uncapped; `known-few → -J --score iKqK`).
  Static frequency-ordering was measured and **rejected** (`PERFORMANCE.md` §7.2).
- `--infl-order` **influence-ordered first-improvement** (**not recommended** — measured, dominated by `-J`; experimental; implies `-I`; mutually
  exclusive with `-J`; needs `-c`; off by default). Orders the move sweep by the board-state
  **influence** `w(a,b)=ct_count[a]+ct_count[b]+pt_count[a]+pt_count[b]` (two 26-bin histograms
  over the ciphertext and current decrypt — the §4.5/§4.6 weight) instead of `-J`'s measured
  score-delta, so it is nearly **free** (no per-move pre-scan). Per-restart, deterministic
  (`-T`-independent). **Measured, dominated**: at matched compute it beats plain `-I` (+5–10pp)
  but loses to `-J` (−4 to −6pp at L60–90) — `-J`'s score-order wins wherever there is signal
  to rank on (`PERFORMANCE.md` §4.6). Kept as a documented-dominated opt-in, not recommended.
- `-M` **cap-as-target** climb rule (needs `-c`; off by default). Changes what the plug
  cap means during the climb: by default the cap is only a *growth ceiling* (at/over the
  cap, a brand-new **add** is blocked but count-preserving reshuffles are allowed), so an
  over-cap board — the common case when a big `--random` kick lands on a small stage cap —
  can converge still holding more plugs than the cap, merely reshuffled. `-M` makes the
  cap a strict **descent target**: at/over the cap only **count-reducing** moves are
  allowed (**merge** — both ends already plugged to different partners → −1 — and
  **remove**), blocking adds *and* count-preserving endpoint-moves, so the climb must shed
  plugs down to the cap while keeping the strongest descent move (the merge). Measured a
  **matched-compute win that grows as the true plug count falls below the cap**: neutral-to
  -**+2.6pp** mean %-correct on realistic 10-plug boards (`--score i4q10 --random N`, best at
  the true-count kick `N≈10`), and **+3…+20pp** on **known-few-plug** boards (`--score i4q6 --random N`,
  largest at the short/hard end — +20pp at L40) — because forcing a clean descent stops the
  baseline wasting its climb reshuffling an over-cap board. It is also **cheaper per climb**
  (up to ~2.7× fewer `score_iter` in the `q6` regime: quad converges from a tidy ≤cap
  basin), so at matched compute it earns many more restarts. Most useful with a **tight
  `--score` target cap**; near-inert (harmless) with no cap set. `-T`-deterministic. (The reason
  the IC-pre-pass cap in `--score i4q…` is a *flat plateau* by default is that without `-M` the
  cap can't pull an over-cap board down; `-M` is what makes a tight cap bite — see
  `PERFORMANCE.md` §7.3.)
- `--repair3` **3-plug re-pair barrier cross** (**not recommended** — measured, dominated; needs `-c`; off by default). The 3-plug
  generalisation of `try_repair` (`try_repair_3()`): tried only at convergence, once the
  toggle climb **and** the 2-plug `try_repair` have both stalled, it rematches three existing
  plugs (six letters) into a different pairing (the 8 genuine count-neutral reshufflings that
  share no pair with the original) and keeps the best improving one — a deeper local-optima
  escape the single toggle and 2-plug re-pair can't express. Count-neutral (no cap gating);
  costs O(C(np,3)·8) `score_iter` (960 at 10 plugs) per call, paid only at convergence.
  Default off (baseline byte-identical); `template<bool EX>`/`plug_fixed` like `try_repair`.
  **Measured, dominated**: at matched compute it loses ~2pp mean %-correct — the per-climb
  cost is better spent on more `-R` restarts (`PERFORMANCE.md` §4.7). A documented-dominated
  opt-in, not recommended — same verdict as `--exhaust`.
- `--no-repair` **disable the default 2-plug `try_repair` barrier cross** (**not recommended** — ablation/measurement flag; needs `-c`; off by
  default). An ablation/measurement flag: the 2-plug re-pair is normally always-on (it earns
  its keep at long lengths — `archived/CODE_REVIEW_HISTORY.md` §9 item 7), and this turns it
  off so its value can be A/B'd (e.g. at short lengths where its convergence scan is a larger
  fraction of a fast climb). Default off keeps the climb byte-identical; the flag only skips the
  `try_repair` call at each convergence.
- `--gainfix[=GATE]` **quadgram-gain directed-repair cascade** (**not recommended** — prefer `--gainfix-best3` on the plain sweep; kept as the `-F`/`--exhaust`-compatible variant; needs `-c`; quad-only; off by
  default). At each quad convergence, uses per-position quad **gain** to propose plug corrections on
  *both* plugboard contacts — the exit re-plug `{S[pt[j]], bx}` and the reciprocal entry re-plug
  `{ct[j], core_j(S[bx])}` (machine-exact via the precomputed rotor core; self-encryption pruned
  since Enigma never maps a letter to itself) — ranks them by the full re-decode score, and runs a
  **2-ply cascade**: apply the best plug *even if downhill* (which un-masks a masked second plug),
  keep the pair only if the net beats the converged score, then let the cheap climb resume and
  finish it (the reclimb amplification is free from the `do/while` loop). **Gated** by a
  near-solution per-symbol score threshold (`GATE`, default `-4.9` English-quad-calibrated; tune per
  language) so it fires only on promising boards and skips the ~76% junk — which is what makes it a
  small **matched-compute win** (+0.2–0.3pp mean / +0.5–0.6pp exact on short English, ~zero added
  `score_iter`); *ungated it is dominated*. Default off (baseline byte-identical); `-T`-deterministic;
  `template<bool EX>`/`plug_fixed` like `try_repair`. See `PERFORMANCE.md` §4.10.
- `--gainfix-best` **best-board-only gain cascade** (**not recommended** — superseded by
  `--gainfix-best3`, which is strictly ≥ it at near-free cost; kept as the simpler/lighter variant and
  for ablation; needs `-c`; mutually exclusive with `--gainfix`;
  off by default). The fixed-cost alternative to per-convergence `--gainfix`: runs the gain cascade
  **once, unconditionally (no score gate)**, on the single best board after all `-R` restarts (its key
  + stecker recorded at the merge), then one finishing climb. Costs a **fixed** ~950 `score_iter`
  independent of `-R`, so it is ~free at high `-R` and generally ≥ per-convergence `--gainfix` (an
  N=100 matched-compute sweep, English L40–50: both give +0.2–0.7pp mean %-correct vs base across the
  whole `-R 8…2560` range with no decay; `--gainfix-best` wins or ties 8 of 10 `-R` rows). Exact
  recovery does saturate at extreme `-R` (all modes tie once restarts alone find every recoverable
  board — the residual is the scoring-failure floor), but the mean gain persists. Simple sweep only
  (not `-F`/`--exhaust`). `-T`-deterministic. See `PERFORMANCE.md` §4.10.
- `--gainfix-best3` **best-board finisher with a deeper 3-plug-tangle escalation** (**recommended** —
  the stronger of the two finishers, strictly ≥ `--gainfix-best` at its near-free default K=8; needs `-c`;
  mutually exclusive with `--gainfix`/`--gainfix-best`; off by default). Like `--gainfix-best`, but the
  once-only best-board finisher also runs a **"sacrifice + reclimb"** step when the 2-ply cascade finds
  nothing: it ranks the `(plug1,plug2)` sacrifice pairs by 2-plug score and, for the top-`K` (K=8),
  commits the sacrifice (both plugs, possibly downhill) and runs a full plain reclimb — letting the
  ordinary climb find the completing plug(s) and shed spurious ones — keeping the best. No explicit
  plug3 search (the completing plug is the top move the reclimb finds anyway; a full climb per sacrifice
  recovers *more* than committing one fixed plug — it matches the explicit 3-ply at K=6 and beats it at
  K=12). Targets 3-plug tangles the 2-ply pair can't cross: real-tool capped, it solves **56% of fixable
  ≥80%-base boards vs `--gainfix-best`'s 41%**, and beats 2-ply by **+~1.3pp mean / +2…5pp exact** at
  matched compute (english+german L40), for ~+2–3 ms wall (<1% for `-R`≥640). Simple sweep only.
  `-T`-deterministic (internal reclimb reuses `hillclimb` with gainfix off — no recursion). See
  `PERFORMANCE.md` §4.11.
- `-A N` recover the plugboard by **simulated annealing** instead of the greedy climb
  (needs `-c`; `0` = off, use the greedy climb). `N` is the move budget — SA's
  cost/quality knob, the analogue of `-R`. One geometric cool-down per key: an IC
  pre-pass seeds the board (mirrors `-S iq`), acceptance-ratio calibration sets the
  temperature from a warm-up sample (`anneal_once()`), the walk accepts worsening
  `toggle-connect` moves with probability `exp(Δ/T)`, and a final greedy quench lands
  on a local optimum. `χ0 = 0.12` (a *cool* start) was tuned by a quality-per-climb-time
  sweep — the surface is greedy-friendly, so a mostly-downhill walk with occasional
  escapes matches or beats greedy `-R --score iq` at equal compute (the guessed `χ0 = 0.8`
  lost ~2×; reheating and chain-length sweeps didn't help). All randomness is from the
  per-key RNG stream (same `opt_seed + key_index` mix as `-R`), so `-A` is
  `-T`-independent. It composes with `-R` (each restart is an independent SA trajectory)
  and `-F` (SA runs in tier 2). SA is a *peer* of the greedy restart climb, not a strict
  win — see `archived/CODE_REVIEW_HISTORY.md` §9 item 5 and `archived/SIMULATED_ANNEALING.md` §15. **SA honours
  the `--score` target-stage plug cap** (`opt_stages[last].cap`): `-A N --score qK` caps the whole
  trajectory (IC pre-pass, the cap-aware `apply_toggle`, and the quench) at `K` pairs.
  When the true plug count is known and below 13, that prior is a *measured win on short
  messages at modest budgets* (it stops SA adding spurious plugs a noisy short-message
  quad score would reward), neutral once the message/budget is large enough to recover
  the board unaided, and a loss if set below the true count — `archived/SIMULATED_ANNEALING.md`
  §16. With no `--score` the cap defaults to uncapped (13), so plain `-A` is unchanged.
- `-R N` / `--restarts N` plugboard hill-climb random restarts (**REDESIGN Part B,
  Option A — kicked-only**): `-R 0` (**the default**) is one deterministic climb from
  the seed, **no kick** (fully reproducible); `-R N` (N≥1) is exactly **N kicked climbs**
  (each from the seed plus a fresh `--random` kick, best kept) — the un-kicked seed climb
  is *not* additionally run. Per-restart RNG seeded from `opt_seed + flat key index +
  restart`, so each climb is an independent unit and the result stays independent of `-T`.
  ~`N`× the `-c` cost. The restart count is separate from the schedule string (`--score`).
  A non-fatal **pigeonhole warning** fires when `-R N` exceeds the distinct `K`-pair kicks
  among the free letters (they must then repeat).
- `--random K` (long-only) **kick size**: `K` random plug pairs injected per restart
  (0–13; default **10**, near the typical plug count). `--random 0` is a legal control
  (no perturbation — N restarts then repeat the seed climb). Needs `-c` (errors otherwise,
  since a kick does nothing in a bare rotor scan). Replaces the old `-S rN` token.
- `--exhaust E` (long-only) **partial plugboard exhaustion** (**not recommended** — measured, dominated; §3.6 in `PERFORMANCE.md`):
  force `E` **extra** plug pairs among the free letters (on top of any `-s` pairs) — `E`
  counts *forced* pairs, not total. It tries *every* set of `E` disjoint pairs (pinned like
  `-s`), runs the staged climb from that seed, and keeps the best. It **composes** with the
  kick and restarts (fixing the earlier silent no-op): for each forced combo, `-R N` runs N
  kicked climbs (the kick perturbs only the still-free letters). `--exhaust 1` (no `-s`) = the
  325 first pairs; larger `E` explodes as `free!/(2^E E! (free−2E)!)` (~45k for 2, ~3.5M for
  3). **Parallel** (REDESIGN Part D): the *first forced pair* is the work unit — ≤325 units
  per key, spread across threads like restarts, each running its own sub-exhaustion ×
  restarts against a per-worker pin set — so `--exhaust` scales with `-T` and stays
  `-T`-independent. Validation forbids `-A`, requires `-c`, and bounds `E` by the free plug
  pairs (13 − `-s` pairs). Deterministic. **Measured, dominated** — at matched `score_iter`
  a high-`-R` greedy climb beats it by 10–40pp exact (§3.6); an exploration tool, not
  recommended. Replaces the old `-S aN` token (note the semantics change: `aN` counted
  *total* pinned pairs, `--exhaust E` counts *forced* pairs).
- `-e N` random seed for the restart perturbation. Resolved as `-e` > `$ENIGMA_SEED`
  > a fresh `std::random_device` draw, so **by default every run explores different
  restarts**; the chosen seed is echoed by `show_settings()` (when restarts are
  active) so a random run can be reproduced with `-e`. `opt_seed == 0` reproduces the
  historical pre-seed behaviour exactly (the RNG mixes `opt_seed + key_index`), which
  is why `tests/` and the `crackquality`/`bench` harnesses pin `ENIGMA_SEED=0` for
  deterministic, cross-ref-comparable runs.
- `-S <schedule>` / `--score <schedule>` staged plugboard climb — a string of
  `<letter><optional cap>` **model tokens** `i`/`m`/`b`/`t`/`q`/`a` parsed by
  `parse_schedule()` into `opt_stages[]`. Each is a climb stage run in order; the number
  caps the **plug pairs** that stage may set (1–13; omitted = uncapped, 13). The **last**
  model token is the target/ranking model (sets `opt_scoring`), so the target lives *in*
  the string — e.g. `--score i6q` = IC capped at 6 pairs, then quad uncapped. A lower-order
  early stage steers the first plugs into a better basin (its surface is smoother when few
  plugs are set); **`--score i…q` (IC pre-pass) is the best measured** for a quad target — much
  better than bigram, extra stages after IC add little. **The recommended target is now `a`
  (weighted), staged as `--score m4a10`** (mono pre-pass then weighted, both capped) — the `a`
  stage reads the log-linear `all8` table, so `-S m4a10` is byte-identical to the winning tuning
  recipe. The schedule carries **only** model stages: the
  per-restart kick and the exhaustion are their own options (`--random` / `--exhaust`), not
  schedule tokens (REDESIGN Part B moved the old `rN`/`aN` tokens out). Per-`machine`
  `scoring` field (never a global → race-free); deterministic; the `--score` stages,
  `--random` kick and `-R` count compose. Without `-c`, a `--score` schedule that carries
  climb-only detail (more than one stage, or any cap) emits a non-fatal warning and the run
  ranks by the target model (there is no climb to apply the stages to). (Replaces the
  earlier separate `-L` cap, which was folded into the per-stage numbers — see
  `archived/CODE_REVIEW_HISTORY.md` §9.)
- `-l lang` scoring language — **required** for `-m/-b/-t/-q/-a` (no default), not
  used by `-i`. `-l` alone does nothing: it takes effect only with an n-gram model,
  so it is `-q -l english`, not `-l english`, that scores with English quadgrams.
- `-i/-m/-b/-t/-q/-a` scoring model: IC / mono / bi / tri / quad / weighted-all. **IC is
  the default** — the only model needing no `-l`, so the tool runs with no scoring options
  (an n-gram default would be inconsistent: it requires a language, which has no default).
  **`-a` (weighted) is the sharpest and the recommended model when the language is known**
  (see its entry below); quad (`-q`) is the plain single-order alternative. Each selector is
  a **thin alias for a single uncapped `--score <model>` stage** (REDESIGN Part C); it sets
  the scan **ranking** model and the climb **target** model. Setting the model to *conflicting*
  values is a **fatal error** — two disagreeing selectors (`-m -q`, `-q -a`) or a selector vs a
  different `--score` target (`-m --score q`) — since the intent is genuinely ambiguous;
  agreement is silent (`-q -q`, `-a --score m4a10`, `-q --score i4q10`).
- `-a` **weighted all-order scoring** (**recommended** when the language is known; needs `-l`;
  a schedule token too — `-S m4a10`). Scores each quadgram window as a **log-linear mixture**
  of all four orders — `a·log p(ABCD) + b·log p(BCD) + c·log p(CD) + d·log p(D)` with baked
  weights `(1, 0.6, 0.3, 0.15)` and the **symmetric folding** (every sub-gram a window contains,
  divided by its window-multiplicity 2/3/4, so leading edge grams are included). It is a
  **geometric (Product-of-Experts) mixture** that stays in joint log-prob space (weights `(1,0,0,0)`
  = plain quad), folded once into a quad-shaped `all8` table at load — so the per-character scoring
  **hot path and gainfix are unchanged** (they treat `all8` exactly like `quad8`). Measured the
  **first short-message scoring win** in the tuning history: +~1–2pp mean %-correct at L40–100 across
  all four languages (2000-trial German confirms +1.3pp avg, all lengths positive), neutral by L≥190
  where quad already saturates. The linear (Jelinek-Mercer) form was tried and **lost** (the
  conditional reframing it forces is the cost); log-linear wins because it is *conjunctive* — a
  candidate must look plausible at every order at once. See `PERFORMANCE.md` / PR #106. Because `-a`
  is the sharpest model, the recommended recipe is `-c -S m4a10 -J --gainfix-best3 -a -l <lang>`.
- `-p file` compare the recovered plaintext against a known plaintext file
- `-p file` compare the recovered plaintext against a known plaintext file
- `-F N` / `-F N%` key pre-filter (**not recommended** — situational: a long-message throughput tool, unreliable on the short/hard end and proxy-measured; needs `-c`; `0` = off): a two-tier search — tier 1
  ranks *every* key by a single **cheap IC climb** and keeps the top `N` (or top `N%`
  of the resolved keyspace); tier 2 runs the full `-R`/`-S` climb on only those. The
  big *throughput* win (~8–20× over climbing every key), so more restarts are
  affordable per surviving key. Both tiers are parallel and `-T`-deterministic
  (per-thread top-N min-heap, deterministic tie-break). Details worth knowing:
  - **Tier 1 is a *climb*, not a plugboard-free scan.** A raw IC *scan* fails
    (rotor-only decrypt is ~95% scrambled under a full board, ~0% top-1 recall); an IC
    *climb* partially recovers the stecker and discriminates.
  - **Tier 1 climb is capped at `filter_climb_cap = 5` plug pairs.** Capping both
    speeds tier 1 up and *improves* recall — an uncapped climb lets wrong keys overfit
    IC and bury the true key. Measured both-axes win (+~16pp recall, ~1.4× faster;
    harmless on easy keyspaces). `cap≈5` (near the true plug count) is the optimum.
  - **`N%` scales with the keyspace** (recall tracks the *fraction* kept, not the
    absolute count); absolute `N` bounds tier-2 cost. Both forms are supported.
  - **Recall is strongly length-dependent** (measured via `--true-key` /
    `FILTERRECALL=1`; `CRACKQUALITY_TESTS.md` §2): on a 6-order proxy at 10 plugs the
    true key's median tier-1 rank is ~12k/105k at L120 (effectively unrecoverable),
    ~27 at L200 (bimodal), and 1 at L300. So `-F` is sound for realistic-length
    traffic (~L300) and unreliable on the short/hard end — and the real 60-order
    keyspace is worse than the proxy.
  - **Chi-squared was benched as the tier-1 model and lost to IC** (χ² is gameable by
    the plugboard permutation) — IC stays. See `archived/CODE_REVIEW_HISTORY.md` §9 item 2.
  - **Tier 1 shows a live `\r` progress line** (`ranking NN% (done / total keys)`)
    while it ranks, but only when stderr is a terminal (`isatty`) so redirected logs and
    the tests stay clean. A shared atomic counter drives it, and because each atomic add
    owns a disjoint slice, exactly one thread prints each 1% step — no races, `-T`-safe.
- `-d dir` directory holding the n-gram files (else `$ENIGMA_DATA`, else `ngrams`)
- `-T N` worker threads for the search (default 1, max 256). Parallelises over the
  `keys × restarts` work space, so it scales even a **fully-specified rotor key** with
  `-c -R N` (the restarts are spread across threads) — not just wildcarded keyspaces.
  `-T`-independent (deterministic regardless of thread count).

Every run echoes the resolved configuration (scoring model, language, n-gram data
directory, machine settings, plugboard, ciphertext length) to stderr.

> **Gotcha — match `-l` to the plaintext language, especially for `-q`.** There
> is no default language: `-m/-b/-t/-q` require `-l`, and the n-gram tables are
> highly language-specific (most of all quadgrams). Scoring an English message
> with, say, `-l german` typically fails — the german table scores ~0 for
> English quadgrams and the correct key does not stand out. Lower-order models
> (`-m/-b/-t`) tolerate a mismatch better, and `-i` (index of coincidence) is
> language-independent and needs no `-l`. Use `-l` matching the plaintext.
>
> **Quad works for every language (after the table-loading fix); `-q` is the
> default recommendation.** An earlier eval round suggested German needed a
> lower order (`-b`/`-t`), but that was a **bug**: `load_counts` truncated the
> non-English tables at the first accented gram, so the "german quad" scorer ran
> on only its 29 most frequent quadgrams (4.9% of the table). Fixed by folding each
> accented gram to its A-Z base and accumulating counts (`ä→A`, `ø→O`, `ç→C`, …;
> `load_counts`, and the ciphertext/plaintext readers fold input the same way and
> warn on non-mappable characters — see `PERFORMANCE.md` §6.9). With the full table
> German quad is search-bound and fully solvable like English (German L90 mean
> %-correct 24.7 → **91.1** before → after the fix); on an orthogonal four-language
> grid german/danish/french all crack comparably to English. Model order is *not*
> meaningfully language-dependent once the tables load — use `-q`. The residual gap
> is *genuine telegraphic* German (operational orthography off-distribution for the
> prose tables — §6.6), not the model order.

## Architecture / how it works

A single pass through `main()`:

1. Parse and validate options (`getopt`).
2. `readciphertext()` reads stdin, uppercases, and keeps only A–Z.
3. Load the n-gram table matching the chosen scoring model and language; each
   count is stored as the log10 probability `log10(count / total)` (unseen grams
   floored at `log10(1 / total)` — scored as a single occurrence, a hapax), so the
   additive scorers sum a log-likelihood and `score_iter`'s per-symbol average is a
   cross-entropy (dits/char). IC is a separate normalised ratio and is left untouched.
   `ngrams_read()` quantises each table directly into a **uint8 fixed-point** copy
   (`mono8`/`bi8`/`tri8`/`quad8`, `ngram_scale = 32`) that the scorer reads: `q =
   round((v − bias)·32)`, with a **per-table `bias` = the table's minimum log10 value**
   so all 256 levels land on the actual `[vmin, vmax]` range (biases in
   `ngram_bias[SCORE_*]`). The hapax floor caps each range at `log10(max_count)`, and
   the widest table (english trigrams, ~7.9 log10 units) fits the `255/32 ≈ 8`-unit
   uint8 window without clipping. This matters for **quad** — the hot, largest table
   (26⁴ entries): uint8 shrinks it to 0.45 MB (vs 1.8 MB float / 0.9 MB int16) so it
   stays cache-resident during the scan (measured faster; see Performance notes).
   mono/bi/tri are tiny and already cache-resident — same representation only for
   consistency. Scorers sum uint8 into a `long` and recover the log-prob sum as
   `isum/32 + n·bias`. Recovery quality is identical to the wider types for every model,
   measured across all four languages.
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
     of the flat work space; each worker decodes a flat index → (wheel-order,
     ring, start) by mixed radix, points its private `machine` at that wheel
     order's shared table (swapped, never recomputed, on a boundary), and:
     - **The work space is `keys × restarts`, not just keys** (`restarts` = `-R` under
       `-c`, else 1). The `-R` plugboard restarts of a key are independent — each draws
       from its own `(key,restart)` RNG seed (`restart_seed`) rather than one stream
       advanced sequentially — so they are spread across threads too. Restart is the
       innermost dimension, so consecutive items share a key and reuse its `setup_mapping`.
       **This is what lets a *fully-specified rotor key* (one key) still use every
       thread** — the old key-only scheme left that case single-threaded (`-T` a no-op).
       The `-F` tiers and the plain scan keep one item per key. Determinism is preserved
       by a lowest-work-index tie-break in the best-merge (`better_cand`), since parallel
       restarts of one key often converge to the same score.
   - `setup_mapping()` steps the rotors over the message length and records, per
     position, a pointer `rows[pos]` to that position's rotor-stack substitution
     row (folding in the stepping). The scan points `rows[pos]` straight into the
     shared `subst_array` (no copy); hill-climb copies the row into a contiguous
     `mapping[]` first (it re-reads each row many times);
   - `decode()` + `score_iter()` produce and score the candidate (the n-gram
     scorers fuse the decode into their loop). The best is merged under a mutex
     (which also serialises the live progress line). Parallelising the flat key
     space means rings/starts scale even when the wheels are fixed.
   - With `-c`, `hillclimb()` greedily improves the plugboard: each pass scans a single
     **"toggle a–b"** operator over all 325 letter pairs — one operator that, by the
     current state of a and b, *adds* a new plug (both ends free), *moves* an endpoint
     (one end plugged), *merges* two plugs into one (both plugged, different partners), or
     *removes* the a–b plug (a and b already paired) — and takes the single best improving
     toggle, run to convergence. (Folding removal in as the already-paired case is what
     lets one scan replace the former separate switch-scan + removal-loop; a switch still
     wins ties over a removal, so it stays byte-identical.) Then one "re-pair" move
     (`try_repair()`, re-match two plugs into the other pairing — the one count-neutral
     two-plug move a single toggle can't express) is tried as a barrier-cross, and if it
     improves the cheap climb resumes; re-pair is a general local-optima escape gated to
     fire only at convergence (~zero cost). The plug cap gates the toggle by count-effect:
     an *add* is blocked at/over the cap, and with `-M` a count-preserving *move* too, so
     only the count-reducing *merge*/*remove* remain (cap as a strict descent target). See
     `archived/CODE_REVIEW_HISTORY.md` §9 item 7.
6. The best-scoring plaintext is printed; optionally compared to `-p` file. A
   final stderr diagnostic reports the number of rotor combinations analysed
   (`= total_keys`, brute force has no early exit) and plugboards scored (total
   `score_iter` calls, summed from a per-`machine` counter so the hot path stays
   lock-free; `-T`-independent), then wall-clock time, thread count, the number/size
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
two-pass on both compilers, markedly so on clang/ARM). **Quantified once** (Bench
CI, `make bench LONG=1` A/B storing-into-`m.plaintext` vs fused, hillclimb hot
path): g++ **+6.8%** (x86_64) / **+7.0%** (arm64), clang **+28.4%** (x86_64) /
**+45.6%** (arm64) slower with the store; the scan is +0.6–2.6% under g++,
+8.8–9.9% under clang. So the store is small-but-negative on g++ and a large
regression on clang — the reason the fused form is kept (measurement PR #77,
store-variant commit `71d8633`; the plaintext is materialised lazily only on a
new best, so nothing needs it per-scoring). An even earlier
16-byte-blocked decode was never shown to win and was removed too; the scalar
fused loop is the current form.

The tables the scorers read are **uint8 fixed-point** (`mono8`/`bi8`/`tri8`/`quad8`,
`ngram_scale = 32`, per-table `bias`), not float, and each scorer sums uint8 into a
`long` (exact and order-independent, a small determinism bonus) then recovers the
log-prob sum as `isum/32 + n·bias`. This is a cache win for **quad** — the hot, largest
table: shrinking it to 0.45 MB (from 1.8 MB float / 0.9 MB int16) keeps it cache-resident
during the **scan**, where every key decodes a fresh message and hits cold cells. The
narrowing happened in three shipped steps, each measured on `make bench`: float→int16
(0.9 MB) gave **search −16/−17%**, then int16→uint8 (0.45 MB) a further **~−4/−6%**
(~−20% total vs float). It is machine-dependent (the win is the cache-level latency gap,
so it shrinks or vanishes on CPUs where the boundary sits elsewhere). The **hill-climb
re-scores one message** and keeps its few cells warm, so it gains less. mono/bi/tri are
tiny and already L1/L2-resident: 8-bit gives them **no measurable speed-up**; they carry
it only for representational consistency. The uint8 range fits because unseen grams are
floored at a hapax (`log10(1/total)`), capping each table at `log10(max_count)` — the
widest (english trigrams ~7.9 units) fits the `255/32 ≈ 8`-unit window; recovery is
**neutral** vs int16 (`make crackquality`, 160 trials/length, all four languages).
Rejected precision/SIMD alternatives on record: 16-bit and lower resolutions were the
step *before* 8-bit (8-bit won); `-march=native` (no win — the gather-bound loop does not
auto-vectorise), hardware SIMD gathers (latency-bound, not throughput-bound), and the
delta-scorer (`archived/SIMULATED_ANNEALING.md` §6.2). 4-bit would need <16 levels over a ~8-unit
range and is not viable.

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
>
> The **climb move loop** is aliasing-sensitive the same way (Part D finding): its
> `plug_fixed` reads must stay a **plain-global** access — routing them through a struct
> member, a `thread_local`, or an opaque pointer parameter cost ~18% on one compiler or
> the other. Hence the `template<bool EX>` climb chain (the common `EX=false` instantiation
> folds to the plain global; only `--exhaust` uses `EX=true`) and the compiler-conditional
> storage for the exhaust scratch (`PLUG_FIXED_EX`: `thread_local` under clang, a `machine`
> member under g++ — each compiler's measured-neutral form; verified byte-identical to the
> clean build under clang). Also beware: the climb/scan benches on a shared box can be
> **bimodal** — before trusting a regression, re-run and check base-vs-base; disassembly
> comparison (`objdump -d`) settles whether codegen actually changed. The shipped form was
> verified neutral **end-to-end on Apple-silicon clang** (M2, the layout-sensitive target):
> `make bench LONG=1` vs a pre-REDESIGN base, all four benchmarks within ±0.7%.

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
  reentrant (the precondition for multi-threading — see `archived/CODE_REVIEW_HISTORY.md` §5/§6).
  The read-only data stays file-scope global and shared: the wiring tables
  (`rotor_fwd/rev`, `notch`, `reflector`) built by `init()`, the n-gram tables,
  and the `ciphertext` / `num_ciphertext` / `textlength` input. (`-Wshadow` is on;
  the redundant `textlength`/`ciphertext`/`plaintext` parameters that used to
  shadow the globals were removed earlier.)
- The live diagnostics are `showconfig` (echo the winning key + plugboard on a new
  best) and `show_settings` (echo the resolved config at startup). Progress lines are
  fixed-width columns under a one-time header (`Score W R G S Text`, printed by
  `showconfig_header` before the first line — `best_result.header_shown`): score,
  reflector+wheels, ring, start, plugboard (room for all 13 pairs) and the first 15
  characters of the decoded text — the preview is decoded on the fly from the
  machine's *current* board (`m.plaintext` can be stale mid-climb); worst case 78
  chars, inside a 79-column terminal. With `-c` the echo is per plugboard
  IMPROVEMENT, not per finished climb: every accepted climb/SA move whose
  (target-model) score beats everything echoed so far prints a progress line
  (`report_climb_progress`, called on accepted
  moves only — nothing on the 325-move scoring scans, so the hot path is untouched).
  Display state lives in `best_result.shown` (atomic), never read by the merge logic,
  so which candidate WINS stays `-T`-deterministic; which lines appear is thread-timing
  dependent, as before. Staged pre-pass stages and the `-F` tier-1 filter score in a
  different model and stay silent (`m.report` + target-model gate).
  The unused debug scaffolding has been removed: the `SHOWHILLCLIMB` compile-time
  climb-trace path (and its vestigial per-climb `iter` counter), the `#if 0` blocks and
  the dead `ciphertext_letterdist`/`compare`/`count`/`order` cluster that only fed one,
  and the earlier `all_subst_score`/`map`/`opt_threads`/`opt_logfilename` dead code
  (see `archived/CODE_REVIEW_HISTORY.md` §3).
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
  CodeQL workflow runs on PRs and weekly. Keep all of these green. A `Bench`
  workflow (`.github/workflows/bench.yml`) additionally runs `make bench LONG=1`
  on a {g++, clang++} × {x86_64, arm64-Linux} matrix — on PRs as a same-machine
  A/B vs the PR base, but **advisory only** (`continue-on-error`): shared runners
  are noisy/bimodal, so treat a flagged cell as "re-check on quiet hardware and
  compare disassembly", never as an automatic block (or a pass as proof).

## Status & remaining work

The still-open issues and roadmap live in `CODE_REVIEW.md`; the historical audit
(original findings, now fixed, plus the design rationale and rejected experiments)
is archived in `archived/CODE_REVIEW_HISTORY.md`. Most findings have been fixed —
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
`archived/CODE_REVIEW_HISTORY.md` §5). On **cracking quality for short messages** the
`make crackquality` harness shows every miss is a *search* failure (the plugboard
hill-climb sticking in local optima); the search levers shipped so far are random
restarts (`-R N`), the staged climb (`-S`), the **key pre-filter** (`-F N`, a
cheap-IC-climb tier that shortlists keys so the full climb runs only on the top
`N` — ~8–20× throughput, see `archived/CODE_REVIEW_HISTORY.md` §9 item 2), and **simulated annealing**
(`-A N`, tuned `χ0 = 0.12`; a peer of the greedy restart climb at equal compute —
`archived/SIMULATED_ANNEALING.md` §15). The heavier metaheuristics once listed as open —
tabu and **GA** — have since been **measured down**: `--restart-tt` (PR #100) found restarts
already almost never revisit a basin (near-total exact-board diversity at `--random 10`), so a
tabu visited-set has nothing to forbid; and an oracle probe of the GA precondition
(`PERFORMANCE.md` §3.10) found the crossover *material* exists (correct plugs union to ~8/10
across restarts) but is **unselectable** — board-fitness picks only ~2.5/10 and per-plug consensus
is worse (~1.1/10, amplifying the climb's decoy attractors). So the search frontier was no
longer a heavier search metaheuristic; it was **a sharper scoring model** and **restart
diversity**. Both have since been resolved, and the result closes the short-message frontier
to *smarter* methods (`PERFORMANCE.md` §6.15):

- **Scoring — resolved, a win: the weighted all-order model `-a`** (PR #106). A log-linear
  (geometric / Product-of-Experts) mixture of all four n-gram orders, folded once into a
  quad-shaped table so the hot path is untouched — the **first measured short-message scoring
  gain** (+~1–2pp mean %-correct at L40–100, all four languages; 2000-trial German confirmed).
  It is now near-optimal on **both** scoring axes: the **scoring-failure floor is ~1%** (SPLIT
  under `-a`: with the correct rotor key the true plugboard already scores highest ~99% of the
  time — so *discrimination* has essentially no ceiling left to recover), and the **climb-surface
  smoothness is flat** (an 8× sweep of the order weights moves search-fail% by <1pp). `-a` in fact
  won by *smoothing the climb surface* (fewer search failures), not by lifting an information
  floor. Scoring is tapped.
- **Search — resolved, compute-bound with no selectable shortcut.** At short lengths the residual
  is ~99% *search* failure, and the coverage curve is **still climbing at R=256** (~+15–25pp per
  4× R — the true basin is reachable, just a rare deep target). The apparent restart diversity is
  an illusion: 64 restarts give ~60 distinct *exact* boards but only **~15 distinct correct-plug
  states** (a 4× overcount — the rest is spurious-plug noise on a few basins), with per-restart
  depth ~0.7/10 and the truth assembled only in the **union (~9/10)**. Every *smart* lever to
  exploit that — recombination (GA), a truth-targeted kick, coarse basin-repelling — needs a
  truth-free way to tell the ~9/10 real plugs from the noise, and **per-plug consensus is only
  ~1.1/10 correct** (the frequent plugs are decoys). No such signal exists in the converged-board
  population, so the **only reliable lever is raw compute** — more restarts via `-T`, which scales
  predictably.

Read `CODE_REVIEW.md` (and, for the detailed design rationale, rejected experiments, and the
frontier measurements, `PERFORMANCE.md` and `archived/CODE_REVIEW_HISTORY.md`) before changing
the search or scoring code.
