# refinement.md — a derived, staged refinement for `--ring-stride`

**Status: SHIPPED, verified, and its measurement gaps closed.** `enigma.cc`
derives the refinement's offsets; the enumerated `±mid_ring_window` band and the
constant it used are gone. Measured equivalent to the enumerated refinement and
~130× cheaper.

Read **§11** for what the build turned up (including why the equivalence
question had the wrong baseline), **§12** for the 1 200-trial machine-variant
matrix, and **§8** for the case matrix — every check it lists now exists in
`tests/run_tests.sh`. The three conditions the original runs did not cover —
K≥8, a hidden plugboard, and messages long enough for the left wheel to step —
have since been measured too (`eval/ring_stride_scope_probe.py`); §10 records
the results. Sections written before the build are kept as they were, with the
correction noted at the point it applies.

The goal was to replace the refinement's `25 × 130 × 26 = 84 500` enumerated
candidates with `25 × 26 = 650` **derived** ones, at equal recovery.

---

## 1. What the refinement is for

`--ring-stride K` scans only `ring2 ∈ {0, K, 2K, …}`. When it works, the coarse
winner is *the truth with the right wheel's turnover mistimed* — a near-solution
whose plaintext is right except where the mistiming bites. The refinement's job
is to convert that near-solution into the exact key by testing the 25 skipped
`ring2` values.

It is explicitly **not** the job of the refinement to rescue a coarse winner
that landed in an unrelated basin. That is a failure of the coarse pass, and
searching for it turns a targeted correction into a second search.

## 2. The algebra the design rests on

From `setup_mapping()`, with `S(i)` the number of middle-wheel steps and `D(i)`
the number of left-wheel steps in the first `i` characters, the three offsets
the substitution consumes at character `i` are

```
a_i = o0 + D(i)          b_i = o1 + S(i)          c_i = o2 + i
```

where `o_w = start_w − ring_w`. Two facts follow, and they are the whole design:

- **`c_i` has no schedule term.** The right wheel's contribution is a function
  of `o2` alone, so a candidate must carry the coarse winner's `o2` exactly:
  `start2 = ring2 + o2`. Measured: 0 losses in 600 trials (`lock-start2`).
- **`b_i` and `a_i` do.** `S` and `D` are driven by the *absolute* positions
  (the notches read `g1` and `g2`, never an offset), so changing `ring2` changes
  `S`, and the offset must move with it to keep `b_i` fixed. This applies to the
  **left** wheel as well as the middle one: `D` counts double steps, a `ring2`
  shift moves those too, and `o0` then has to move with `D` exactly as `o1`
  moves with `S`. The difference is only one of frequency — see §5 and §7.3.

## 3. Why the offset moves — the mechanism, with a worked case

The shift is not a small perturbation of the turnover time. `start2` moving by δ
moves the turnover by δ **modulo 26**, so it can carry a turnover across the
start of the message, changing the step count for the whole message rather than
for a δ-length window.

A measured case (L=60, K=2, wheels V-II-IV; middle notch E, right notch J):

```
truth   ring1 R start1 Y → o1 = 7 | ring2 H start2 J → o2 = 2
coarse  ring1 S start1 A → o1 = 8 | ring2 I start2 K → o2 = 2

middle-wheel step positions   truth : [1, 27, 53]
                              coarse: [26, 52]
e(i) = S_coarse(i) − S_true(i) = −1 on 58 of 60 positions, 0 at i = 26, 52
```

The truth's `start2` is **J**, the right wheel's notch, so its middle wheel
steps at character 1. The coarse `start2` is **K**, one past, so its first step
is a revolution later. Then

```
b_i(coarse) = 8 + S*(i) − 1 = 7 + S*(i) = b_i(truth)   on 58 of 60 positions
```

Side by side, the two machines are in lockstep — the coarse wheel's ring sits
one letter further along **and** the wheel is one step behind, all the way
through:

```
 i | truth: window ring  b_i | coarse: window ring  b_i |
26 |          Z     R      8 |           B     S      9 | DIFFERS
27 |          A     R      9 |           B     S      9 | same
51 |          A     R      9 |           B     S      9 | same
52 |          A     R      9 |           C     S     10 | DIFFERS
53 |          B     R     10 |           C     S     10 | same
```

