#!/usr/bin/env python3
"""Does the 2-plug re-pair (try_repair) earn its keep at SHORT lengths?

    python3 eval/repair_ab.py --pilot 20 --trials 2000 --lengths 40 60 80 100

ENHANCEMENTS.md item 19.  CLAUDE.md's `--no-repair` entry calls the re-pair
"~zero cost" because it fires only at convergence, and says its value at
short lengths is unmeasured; the GPU probes then put try_repair plus the
outer loop at 16.9% of a natural climb at L~105
(eval/results-gpu-ablation-m1.txt run 2).  A GPU lane and a CPU core do
not price a convergence scan alike, so this measures the CPU directly, in
the unit the flag was built for: paired break50, rotor key given, board
hidden, the recommended recipe, authentic HG Nord decrypts.

THREE ARMS, because the question has two halves:

  on       the default, -R R
  off      --no-repair, -R R      the re-pair's QUALITY, restarts matched
  off+     --no-repair, -R R'     the re-pair's VALUE, wall time matched:
                                  R' = R x (cost of a climb with / without)

The cost ratio is measured, not assumed, by a pilot per length: the same
fixtures timed at two restart counts under each arm, the per-restart cost
being the slope -- which removes process startup exactly, where a single
timing at -R 8 is ~95% startup (CLAUDE.md, the -S k entry).  If `off+`
beats `on`, the re-pair costs more restarts than it is worth at that
length and the default is length-gated; if `on` beats `off+`, it earns its
keep there too.

Paired (the same fixture in every arm), deterministic (ENIGMA_SEED=0, the
fixture stream keyed on the trial index so the trials can run in
parallel), McNemar on the discordant pairs, mean %-correct as secondary.
"""

import argparse
import os
import random
import re
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
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
    return p.stdout.strip()


def fixture(rng, corpus, L):
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
    r = "".join(rng.choice(LET) for _ in range(3))
    g = "".join(rng.choice(LET) for _ in range(3))
    ls = list(LET)
    rng.shuffle(ls)
    pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
    key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    ct = run(key + ["-s", pb], pt)
    return pt, key, ct


def mcnemar(a, b):
    n = a + b
    if n == 0:
        return "p = 1.000 (no discordant pairs)"
    k = min(a, b)
    p = min(1.0, 2 * sum(comb(n, i) for i in range(k + 1)) / 2 ** n)
    return f"p = {p:.3f}"


CORPUS = None


def load_corpus():
    global CORPUS
    if CORPUS is None:
        CORPUS = "".join(
            decrypts(os.path.join(HERE, "enigma-messages.txt"))
            + decrypts(os.path.join(HERE, "enigma-army-messages-1941.txt")))
    return CORPUS


def trial_fixture(seed, L, idx):
    """Deterministic per (seed, L, idx), so workers need no shared stream."""
    corpus = load_corpus()
    rng = random.Random(seed * 1000003 + L * 1009 + idx)
    while True:
        pt, key, ct = fixture(rng, corpus, L)
        if len(ct) == L:
            return pt, key, ct


def pct(out, pt, L):
    return 100.0 * sum(a == b for a, b in zip(out, pt)) / L


def one_trial(job):
    seed, L, idx, R, Rp = job
    pt, key, ct = trial_fixture(seed, L, idx)
    base = key + RECIPE + ["-T", 1]
    on = pct(run(base + ["-R", R], ct), pt, L)
    off = pct(run(base + ["-R", R, "--no-repair"], ct), pt, L)
    offp = pct(run(base + ["-R", Rp, "--no-repair"], ct), pt, L)
    return on, off, offp


