#!/usr/bin/env python3
"""Tune -a's order weights and -f's IC weight ON WEHRMACHT specifically.

    python3 eval/weight_sweep.py decay   --target f --lengths 100 167
    python3 eval/weight_sweep.py refine  --target f --start 1,0.6,0.3,0.15
    python3 eval/weight_sweep.py lam     --start 1,0.6,0.3,0.15
    python3 eval/weight_sweep.py confirm --arms 1,0.6,0.3,0.15 1,0.9,0.5,0.3

WHY THIS EXISTS.  `-a`'s four log-linear weights (1, .6, .3, .15) were tuned
across four PROSE languages in PR #106 and have never been retuned for
telegraphic German, while `-f`'s lambda = 30 was at least *checked* against it.
CLAUDE.md's standing rule is that scoring results do not transfer between the
two writing styles, so the prose fit is an assumption everywhere it is used on
real traffic.

THE HYPOTHESIS BEING TESTED, not just the optimum.  `-a` plausibly wins by
SMOOTHING a defective quad table rather than by adding information -- which is
what archived/PERFORMANCE.md 6.4 already found for the mechanism ("it won by
smoothing the climb surface, not by lifting an information floor").  If so the
optimal mixture should track how bad the quad table is, and wehrmacht's is bad:
it is not counted at all but a REWEIGHTING of german's 80.1% support, with 843
quadgrams clipped at W_MAX holding ~68% of the mass.

  PRE-REGISTERED PREDICTION: wehrmacht's optimum sits at HIGHER low-order
  weights than the prose default.  If it lands at or below the default, the
  table-defect reading loses its main prediction here and should be recorded as
  such rather than quietly dropped.

DESIGN.  Paired break50 (>=50% of letters recovered) on authentic HG Nord
decrypts, plugboard tier -- rotor key given, 10-pair board hidden -- with the
recommended recipe, exactly the design of prepass_ab.py and jorder_ab.py.  Both
arms come from ONE binary via $ENIGMA_AW / $ENIGMA_IC_BLEND, so nothing differs
but the coefficients.  Every arm sees the identical trial (same excerpt, key and
board), and the run is deterministic under ENIGMA_SEED=0, so a busy box only
makes it slower.

  The baseline arm is run ONCE per (seed, length) and cached, since every cell
  is compared against the same one.  That halves the sweep and, more
  importantly, makes the pairing exact by construction.

  There is no "noise floor" control to run: the arms are deterministic, so
  baseline-against-itself has zero discordant pairs by construction.  The
  uncertainty here is trial sampling, which McNemar prices directly.

  THE WINNER'S CURSE IS THE REAL TRAP, and it is this file's version of the
  best-of-K problem `--confidence` documents.  The cell that wins a sweep is a
  MAXIMUM over many cells, so its measured gain is biased upward by selection
  and re-measures lower.  `confirm` therefore re-runs the finalists on a FRESH
  seed with more trials; only that number may be quoted as the effect.
"""

import argparse
import os
import random
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from math import comb

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
DEFAULT_W = (1.0, 0.6, 0.3, 0.15)
DEFAULT_LAM = 30.0


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


CORPUS = None


def load_corpus():
    global CORPUS
    if CORPUS is None:
        CORPUS = "".join(
            decrypts(os.path.join(HERE, "enigma-messages.txt"))
            + decrypts(os.path.join(HERE, "enigma-army-messages-1941.txt")))
    return CORPUS


def run(argv, text, env_extra=None):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    if env_extra:
        env.update(env_extra)
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
    return pt, key, run(key + ["-s", pb], pt)


def trial_fixture(seed, L, idx):
    """Deterministic per (seed, L, idx), so workers need no shared stream."""
    corpus = load_corpus()
    rng = random.Random(seed * 1000003 + L * 1009 + idx)
    while True:
        pt, key, ct = fixture(rng, corpus, L)
        if len(ct) == L:
            return pt, key, ct


def mcnemar(a, b):
    n = a + b
    if n == 0:
        return 1.0
    k = min(a, b)
    return min(1.0, 2 * sum(comb(n, i) for i in range(k + 1)) / 2 ** n)


def recipe(target, restarts):
    """The recommended recipe, with the target model swapped in.

    The k4 pre-pass is mono+IC and so is untouched by $ENIGMA_AW -- only the
    TARGET stage reads the all8 table.  That is what keeps the sweep a
    measurement of the coefficients rather than of the schedule.
    """
    sched = "k4a10" if target == "a" else "k4f10"
    return ["-c", "-K", "--polish", "-S", sched, "-" + target,
            "-l", "wehrmacht", "-T", "1", "-R", str(restarts)]


def one_trial(job):
    seed, L, idx, target, restarts, w, lam = job
    pt, key, ct = trial_fixture(seed, L, idx)
    env = {}
    if w is not None:
        env["ENIGMA_AW"] = ",".join(f"{x:g}" for x in w)
    if lam is not None:
        env["ENIGMA_IC_BLEND"] = f"{lam:g}"
    out = run(key + recipe(target, restarts), ct, env)
    pct = 100.0 * sum(a == b for a, b in zip(out, pt)) / L
    return pct >= 50.0