A ring setting does not move during a message; a wheel does. So `(start1,
ring1)` is only a coordinate — what the cipher uses is the running alignment
`b_i`, and the same `b_i` can be written in more than one coordinate system when
the wheel arrives on a different schedule. The two machines part company only at
the characters where one has stepped and the other has not yet caught up (i=26,
52), resynchronising immediately after. Two clocks, one set a minute fast and
started a minute late: they agree all day bar the minute one has ticked and the
other has not.

The `+1` on the offset exactly cancels the `−1` on the step count, `o2` is
identical, and no double step fires so `a_i` matches — the coarse candidate
decodes **58 of 60 characters correctly**. That is why it won the coarse pass.
The offset shift is not damage; it is the compensation that made the
near-solution readable.

Note also that the truth's key offset is 7 while `b_1` is already 8: stepping
precedes the first character, and the truth's `start2` **is** the right wheel's
notch, so the middle wheel steps immediately. The key offset is an alignment the
machine never actually uses — which is why a key offset of 8 is not "wrong"
operationally. Both keys put the wheel at alignment 8 for character 1.

At the true `ring2` the schedule is the true one again, so the correct offset
there is 7. No candidate that preserves the coarse offset of 8 can reach it, at
any absolute position.

**General rule.** `o1_coarse − o1_true = −(S_coarse − S_true)`: the number of
turnovers the `ring2` shift moved into or out of the message. It fires when the
right wheel's notch lies within δ of `start2`, roughly δ/26 of keys.

**The magnitude is bounded by 1, not merely small.** The middle wheel's
turnovers form an arithmetic progression of spacing 26; the candidate's is the
same lattice shifted by δ. Two interleaved lattices of equal spacing alternate —
between consecutive points of one lies exactly one point of the other — so for
every prefix

```
|S_coarse(i) − S_true(i)| ≤ 1        single-notch right wheel, no double step
```

whatever δ and whatever the message length. All 15 measured misses show exactly
`|Δ| = 1`, as they must. Two mechanisms lift it to 2, and only to 2:

- **Two-notch right wheels (VI–VIII).** The turnover set is a union of two such
  lattices, each able to gain or lose a point. Note the probe draws wheels from
  I–V, so its uniform ±1 is partly a property of the trial design; this
  mechanism is reachable at `-x 6` and above.
- **A straddled double step.** The middle wheel's own notch firing inside the
  message for one candidate and not the other adds a step outside the carry
  lattice. Rare at short lengths — the middle wheel reaches its own notch about
  once per 676 characters.

§7.11's enumeration (every rotor pair × 26 start1 × 26 start2 × every shift, at
L=600) found the divergence never exceeds 2, which is where `mid_ring_window =
2` comes from. The derivation below does not depend on the bound — it computes Δ
— so this matters only if a fixed band is preferred: ±1 suffices for I–V, ±2 is
the honest general choice.

## 4. The derivation

Both schedules are computable from keys alone — no knowledge of the truth. For a
candidate `(ring2 = v, start2 = v + o2, start1 = g1)`:

```
S_c    = step schedule of the coarse winner, from (start1_c, start2_c)
S_cand = step schedule of the candidate,     from (g1, v + o2)
Δ(i)   = S_c(i) − S_cand(i)                       for i = 1..L
V      = { distinct values Δ takes over the message }
o1     = o1_c + δ        for each δ ∈ V
ring1  = g1 − o1                                  (mod 26)
```

**Emit one candidate per element of `V`; do not reduce `Δ` to a mode.** A mode
is a guess that can be wrong on a short message where the schedules alternate
evenly, and the thing it guesses at is cheap to enumerate: `|Δ(i)| ≤ 2` bounds
`V` to at most 5 values, and in practice it is 1 or 2. Enumerating cannot be
wrong; a statistic can.

**The resulting set is a subset of a set already measured to lose nothing.**
`lock-start2` is `25 × (26 start1 × 5 offsets) × 1` = 3 250 and lost 0 of 600.
The derived set uses the same 26 `start1` values and the offsets `o1_c + δ` for
`δ ∈ V ⊆ [−2, +2]` — so it is exactly `lock-start2` **pruned to the offsets `Δ`
actually takes**, and its worst case (`|V| = 5`) *is* `lock-start2`. That makes
the verification question precise: not "is the derived set safe?" but "does
pruning the 5 offsets down to `V` lose anything?" — and by the algebra it
cannot, except in failure mode 2 of §7, where the coarse winner's own `o1` is
wrong for reasons the schedule does not explain.

