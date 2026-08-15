#!/usr/bin/env python3
"""ENHANCEMENTS.md item 5(a): anchor on the X's, then compare segments.

The shipped matcher (`doubling_probe.py`) scans every (start, length) window
looking for `W X V`.  The pattern telegraphic German is supposed to write is
`X PARIS X PARIM X` -- the doubled word a WHOLE X-delimited segment on both
sides -- so the alternative is to split on X first and compare adjacent
segments.  The length loop then disappears, because the segmentation already
fixes the candidate lengths.

Item 5(a) recorded three predictions, all reasoned rather than measured:

  1. cost falls by ~two orders of magnitude;
  2. short words become viable -- L>=6 is forced by the UNANCHORED scan's
     chance rate, and two flanking X's should cut the null by ~250x, so L=4
     or 5 should become affordable;
  3. recall barely moves, because the flanking measurement says 71% both
     sides, 25% left only, 4% right only and 0% neither, and the left-only
     cases are mostly end-of-message, which a segmentation gets for free.

This script measures all three.  Text-level only -- it does no cracking.

    python3 eval/doubling_anchored_probe.py
"""
import random
import re
import sys

DB = "eval/enigma-army-messages-1941.txt"
NULL_TRIALS = 20000
MAXLEN = 16
# One hit in NULL_TRIALS -- the finest rate this many shuffles can resolve.
FLOOR = 100.0 / NULL_TRIALS


def decrypts():
    """Recorded plaintexts as raw letter streams.  Records carrying '-' or '['
    are skipped: those mark unreceived letters and editorial repairs, either of
    which would fabricate or destroy a repeat."""
    out = []
    for b in re.split(r"\n### ", open(DB, encoding="utf-8").read())[1:]:
        m = re.search(r"DECRYPT:\s+((?:.|\n)*?)\n[A-Z]", b)
        if not m:
            continue
        d = "".join(m.group(1).split())
        if "-" in d or "[" in d:
            continue
        out.append((b.split("\n")[0].split("(")[-1].rstrip(") "), d))
    return out


def windows(t, k, maxmm=0):
    """The shipped rule: every W X V window with |W| = |V| >= k."""
    out = []
    for i in range(len(t)):
        for L in range(k, MAXLEN + 1):
            if i + 2 * L + 1 > len(t):
                break
            w, v = t[i:i + L], t[i + L + 1:i + 2 * L + 1]
            if t[i + L] != "X" or "X" in w or "X" in v:
                continue
            if sum(1 for a, b in zip(w, v) if a != b) <= maxmm:
                out.append((i, L, w, v))
    return out


def cost_windows(t, k):
    """Window comparisons the scan performs -- its cost unit."""
    return sum(1 for i in range(len(t))
               for L in range(k, MAXLEN + 1) if i + 2 * L + 1 <= len(t))


def segments(t):
    """Maximal X-free runs.  Message start and end count as separators, so the
    end-of-message case prediction 3 relies on is handled for free."""
    return [m.group() for m in re.finditer(r"[^X]+", t)]


def anchored(t, k, maxmm=0, gap=1):
    """Segment pairs at distance <= gap matching within maxmm substitutions.

    gap=1 is adjacent segments only (X W X V X).  Larger gap also catches a
    word repeated later in the message with other words in between, which the
    windowed scan cannot see at any cost.
    """
    segs, out = segments(t), []
    for a in range(len(segs)):
        for b in range(a + 1, min(a + 1 + gap, len(segs))):
            w, v = segs[a], segs[b]
            if len(w) != len(v) or not k <= len(w) <= MAXLEN:
                continue
            if sum(1 for x, y in zip(w, v) if x != y) <= maxmm:
                out.append((w, v))
    return out


def cost_anchored(t, k, gap=1):
    n = len(segments(t))
    return sum(1 for a in range(n) for _ in range(a + 1, min(a + 1 + gap, n)))


def null_rate(recs, seed, fn, trials=NULL_TRIALS):
    """Shuffled real decrypts: preserves each message's letter frequencies,
    including the high X rate, which is the thing a repeat-with-X test could
    otherwise be fooled by.

    The RNG is seeded PER CELL rather than shared, so a cell's number does not
    depend on how many cells ran before it.  With a shared stream, adding or
    removing a row silently moved the others -- and at these rates one hit in
    20000 is 0.005%, so an apparent 0.000% -> 0.005% move is a single draw.
    """
    rng = random.Random(seed)
    hit = 0
    for _ in range(trials):
        d = list(rng.choice(recs)[1])
        rng.shuffle(d)
        hit += bool(fn("".join(d)))
    return 100.0 * hit / trials


