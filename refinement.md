# refinement.md — a derived, staged refinement for `--ring-stride`

**Status: design, not shipped.** The algebra and the failure analysis below are
established (measurements in `archived/PERFORMANCE.md` §7.11 and the probe
results in `eval/results-ring-stride-refine-*.txt`). The *design* has not been
built or A/B'd. Nothing in `enigma.cc` implements it yet.

The goal: replace the refinement's `25 × 130 × 26 = 84 500` enumerated
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
  `S`, and the offset must move with it to keep `b_i` fixed.

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

stage 4 — left wheel (only when D ≢ 0, i.e. the left wheel steps)
    derive the same way with D in place of S and correct o0

emit(v, g1):
    if the caller pinned ring1:  ring1 = that value            # no derivation
    else:                        for δ in V(g1, v): ring1 = g1 − (o1_c + δ)

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
so the sweep must be the whole range, not a window. Stage 4 is inert for short
messages.

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
| shipped | `25 · 130 · 26` = 84 500 | ~25 100 (measured) |
| lock-start2 (measured, 0 loss) | `25 · 130 · 1` = 3 250 | ~970 |
| **this design** | `25 · 26 · 1` = **650** | **~100–175** |

The scored column is smaller because §7.12's middle-wheel collapse applies
inside the refinement exactly as it does to the coarse pass (`search_worker`
consults the same `g_mid_rep_mask`): most of the 26 `start1` values are
decode-equivalent, leaving `⌈L/26⌉+1` representatives — 4 at L=60, 7 at L=150 —
so ~25 × 4 to ~25 × 7 keys are actually scored. **The derived figures in that
column are arithmetic from the class-count formula, not measured**; the 650 and
the shipped ~25 100 are.

**The worst case is exactly `lock-start2`.** `|V| ≤ 5`, so the derived set never
exceeds `25 × 26 × 5` = 3 250 — the same count as the band-with-derived-start2
that measured 0 losses in 600. The design cannot cost more than the fallback it
prunes, which is a useful property to hold on to when reviewing it: the only way
it goes wrong is by pruning too much, never by costing too much.

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
3. **`Δ` ambiguity.** If `Δ` takes several values over the message with no
   dominant one, the mode is arbitrary. Stage 4 handles it by trying each.
4. **Two-notch right wheels (VI–VIII).** More crossings per revolution, so `Δ`
   is more often non-zero. The derivation handles this automatically — it is
   computed, not assumed — which is an advantage over the fixed ±2 band.

## 8. Verification before shipping

Same standard the offset band itself was held to — **equivalence, not net
rate**. A cheaper set is acceptable only if it recovers everything the full one
recovers; a smaller candidate set can also dodge decoys and post a *higher* net
rate while losing real keys (observed: `lock-both` and `lock-off1` both did).

1. Paired equivalence vs the shipped set: K=2/3/5/13, L=60/150, ≥600 trials,
   authentic Wehrmacht excerpts, plugboard given via `-s` — the harness in
   `eval/ring_stride_refine_shape_probe.py` already does this; add the derived
   shape as another column.
2. End-to-end through the binary, not only the offline model, matching
   `eval/ring_stride_window_probe.py`'s methodology.
3. The existing `--ring-stride` regressions in `tests/run_tests.sh`, including
   the K/2-window key and the boundary keys.
4. **A Norway (`-n`) case.** `wheel_task` carries raw wheel numbers while a
   `machine` carries translated ones; every past bug in this area was invisible
   outside `-n` (`archived/PERFORMANCE.md` §7.11).
5. `make bench` is not expected to move — the refinement is outside the hot loop
   — but the run-level key count should drop as §6 predicts, which is a cheap
   sanity check that the derivation is actually being used.

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

## 10. Open decisions and gaps — read before coding

Decisions 1 and 2 below were open and are now **settled** — recorded here with
the reasoning, since both are the kind of choice that looks arbitrary later.
Decision 3 remains open and needs a measurement.

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
3. **Whether a ±1 band survives on top of the derived value — OPEN.** Failure
   mode 2 in §7 — the coarse winner's own `o1` being wrong for scoring reasons
   rather than schedule reasons — is not corrected by the derivation, and is the
   *only* way the pruning in decision 2 can lose a key. A ±1 band would cover it
   at 3× the candidate count (still ~2 000, still trivial). Nobody has run it.

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