The same derivation applies to the left wheel with `D` in place of `S`; `D ≡ 0`
whenever the left wheel never steps (messages under ~676 characters, absent a
double step), which is why pinning `ring0`/`start0` works today.

## 5. The staged refinement

```
input: coarse winner (reflector, wheel order, r0 g0, r1 g1, r2 g2)
       R2 = the 25 skipped ring2 values
       G1 = the caller's start1 range   (0..25 wildcarded, else the pinned value)

stage 1 — right wheel: derived, never searched
    for v in R2:  start2(v) = (v + o2_c) mod 26

stage 2 — middle wheel, coarse absolute first
    for v in R2:  emit(v, g1_c)

stage 3 — middle wheel, the rest of the sweep
    for v in R2, for g1 in G1 \ {g1_c}:  emit(v, g1)

emit(v, g1):
    if the caller pinned ring1:  ring1 = that value            # no derivation
    else:                        for δ in V(g1, v): ring1 = g1 − (o1_c + δ)
    for δ0 in V0(g1, v):         ring0 = g0_c − (o0_c + δ0)    # left wheel, same rule

keep the best-scoring candidate; adopt it only if it beats the coarse score
```

**`ring1` is derived only when the caller left it wildcarded.** With `ring1`
pinned — which includes the tool's own default `-r AA.` — the offset is not
free: each `start1` in the sweep already carries a determined offset `start1 −
ring1`, the sweep covers every one of them, and deriving would override a
constraint the caller stated. With `start1` pinned instead, `G1` is a single
value and the derivation still applies to `ring1`. This mirrors the `else`
branch the enumerated band already has, and it is why the sweep is written over
the caller's range rather than over `0..25`.

Stages 2 and 3 are ordered by prior likelihood, not necessity: stage 2 covers
the common case (the coarse absolute is still a valid class representative),
stage 3 the case where it is not — measured absolute moves of ±1 up to **±11**,
so the sweep must be the whole range, not a window.

**The left wheel gets the same derivation, ungated.** `a_i = o0 + D(i)`, and `D`
counts double steps — the left wheel advances exactly when the middle wheel sits
on its own notch. A `ring2` shift moves the middle schedule, so it moves the
double steps too, and one that sits near either end of the message can be
carried in or out of it, changing `D` for the whole message and therefore `o0`.
`V0` is computed from the same schedule walk that produces `V` (both `S` and `D`
fall out of one pass over `(start1, start2)`), so this costs nothing extra and
**must not be gated on "the left wheel steps"**: the derivation self-gates,
returning `V0 = {0}` whenever the two schedules agree, which is the common case.
An explicit gate is one more condition to get wrong for no saving.

Note the left wheel needs no absolute sweep, only the offset correction: nothing
steps it but the middle notch and it has no notch of its own, so `D` depends
only on `(start1, start2)` — the same inputs as `S`.

An early exit after stage 2 on "score beats the coarse result" is tempting and
is **not** justified by anything measured — a later stage can still be better.
Treat it as a separate experiment, not part of the design.

## 6. Cost

Per invocation, the derived set is one axis wide on everything but two:

| axis | values | how |
|---|--:|---|
| `ring2` | 25 | the skipped values, all of them |
| `start2` | 1 | derived, `ring2 + o2_c` |
| `start1` | 26 | swept — the winner's is a class representative |
| `ring1` | `|V|` | derived, `start1 − (o1_c + δ)`, one per `δ ∈ V` |
| | **650 × `|V|`** | `|V|` = 1 or 2 in practice, ≤ 5 |

| scheme | enumerated | scored |
|---|--:|--:|
| enumerated (was) | `25 · 130 · 26` = 84 500 | ~25 100 |
| **derived (is)** | `25 · 26 · \|V\|` | **650–1 300** |

Measured `Analysed N` on a 99-character message, `-u B -w 123`, before and
after:

| keyspace | K | no stride | enumerated | derived |
|---|--:|--:|--:|--:|
| `-r AA. -g ...` | 2 | 456 976 | 245 388 | **229 231** |
| `-r AA. -g ...` | 3 | 456 976 | 175 084 | **158 927** |
| `-r A.. -g ...` | 3 | 2 618 824 | 925 141 | **907 170** |
| `-r AA. -g AA.` | 2 | 676 | 988 | **363** |
| `-r A.. -g A..` | 2 | 100 724 | 68 987 | **50 787** |

The refinement itself went from a flat 16 900 to 650–1 300 — still constant in
`K`, still independent of the coarse pass, but two orders of magnitude smaller.

