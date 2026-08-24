#!/usr/bin/env python3
"""How much of a staged climb is the CAP-STAGE PRE-PASS?

Times `-S <pre>`, `-S <target>` and `-S <pre><target>` at two restart counts
and takes the SLOPE, which is the only honest way to do it here: the arms have
different startup floors (a mono-only schedule loads 26 numbers, an f10 one
loads the 0.45 MB all8 table), so subtracting a single measured floor from
both understates the pre-pass badly.

Usage:
  python3 eval/capstage_share.py --lengths 60 100 200 400 --pre k4 --target f10
"""

import argparse
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(args, text):
    p = subprocess.run([ENIGMA] + [str(a) for a in args], input=text,
                       capture_output=True, text=True, check=False)
    return p.stdout.strip(), p.stderr


def timed(args, text, reps):
    """min wall over reps, and the plugboards-scored count."""
    best, scored = 1e9, 0
    for _ in range(reps):
        t0 = time.perf_counter()
        _, err = run(args, text)
        best = min(best, time.perf_counter() - t0)
        m = re.search(r"scored (\d+) plugboards", err)
        if m:
            scored = int(m.group(1))
    return best, scored


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lengths", type=int, nargs="+",
                    default=[60, 100, 200, 400])
    ap.add_argument("--pre", default="k4")
    ap.add_argument("--target", default="f10")
    ap.add_argument("--restarts", type=int, default=200)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)
    both = args.pre + args.target

    print(f"# {args.pre} / {args.target} / {both}, -R {args.restarts}, "
          f"min of {args.reps}, slope-fitted")
    print(f"{'L':>4} {'pre us/rs':>10} {'calls':>7} {'ns/call':>8} "
          f"{'all us/rs':>10} {'calls':>7} "
          f"{'pre % wall':>11} {'pre % calls':>12}")
    for L in args.lengths:
        off = rng.randrange(0, len(corpus) - L)
        pt = corpus[off:off + L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)

        base = key + ["-c", "-J", "-l", "wehrmacht", "-e", "7", "-T", 1]
        row = {}
        for sch in (args.pre, both):
            t0, c0 = timed(base + ["-S", sch, "-R", 0], ct, args.reps)
            t1, c1 = timed(base + ["-S", sch, "-R", args.restarts], ct,
                           args.reps)
            row[sch] = ((t1 - t0) / args.restarts * 1e6,
                        (c1 - c0) / args.restarts)
        pu, pc = row[args.pre]
        au, ac = row[both]
        print(f"{L:>4} {pu:>10.1f} {pc:>7.0f} {pu * 1000 / pc:>8.1f} "
              f"{au:>10.1f} {ac:>7.0f} "
              f"{100 * pu / au:>10.1f}% {100 * pc / ac:>11.1f}%", flush=True)


if __name__ == "__main__":
    main()