def main():
    recs = decrypts()
    n = len(recs)
    print("%d messages with a clean recorded plaintext" % n)
    seg = [len(s) for _, d in recs for s in segments(d)]
    print("%d X-delimited segments, mean length %.1f, median %d\n"
          % (len(seg), sum(seg) / len(seg), sorted(seg)[len(seg) // 2]))

    print("PREDICTION 1 -- cost, comparisons per message")
    cu = sum(cost_windows(d, 6) for _, d in recs) / n
    ca = sum(cost_anchored(d, 6) for _, d in recs) / n
    print("   unanchored scan (k=6)      %8.1f" % cu)
    print("   anchored, adjacent pairs   %8.1f   %5.0fx cheaper" % (ca, cu / ca))
    # ...but put that saving beside the thing it would run next to.  One
    # plugboard climb on a 151-letter message scores 18441 boards (measured:
    # -c -f -l wehrmacht -S i4f10 -J --polish -R 1), each touching every
    # letter.  The doubling check runs once per CONVERGED board, so that is
    # the honest denominator.
    boards, mlen, avg = 18441, 151, (6 + MAXLEN) / 2.0
    ops = boards * mlen
    print("   in context: one climb = %d boards x %d letters = %.2fM ops"
          % (boards, mlen, ops / 1e6))
    print("   so the scan is %.2f%% of ONE climb, the anchored form %.3f%%."
          % (100.0 * cu * avg / ops, 100.0 * ca * avg / ops))
    print("   VERDICT: true (%.0fx), and irrelevant -- cost does not bind.\n"
          % (cu / ca))

    print("PREDICTION 2 -- does a lower null make short words usable?")
    print("   %-5s %-4s %-11s %-11s %s" % ("len", "mm", "null unanch",
                                           "null anch", "reduction"))
    for k in (3, 4, 5, 6):
        nu = null_rate(recs, 1000 + k, lambda t, k=k: windows(t, k, 1))
        na = null_rate(recs, 2000 + k, lambda t, k=k: anchored(t, k, 1))
        rat = ("%.0fx" % (nu / na)) if na else \
              ("n/a" if nu <= FLOOR else ">=%.0fx" % (nu / FLOOR))
        print("   %-5d %-4d %10.3f%% %10.3f%%  %s" % (k, 1, nu, na, rat))
    print("   (resolution is one hit in %d = %.3f%%, so a 0.000%% and a"
          % (NULL_TRIALS, FLOOR))
    print("   %.3f%% differ by a single draw and mean the same thing)" % FLOOR)
    print("   The reduction is real.  But the premise is not: the SHIPPED")
    print("   setting len>=6 mm<=1 already measures 0.000% unanchored, so")
    print("   there was no chance rate to relieve.  And under anchoring a")
    print("   shorter length adds nothing anyway --")
    print("   %-6s %-12s %s" % ("len>=", "unanchored", "anchored"))
    for k in (7, 6, 5, 4, 3):
        print("   %-6d %-12d %d"
              % (k, sum(1 for _, d in recs if windows(d, k, 1)),
                 sum(1 for _, d in recs if anchored(d, k, 1))))
    print("   6 -> 5 gains +1 unanchored and +0 anchored: the binding")
    print("   constraint is the X-enclosure, not the length.")
    print("   VERDICT: true on the null, with nothing to spend it on.\n")

    print("PREDICTION 3 -- recall")
    print("   %-5s %-4s %-14s %-14s %s" % ("len", "mm", "unanchored",
                                           "anchored", "lost"))
    for k in (7, 6, 5, 4, 3):
        for mm in (0, 1):
            u = sum(1 for _, d in recs if windows(d, k, mm))
            a = sum(1 for _, d in recs if anchored(d, k, mm))
            print("   %-5d %-4d %2d of %d (%3.0f%%) %2d of %d (%3.0f%%)  %+d"
                  % (k, mm, u, n, 100.0 * u / n, a, n, 100.0 * a / n, a - u))
    print("\n   And the loss is ONE-DIRECTIONAL by construction:")
    for k in (4, 5, 6):
        U = {kn for kn, d in recs if windows(d, k, 1)}
        A = {kn for kn, d in recs if anchored(d, k, 1)}
        print("     len>=%d mm<=1  only-anchored %d   only-unanchored %2d"
              % (k, len(A - U), len(U - A)))
    print("   Zero only-anchored, always -- an adjacent equal-length segment")
    print("   pair IS a window hit at that (i, L), so anchoring can only")
    print("   remove recall, never add it.")
    print("   VERDICT: false.  Recall is the axis that decides it.\n")

    print("WHY -- the doubled word is NOT always enclosed by X")
    U = {kn for kn, d in recs if windows(d, 6, 1)}
    A = {kn for kn, d in recs if anchored(d, 5, 1)}
    print("   unanchored len>=6 finds %d, anchored len>=5 finds %d; the %d"
          % (len(U), len(A), len(U - A)))
    print("   messages between them are all genuine doublings:")
    for kn, d in recs:
        if kn not in U - A:
            continue
        i, L, w, v = windows(d, 6, 1)[0]
        a, b = max(0, i - 20), min(len(d), i + 2 * L + 21)
        print("     %-7s ...%s[%s]X[%s]%s..." % (kn, d[a:i], w, v,
                                                 d[i + 2 * L + 1:b]))
    print("   Operators run the doubled word together with what follows, so")
    print("   the boundary an anchored rule needs is often simply not there.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