Against a full run: the shipped refinement is ~2% of the keys analysed with
`ring1`/`start1` open and ~35% in the single-task corner where the flag is
already a documented net loss. At 650 it is ~0.015% and ~0.3%. **This retires
the refinement-width question**: no window cap, constant or `K`-dependent, has
anything left to save (see `archived/PERFORMANCE.md` §7.11 for the width
measurements this replaces).

## 7. Where it can still fail

1. **The coarse winner is not a near-solution.** Out of scope by design (§1); no
   refinement shape recovers it, and the shipped one only sometimes does by
   accident of breadth.
2. **The coarse winner's own `o1` is wrong for reasons other than the schedule**
   — the argmax on a partly-garbled decode need not be exactly the truth's
   middle setting. The derivation corrects the schedule term, not this. A ±1
   band on top of the derived value would cover it at 3× the cost; whether it is
   needed is an open measurement.
3. **The left-wheel pin, in the code as it stands today.** `enigma.cc` pins
   `ring0`/`start0` and justifies it as "exact and ring2-independent (§7.10's
   unconditional offset collapse holds regardless of what ring2 is)". §7.10 is
   about the *degeneracy* — shifting `ring0` and `start0` together is decode-
   identical — which is not the same claim as *pinning `o0` across a `ring2`
   change*. That is exact only while `D_coarse ≡ D_candidate`, and a straddled
   double step breaks it exactly as it breaks the middle wheel. The chance of a
   double step occurring at all is roughly `L/676` — ~9% at L=60, ~22% at L=150,
   ~44% at L=300 — and only a fraction of those are straddled, so this is rare
   rather than negligible, and it grows with message length.

   **The measurements cannot see it.** Every arm in the shape comparison pins
   `ring0`/`start0`, including the `shipped` baseline, so a key lost this way is
   lost by *all* of them; the "lost" metric only counts keys the shipped set
   recovered. Detecting it needs an oracle arm that sweeps `o0` over 26, or a
   check of the truth's `o0` against the coarse winner's in the shipped-failure
   cases. The derived design fixes it as a side effect (`V0` above); the shipped
   code does not.
4. **Two-notch right wheels (VI–VIII).** More crossings per revolution, so `Δ`
   is more often non-zero. The derivation handles this automatically — it is
   computed, not assumed — which is an advantage over the fixed ±2 band.

## 8. Verification before shipping

> **DONE — every case below now exists, and the ✅ marks are the pre-build
> state.** Cases 3–6, 10, 11, 12 and 15 were the new ones; all are checks in
> `tests/run_tests.sh` today: M4 under `--ring-stride`, a two-notch right wheel
> at K=2/3/13, a key that both turns over at character 1 and double-steps (so
> the left-wheel derivation is reached at all), the four-way caller-pin matrix,
> and an assertion that the derivation *responds to the schedule* rather than
> emitting a fixed set — the refinement must be strictly larger where the left
> wheel steps than where it does not, which a banded or pinned build could not
> produce. Run A and run B were both done; §11 records what they turned up and
> §12 the 1 200-trial variant matrix. The paragraph below beginning "Cases 3, 4,
> 5 and 6 do not exist" is therefore **historical** — it is what prompted the
> work, kept because it names why recovery alone is a weak assertion here.

Same standard the offset band itself was held to — **equivalence, not net
rate**. A cheaper set is acceptable only if it recovers everything the full one
recovers; a smaller candidate set can also dodge decoys and post a *higher* net
rate while losing real keys (observed: `lock-both` and `lock-off1` both did).

Two statistical runs, then a case matrix.

**A. Paired equivalence, offline.** Add the derived shape as another column in
`eval/ring_stride_refine_shape_probe.py`: K=2/3/5/13, L=60/150, ≥600 trials,
authentic Wehrmacht excerpts. The question is narrow (§4): does pruning the five
offsets to `V` lose anything the full band finds?

**B. End-to-end through the binary**, matching the methodology of
`eval/ring_stride_window_probe.py` — the offline model pins `ring0`/`start0` at
the truth and omits `-c`, so it cannot see either of those interactions.

