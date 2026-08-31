#!/usr/bin/env python3
"""Does the X-structure term convert into BREAKS?  Paired end-to-end A/B.

    python3 eval/xstruct_ab.py --trials 200 --lengths 40 60 --mu 30 100 300

WHY THIS RUN EXISTS.  The offline probe (eval/results-xstruct.txt) says the
term discriminates, but its decoys were produced by n-gram and IC models, none
of which optimises X structure -- so the term faced a pool never selected
against it and those numbers are an UPPER BOUND.  Only a climb UNDER the
augmented score can produce X-aware decoys, which is exactly what this does:
each arm searches with its own mu, so the term must survive a search that can
game it.  If a wrong board can be found whose X land at plausible word
lengths, this is where the gain disappears.

THE GAP TABLE IS FITTED ON A HELD-OUT HALF OF THE CORPUS and the plaintexts
are drawn from the other half.  Without that the model has seen the test
message, and the whole run measures memorisation.  The prototype used
leave-one-message-out for the same reason; a shipped table cannot do that, so
the split is the honest analogue.

JUDGED ON BREAK50 -- the number of trials recovering at least half the
plaintext -- as CLAUDE.md requires for a search or scoring change, with mean
%-correct and exact recovery reported as secondary.  break50 is a COUNT, so
the paired arms take McNemar directly.
"""

import argparse
import math
import os
import random
import re
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
XGAP_MAX = 40


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def gap_counts(msgs):
    h = Counter()
    for t in msgs:
        pos = [i for i, c in enumerate(t) if c == "X"]
        for a, b in zip(pos, pos[1:]):
            h[min(b - a - 1, XGAP_MAX)] += 1
    return h


def run(argv, text, mu=0.0, gaps=None):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    if mu:
        env["ENIGMA_XSTRUCT"] = str(mu)
        env["ENIGMA_XGAPS"] = gaps
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
    # exact two-sided binomial at p = 0.5
    s = sum(math.comb(n, k) for k in range(0, min(only_a, only_b) + 1))
    return min(1.0, 2.0 * s / (2 ** n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=200)
    ap.add_argument("--lengths", type=int, nargs="+", default=[40, 60])
    ap.add_argument("--mu", type=float, nargs="+", default=[30.0, 100.0, 300.0])
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

    # HELD-OUT SPLIT: fit the gap table on one half, draw plaintexts from the
    # other. The fit half is chosen by a fixed shuffle so it is reproducible.
    idx = list(range(len(msgs)))
    rng.shuffle(idx)
    fit_ids = set(idx[:len(idx) // 2])
    fit_msgs = [m for i, m in enumerate(msgs) if i in fit_ids]
    test_msgs = [m for i, m in enumerate(msgs) if i not in fit_ids]
    h = gap_counts(fit_msgs)
    gapfile = os.path.join("/tmp", f"xgaps-fit-{args.seed}.txt")
    with open(gapfile, "w") as f:
        for g in range(XGAP_MAX + 1):
            f.write(f"{g} {h.get(g, 0)}\n")

    print(f"# gap table fitted on {len(fit_msgs)} held-out messages "
          f"({sum(h.values())} gaps); plaintexts from the other "
          f"{len(test_msgs)}")
    print(f"# -c -K --polish -S {args.schedule} -f -l wehrmacht "
          f"-R {args.restarts}, {args.plugs}-pair board hidden, key given")
    print(f"# JUDGED ON BREAK50; mean %-correct and exact are secondary\n")

    arms = [0.0] + list(args.mu)
    for L in args.lengths:
        pool = [m for m in test_msgs if len(m) >= L]
        res = {mu: {"b50": 0, "exact": 0, "mean": 0.0} for mu in arms}
        pairs = {mu: [0, 0] for mu in args.mu}   # [only base, only this mu]
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
                out = run(crack, ct, mu, gapfile)
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
        print(f"  {'mu':>7} {'break50':>12} {'exact':>10} {'mean%':>8}"
              f"   {'discordant (only base / only mu)':>34}   p")
        for mu in arms:
            d = res[mu]
            tag = ""
            if mu in pairs:
                a, b = pairs[mu]
                tag = f"{a:>14} / {b:<5}   {mcnemar(a, b):.3f}"
            else:
                tag = " " * 22 + "(baseline)"
            print(f"  {mu:>7.0f} {d['b50']:>6}/{n:<5} {d['exact']:>5}/{n:<4} "
                  f"{d['mean'] / max(n, 1):>7.1f}   {tag}")
        print()


if __name__ == "__main__":
    main()
