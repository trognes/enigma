#!/usr/bin/env python3
"""Is -K actually FASTER than -J, on a given schedule and length?

    python3 eval/jorder_speed.py --schedule k4f10 --length 100 --fixtures 40

WHY THIS EXISTS.  The -J/-K recovery grid reports plugboards scored, and that
counter is NOT a compute ratio: the IC ranking's O(26) work per move happens
outside the counted score loop, so it prices the scans -K removes and not the
work it adds.  On the schedules with a low-order pre-pass the counter says
-11% while two hand-timed fixtures said -3.3% and -19.7% -- a spread far too
wide to quote, and the question was left open.

WHY TWO FIXTURES WERE NEVER GOING TO SETTLE IT.  The ratio is not a per-move
cost ratio.  The two arms visit moves in different orders, so they converge in
different numbers of moves, and how many depends on the message and key.  The
per-fixture ratio is therefore a real distribution, not a constant measured
with error -- so the thing to estimate is its MEAN over fixtures, with a CI,
and the number of fixtures is what buys precision.

THE CONTROL IS WHAT SEPARATES THE TWO SOURCES OF SPREAD.  A third arm re-times
-J against itself on the same fixture.  Its spread is measurement noise; any
spread in -K/-J beyond that is trajectory.  Without it a wide distribution and
a noisy box look identical.

  REPS OUTER, ARMS INNER.  Each repetition runs -J, -K and the control back to
  back, so a box that drifts during the run drifts under all three arms rather
  than under whichever was timed last.  The per-arm time is the min over reps,
  as tests/bench.sh does.

  STARTUP IS SUBTRACTED, per fixture and per arm, via an -R 0 run.  A whole
  invocation is ~0.1 s of n-gram load whatever the restart count, and leaving
  it in dilutes every ratio toward 1 -- the wrong direction for a test asking
  whether a difference exists at all.  (-R 0 is one climb rather than zero, so
  the subtraction leaves R-1 climbs; at -R 1024 that is a 0.1% bias, equal in
  both arms.)

  SINGLE-THREADED, AND THE BOX MUST BE IDLE.  This is a timing measurement;
  -T 1 throughout and no parallelism, unlike the recovery harness next door
  which can use every core because it measures recovery.
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
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(argv, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = os.path.join(HERE, os.pardir, "ngrams")
    p = subprocess.run([ENIGMA] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def timed(argv, text):
    t0 = time.perf_counter()
    run(argv, text)
    return time.perf_counter() - t0


def ci95(xs):
    if len(xs) < 2:
        return 0.0
    return 1.96 * statistics.stdev(xs) / sqrt(len(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixtures", type=int, default=40)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--length", type=int, default=100)
    ap.add_argument("--restarts", type=int, default=1024)
    ap.add_argument("--schedule", default="k4f10")
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=4242)
    ap.add_argument("--progress", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length
    rng = random.Random(args.seed)
    tail = ["-S", args.schedule, "-f", "-l", "wehrmacht", "-T", 1]
    # "ctl" re-times -J on the same fixture: its spread is the noise floor.
    arms = (("J", "-J"), ("K", "-K"), ("ctl", "-J"))

    ratios, controls, tj_all = [], [], []
    for f in range(args.fixtures):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(args.plugs))
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)
        if len(ct) != L:
            continue

        best = {a: [1e9, 1e9] for a, _ in arms}      # [at -R N, at -R 0]
        for _ in range(args.reps):
            for a, flag in arms:
                argv = key + ["-c", flag] + tail
                t = timed(argv + ["-R", args.restarts], ct)
                best[a][0] = min(best[a][0], t)
                t0 = timed(argv + ["-R", 0], ct)
                best[a][1] = min(best[a][1], t0)

        tj = best["J"][0] - best["J"][1]
        tk = best["K"][0] - best["K"][1]
        tc = best["ctl"][0] - best["ctl"][1]
        if tj <= 0:
            continue
        ratios.append(tk / tj)
        controls.append(tc / tj)
        tj_all.append(tj)
        if args.progress:
            print(f"  fixture {f + 1}/{args.fixtures}: -J {tj:.3f}s  "
                  f"-K/-J {tk / tj:.3f}  ctl {tc / tj:.3f}", flush=True)

    n = len(ratios)
    if n < 2:
        sys.exit("too few usable fixtures")
    mr, mc = statistics.mean(ratios), statistics.mean(controls)
    hr, hc = ci95(ratios), ci95(controls)
    print(f"\n# -S {args.schedule}, -f -l wehrmacht, L={L}, -R "
          f"{args.restarts}, {args.plugs}-pair board, {n} fixtures x "
          f"{args.reps} reps, seed {args.seed}")
    print(f"# -J search time: mean {statistics.mean(tj_all):.3f}s "
          f"(min {min(tj_all):.3f}, max {max(tj_all):.3f})")
    print(f"-K / -J    {mr:.4f}  95% CI [{mr - hr:.4f}, {mr + hr:.4f}]"
          f"   = {100 * (mr - 1):+.1f}%  [{100 * (mr - hr - 1):+.1f}, "
          f"{100 * (mr + hr - 1):+.1f}]")
    print(f"-J / -J    {mc:.4f}  95% CI [{mc - hc:.4f}, {mc + hc:.4f}]"
          f"   = {100 * (mc - 1):+.1f}%  [{100 * (mc - hc - 1):+.1f}, "
          f"{100 * (mc + hc - 1):+.1f}]   <- noise floor")
    print(f"spread     -K/-J sd {statistics.stdev(ratios):.4f}, "
          f"control sd {statistics.stdev(controls):.4f}  "
          f"(excess = trajectory, not noise)")
    # Paired against the control: both are ratios to the same -J, so the
    # difference removes whatever that fixture's -J happened to cost.
    d = [a - b for a, b in zip(ratios, controls)]
    md, hd = statistics.mean(d), ci95(d)
    print(f"-K minus control: {md:+.4f}  95% CI [{md - hd:+.4f}, "
          f"{md + hd:+.4f}]  -> "
          f"{'ESTABLISHED' if md + hd < 0 else 'not established'}")


if __name__ == "__main__":
    main()
