#!/usr/bin/env python3
"""WHY does --ring-stride miss? Classifies each stride-specific miss by which part of the
key the coarse pass got wrong -- which decides whether the miss is fixable at all, and by
what.

The motivating observation (PERFORMANCE.md 7.11): widening the refinement from a +/-K/2
window to EVERY skipped ring2 moved K=3 by +4.5/+7.0pp but moved K=2 by nothing. K=2's
coarse grid is never more than 1 away from the true ring2, so if its misses were window
failures, widening would have fixed them. It did not -- so something else is being lost.

FIRST RESULT: the obvious hypothesis is dead. The guess was that the approximation
corrupts the coarse pass enough that the true WHEEL ORDER / REFLECTOR loses outright,
which the refinement could never recover since it pins those to the coarse winner -- the
case for 7.11's one untested mitigation, refining the top-M coarse candidates. Measured
across 96 stride-specific misses (L=40/60, K=2/3, 200 trials each): the coarse winner had
the correct reflector+wheel-order in 100% of them. There is nothing for a top-M refinement
to find, and that idea is dead rather than untested. (This holds either side of the bug
below -- the wheels were never what the stride was losing.)

SECOND RESULT: what this probe actually found was a BUG, not a property of the stride.
The component breakdown said the PINNED offset0 was wrong in 54-75% of misses, a coherent
story implicating a pin whose justifying comment was independently questionable. It was
wrong. --polish's plugboard finisher shared an `if` with the --ring-stride refinement
without re-checking its own flag, so every strided run got a plugboard climb plus gain
cascade with no -c requested, adding spurious plugs to the -s board (PERFORMANCE.md 7.11,
"the accuracy cost was a --polish guard bug"). Those offset counts were mostly the
finisher corrupting the board, not the pin. After the fix the stride-specific miss rate
drops from 10-21% to 2-4%, so the population this script dissects is now a tenth the size.

The lesson is in HOW it was caught, since the aggregate table was self-consistent and
pointed the wrong way: dumping individual failing cases. The first one printed had every
identifiable key component correct yet a wrong plaintext -- impossible under the model --
and that impossibility was the thread to pull. Prefer examples over rates when a rate
tells a story you are about to act on.

The four components are still reported separately because the refinement treats them
differently, which is what makes the breakdown diagnostic: offset0 is PINNED to the coarse
winner, offset1 is re-opened, ring2 is swept, start2 is left open. A component that is
wrong while pinned implicates the pin; a component that is wrong while searched implicates
the score.

Method mirrors eval/ring_stride_wehrmacht_probe.py exactly (same corpus, key generation,
scoring model and -s plugboard-given setup) so the miss populations are comparable; the
only addition is parsing the WINNING KEY out of the progress output and diffing it
against the truth component by component.

Two things the comparison has to get right:
  - ring0 is always reported as 'A' and ring1/start1 may be a CLASS REPRESENTATIVE (7.10,
    7.12), so wheel 0's and wheel 1's ring/start must be compared as the OFFSET
    (start - ring) mod 26 -- the only identifiable form. Comparing the letters directly
    would flag a perfectly correct key as wrong. ring2 and start2 have no such caveat
    (the right-hand notch makes them separately real) and are compared as letters.
  - Trials where K=1 also fails are the pre-existing scoring/search floor and are excluded
    outright; they are not stride misses.

Usage: python3 eval/ring_stride_miss_anatomy.py
Env: LENGTHS ("40 60"), KS ("2 3"), TRIALS (200), SEED (0), THREADS (4)
"""
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "40 60").split()]
KS = [int(x) for x in os.environ.get("KS", "2 3").split()]
TRIALS = int(os.environ.get("TRIALS", "200"))
SEED = int(os.environ.get("SEED", "0"))
THREADS = os.environ.get("THREADS", "4")

# A progress line is "  <score> <refl+wheels> <ring> <start> [plugs...] <text>".
PROGRESS = re.compile(r"^\s*-?\d+\.\d+\s+(\S+)\s+([A-Z]{3})\s+([A-Z]{3})\s")