**C. The case matrix.** Every entry is a `tests/run_tests.sh` check; §12 says
why the design covers the machine variants among them by construction. Size each
to the property under test — pin the reflector and wheel order, wildcard only
what the case needs — because the sanitizer job runs the whole suite at ~10×
(CLAUDE.md's rules for adding checks). ✅ marks what exists today and must keep
passing; the rest are new.

1. ✅ **Standard, K=2/3/5/14/26.** `-u B -w 123 -r AAZ -g XKP`. The ordinary
   path, plus K=26's single coarse anchor.
2. ✅ **Norway `-n`.** The 439-letter Norwegian message, `-n -w 352 -r "L.." -g
   "O.."`. `wheel_task` carries **raw** wheel numbers while a `machine` carries
   translated ones; a double translation is invisible in every other mode, and
   the entire stride matrix once passed over a broken `-n` path.
3. **M4 `-4`.** `-4 -u b -w B123 -r AAA. -g A...`. The refinement reuses
   `tasks[cur_wo]`, which carries the Greek wheel and its offset; a
   reconstruction that drops them is invisible outside `-4`.
4. **Two-notch right wheel.** `-x 8` with VI/VII/VIII rightmost (notches M and
   Z), e.g. `-w 126`. The turnover set becomes **two** lattices, so `|Δ|` can
   reach 2 and §7.12's class count changes from `⌈L/26⌉+1` to `⌈L/13⌉+1`. The
   probe draws I–V only, so nothing measured so far exercises this at all.
5. **Double step / left wheel steps.** Middle wheel II (notch E) with `start1`
   two steps short of E, and `start2` on the right wheel's notch so the first
   turnover lands at character 1. Makes `D ≢ 0`, exercising `V0`; without it the
   left-wheel derivation is dead code the suite never runs.
6. **Turnover at character 1.** `start2` = the right wheel's notch letter (the
   §3 worked case). The whole-message step-count difference — the mechanism the
   design exists for, and where a `Δ`-selection bug shows first.
7. ✅ **ring2 wrap at A/Z.** The measured keys `B 451 AAZ VKZ`, `B 351 AAZ NLV`,
   `C 324 AAZ JEY`. ring2 is circular; the value list must carry the wrapped
   set.
8. ✅ **Coarse winner outside `⌊K/2⌋`.** `-u A -w 123 -r DLT -g ACG` at K=3.
   Every skipped ring2 must be covered, not a window.
9. ✅ **Plugboard untouched without `-c`.** `-u A -w 145 -r FFR -g RTB` with a
   10-pair `-s` board. The `--polish` guard; the derived path must not re-open
   it.
10. **Caller-pin matrix.** All four combinations of `-r AA.` / `-r A..` against
    `start1` pinned / wildcarded. Decision 1 of §10 — derive `ring1` only when
    it is wildcarded. `-r AA.` is the tool's own default, so getting this wrong
    breaks the common case.
11. **`|V| > 1`.** Any key where `Δ` takes two values, which is the normal case
    at short L. Checks that the implementation emits one candidate per element
    instead of collapsing to a mode.
12. **Derived == shipped.** Same ciphertext through both builds, byte-comparing
    the plaintext across the cases above — the equivalence claim per case rather
    than in aggregate.
13. ✅ **`-T` independence.** `-T 1` against `-T 4` on cases 1–9. The refinement
    merges under one `best_result`; determinism must survive the restructuring.
14. ✅ **Key counts.** `Analysed N` flat over K=13..25 and falling at K=26 — the
    coarse set is still `{v : v ≡ 0 mod K}`.
15. **Key count drops.** `Analysed N` for the derived build against the shipped
    one, confirming the derivation is used rather than silently bypassed.

**Cases 3, 4, 5 and 6 do not exist in any form today** — no M4 `--ring-stride`
case, no two-notch right wheel anywhere in the stride tests, and nothing that
deliberately makes the left wheel step. Cases 5 and 6 also want an assertion on
the *derivation* rather than only on recovery: that `V0 ≠ {0}` fires at least
once (case 5) and that `V ≠ {0}` fires (case 6). Recovery alone can pass with
the derivation disabled, because the coarse winner is often already right.

**Each new regression must fail against the pre-change binary.** The suite has
been bitten twice by tests that passed on the buggy build — the first `--polish`
guard test converged immediately on an easy board, and the whole stride matrix
passed over a Norway path that was broken. Verify the failure before committing
the check.

`make bench` is not expected to move; the refinement is outside the hot loop.

## 9. Implementation notes

- **`(ring2, start2)` becomes a diagonal.** `search_range` holds rectangles
  only, so the 25 pairs cannot be one range; they become 25 pinned sub-searches,
  each with its own `start2`. This is the same shape as the existing band, which
  already runs one sub-search per pinned `(ring1, start1)` pair.
