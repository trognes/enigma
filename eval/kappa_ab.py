#!/usr/bin/env python3
"""Does the lagged-kappa term convert into BREAKS?  Paired end-to-end A/B.

    python3 eval/kappa_ab.py --trials 300 --lengths 40 60 100 --mu 0.3 1.0

WHY THIS RUN EXISTS.  The offline screen (eval/kappa_probe.py) passed under
both generator sets -- and so did X-structure's, which then INVERTED end to
end (eval/results-xstruct-ab.txt), because a climb under the augmented score
can game a term its decoys were never selected against.  Each arm here
searches WITH its own mu, so the term must survive a search that can attack
it: the only test that counts.

Unlike the X-gap model there is NO fitted table and nothing to hold out: the
per-lag null is the candidate's own expected match rate, computed from its
letter histogram.  The corpus supplies plaintexts only.

JUDGED ON BREAK50 with McNemar on the paired discordants, as CLAUDE.md
requires; mean %-correct and exact recovery secondary.
"""

import argparse
import math
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(argv, text, mu=0.0):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    if mu:
        env["ENIGMA_KAPPA"] = str(mu)
    p = subprocess.run([ENIGMA] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip()


def pct_correct(a, b):
    if not a or len(a) != len(b):
        return 0.0
    return 100.0 * sum(x == y for x, y in zip(a, b)) / len(a)


def mcnemar(only_a, only_b):
    n = only_a + only_b
    if n == 0:
        return 1.0
    s = sum(math.comb(n, k) for k in range(0, min(only_a, only_b) + 1))
    return min(1.0, 2.0 * s / (2 ** n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=300)
    ap.add_argument("--lengths", type=int, nargs="+", default=[40, 60, 100])
    ap.add_argument("--mu", type=float, nargs="+", default=[0.3, 1.0])
    ap.add_argument("--restarts", type=int, default=8)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--schedule", default="k4f10")
    ap.add_argument("--seed", type=int, default=21)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")
    msgs = (decrypts(os.path.join(HERE, "enigma-messages.txt"))
            + decrypts(os.path.join(HERE, "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)

    print(f"# -c -K --polish -S {args.schedule} -f -l wehrmacht "
          f"-R {args.restarts}, {args.plugs}-pair board hidden, key given")
    print(f"# score = shipped fused + mu * (top-3 lagged-kappa z, lags 1..30)")
    print(f"# JUDGED ON BREAK50; mean %-correct and exact are secondary\n")

    arms = [0.0] + list(args.mu)
    for L in args.lengths:
        pool = [m for m in msgs if len(m) >= L]
        res = {mu: {"b50": 0, "exact": 0, "mean": 0.0} for mu in arms}
        pairs = {mu: [0, 0] for mu in args.mu}
        n = 0
        for _ in range(args.trials):
            src = rng.choice(pool)
            off = rng.randrange(0, len(src) - L + 1)
            pt = src[off:off + L]
            w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
            r = "".join(rng.choice(LET) for _ in range(3))
            g = "".join(rng.choice(LET) for _ in range(3))
            ls = list(LET)
            rng.shuffle(ls)
            pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(args.plugs))
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct = run(key + ["-s", pb], pt)
            if len(ct) != L:
                continue
            crack = key + ["-c", "-K", "--polish", "-S", args.schedule,
                           "-f", "-l", "wehrmacht", "-R", args.restarts,
                           "-T", 1]
            n += 1
            got = {}
            for mu in arms:
                out = run(crack, ct, mu)
                p = pct_correct(out, pt)
                got[mu] = p >= 50.0
                res[mu]["b50"] += 1 if p >= 50.0 else 0
                res[mu]["exact"] += 1 if out == pt else 0
                res[mu]["mean"] += p
            for mu in args.mu:
                if got[0.0] and not got[mu]:
                    pairs[mu][0] += 1
                elif got[mu] and not got[0.0]:
                    pairs[mu][1] += 1

        print(f"L={L}, {n} paired trials")
        print(f"  {'mu':>6} {'break50':>12} {'exact':>10} {'mean%':>8}"
              f"   {'discordant (only base / only mu)':>34}   p")
        for mu in arms:
            d = res[mu]
            if mu in pairs:
                a, b = pairs[mu]
                tag = f"{a:>14} / {b:<5}   {mcnemar(a, b):.3f}"
            else:
                tag = " " * 22 + "(baseline)"
            print(f"  {mu:>6.2f} {d['b50']:>6}/{n:<5} {d['exact']:>5}/{n:<4} "
                  f"{d['mean'] / max(n, 1):>7.1f}   {tag}")
        print()


if __name__ == "__main__":
    main()
