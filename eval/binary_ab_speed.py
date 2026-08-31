#!/usr/bin/env python3
"""Paired wall-time A/B of TWO BINARIES on the same climb, with an interval.

    python3 eval/binary_ab_speed.py --base /tmp/bin-base --head /tmp/bin-head \
            --schedule k4f10 --length 100 --fixtures 24

WHY NOT `make bench`.  Its five tiers do not exercise a low-order climb stage
at all -- search, icscan and crib run without -c, hillclimb is -q and fused is
-f -- so a change to hist_probe (the -S i/m/k probe) is invisible to it.  Its
cells are a useful CONTROL for such a change and no evidence at all about it.

WHAT THIS ESTIMATES.  Not "the" speedup: the two binaries converge in the same
number of moves (the climb is byte-identical), but each fixture's ratio still
varies with how much of its time sits in the stage under test, so the ratio is
a DISTRIBUTION over fixtures.  The thing to report is its mean with a 95% CI,
and the fixture count is what buys precision -- reps only sharpen each
fixture's own estimate.

THREE THINGS MAKE THE NUMBER HONEST, and the first is the one usually missed:

  ARM ORDER ALTERNATES PER REP.  A box that warms up during a run makes
  whichever arm goes last look faster.  Timing base-then-head every time gives
  the head that bias for free -- an earlier run of exactly this measurement had
  its control reading -1.3%..-3.4% for that reason, on identical binaries.
  Alternating the order cancels a drift that is linear in time.

  A CONTROL ARM runs the BASE binary a second time, under the same alternation.
  Its spread is the measurement floor; any spread in head/base beyond it is
  real.  Without it a wide distribution and a noisy box look identical.

  STARTUP IS SUBTRACTED, per fixture and per arm, via an -R 0 run.  A whole
  invocation carries ~0.1 s of n-gram load whatever the restart count, and
  leaving it in drags every ratio toward 1 -- the wrong direction for a test
  asking whether a difference exists.
"""

import argparse
import os
import random
import re
import statistics
import subprocess
import sys
import time
from math import sqrt

HERE = os.path.dirname(os.path.abspath(__file__))
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(binary, argv, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = os.path.join(HERE, os.pardir, "ngrams")
    p = subprocess.run([binary] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip()


def timed(binary, argv, text):
    t0 = time.perf_counter()
    run(binary, argv, text)
    return time.perf_counter() - t0


def ci95(xs):
    if len(xs) < 2:
        return 0.0
    return 1.96 * statistics.stdev(xs) / sqrt(len(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--head", required=True)
    ap.add_argument("--schedule", default="k4f10")
    ap.add_argument("--length", type=int, default=100)
    ap.add_argument("--restarts", type=int, default=1024)
    ap.add_argument("--fixtures", type=int, default=24)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--climb-rule", default="-K")
    ap.add_argument("--seed", type=int, default=4242)
    args = ap.parse_args()

    for b in (args.base, args.head):
        if not os.path.exists(b):
            sys.exit(f"no such binary: {b}")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length
    rng = random.Random(args.seed)
    tail = ["-c", args.climb_rule, "--polish", "-S", args.schedule,
            "-f", "-l", "wehrmacht", "-T", 1]
    # "ctl" is the base binary again: its spread is the floor.
    arms = (("base", args.base), ("head", args.head), ("ctl", args.base))

    ratios, controls, base_t = [], [], []
    for f in range(args.fixtures):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(args.plugs))
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct = run(args.base, key + ["-s", pb], pt)
        if len(ct) != L:
            continue

        best = {a: [1e9, 1e9] for a, _ in arms}
        for rep in range(args.reps):
            # ALTERNATE the order: on odd reps the arms run last-to-first, so a
            # drift that is linear in time cancels between the two halves.
            order = arms if (rep % 2 == 0) else tuple(reversed(arms))
            for name, binary in order:
                argv = key + tail
                t = timed(binary, argv + ["-R", args.restarts], ct)
                best[name][0] = min(best[name][0], t)
                t0 = timed(binary, argv + ["-R", 0], ct)
                best[name][1] = min(best[name][1], t0)

        tb = best["base"][0] - best["base"][1]
        th = best["head"][0] - best["head"][1]
        tc = best["ctl"][0] - best["ctl"][1]
        if tb <= 0:
            continue
        ratios.append(th / tb)
        controls.append(tc / tb)
        base_t.append(tb)

    n = len(ratios)
    if n < 2:
        sys.exit("too few usable fixtures")
    mr, mc = statistics.mean(ratios), statistics.mean(controls)
    hr, hc = ci95(ratios), ci95(controls)
    print(f"\n# -S {args.schedule}, L={L}, -R {args.restarts}, "
          f"{args.climb_rule}"
          f", {args.plugs}-pair board, {n} fixtures x {args.reps} reps"
          f" (alternating order), seed {args.seed}")
    print(f"# base climb time: mean {statistics.mean(base_t):.3f}s "
          f"(min {min(base_t):.3f}, max {max(base_t):.3f})")
    print(f"head/base  {mr:.4f}  95% CI [{mr - hr:.4f}, {mr + hr:.4f}]"
          f"   = {100 * (mr - 1):+.1f}%  "
          f"[{100 * (mr - hr - 1):+.1f}, {100 * (mr + hr - 1):+.1f}]")
    print(f"base/base  {mc:.4f}  95% CI [{mc - hc:.4f}, {mc + hc:.4f}]"
          f"   = {100 * (mc - 1):+.1f}%  "
          f"[{100 * (mc - hc - 1):+.1f}, {100 * (mc + hc - 1):+.1f}]"
          f"   <- floor")
    d = [a - b for a, b in zip(ratios, controls)]
    md, hd = statistics.mean(d), ci95(d)
    print(f"head minus control: {md:+.4f}  95% CI [{md - hd:+.4f}, "
          f"{md + hd:+.4f}]  -> "
          f"{'ESTABLISHED' if md + hd < 0 else 'not established'}")


if __name__ == "__main__":
    main()
