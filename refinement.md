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

The shift is not a small perturbation of the turnover time. `start2` moving by
δ moves the turnover by δ **modulo 26**, so it can carry a turnover across the
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

The `+1` on the offset exactly cancels the `−1` on the step count, `o2` is
identical, and no double step fires so `a_i` matches — the coarse candidate
decodes **58 of 60 characters correctly**. That is why it won the coarse pass.
The offset shift is not damage; it is the compensation that made the
near-solution readable.

At the true `ring2` the schedule is the true one again, so the correct offset
there is 7. No candidate that preserves the coarse offset of 8 can reach it, at
any absolute position.

**General rule.** `o1_coarse − o1_true = −(S_coarse − S_true)`: the number of
turnovers the `ring2` shift moved into or out of the message. It fires when the
right wheel's notch lies within δ of `start2` (~δ/26 of keys) and reaches ±2
when two crossings are possible — a two-notch right wheel (VI–VIII) or a
straddled double step. That bound is where `mid_ring_window = 2` comes from.

## 4. The derivation

Both schedules are computable from keys alone — no knowledge of the truth. For
a candidate `(ring2 = v, start2 = v + o2, start1 = g1)`:

```
S_c    = step schedule of the coarse winner, from (start1_c, start2_c)
S_cand = step schedule of the candidate,     from (g1, v + o2)
Δ(g1, v) = S_c(i) − S_cand(i)                     evaluated over i = 1..L
o1     = o1_c + mode_i Δ(g1, v)
ring1  = g1 − o1                                  (mod 26)
```

`Δ` takes at most 3 distinct values over a message (the enumerated divergence
bound is 2), so the `mode` can be replaced by *trying each distinct value*,
which removes the statistic and makes the step exact rather than typical. The
same derivation applies to the left wheel with `D` in place of `S`; `D ≡ 0`
whenever the left wheel never steps (messages under ~676 characters, absent a
double step), which is why pinning `ring0`/`start0` works today.

## 5. The staged refinement

```
input: coarse winner (reflector, wheel order, r0 g0, r1 g1, r2 g2), skipped set V

stage 1 — right wheel (derived, no search)
    for v in V:  start2(v) = (v + o2_c) mod 26

stage 2 — middle wheel, coarse absolute (25 candidates)
    for v in V:  g1 = g1_c;  Δ = mode Δ(g1, v);  ring1 = g1 − (o1_c + Δ)

stage 3 — middle wheel, absolute sweep (+625 candidates)
    for v in V, for g1 in 0..25 \ {g1_c}:  as stage 2

stage 4 — ambiguous Δ (only where Δ took more than one value)
    retry those candidates with each remaining distinct value of Δ

stage 5 — left wheel (only when D ≢ 0, i.e. the left wheel steps)
    derive Δ0 from D the same way and correct o0

keep the best-scoring candidate; adopt it only if it beats the coarse score
```

Stages 2 and 3 are ordered by prior likelihood, not by necessity: stage 2 covers
the common case (the coarse absolute is still a valid class representative),
stage 3 the case where it is not — measured absolute moves of ±1 up to **±11**,
so the sweep must be all 26, not a window. Stage 5 is inert for short messages.

An early exit after stage 2 on "score beats the coarse result" is tempting and
is **not** justified by anything measured — a later stage can still be better.
Treat it as a separate experiment, not part of the design.

## 6. Cost

| scheme | candidates | note |
|---|--:|---|
| shipped | `25 · 130 · 26` = 84 500 | enumerated band × full start2 |
| lock-start2 (measured, 0 loss) | `25 · 130 · 1` = 3 250 | derived start2 |
| **this design** | `25 · 26 · 1` = **650** | + a few for ambiguous Δ |

Against a full run: the shipped refinement is ~2% of the keys analysed with
`ring1`/`start1` open and ~35% in the single-task corner where the flag is
already a documented net loss. At 650 it is ~0.015% and ~0.3%. **This retires
the refinement-width question**: no window cap, constant or `K`-dependent, has
anything left to save (see `archived/PERFORMANCE.md` §7.11 for the width
measurements this replaces).

## 7. Where it can still fail

1. **The coarse winner is not a near-solution.** Out of scope by design (§1);
   no refinement shape recovers it, and the shipped one only sometimes does by
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
5. `make bench` is not expected to move — the refinement is outside the hot
   loop — but the run-level key count should drop as §6 predicts, which is a
   cheap sanity check that the derivation is actually being used.

## 9. Implementation notes

- **`(ring2, start2)` becomes a diagonal.** `search_range` holds rectangles
  only, so the 25 pairs cannot be one range; they become 25 pinned sub-searches,
  each with its own `start2`. This is the same shape as the existing band, which
  already runs one sub-search per pinned `(ring1, start1)` pair.
- **Each sub-search is now a single key.** 650 one-key `search_worker` calls
  would be mostly call overhead; a direct decode-and-score path is likely the
  right implementation, with `search_worker` reserved for the plugboard case.
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
