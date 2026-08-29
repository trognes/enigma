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
    ap.add_argument("--skip-ladder", type=int, nargs="+", default=None,
                    help="report the skip rate at each -R over --trials keys. "
                         "This is the quantity that decides everything: the "
                         "saving is ~0.53x the skip rate, so a budget where "
                         "dedup rarely fires cannot fund extra restarts.")
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

    if args.skip_ladder:
        specs = make_specs(corpus, L, args.trials, args.seed)
        cases = []
        for pt, w, r, g, pb in specs:
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct, _ = run(key + ["-s", pb], pt)
            cases.append((key, ct))
        print(f"# skip rate, L={L}, -S {args.schedule} --random {args.kick}, "
              f"{len(cases)} keys")
        print(f"{'R':>7} {'skipped':>9} {'of seeds':>9} {'skip%':>7} "
              f"{'saving':>8} {'affords':>9}")
        for R in args.skip_ladder:
            sk = tot = 0
            for key, ct in cases:
                _, err = climb(key, ct, R, True)
                m = re.search(r"Skipped (\d+) full climbs on duplicate seeds "
                              r"of (\d+)", err)
                if m:
                    sk += int(m.group(1))
                    tot += int(m.group(2))
            rate = sk / tot if tot else 0.0
            sav = 0.53 * rate          # the documented ~half-the-skip-rate rule
            print(f"{R:>7} {sk:>9} {tot:>9} {100.0 * rate:>6.1f}% "
                  f"{100.0 * sav:>7.1f}% {R / (1.0 - sav):>9.1f}")
        return

    if args.timing:
        # THE COST MUST BE A SLOPE, NOT A TOTAL.  ~0.105 s of every invocation
        # is process startup, which is 18% of a whole run at -R 1000 and 96%
        # at -R 10 -- so comparing totals understates the saving badly at high
        # -R and reports pure noise at low -R.  Fit time against R instead and
        # solve for the restart count arm B affords, startup included once.
        specs = make_specs(corpus, L, 6, args.seed)
        cases = []
        for pt, w, r, g, pb in specs:
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct, _ = run(key + ["-s", pb], pt)
            cases.append((key, ct))
        R = args.restarts
        ladder = sorted({0, R, 2 * R, 4 * R})
        print(f"# cost, L={L}, -S {args.schedule} --random {args.kick}, "
              f"{len(cases)} keys, min of 3 reps, single-threaded")
        print(f"{'dedup':>6} " + " ".join(f"{'R=' + str(x):>9}" for x in ladder)
              + f" {'us/restart':>11} {'skip@R':>8}")
        fit = {}
        for dedup in (False, True):
            times = []
            for x in ladder:
                best = None
                for _ in range(3):
                    t0 = time.perf_counter()
                    for key, ct in cases:
                        climb(key, ct, x, dedup)
                    t = time.perf_counter() - t0
                    best = t if best is None else min(best, t)
                times.append(best)
            n = len(ladder)
            mx = sum(ladder) / n
            my = sum(times) / n
            num = sum((a - mx) * (b - my) for a, b in zip(ladder, times))
            den = sum((a - mx) ** 2 for a in ladder)
            slope = num / den
            fit[dedup] = (slope, my - slope * mx)
            skip = ""
            if dedup:
                _, err = climb(cases[0][0], cases[0][1], R, True)
                m = re.search(r"Skipped (\d+) full climbs on duplicate seeds "
                              r"of (\d+) \(([0-9.]+)%\)", err)
                skip = (m.group(3) + "%") if m else "n/a"
            print(f"{'on' if dedup else 'off':>6} "
                  + " ".join(f"{t:9.3f}" for t in times)
                  + f" {slope * 1e6 / len(cases):11.1f} {skip:>8}")
        sa, ia = fit[False]
        sb, _ = fit[True]
        if sb > 0:
            # arm A's whole-run cost at R, solved for arm B's restart count
            cost_a = ia + sa * R
            rb = (cost_a - ia) / sb
            print(f"\nper-restart saving {100.0 * (1.0 - sb / sa):.1f}%  ->  "
                  f"arm B affords -R {rb:.1f} at arm A's -R {R} cost")
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