- **Each sub-search is now a single key**, so there are 650 of them instead of
  today's 130. Keep `search_worker` anyway rather than adding a direct
  decode-and-score path: under `-c` the refinement passes `restarts_par`, so
  each candidate carries a full plugboard climb, and re-implementing that
  outside `search_worker` would change behaviour as well as duplicate it. Per
  call the parallelism is thinner (one key × restarts instead of 650 keys ×
  restarts), but under `-c` the restarts still spread across threads, and
  without `-c` the whole refinement is 650 decodes — parallelism is moot at that
  size.
- **Reuse `tasks[cur_wo]` verbatim** for the wheel order and reflector; never
  rebuild a `wheel_task` from a `machine`'s already-translated fields.
- **Snapshot every pinned value before the first sub-search.** The plain-scan
  path leaves `m.ringstellung` / `m.grundstellung` in a stepped state (the
  documented lazy restore), so re-reading them between sub-searches picks up
  stale values.
- The schedule needed for `Δ` is exactly what `setup_mapping()` already walks;
  the derivation needs the step *counts*, so it can be lifted from a single pass
  over the message per `(start1, start2)` pair — 26 pairs per candidate `ring2`,
  computed once and reused.

## 10. Decisions and gaps — all settled

**All three decisions below are settled and the gaps are closed**; the section
is kept because each choice is the kind that looks arbitrary later without the
reasoning that produced it. (An earlier version of this lead-in said decision 3
was still open, which item 3 itself already contradicted.)

1. **Caller-pinned `ring1` / `start1` — SETTLED.** Derive `ring1` only when the
   caller left it wildcarded; when it is pinned (including under the tool's own
   default `-r AA.`), use the caller's value and sweep `start1` over its range,
   which already covers every offset that pin permits. When `start1` is pinned
   instead, the sweep is one value and the derivation still applies. See §5.
2. **How `Δ` is selected — SETTLED.** Enumerate `V`, the distinct values `Δ`
   takes over the message, and emit one candidate per element. No mode, no
   statistic, and no separate "ambiguous" stage. `|V| ≤ 5` and is 1 or 2 in
   practice, so the cost is bounded by `lock-start2`, which the derived set is a
   pruning of. See §4.
3. **Whether a ±1 band survives on top of the derived value — SETTLED, no.**
   Built as `widen_deltas()` behind `ENIGMA_REFINE_BAND` and measured over 360
   paired end-to-end trials: it changed **not one recovery**. The reason is §11
   — the keys it was meant to rescue turned out not to be search failures at
   all. Shipped at 0; the knob remains for measurement.

Two accounting details that are easy to get wrong and hard to notice:

- **`extra_keys_analysed` must mirror the §7.12 collapse the same way it does
  today** — count what is scored, not what is enumerated, and consult the mask
  with the **candidate's** `start2`, not the coarse winner's. `search_worker`
  gets this right automatically once `start2` is pinned per sub-search; the
  accounting beside it is hand-written and will not.
- **Progress plumbing and determinism are unchanged only if `search_worker` is
  kept.** The `g_progress` swap, the `rbest.shown` high-water mark, the header
  suppression and the lowest-work-index tie-break all already exist; a new
  scoring path would have to reproduce every one of them.

Finally, the scope of what has actually been measured, which is narrower than
the design's claims:

- `lock-start2`'s 0-losses-in-600 covers **K=2/3/5, L=60/150, wheels I–V, no
  `-c`, offline model** (`-a`, ring0/start0 pinned at the truth). It does not
  cover K≥8, two-notch right wheels, long messages where the left wheel steps,
  or a hidden plugboard.
- The derived design itself has **never been run**. Its 650-candidate figure is
  arithmetic; its equivalence with the shipped set is a prediction from the
  algebra, not a result.

> **Both bullets are superseded — this section is the pre-implementation state,
> kept for the reasoning that led to the design.** The derived set has since
> shipped and been measured end-to-end (§11) and across machine variants (§12):
> two-notch right wheels, M4 and the derived shape itself are all covered now.
>
> **And the three remaining gaps have since been measured too**
> (`eval/ring_stride_scope_probe.py`; `archived/PERFORMANCE.md` §7.11). **K≥8**:
> K=8/10/13 cost the same ~1% stride-specific miss as the documented K=2/3,
> which closes a real hole: the flat-cost-curve claim above K=13 had been
> measured on *keys analysed*, never on accuracy. **Left-wheel stepping**:
> clean, 0.0% at every stride through K=26 on random keys at L=600, with the
> wheel verified to have stepped in 54 of 60 trials. **Hidden plugboard**: the
> one place anything moved — 4 losses in 69 trials at K=13 against 0 in 72 for a
> paired given-board control, consistent in direction across two seeds but
> **p ≈ 0.13, suggestive rather than established**, and only at a stride already
> outside the recommended K≤3.

