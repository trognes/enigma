# ENHANCEMENTS.md — the open issues

What is still worth doing. Everything else — how each of these was arrived at,
what was tried and measured down, and the pitfalls list — is history and lives
in `archived/`, chiefly `archived/IMPROVEMENTS.md` (open work and pitfalls as of
archiving), `archived/PERFORMANCE.md` (every measurement), `archived/cribs.md`
(the crib programme) and `archived/refinement.md` (the derived `--ring-stride`
refinement). **`archived/` is read-only** — cite it by section, never edit it.

**Where things stand.** The tool is correct, warning-free under the project's
flag set on g++ and clang, clean under ASan/UBSan/TSan/valgrind/cppcheck/
clang-tidy, multi-threaded, and supports standard / Norway / M4. Nothing below
is a known bug. Two measurements frame the list and both say the same thing —
**there is little apparent headroom left**, though neither is a proof and this
conclusion has been drawn before and been wrong:

- **Search looks compute-bound.** Essentially every miss on the
  plugboard-recovery tier is a *search* failure, and the exact-recovery curve is
  still climbing at `-R 256`. No truth-free way to exploit the restart
  population is known, so raw `-R` bought with `-T` remains the dependable
  lever.
- **Scoring looks near its ceiling.** The discrimination floor is ~1% at L40–60
  and ~0 beyond, and the residue at L40 is information rather than model —
  unicity distance is ≈25 characters. But this was the stated position
  immediately before `-f` shipped and gained +3.0–4.4pp, so treat "no headroom"
  as a prior, not a finding.

Recommended recipe: `-c -S m4f10 -J --polish -f -l <lang> -T <cores> -R <high>`.

---

## Search and scoring

**1. ILS with incumbent-walk acceptance.** Nominally open, a long shot:
converged boards are *scattered* rather than clustered near the truth, and
clustering is the structure ILS would need. → `archived/IMPROVEMENTS.md` §2.

**2. The narrow L40 scoring re-opening.** The only observed scoring failures sit
at the identifiability floor — 5–10% at L40, 0 elsewhere. Length-sensitive
scoring could only help at L ≲ 40, where recovery is already near the
information floor, so the payoff is small. → `archived/IMPROVEMENTS.md` §2, §4.

## Keyspace reductions

**3. Two-notch wheels collapse ring × start by 13 — ✅ SHIPPED, always-on.**
VI, VII and VIII notch at `M` and `Z`, exactly 13 apart, so shifting that
wheel's ring and start together by 13 is a byte-identical decode —
unconditionally, at every length. `search_worker()` now skips `ring2 ≥ 13`
whenever the task's right wheel qualifies, gated on ring2 *and* start2 both
being fully wildcarded.

**Only the right wheel needed building.** §7.12's middle-wheel collapse derives
its classes by *simulating* the stepping rather than from a formula, so it
already picked the two-notch case up — measured at L=700, a two-notch middle
wheel gives 13.0 start1 classes against 26.0 for a single-notch one. The issue
used to claim 34.8%, which assumed neither half was banked; the increment
against the real baseline was ~20%.

Measured: exactly **2×** with a two-notch wheel in either position, **4×** with
one in both, and no length term — 2× at L=40 and at L=900 alike, where §7.12's
is 7.4× at L=40 and 1.00× past L≈676. **0% under the default `-x 5`**, so it
pays only for Kriegsmarine traffic; note `-4` also defaults to `-x 5` though M4
naval used I–VIII. → `CLAUDE.md` "Two-notch wheels".

**4. Does the middle-wheel collapse's saving convert?** The §7.12 reduction is
3–5× at short lengths and the compute is saved; whether spending it on `-R`
raises recovery is untested. The same question was asked of `--ring-stride` and
answered "a wash". → `archived/IMPROVEMENTS.md` §2.

