#!/usr/bin/env python3
"""Can the refinement's 25 x 130 x 26 be reduced by fixing offsets?

The shipped refinement enumerates, per invocation:

    25 ring2 values  x  130 (ring1, start1) pairs  x  26 start2      = 84 500

Only offsets enter each wheel's substitution, which suggests both 26s are
redundant and the whole thing could be 25 x 5 = 125. The two reductions are not
equally safe, because an absolute position is not always redundant -- it is what
the NOTCH reads:

  * start2 (the 26): only (start2 - ring2) enters the right wheel's
    substitution, and its absolute value sets the turnover TIMING, which is the
    very thing a candidate ring2 is varying. Locking start2 = ring2 + offset2 of
    the coarse winner is the "offset-locked" scheme below.
  * start1 (the 26 inside 130 = 26 start1 x 5 offsets): absolute start1 gates
    the middle wheel's OWN notch, hence the double step. Pinning it to the coarse
    winner's value assumes the winner's start1 is the true one -- and under
    archived/PERFORMANCE.md 7.12 the reported start1 is only a class
    representative. The reproducible case in 7.11 is exactly this shape (true
    ring1/start1 Q/D, coarse winner R/E -- the SAME offset, both absolutes +1),
    and pinning lost it.

So this probe scores five candidate sets against the same coarse winner, from the
shipped one down to pinning every offset to the coarse solution:

    shipped      25 x 130 x 26 = 84 500
    lock-start2  25 x 130 x  1 =  3 250    (start2 co-varies with ring2)
    lock-both    25 x   5 x  1 =    125    (also pins absolute start1)
    lock-off1    25 x  26 x  1 =    650    (middle OFFSET pinned, start1 open)
    lock-all     25 x   1 x  1 =     25    (every offset pinned to the coarse one)

"recovered" = the set's best-scoring candidate decodes to the true plaintext (or
the coarse winner already did). Equivalence against the shipped set is the
metric, in the style of 7.11's band verification: a cheaper set is only
acceptable if it recovers everything the full one does.

Scope: ring0/start0 are pinned at the truth (7.10's collapse is exact and
ring2-independent, so this costs nothing) and the reflector/wheel order are
given, which makes the coarse pass small enough to run exactly. Scoring is the
weighted all-order model (-a), matching eval/ring_stride_geometry_probe.py.

Usage: python3 eval/ring_stride_refine_shape_probe.py [--trials N] [--k 3]
       [--lengths 60,150] [--seed S]
"""

import argparse
import os
import random
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ring_stride_geometry_probe import (          # noqa: E402
    subst_array, positions, crypt, score_table, score, corpus_texts, num, selftest)

MID_RING_WINDOW = 2         # enigma.cc's mid_ring_window
SHAPES = ["shipped", "lock-start2", "lock-off1", "lock-both", "lock-all"]


def decode_set(S, c, cand, ring0, start0):
    """Score a list of (ring1, start1, ring2, start2) candidates.
    Returns the array of per-symbol scores, in candidate order."""
    n = len(c)
    # group by (start1, start2): the stepping schedule depends only on those two
    # (plus start0, pinned), so it is computed once per group
    groups = {}
    for i, (r1, g1, r2, g2) in enumerate(cand):
        groups.setdefault((g1, g2), []).append((i, r1, r2))
    out = np.empty(len(cand), dtype=np.float64)
    for (g1, g2), members in groups.items():
        p = positions([start0, g1, g2], WHEELS, n)
        o0 = (p[:, 0] - ring0) % 26
        idx = np.array([m[0] for m in members])
        r1s = np.array([m[1] for m in members])[:, None]
        r2s = np.array([m[2] for m in members])[:, None]
        o1 = (p[None, :, 1] - r1s) % 26
        o2 = (p[None, :, 2] - r2s) % 26
        pt = S[o0[None, :], o1, o2, c[None, :]]
        out[idx] = score(pt, TAB)
    return out


