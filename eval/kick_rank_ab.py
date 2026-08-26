#!/usr/bin/env python3
"""Paired A/B of --biased-random's ranking model: IC against k (mono + IC).

Both arms run the recommended telegraphic recipe on the same trial -- same
excerpt, same rotor key, same hidden 10-pair board, same --biased-random
temperature -- and differ only by $ENIGMA_KICK_RANK.  The rotor key is GIVEN,
so this measures the plugboard-recovery tier.

The offline probe (eval/results-kickrank.txt) says k ranks single plugs better
at every length; this asks whether that converts into breaks, which is a
different question and the one that decides the default.

Usage:
  python3 eval/kick_rank_ab.py --trials 600 --length 167 \\
      --restarts 1 2 3 4 5 --temp 1 --seed 1 --schedule k4f10

--schedule is the variable the REDUNDANCY hypothesis turns on: under k4f10 the
pre-pass and a k-ranked kick agree, so the kick may be adding nothing that the
pre-pass would not have found; under i4f10 the roles invert and IC becomes the
redundant one.  If redundancy is the mechanism, the sign of (k - IC) flips
between the two schedules.
"""

import argparse
import concurrent.futures
import os
import random
import re
import subprocess
import sys
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


def run(args, text, rank=None):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    if rank is not None:
        env["ENIGMA_KICK_RANK"] = rank
    p = subprocess.run([ENIGMA] + [str(a) for a in args], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip()


def mcnemar(only_a, only_b):
    n = only_a + only_b
    return (only_b - only_a) / sqrt(n) if n else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=600)
    ap.add_argument("--length", type=int, default=167)
    ap.add_argument("--restarts", type=int, nargs="+", default=[1, 2, 3, 4, 5])
    ap.add_argument("--temp", type=float, default=1.0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--schedule", default="k4f10",
                    help="--score schedule; the PRE-PASS half is the variable "
                         "the redundancy hypothesis turns on")
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=1,
                    help="trials in parallel. This measures BREAK COUNTS, not "
                         "timings, and every trial is deterministic given its "
                         "own inputs -- so unlike a bench harness this one is "
                         "free to use every core without corrupting anything. "
                         "Verified identical to --jobs 1.")
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length
    if len(corpus) < L + 1:
        sys.exit("corpus too short")
    print(f"# {len(corpus)} letters of authentic HG Nord decrypts, L = {L}, "
          f"-S {args.schedule}, {args.trials} trials/cell, "
          f"T = {args.temp}, seed {args.seed}", file=sys.stderr)

    base = ["-c", "-J", "--polish", "-f", "-l", "wehrmacht",
            "-S", args.schedule, "-T", args.threads, "-e", "7"]

    print(f"{'R':>3} {'IC':>6} {'k':>6} {'effect':>8} {'onlyIC/onlyK':>14} "
          f"{'z':>7}   mean%IC  mean%k")
    tot = {"i": 0, "k": 0}
    tot_only = [0, 0]

    def trial(spec):
        """One paired trial. Returns (broke_ic, broke_k, pct_ic, pct_k)."""
        pt, w, r, g, pb, R = spec
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct = run(key + ["-s", pb], pt)
        out = []
        for arm in ("i", "k"):
            o = run(key + base + ["-R", R, "--biased-random", args.temp],
                    ct, rank=arm)
            c = sum(a == b for a, b in zip(o, pt))
            out.append((2 * c >= L, 100.0 * c / L))
        return out[0][0], out[1][0], out[0][1], out[1][1]

    for R in args.restarts:
        rng = random.Random(args.seed)          # same trials at every R
        specs = []
        for _ in range(args.trials):
            pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
            w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
            r = "".join(rng.choice(LET) for _ in range(3))
            g = "".join(rng.choice(LET) for _ in range(3))
            ls = list(LET)
            rng.shuffle(ls)
            pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
            specs.append((pt, w, r, g, pb, R))

        if args.jobs > 1:
            # THREADS, not processes: every trial is subprocess-bound, so the
            # GIL is released while the binary runs, and a thread pool takes a
            # closure where multiprocessing.Pool cannot pickle one. map()
            # preserves order, though nothing here depends on it.
            with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
                res = list(ex.map(trial, specs))
        else:
            res = [trial(sp) for sp in specs]

        brk = {"i": 0, "k": 0}
        pct = {"i": 0.0, "k": 0.0}
        only_i = only_k = 0
        for gi, gk, pi, pk in res:
            brk["i"] += 1 if gi else 0
            brk["k"] += 1 if gk else 0
            pct["i"] += pi
            pct["k"] += pk
            if gi and not gk:
                only_i += 1
            if gk and not gi:
                only_k += 1

        tot["i"] += brk["i"]
        tot["k"] += brk["k"]
        tot_only[0] += only_i
        tot_only[1] += only_k
        eff = (100.0 * (brk["k"] - brk["i"]) / brk["i"]) if brk["i"] else 0.0
        print(f"{R:>3} {brk['i']:>6} {brk['k']:>6} {eff:>+7.1f}% "
              f"{str(only_i) + '/' + str(only_k):>14} "
              f"{mcnemar(only_i, only_k):>+7.2f}   "
              f"{pct['i'] / args.trials:6.1f}  {pct['k'] / args.trials:6.1f}",
              flush=True)

    eff = (100.0 * (tot["k"] - tot["i"]) / tot["i"]) if tot["i"] else 0.0
    print(f"\npooled {tot['i']} -> {tot['k']}  {eff:+.1f}%  "
          f"{tot_only[0]}/{tot_only[1]}  z = {mcnemar(*tot_only):+.2f}")


if __name__ == "__main__":
    main()