**5. A `--ring-stride` for the middle wheel — premise measured, not built.**
Striding `ring1` costs 3.1% (K=2) / 5.1% (K=3) of the true `offset1`, roughly
competitive with the right-wheel stride, and the two compose multiplicatively.
**Read the failed attempt first**: striding `ring1` directly measured 2.4×
*worse than not striding*, because thinning `ring1` switches off §7.12's exact
4.86× collapse. The sound axis is to thin the *representatives* §7.12 already
produces — which the measured numbers do **not** cover. →
`archived/IMPROVEMENTS.md` §2.

## Cribs

Detail for all four: `archived/cribs.md` §13.

**6. Crib supply at network scale.** The library covers 83% of held-out
messages, but on a 58-message corpus, and 47 of the 57 hits are 8–11 letters —
seed-only lengths. Whether a real network yields *long* cribs is the question
the whole feature rests on, and no larger corpus is available.

**7. Reject or rank?** The deduction rejects a rotor setting outright. Ranking
would tolerate a slightly-wrong crib — which matters, because garbling is real
(two of five `SIEGFRIED` messages are corrupted) and exact matching cannot see
through it.

**8. The X-separator variant.** Knowing only *where* the word separators sit is
a valid crib and an unusually efficient one: every deduction chains from the
same letter, so 14 separator positions reject as strongly as a 22-letter phrase.
The positions have to come from somewhere. **Measurable offline with
`eval/crib_menu.py` before any C++** — and the only open crib item that attacks
supply, which is the actual constraint.

**9. Menu reuse across alignments.** Shifting a crib by one position changes
every edge, so probably not — but worth checking before assuming the alignment
sweep pays full price each time.

## Measurement gaps

**10. `--tune-phase` at operational lengths (~L300+).** At L=200 and matched
compute it breaks more messages than an exhaustive ring sweep (63/80 vs 51/80,
p = 0.043) but scores lower mean %-correct, because a wrong *offset* is
unrecoverable — it fails less often and worse. The capture radius grows as
`~0.4·L/26`, so at operational lengths those catastrophic failures should get
rarer, and the trade could stop being a trade. Untested. → `CLAUDE.md` "Tuning
the rotor phase"; `archived/PERFORMANCE.md` §7.15.

**11. `--ring-stride` with a hidden plugboard at K=13.** The one cell where
anything moved: 4 losses in 69 trials against 0 in 72 for a paired given-board
control, direction consistent across two seeds but **p ≈ 0.13 — suggestive, not
established**, and only at a stride already outside the recommended K≤3.
Settling it needs ~200 trials (~3–4 h) and buys nothing operational. →
`archived/PERFORMANCE.md` §7.11; `eval/ring_stride_scope_probe.py`.

## Maintainability and packaging

All 🟢, none urgent. → `archived/IMPROVEMENTS.md` §2.

**12. `-Wconversion` (~52 warnings) deliberately deferred.** 43 are `int →
unsigned char` narrowings in the hottest loops; that many casts clutter the hot
path for a low-value nit on deliberately C-style code. A future ratchet, not a
bug.

**13. No `install` target**, and the n-gram files are not declared as build/run
dependencies. Fine for development; add if the tool is packaged.

**14. Single-file distribution.** Embedding the tables was declined once, but
the shipped uint8 tables are ~4× smaller than the float tables that analysis
assumed, so a blob is much cheaper now. Keep `-d` / `$ENIGMA_DATA` as the
override.

**15. The `Scoring:` line can exceed 79 columns** when the `-d` path is long.
Path length is unbounded and cannot be shortened without hiding it; every other
status line is guaranteed to fit.

---

## Before changing search or scoring code

Read `archived/IMPROVEMENTS.md` §4 — **"Measured down — do not re-attempt"** —
and §5, the pitfalls. Between them they record what has already been built and
lost, and the traps that produced confident wrong answers here: benchmark deltas
that were really startup costs, byte-counting width checks, regression tests
that passed against the buggy binary, harnesses whose corpus moved underneath
them, and equality checks that compared everything except the thing that
changed.
