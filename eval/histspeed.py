#!/usr/bin/env python3
"""How much the histogram-form pre-pass is worth, per schedule and per length.

Compares ENIGMA_HIST=1 against ENIGMA_HIST=0 on the SAME BINARY.  That is the
right instrument here and not merely the convenient one: the two arms run
identical code with one branch taken differently, so nothing is confounded by
code layout, and the climb's decisions are byte-identical by construction --
only the time changes.  Comparing against a binary built from an older commit
would fold in whatever the linker did that day, which is the ~18% hazard the
move loop is documented to have.

Timing is the per-restart SLOPE (-R 0 vs -R N), not a whole invocation: the
schedules have very different startup floors (-S m4f10 and -S k4f10 load 26
monogram numbers, -S i4f10 loads none, all three load the 0.45 MB all8 table),
and at a low -R the floor is most of the wall time.

--control adds a HIST=1 vs HIST=1 arm.  Run it: every non-zero number it
reports is pure measurement noise, and the earlier version of this measurement
had a +/-10% floor that made single cells meaningless.

Usage:
  python3 eval/histspeed.py --lengths 100 167 --restarts 400 --reps 5 --control
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


def run(args, text, hist=None):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    if hist is not None:
        env["ENIGMA_HIST"] = str(hist)
    p = subprocess.run([ENIGMA] + [str(a) for a in args], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def timed(args, text, reps, hist):
    """min wall over reps, plus the plugboards-scored count."""
    best, scored = 1e9, 0
    for _ in range(reps):
        t0 = time.perf_counter()
        _, err = run(args, text, hist)
        best = min(best, time.perf_counter() - t0)
        m = re.search(r"scored (\d+) plugboards", err)
        if m:
            scored = int(m.group(1))
    return best, scored


def slope(base, ct, sch, R, reps, hist):
    """us per restart and calls per restart, from the -R 0 / -R N difference."""
    t0, c0 = timed(base + ["-S", sch, "-R", 0], ct, reps, hist)
    t1, c1 = timed(base + ["-S", sch, "-R", R], ct, reps, hist)
    return (t1 - t0) / R * 1e6, (c1 - c0) / R


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lengths", type=int, nargs="+", default=[100, 167])
    ap.add_argument("--schedules", nargs="+",
                    default=["m4f10", "i4f10", "k4f10"])
    ap.add_argument("--restarts", type=int, default=400)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--control", action="store_true",
                    help="add a HIST=1 vs HIST=1 arm: pure noise, by design")
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)

    print(f"# ENIGMA_HIST=1 vs 0, same binary, -c -J -l wehrmacht, "
          f"-R {args.restarts}, min of {args.reps}, slope-fitted")
    print(f"{'L':>4} {'schedule':>8} {'off us/rs':>10} {'on us/rs':>9} "
          f"{'speedup':>8} {'calls/rs':>9}"
          + (f" {'control':>8}" if args.control else ""))
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
        base = key + ["-c", "-J", "-l", "wehrmacht", "-T", 1]

        for sch in args.schedules:
            us_off, calls = slope(base, ct, sch, args.restarts, args.reps, 0)
            us_on, _ = slope(base, ct, sch, args.restarts, args.reps, 1)
            row = (f"{L:>4} {sch:>8} {us_off:>10.1f} {us_on:>9.1f} "
                   f"{us_off / us_on:>7.2f}x {calls:>9.0f}")
            if args.control:
                us_c, _ = slope(base, ct, sch, args.restarts, args.reps, 1)
                row += f" {100 * (us_c / us_on - 1):>+7.1f}%"
            print(row, flush=True)


if __name__ == "__main__":
    main()
