#!/usr/bin/env python3
"""Does --int change what the climb converges to?  Two tests, one binary.

    python3 eval/intscore_ab.py --identity 30 --trials 2000 \
            --lengths 60 107 167

--int compares the climb's scores as exact 64-bit integer keys instead of
doubles (metal/DESIGN.md 3a).  The ordering can differ from the default's
only for near-ties inside B's rounding, ~1e-12 relative, so the expectation
is that NOTHING changes -- and that is the measurement, not the claim.

1. IDENTITY (the direct test).  For N fixtures per length -- a random
   excerpt of authentic HG Nord plaintext, a random rotor key, a random
   10-pair board -- run the recommended recipe at -R 64 with --dump-all,
   with and without --int, and compare the converged (score, board) of
   EVERY restart.  A single differing restart is a decision the two
   arithmetics made differently.  Also compares the stdout decrypt and the
   plugboards-scored counter.

2. RECOVERY (the test decision 1 cares about).  Paired break50 of --int
   against the default, rotor key given, board hidden, -R 8, the design of
   eval/prepass_ab.py; McNemar on the discordant pairs.  If test 1 passes
   with zero differences this cannot differ, and running it anyway is what
   turns "cannot" into "did not".

Compute is matched by construction: the same trials, the same restarts, the
same binary.  Deterministic (ENIGMA_SEED=0), so a busy box only slows it.
"""

import argparse
import os
import random
import re
import subprocess
import sys
from math import comb

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-K", "--polish", "-S", "k4f10", "-f", "-l", "wehrmacht"]


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
    env["ENIGMA_DATA"] = NGRAMS
    p = subprocess.run([ENIGMA] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def dumps(stderr):
    return sorted(ln for ln in stderr.splitlines() if ln.startswith("dumpall "))


def scored(stderr):
    m = re.search(r"scored (\d+) plugboards", stderr)
    return int(m.group(1)) if m else -1


def fixture(rng, corpus, L):
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
    r = "".join(rng.choice(LET) for _ in range(3))
    g = "".join(rng.choice(LET) for _ in range(3))
    ls = list(LET)
    rng.shuffle(ls)
    pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
    key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    ct, _ = run(key + ["-s", pb], pt)
    return pt, key, ct


def mcnemar(a, b):
    n = a + b
    if n == 0:
        return "no discordant pairs"
    k = min(a, b)
    p = min(1.0, 2 * sum(comb(n, i) for i in range(k + 1)) / 2 ** n)
    return f"McNemar p = {p:.3f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--identity", type=int, default=30,
                    help="fixtures per length for the dump-all comparison")
    ap.add_argument("--restarts", type=int, default=64)
    ap.add_argument("--trials", type=int, default=2000,
                    help="paired break50 trials per length (0 = skip)")
    ap.add_argument("--lengths", type=int, nargs="+", default=[60, 107, 167])
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args()
    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)
    print(f"# --int vs default, one binary, {RECIPE}, 10 plugs, rotor key "
          f"given\n")

    # ---- 1. identity -------------------------------------------------------
    print(f"identity: {args.identity} fixtures per length, -R {args.restarts}, "
          f"every restart's converged (score, board) compared via --dump-all")
    for L in args.lengths:
        rows = 0
        diff_rows = 0
        diff_out = 0
        diff_cnt = 0
        n = 0
        for _ in range(args.identity):
            pt, key, ct = fixture(rng, corpus, L)
            if len(ct) != L:
                continue
            base = key + RECIPE + ["-R", args.restarts, "--dump-all",
                                   "-T", args.threads]
            o0, e0 = run(base, ct)
            o1, e1 = run(base + ["--int"], ct)
            d0, d1 = dumps(e0), dumps(e1)
            n += 1
            rows += len(d0)
            if d0 != d1:
                diff_rows += sum(1 for a, b in zip(d0, d1) if a != b) \
                             + abs(len(d0) - len(d1))
            if o0 != o1:
                diff_out += 1
            if scored(e0) != scored(e1):
                diff_cnt += 1
        print(f"  L={L:>3}: {n} fixtures, {rows} restarts compared: "
              f"{diff_rows} differing restarts, {diff_out} differing "
              f"decrypts, {diff_cnt} differing plugboards-scored counts")
    print()

    # ---- 2. recovery -------------------------------------------------------
    if args.trials <= 0:
        return
    print(f"recovery: paired break50, {args.trials} trials per length, -R 8")
    print(f"  {'L':>4} {'default':>8} {'--int':>8} {'only-def':>9} "
          f"{'only-int':>9}  verdict")
    for L in args.lengths:
        b0 = b1 = od = oi = 0
        n = 0
        for _ in range(args.trials):
            pt, key, ct = fixture(rng, corpus, L)
            if len(ct) != L:
                continue
            base = key + RECIPE + ["-R", 8, "-T", 1]
            o0, _ = run(base, ct)
            o1, _ = run(base + ["--int"], ct)
            n += 1
            r0 = 100.0 * sum(a == b for a, b in zip(o0, pt)) / L >= 50
            r1 = 100.0 * sum(a == b for a, b in zip(o1, pt)) / L >= 50
            b0 += r0
            b1 += r1
            od += r0 and not r1
            oi += r1 and not r0
        print(f"  {L:>4} {b0:>4}/{n:<3} {b1:>4}/{n:<3} {od:>9} {oi:>9}  "
              f"{mcnemar(od, oi)}")


if __name__ == "__main__":
    main()
