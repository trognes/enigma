#!/usr/bin/env python3
"""Does --seed-dedup convert its compute saving into BREAKS?

This is the comparison SEED_DEDUP.md section 8 retired before it ran, and the
reason CLAUDE.md calls the distinct-seed figures "arithmetic": every shipped
measurement holds -R fixed and reports time saved, never spending the saving
on more restarts.

WHAT MAKES IT CLEAN.  --seed-dedup is answer-identical at a fixed -R -- the
climbs it skips are byte-identical duplicates -- so it changes price and
nothing else.  The quality question is therefore exactly "does -R N' beat
-R N", with dedup's whole job being to make N' cost what N costs.  Arm B
carries the flag anyway rather than simulating it with a bare higher -R,
because the point is to price the shipped feature.

SIZE THE RUN BEFORE SPENDING IT.  eval/results-tabu-probe.txt measured this
recipe at L=100: -R 100/148/1000/2040 -> 47/51/66/70 breaks per 100.  At
-R 1000 the skip rate is 43.5% and the saving runs ~0.53x the skip rate, so
matched cost buys ~1.30x the restarts, which on that ladder is worth about
+1.3pp.  An effect that small needs thousands of paired trials, and the mean
%-correct -- the graded, lower-variance signal -- will resolve it before the
break count does.

Usage:
  python3 eval/seed_dedup_ab.py --timing --length 100 --restarts 1000
  python3 eval/seed_dedup_ab.py --trials 2000 --length 100 \\
      --restarts 1000 --restarts-b 1300 --jobs 4
"""

import argparse
import concurrent.futures
import os
import random
import re
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


def run(args, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    p = subprocess.run([ENIGMA] + [str(a) for a in args], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def mcnemar(only_a, only_b):
    n = only_a + only_b
    return (only_b - only_a) / sqrt(n) if n else 0.0


def make_specs(corpus, L, trials, seed):
    rng = random.Random(seed)
    specs = []
    for _ in range(trials):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
        specs.append((pt, w, r, g, pb))
    return specs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=2000)
    ap.add_argument("--length", type=int, default=100)
    ap.add_argument("--restarts", type=int, default=1000)
    ap.add_argument("--restarts-b", type=int, default=0,
                    help="arm B's restart count; default = arm A's, i.e. the "
                         "control that must read zero since dedup is "
                         "answer-identical at fixed -R")
    ap.add_argument("--schedule", default="k4f10")
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--kick", type=int, default=10)
    ap.add_argument("--seed", type=int, default=17)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timing", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length

    def climb(key, ct, R, dedup):
        cmd = key + ["-c", "-J", "--polish", "-f", "-l", args.lang,
                     "-S", args.schedule, "-T", 1, "-e", "7",
                     "-R", R, "--random", args.kick]
        if dedup:
            cmd += ["--seed-dedup"]
        return run(cmd, ct)

    if args.timing:
        specs = make_specs(corpus, L, 6, args.seed)
        cases = []
        for pt, w, r, g, pb in specs:
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct, _ = run(key + ["-s", pb], pt)
            cases.append((key, ct))
        print(f"# whole-run cost, L={L}, -S {args.schedule} "
              f"--random {args.kick}, {len(cases)} keys, min of 3 reps, "
              f"single-threaded")
        got = {}
        for dedup in (False, True):
            best = None
            for _ in range(3):
                t0 = time.perf_counter()
                for key, ct in cases:
                    climb(key, ct, args.restarts, dedup)
                t = time.perf_counter() - t0
                best = t if best is None else min(best, t)
            got[dedup] = best
            print(f"  --seed-dedup {'on ' if dedup else 'off'}  "
                  f"{best:8.3f} s for {len(cases)} keys at "
                  f"-R {args.restarts}")
        # report the skip rate the run itself prints, so the saving can be
        # checked against the ~0.53x-of-skip-rate rule
        _, err = climb(cases[0][0], cases[0][1], args.restarts, True)
        m = re.search(r"Skipped (\d+) full climbs on duplicate seeds of "
                      r"(\d+) \(([0-9.]+)%\)", err)
        if m:
            print(f"  skip rate {m.group(3)}%  "
                  f"({m.group(1)} of {m.group(2)} seeds)")
        if got[True] > 0:
            sav = 1.0 - got[True] / got[False]
            print(f"\nsaving {100.0 * sav:.1f}% of wall time -> arm B affords "
                  f"-R {round(args.restarts / (1.0 - sav))} at arm A's cost")
        return

    rb = args.restarts_b or args.restarts
    specs = make_specs(corpus, L, args.trials, args.seed)
    print(f"# L={L}, {args.trials} trials, -S {args.schedule}, "
          f"--random {args.kick}, rotor key given, 10-pair board hidden; "
          f"A: -R {args.restarts} plain, B: -R {rb} --seed-dedup",
          file=sys.stderr)

    def trial(spec):
        pt, w, r, g, pb = spec
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)
        out = []
        for R, dedup in ((args.restarts, False), (rb, True)):
            o, _ = climb(key, ct, R, dedup)
            c = sum(x == y for x, y in zip(o, pt))
            out.append((2 * c >= L, 100.0 * c / L))
        return out

    if args.jobs > 1:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            res = list(ex.map(trial, specs))
    else:
        res = [trial(sp) for sp in specs]

    hit = {"A": 0, "B": 0}
    pct = {"A": 0.0, "B": 0.0}
    only_a = only_b = 0
    for a, b in res:
        hit["A"] += 1 if a[0] else 0
        hit["B"] += 1 if b[0] else 0
        pct["A"] += a[1]
        pct["B"] += b[1]
        if a[0] and not b[0]:
            only_a += 1
        if b[0] and not a[0]:
            only_b += 1

    n = len(res)
    print(f"{'arm':>4} {'restarts':>9} {'dedup':>6} {'>=50%':>12} "
          f"{'mean %correct':>14}")
    print(f"{'A':>4} {args.restarts:>9} {'off':>6} "
          f"{str(hit['A']) + '/' + str(n):>12} {pct['A'] / n:>14.1f}")
    print(f"{'B':>4} {rb:>9} {'on':>6} "
          f"{str(hit['B']) + '/' + str(n):>12} {pct['B'] / n:>14.1f}")
    z = mcnemar(only_a, only_b)
    se = 100.0 * sqrt(only_a + only_b) / n if (only_a + only_b) else 0.0
    print(f"\ndiscordant A/B {only_a}/{only_b}   z = {z:+.2f}   "
          f"effect {100.0 * (only_b - only_a) / n:+.2f}pp "
          f"+/- {1.96 * se:.2f}   (positive favours B)")


if __name__ == "__main__":
    main()