def pilot(seed, L, n, r_lo, r_hi, reps=5):
    """Per-restart cost with and without the re-pair: the slope between two
    restart counts on the same fixtures, min of `reps` each.

    MIN OF 2 WAS TOO THIN, and one bad timing cost a false positive.  The
    slope is a DIFFERENCE of two sums, so a single disturbance in the
    high-restart arm inflates it without bound -- and in the first full
    run it did: L=60 read 1.337 between neighbours at 1.046 and 1.033,
    its `on` cost exceeding both L=80's and L=100's, which is backwards
    for a climb linear in L.  Re-timing the identical fixtures twice gave
    1.069 and 1.099.  min of 5 costs ~60 s a length and buys the
    difference between a measurement and a coin flip.
    """
    fx = [trial_fixture(seed + 7777, L, i) for i in range(n)]
    cost = {}
    for flag in ([], ["--no-repair"]):
        secs = {}
        for R in (r_lo, r_hi):
            best = None
            for _ in range(reps):
                t0 = time.perf_counter()
                for _, key, ct in fx:
                    run(key + RECIPE + ["-T", 1, "-R", R] + flag, ct)
                t = time.perf_counter() - t0
                best = t if best is None else min(best, t)
            secs[R] = best
        cost["off" if flag else "on"] = (secs[r_hi] - secs[r_lo]) \
            / ((r_hi - r_lo) * n)
    return cost


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=2000)
    ap.add_argument("--lengths", type=int, nargs="+",
                    default=[40, 60, 80, 100])
    ap.add_argument("--restarts", type=int, default=8)
    ap.add_argument("--pilot", type=int, default=24,
                    help="fixtures per length for the cost ratio")
    ap.add_argument("--reps", type=int, default=5,
                    help="timing reps per pilot cell; min is kept")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args()
    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    print(f"# try_repair A/B: {' '.join(RECIPE)}\n"
          f"# 10 plugs hidden, rotor key given, {args.trials} paired "
          f"trials per length,\n# -R {args.restarts}, seed {args.seed}, "
          f"{args.workers} workers\n")

    # -R 8 against -R 128: a per-restart cost of ~0.2 ms needs thousands of
    # restarts per arm before the slope stands above the timer, and the
    # smoke test's 3 fixtures x 56 restarts (29 ms an arm) read the
    # re-pair as CHEAPER, which it cannot be -- it only ever adds work.
    print("cost: per-restart climb time in ms, the slope between -R 8 and")
    print(f"      -R 128 over {args.pilot} fixtures, min of {args.reps} reps")
    rlabel = "R' (off+)"
    print(f"  {'L':>4} {'on':>8} {'off':>8} {'ratio':>7} {rlabel:>10}")
    ratio = {}
    for L in args.lengths:
        c = pilot(args.seed, L, args.pilot, 8, 128, args.reps)
        r = c["on"] / c["off"] if c["off"] > 0 else 1.0
        rp = max(1, round(args.restarts * r))
        ratio[L] = (r, rp)
        flag = ""
        if r < 1.0:
            # The re-pair only ever ADDS work, so this is the timer
            # winning, not a saving.  Say so rather than build the
            # matched-time arm on it.
            flag = "  <- BELOW 1.0: timing noise, not a cost"
        elif rp == args.restarts:
            flag = "  <- rounds to no extra restart; off+ == off"
        print(f"  {L:>4} {1000 * c['on']:8.3f} {1000 * c['off']:8.3f} "
              f"{r:7.3f} {rp:>10}{flag}")
    print()

    print("recovery: paired break50 (>=50% of letters), the same fixture in "
          "every arm")
    print(f"  {'L':>4} {'on':>9} {'off':>9} {'off+':>9}  "
          f"{'on-only/off-only':>16} {'on-only/off+-only':>17}  "
          f"mean%: on/off/off+")
    for L in args.lengths:
        r, rp = ratio[L]
        jobs = [(args.seed, L, i, args.restarts, rp)
                for i in range(args.trials)]
        with ProcessPoolExecutor(max_workers=args.workers) as ex:
            res = list(ex.map(one_trial, jobs, chunksize=8))
        n = len(res)
        b = [sum(1 for t in res if t[k] >= 50) for k in range(3)]
        m = [sum(t[k] for t in res) / n for k in range(3)]
        on_off = (sum(1 for t in res if t[0] >= 50 > t[1]),
                  sum(1 for t in res if t[1] >= 50 > t[0]))
        on_offp = (sum(1 for t in res if t[0] >= 50 > t[2]),
                   sum(1 for t in res if t[2] >= 50 > t[0]))
        print(f"  {L:>4} {b[0]:>4}/{n:<4} {b[1]:>4}/{n:<4} {b[2]:>4}/{n:<4}  "
              f"{on_off[0]:>6}/{on_off[1]:<9} {on_offp[0]:>7}/{on_offp[1]:<9}"
              f"  {m[0]:.1f}/{m[1]:.1f}/{m[2]:.1f}")
        print(f"       restarts matched: {mcnemar(*on_off)};  wall matched "
              f"(off+ at -R {rp}): {mcnemar(*on_offp)}")
    print("\nRead: `off` against `on` is what the re-pair is worth in breaks at"
          "\nequal restarts; `off+` against `on` is whether those breaks cost"
          "\nmore restarts than they buy.  off+ ahead means the default is"
          "\nlength-gated.")


if __name__ == "__main__":
    main()
