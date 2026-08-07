#!/usr/bin/env python3
"""Does an ABSENT crib produce a confident wrong answer?
(archived/cribs.md 7a caution 3.)

    python3 eval/crib_absent_probe.py --trials 40

THE RISK. A crib deduces plugs by arithmetic and pins them, so it is trusted
absolutely. If the crib is not actually in the message, those pins are simply
wrong -- and the caution is that the run might still return a confident-looking
answer, with nothing in the output to say the crib never fitted.

7c does NOT cover this. There the crib genuinely is in the message and the
question is whether the wrong one of its 26 hypotheses wins. Here the crib is
absent altogether, which is the commoner case by far: a library is written
against a network, so most of its cribs miss any given message.

THE TEST NEEDS NO ORACLE. Run the same message twice, once with the absent crib
and once without, and compare the WINNING SCORES:

    crib score > no-crib score   ->  false positive: the wrong pins bought a
                                     better-looking board than an unconstrained
                                     climb found, which is what would fool a
                                     reader
    crib score <= no-crib score  ->  the absent crib cannot outbid an ordinary
                                     climb, so it cannot mislead on score

Recovery is reported beside it, since a false positive only matters if the
board it wins with is also wrong.

The cribs are REAL PHRASES taken from other messages in the corpus, not random
letters -- a random string would rarely survive the deduction at all, which
would flatter the result. A phrase that recurs on the network is exactly the
kind of wrong crib a library actually supplies.
"""
import argparse
import os
import random
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from crib_menu import corpus, random_key                     # noqa: E402
from ring_stride_geometry_probe import txt, plugboard, crypt  # noqa: E402

BIN = os.path.join(HERE, os.pardir, "enigma")
SCORE = re.compile(r"^\s*(-?\d+\.\d+)\s")


def run(ct, key, args, crib=None):
    """Return (winning score, recovered plaintext)."""
    wheels, refl, ring, start = key
    cmd = [BIN, "-u", refl, "-w", "".join(str(w + 1) for w in wheels),
           "-r", txt(ring), "-g", txt(start), "-c", "-J", "-f", "-l", args.lang,
           "-T", str(args.threads), "-R", str(args.restarts)]
    if crib:
        cmd += ["--crib", crib]
    p = subprocess.run(cmd, input=ct, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True,
                       env=dict(os.environ, ENIGMA_SEED="0"))
    best = None
    for line in p.stderr.splitlines():
        m = SCORE.match(line)
        if m:
            best = float(m.group(1))
    return best, re.sub(r"[^A-Z]", "", p.stdout.strip().upper())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--crib", type=int, default=10, help="crib letters [10]")
    ap.add_argument("--length", type=int, default=90)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--restarts", type=int, default=64,
                    help="-R for BOTH arms; the baseline must be able to "
                         "solve, else the comparison is between two failures")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    texts = [t for t in corpus() if len(t) >= args.length]
    if len(texts) < 2:
        sys.exit("need at least two corpus texts")
    rng = random.Random(args.seed)

    fp = 0
    rows = []
    for _ in range(args.trials):
        src = rng.choice(texts)
        off = rng.randrange(len(src) - args.length + 1)
        pt = src[off:off + args.length]
        # A real phrase from a DIFFERENT message, checked to be absent here.
        crib = None
        for _try in range(40):
            other = rng.choice([t for t in texts if t is not src])
            o = rng.randrange(len(other) - args.crib + 1)
            cand = other[o:o + args.crib]
            if cand not in pt:
                crib = cand
                break
        if crib is None:
            continue
        key = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                         args.plugs)
        wheels, refl, ring, start = key
        ct = crypt(pt, wheels, refl, ring, start, plug)

        s_no, got_no = run(ct, key, args)
        s_cr, got_cr = run(ct, key, args, crib)
        if (s_no is None) or (s_cr is None):
            continue
        pc = (100.0 * sum(a == b for a, b in zip(pt, got_cr)) / len(pt)
              if len(got_cr) == len(pt) else 0.0)
        pn = (100.0 * sum(a == b for a, b in zip(pt, got_no)) / len(pt)
              if len(got_no) == len(pt) else 0.0)
        rows.append((s_cr, s_no, pc, pn))
        if s_cr > s_no:
            fp += 1

    if not rows:
        sys.exit("no usable trials")
    n = len(rows)
    print("Does an ABSENT crib out-score an ordinary climb?\n"
          "(%d trials, %d-letter messages, %d-letter crib taken from ANOTHER\n"
          "message and checked absent here, %d cables hidden, rotor key given)\n"
          % (n, args.length, args.crib, args.plugs))
    print("  false positives (crib score > no-crib score): %d / %d = %.0f%%"
          % (fp, n, 100.0 * fp / n))
    solved = sum(1 for r in rows if r[3] >= 95.0)
    print("  trials where the NO-CRIB baseline actually solved: %d / %d"
          % (solved, n))
    if solved:
        fps = sum(1 for r in rows if (r[3] >= 95.0) and (r[0] > r[1]))
        print("  false positives among those:                       %d / %d"
              % (fps, solved))
    print("\n  mean winning score   with absent crib %8.4f"
          % (sum(r[0] for r in rows) / n))
    print("                       without crib      %8.4f"
          % (sum(r[1] for r in rows) / n))
    print("\n  mean %%-correct       with absent crib %7.1f%%"
          % (sum(r[2] for r in rows) / n))
    print("                       without crib     %7.1f%%"
          % (sum(r[3] for r in rows) / n))


if __name__ == "__main__":
    main()