def load_corpus(minlen):
    text = ""
    for fname in ("enigma-messages.txt", "enigma-army-messages-1941.txt"):
        text += open(os.path.join(HERE, fname)).read()
    blocks = re.findall(r"^DECRYPT:\s*(.*(?:\n {13}\S.*)*)", text, re.M)
    clean = [re.sub(r"\s+", "", b) for b in blocks]
    return [b for b in clean if "-" not in b and len(b) >= minlen]


def run(args, inp):
    p = subprocess.run([BIN] + args, input=inp, capture_output=True, text=True, cwd=ROOT)
    key = None
    for line in p.stderr.splitlines():
        m = PROGRESS.match(line)
        if m:
            key = (m.group(1), m.group(2), m.group(3))   # last one wins = the winner
    return p.stdout.strip(), key


def trial(L, corpus, rng, ks):
    u = rng.choice("ABC")
    w = "".join(str(x) for x in rng.sample(range(1, 6), 3))
    r = "".join(rng.choice(ALPHA) for _ in range(3))
    g = "".join(rng.choice(ALPHA) for _ in range(3))
    letters = list(ALPHA)
    rng.shuffle(letters)
    board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(10))
    blk = rng.choice(corpus)
    off = rng.randrange(0, len(blk) - L + 1)
    pt = blk[off:off + L]
    ct, _ = run(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", board], pt)

    base = ["-f", "-l", "wehrmacht", "-u", u, "-w", w, "-r", "...", "-g", "...",
            "-s", board, "-T", THREADS]
    out = {}
    for K in [1] + ks:
        args = base + (["--ring-stride", str(K)] if K > 1 else [])
        out[K] = run(args, ct)
    return pt, (u + w), r, g, out


def off(a, b):
    """The wheel-0/1 ring x start redundancy means only (start - ring) mod 26 is
    identifiable (7.10 total and unconditional, 7.12 partial), so a reported ring/start
    PAIR must be compared as that offset -- comparing the letters directly would flag a
    correct key as wrong whenever the tool reported a class representative."""
    return (ALPHA.index(a) - ALPHA.index(b)) % 26


def parts(ring, start):
    """The four independently identifiable components of a rotor key, in the form the
    refinement actually treats differently: offset0 and offset1 (pinned / re-opened as
    pairs), then ring2 and start2, which the right-hand notch makes separately real."""
    return (off(start[0], ring[0]), off(start[1], ring[1]), ring[2], start[2])


def main():
    print("Anatomy of stride-SPECIFIC misses (K failed where K=1 succeeded).")
    print("Each column is the share of misses in which THAT component of the winning key")
    print("differs from the truth; a miss can differ in more than one, so rows need not")
    print("sum to 100%. offset0/offset1 are (start-ring) mod 26 -- the only identifiable")
    print("form (7.10, 7.12). What the refinement does with each is what matters:")
    print("  offset0  PINNED to the coarse winner    offset1  re-opened")
    print("  ring2    swept (every skipped value)    start2   left open\n")
    print("%5s %4s %7s %8s %10s %10s %10s %10s"
          % ("L", "K", "misses", "of-n", "wheels", "offset0", "offset1", "ring2/start2"))
    for L in LENGTHS:
        corpus = load_corpus(L)
        rng = random.Random(SEED * 1000 + L)     # same stream as the sibling probe
        rows = [trial(L, corpus, rng, KS) for _ in range(TRIALS)]
        for K in KS:
            miss = [(t_uw, t_r, t_g, out) for pt, t_uw, t_r, t_g, out in rows
                    if out[1][0] == pt and out[K][0] != pt and out[K][1] is not None]
            n = len(miss)
            c = [0, 0, 0, 0, 0]     # wheels, offset0, offset1, ring2, start2
            for t_uw, t_r, t_g, out in miss:
                w, r, g = out[K][1]
                truth, got = parts(t_r, t_g), parts(r, g)
                c[0] += (w != t_uw)
                for j in range(4):
                    c[j + 1] += (got[j] != truth[j])
            pct = lambda v: ("%d (%.0f%%)" % (v, 100.0 * v / n)) if n else "-"
            print("%5d %4d %7d %7.1f%% %10s %10s %10s %10s"
                  % (L, K, n, 100.0 * n / TRIALS, pct(c[0]), pct(c[1]), pct(c[2]),
                     "%s / %s" % (pct(c[3]), pct(c[4]))))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