def arm(pool, seed, L, n, target, restarts, w, lam):
    jobs = [(seed, L, i, target, restarts, w, lam) for i in range(n)]
    return list(pool.map(one_trial, jobs, chunksize=8))


CACHE = {}


def baseline(pool, seed, L, n, target, restarts):
    k = (seed, L, n, target, restarts)
    if k not in CACHE:
        CACHE[k] = arm(pool, seed, L, n, target, restarts, None, None)
    return CACHE[k]


def score_cell(pool, args, w, lam, label):
    """Total breaks and discordants against the baseline, pooled over lengths."""
    tot = base_tot = only_c = only_b = 0
    per = []
    for L in args.lengths:
        b = baseline(pool, args.seed, L, args.trials, args.target,
                     args.restarts)
        c = arm(pool, args.seed, L, args.trials, args.target, args.restarts,
                w, lam)
        oc = sum(1 for x, y in zip(c, b) if x and not y)
        ob = sum(1 for x, y in zip(c, b) if y and not x)
        tot += sum(c)
        base_tot += sum(b)
        only_c += oc
        only_b += ob
        per.append(sum(c))
    wtxt = ",".join(f"{x:g}" for x in w) if w else "default"
    ltxt = f" lam {lam:g}" if lam is not None else ""
    print(f"  {label:<10} {wtxt:<22}{ltxt:<10} "
          f"{tot:>5} vs {base_tot:<5} ({tot - base_tot:+4})  "
          f"{only_c:>4}/{only_b:<4} p={mcnemar(only_c, only_b):.3f}  "
          f"{per}", flush=True)
    return tot, only_c, only_b


def geometric(r):
    return (1.0, r, r * r, r * r * r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stage", choices=["decay", "refine", "lam", "confirm"])
    ap.add_argument("--target", choices=["a", "f"], default="f")
    ap.add_argument("--lengths", type=int, nargs="+", default=[100, 167])
    ap.add_argument("--trials", type=int, default=300)
    ap.add_argument("--restarts", type=int, default=8)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--start", default="1,0.6,0.3,0.15")
    ap.add_argument("--lam", type=float, default=None,
                    help="fixed lambda for -f cells (default: the baked 30)")
    ap.add_argument("--arms", nargs="+", default=[],
                    help="confirm: weight vectors, optionally w0,w1,w2,w3:lam")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--step", type=float, default=0.025,
                    help="decay: resolution in r")
    ap.add_argument("--rmax", type=float, default=1.2,
                    help="decay: largest r (1 = all four orders equal)")
    args = ap.parse_args()
    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    start = tuple(float(x) for x in args.start.split(","))
    print(f"# -{args.target} coefficient sweep on wehrmacht, {args.stage}\n"
          f"# {args.trials} paired trials per length {args.lengths}, "
          f"-R {args.restarts}, seed {args.seed}\n"
          f"# baseline = the shipping row {DEFAULT_W}"
          f"{'' if args.target == 'a' else f' lam {DEFAULT_LAM:g}'}\n")
    print(f"  {'cell':<10} {'weights':<22}{'':<10} "
          f"{'breaks vs base':<22} {'only-c/only-b':<14} per-length")

    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        if args.stage == "decay":
            # One free parameter: w = (1, r, r^2, r^3).  A coarse LOCATOR, not
            # the answer -- the shipping row is not geometric (it halves after
            # the first step, r would have to be 0.6 then 0.5), so `refine`
            # relaxes all three afterwards.
            n = int(round(args.rmax / args.step)) + 1
            for i in range(n):
                r = round(i * args.step, 4)
                score_cell(pool, args, geometric(r), args.lam, f"r={r:g}")
        elif args.stage == "lam":
            for lam in [0, 5, 10, 15, 20, 25, 30, 40, 50, 65, 80, 100, 140]:
                score_cell(pool, args, start, float(lam), f"lam{lam}")
        elif args.stage == "refine":
            # Unconstrained coordinate descent on the three free weights.  w[0]
            # is pinned at 1 by scale invariance for -a; for -f the scale is
            # real, so `lam` must be swept again around any winner.
            cur = list(start)
            best = score_cell(pool, args, tuple(cur), args.lam, "start")[0]
            for rnd in range(args.rounds):
                for j in (1, 2, 3):
                    print(f"  -- round {rnd + 1}, axis {j}", flush=True)
                    for mult in (0.5, 0.7, 0.85, 1.2, 1.4, 2.0):
                        cand = list(cur)
                        cand[j] = round(cur[j] * mult, 4)
                        t = score_cell(pool, args, tuple(cand), args.lam,
                                       f"w{j}x{mult:g}")[0]
                        if t > best:
                            best, cur = t, cand
                    print(f"  == best so far {cur} ({best})", flush=True)
            print(f"\nRESULT {args.stage}: {cur} with {best} breaks")
        else:
            for spec in args.arms:
                if ":" in spec:
                    wtxt, ltxt = spec.split(":")
                    lam = float(ltxt)
                else:
                    wtxt, lam = spec, args.lam
                w = tuple(float(x) for x in wtxt.split(","))
                score_cell(pool, args, w, lam, "arm")


if __name__ == "__main__":
    main()