def candidates(shape, coarse, skipped):
    """Build one of the candidate sets from the coarse winner
    (ring1, start1, ring2, start2)."""
    cr1, cg1, cr2, cg2 = coarse
    off1 = (cg1 - cr1) % 26
    off2 = (cg2 - cr2) % 26
    # how far the middle OFFSET may sit from the coarse winner's, and whether
    # absolute start1 is searched or pinned to the winner's value
    band = [0] if shape in ("lock-off1", "lock-all") else \
        list(range(-MID_RING_WINDOW, MID_RING_WINDOW + 1))
    g1s = [cg1] if shape in ("lock-both", "lock-all") else list(range(26))
    out = []
    for r2 in skipped:
        if shape == "shipped":
            g2s = range(26)
        else:
            g2s = [(r2 + off2) % 26]           # keep the coarse winner's offset2
        pairs = [((g1 - (off1 + d)) % 26, g1) for g1 in g1s for d in band]
        for g2 in g2s:
            for (r1, g1) in pairs:
                out.append((r1, g1, r2, g2))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--k", type=int, default=3)
    ap.add_argument("--lengths", default="60,150")
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    global TAB, WHEELS
    selftest()
    TAB = score_table("all", args.lang)
    lengths = [int(x) for x in args.lengths.split(",")]
    texts = corpus_texts(min(lengths))
    rng = random.Random(args.seed)
    K = args.k

    print("# K=%d trials=%d lang=%s  (shipped 25x130x26, lock-start2 25x130x1, "
          "lock-off1 25x26x1, lock-both 25x5x1, lock-all 25x1x1)"
          % (K, args.trials, args.lang))
    print("\t".join(["len", "n", "coarse ok"] + SHAPES
                    + ["lost:" + s for s in SHAPES[1:]]))

    for L in lengths:
        pool = [t for t in texts if len(t) >= L]
        got = {s: 0 for s in SHAPES}
        coarse_ok = 0
        lost = {s: 0 for s in SHAPES[1:]}
        for tr in range(args.trials):
            pt = pool[tr % len(pool)]
            off = rng.randrange(0, max(1, len(pt) - L + 1))
            pt = pt[off:off + L]
            WHEELS = rng.sample(range(5), 3)
            refl = "B"
            ring = [rng.randrange(26) for _ in range(3)]
            start = [rng.randrange(26) for _ in range(3)]
            ct = crypt(pt, WHEELS, refl, ring, start)
            S = subst_array(WHEELS, refl)
            c = num(ct)

            # coarse pass: ring2 on the stride grid, ring1/start1/start2 open
            grid = [v for v in range(26) if v % K == 0]
            cand = [(r1, g1, r2, g2)
                    for r2 in grid for g2 in range(26)
                    for g1 in range(26) for r1 in range(26)]
            sc = decode_set(S, c, cand, ring[0], start[0])
            coarse = cand[int(np.argmax(sc))]
            coarse_score = sc.max()
            truth_pt = pt

            def best_pt(shape):
                skipped = [v for v in range(26) if v != coarse[2]]
                cs = candidates(shape, coarse, skipped)
                s = decode_set(S, c, cs, ring[0], start[0])
                j = int(np.argmax(s))
                if s[j] <= coarse_score:          # refinement kept only if it improves
                    return coarse, coarse_score
                return cs[j], s[j]

            def plaintext(key):
                r1, g1, r2, g2 = key
                p = positions([start[0], g1, g2], WHEELS, len(c))
                o0 = (p[:, 0] - ring[0]) % 26
                o1 = (p[:, 1] - r1) % 26
                o2 = (p[:, 2] - r2) % 26
                return "".join(chr(int(x) + 65) for x in S[o0, o1, o2, c])

            if plaintext(coarse) == truth_pt:
                coarse_ok += 1
            res = {}
            for shape in SHAPES:
                key, _ = best_pt(shape)
                res[shape] = plaintext(key) == truth_pt
                got[shape] += res[shape]
            for shape in SHAPES[1:]:
                if res["shipped"] and not res[shape]:
                    lost[shape] += 1
        n = args.trials
        print("\t".join([str(L), str(n)]
                        + ["%d%%" % round(100 * coarse_ok / n)]
                        + ["%d%%" % round(100 * got[s] / n) for s in SHAPES]
                        + ["%d" % lost[s] for s in SHAPES[1:]]))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
