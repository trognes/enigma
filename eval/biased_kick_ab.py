#!/usr/bin/env python3
"""Paired A/B of --biased-random against the uniform kick, on the SHIPPED
binary rather than a prototype.

Both arms run the recommended telegraphic recipe (-c -J --polish -f -l
wehrmacht -S k4f10) on the same trial -- same excerpt, same rotor key, same
hidden 10-pair board -- differing only by --biased-random.  The rotor key is
GIVEN, so this measures the plugboard-recovery tier; a real sweep is strictly
harder because the true key must also outscore its competitors.

R = 0 is reported as a baseline only.  With -R 0 there is a single
unperturbed climb and therefore no kick, so --biased-random is refused there
by design and the arms cannot differ.

Usage:
  python3 eval/biased_kick_ab.py --trials 200 --length 100 \\
      --restarts 0 1 2 3 4 5 6 7 8 9 10 --temp 1 --seed 1
"""

import argparse
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
    """The DECRYPT field only -- stop at the next FIELD: label, or EMENDED and
    TRANSLATION leak in and the 'corpus' stops being machine output."""
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(args, text):
    p = subprocess.run([ENIGMA] + [str(a) for a in args], input=text,
                       capture_output=True, text=True, check=False)
    return p.stdout.strip()


def mcnemar(only_a, only_b):
    n = only_a + only_b
    return (only_b - only_a) / sqrt(n) if n else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=200)
    ap.add_argument("--length", type=int, default=100)
    ap.add_argument("--restarts", type=int, nargs="+",
                    default=list(range(0, 11)))
    ap.add_argument("--temp", type=float, default=1.0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--threads", type=int, default=1)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length
    if len(corpus) < L + 1:
        sys.exit("corpus too short")
    print(f"# {len(corpus)} letters of authentic HG Nord decrypts, "
          f"L = {L}, {args.trials} trials/cell, T = {args.temp}, "
          f"seed {args.seed}", file=sys.stderr)

    base = ["-c", "-J", "--polish", "-f", "-l", "wehrmacht", "-S", "k4f10",
            "-T", args.threads, "-e", "7"]

    print(f"{'R':>3} {'uniform':>8} {'biased':>8} {'effect':>8} "
          f"{'onlyU/onlyB':>13} {'z':>7}   mean%U  mean%B")
    for R in args.restarts:
        rng = random.Random(args.seed)          # same trials at every R
        brk = {"u": 0, "b": 0}
        pct = {"u": 0.0, "b": 0.0}
        only_u = only_b = 0
        for _ in range(args.trials):
            pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
            w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
            r = "".join(rng.choice(LET) for _ in range(3))
            g = "".join(rng.choice(LET) for _ in range(3))
            ls = list(LET)
            rng.shuffle(ls)
            pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct = run(key + ["-s", pb], pt)

            got = {}
            for arm in ("u", "b"):
                extra = [] if (arm == "u" or R < 1) else \
                        ["--biased-random", args.temp]
                out = run(key + base + ["-R", R] + extra, ct)
                c = sum(a == b for a, b in zip(out, pt))
                pct[arm] += 100.0 * c / L
                got[arm] = (2 * c >= L)
                if got[arm]:
                    brk[arm] += 1
            if got["u"] and not got["b"]:
                only_u += 1
            if got["b"] and not got["u"]:
                only_b += 1

        eff = (100.0 * (brk["b"] - brk["u"]) / brk["u"]) if brk["u"] else 0.0
        print(f"{R:>3} {brk['u']:>8} {brk['b']:>8} {eff:>+7.1f}% "
              f"{str(only_u) + '/' + str(only_b):>13} "
              f"{mcnemar(only_u, only_b):>+7.2f}   "
              f"{pct['u'] / args.trials:6.1f}  {pct['b'] / args.trials:6.1f}",
              flush=True)


if __name__ == "__main__":
    main()