## 11. What shipping it turned up

**The equivalence question had the wrong baseline, and that mattered.** Compared
directly against the enumerated refinement, the derived one looked like it lost
~1.4% of keys — 5 in 360 paired end-to-end trials. Dumping them showed every one
was a key where the **exhaustive `K=1` search also fails**: the truth is not the
top-scoring rotor key, and the enumerated refinement "won" only by never
reaching the higher-scoring decoy that a complete search finds. The derived
refinement agrees with the exhaustive search on those keys, which is the correct
behaviour, not a regression.

So the metric to hold a refinement to is the **stride-specific miss** — the
exhaustive search recovers it and the strided run does not — and on that metric
the derived refinement is clean where the enumerated one is not quite:

| L | K | trials `K=1` recovers | enumerated misses | derived misses |
|---:|---:|---:|---:|---:|
| 60 | 2 | 48 | 0 | 0 |
| 60 | 3 | 48 | 0 | 0 |
| 60 | 5 | 48 | 0 | 0 |
| 150 | 2 | 55 | 0 | 0 |
| 150 | 3 | 55 | 0 | 0 |
| 150 | 5 | 55 | **1** | 0 |
| | | **309** | **1** | **0** |

One miss in 309 is a single trial, so read it as "no worse", not as evidence the
derivation recovers more; what it does rule out is the derived set being the
weaker of the two, which is what the direct binary-to-binary comparison appeared
to show.

This also settles §10's decision 3 in the negative: the ±1 band was built for a
failure the derivation cannot correct, and there were no such failures to
correct — every apparent one was a scoring failure. Shipped at 0.

**The hot path is unaffected, measured on the target that matters.** `make bench
LONG=1 BASE=origin/dev` on an idle Apple M1 with clang — the configuration
CLAUDE.md flags as the layout-sensitive one:

| run | search quick | hillclimb quick | search long | hillclimb long |
|---|--:|--:|--:|--:|
| 1 | −0.1% | +0.2% | −0.1% | −0.7% |
| 2 | −0.6% | +1.0% | −0.5% | −0.2% |

Eight deltas across two independent runs, all within ±1.0%. The repeat is not
redundant: run-to-run spread on the *same* comparison is a noise-floor estimate,
and at ~±1% every delta sits inside it. That is worth more than it looks: the
concern was never the scoring loop (nothing was added to it) but the ~70 lines
added to a translation unit with a documented history of ±20–60% layout swings
under clang on Apple silicon. Two earlier attempts to measure this on a shared
Linux container were worthless — its base-vs-base control, identical source on
both sides, reported **+20.8%** on `search` — so quiet hardware was the only way
to get an answer.

**Two implementation traps, both caught by tooling rather than by reading.**

- **`mod26()` adds a single alphabet**, so it is correct only for `x ≥ −26`. The
  derived offset subtracts a step-count difference from a position, and a
  candidate `start1` far from the coarse winner's can differ by more than one
  step, so the result went below −26 and the modulo returned a negative index.
  UBSan caught it as an out-of-bounds `subst_array` index; ASan then took the
  segfault. `mod26_full()` exists for this. **The delta is not bounded by 2
  here** — that bound applies to a ring2/start2 *shift*, and the sweep compares
  schedules from arbitrary `start1` values.
- **`mid_ring_window` became unused** and only clang's `-Werror` said so. The
  suite and g++ were both silent.

**A user-visible behaviour change fell out of it.** The "`--ring-stride` is not
paying for itself" warning is **removed**, because the case it warned about no
longer exists: the keyspace that used to cost 1.46× more than not striding (`-r
A.. -g A..` at K=2) is now a 1.98× win, and the warning is provably unreachable
— it needs `50 > tasks · rc0 · rc1 · gc0 · gc2 · (26 − rc2)`, while validation
forces `gc2 = 26` and `rc2 ≤ 13`, so the right-hand side is at least 338.
`tests/run_tests.sh` now guards the inversion instead of the warning.

## 12. Machine variants and stepping — why the design covers them

The test matrix in §8 lists M4, two-notch wheels and the stepping phenomena as
cases to check. This is why each is covered by construction rather than by luck,
which is the first thing a reviewer will want to know.

**M4**, three ways. The Greek wheel is *static*: it folds into the effective
reflector at precompute and never steps, so it contributes nothing to `S` or `D`
and the engine remains the 3-stepping-rotor machine the derivation addresses.
The refinement reuses `tasks[cur_wo]` **verbatim**, so the Greek wheel and its
offset ride along untouched — the same reuse that fixes the Norway
double-translation trap. And pinning the Greek offset to the coarse winner's is
sound for exactly the reason pinning `o2` is: it enters the substitution at
*every* position through the effective reflector, so a wrong one garbles
everything and the winner could not have been a near-solution.

**Two-notch right wheels (VI–VIII)** need no special case, because nothing in
the derivation assumes one notch. `step_counts()` reads `notch[w][g]`, a boolean
table with **both** M and Z set for those wheels, and `step_deltas()` collects
whatever distinct differences occur with no bound baked in. This is *stronger*
than what it replaced: the ±2 band's bound came from an enumeration that had to
argue two-notch wheels could not exceed it, while the derivation measures. The
candidate count tracks the wheel — 743 for `-w 123`, 837 for `-w 126`, 1298 for
`-w 168` on the same message — so the delta sets really do differ, and all three
recover exactly.

**The stepping phenomena** are covered because `step_counts()` mirrors
`setup_mapping()`'s stepping line for line, double-step branch included: the
middle wheel sitting on its own notch advances both itself and the left wheel.
That branch is *why* `D` exists and why the left-wheel offset is derived
ungated. A turnover at character 1 works because stepping precedes the first
character in both functions.

**Norway** is covered by the same `tasks[cur_wo]` reuse, plus `step_counts()`
taking TRANSLATED rotor indices (`machine::walzenlage`) because `notch[]` is
indexed that way, while the §7.12 mask beside it takes RAW ones. Mixing those up
is the trap that made the refinement search the wrong rotors under `-n`.

**What the refinement deliberately does NOT re-open:** the wheel order, the
reflector, and M4's Greek wheel. Same argument for all three — each garbles the
whole message when wrong, so a coarse winner carrying one of them wrong is not a
near-solution at all, and §1 puts that outside the refinement's job.

**Designed-for is now also measured-at-scale.** The 309-trial equivalence run
(§11) was wheels I–V on the standard machine, so the two claims above were
carried by the design argument and by *recovery* checks in the suite. The shape
probe's wheel pool now runs to I–VIII (`--wheels`, default 8) and it has an M4
arm (`--m4`, thin reflector plus a Greek wheel at a random offset), which closes
that: **1200 paired trials, `lost:derived` = 0 in all twelve cells** — standard
and M4, `K` = 2/3/5, L = 60/110, 100 trials each — with the derived set matching
the enumeration's recovery percentage for percentage everywhere. 480 of those
trials drew a two-notch wheel into the *right* position, the one that drives the
middle wheel. Full output: `eval/results-ring-stride-refine-variants.txt`.

Two things make that zero worth reading. First, the run has resolution: the
cheaper shapes below `derived` lose 1–9 trials per cell (49 in total), so the
probe does detect losses when they exist. Second, the run scores the shape that
actually **ships**. It previously scored only the shapes that *led to* the
derivation, and the equivalence claim leaned on the derived set being a subset
of the measured-clean `lock-start2` — which bounds it the wrong way, since a
subset can lose exactly where its superset does not. `derived` is now a shape in
its own right, computing `ring1` from the step-count drift the way `enigma.cc`
does.

The two arms differ in difficulty the way their construction predicts and not
otherwise. M4's coarse pass is better at L=110 (68/51/36% vs 62/35/30% for
`K`=2/3/5) and its refined recovery higher (91% vs 85%) — that is the
substitution being different. The stepping-sensitive quantity, whether the
derivation lands on the right `ring1`, is identical across the two.

The probes' shared machine model grew to match, and `selftest()` now anchors
**each** variant against `./enigma` separately — wheels I–V, a two-notch case,
an M4 case — plus the documented `b`+Beta@A ≡ B and `c`+Gamma@A ≡ C
equivalences. A model that quietly disagreed with the binary on the new wirings
or the `MZ` notches would otherwise produce a confident and meaningless zero.
