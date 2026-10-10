#!/usr/bin/env python3
"""Would pruning keys after the first few passes break more messages?

    python3 eval/key_prune_probe.py --lengths 80 107 167 --trials 60

THE IDEA.  The sweep is restart-major: pass 1 climbs every key once, pass 2
again with a fresh kick, and so on.  Today every key gets the same number of
restarts.  The alternative: after P passes, keep only the top fraction f of
keys by their best score so far, and spend the rest of the same budget on the
survivors -- each gets n = P + (B - P)/f restarts instead of B.

THE QUESTION, AND WHY IT IS NOT -F AGAIN.  Pruning only pays if the true key
survives the cut.  To survive it must beat a fraction 1-f of WRONG keys -- a
bar near 2-3 sd -- where a find must beat ALL of them, near 5-6 sd.  A climb
that recovered half the plugs may clear the first and not the second.  -F
tried this with a cheap, capped IC climb as the first tier and ranked the true
key badly below ~200 letters; here the first tier is the full recipe climb.

THE COST IT CAN HIDE.  Survivors are climbed n times instead of B, so the best
WRONG survivor climbs higher too, and the find bar rises with it.  Both arms
are therefore judged against their own bar:

  even     break = climb recovers the board within B restarts at the true
                   key, AND that score clears mu_B + sd_B * sqrt(2 ln K)
  pruned   break = true key survives the cut after P restarts, AND the climb
                   recovers within n, AND the score clears the survivors' bar
                   mu_n + sd_n * sqrt(2 ln (K f))

mu/sd come from wrong keys climbed on the SAME ciphertext: 676 keys at B
restarts for the even arm, and the sampled survivors deepened to n restarts
for the pruned one.  The Gaussian extrapolation is optimistic in the tail
(CLAUDE.md, --confidence), but it is applied to both arms alike.

"Recovers" means a board scoring at least the true board's score, under the
recommended recipe (-S k4f10 -K, -f via the k4f10 target, -l wehrmacht).
Nothing here uses hgnord: every corpus message is in its counts.

MEASURED DOWN (60 trials per length, B = 16, K = 2.47M, results in
eval/results-key-prune.txt).  Pruning never beats the even spread.  Breaks of
60, even against the best and the worst cut, with the paired McNemar z
(negative = pruning loses):

      L    even    best cut (P=8)          worst cut (P=1, f=1%)
     80     12     14 at f=10%,  z +0.82        3,  z -2.71
    107     30     29 at f=1-10%, z -0.33      11,  z -3.96
    167     51     55 at f=10%,  z +1.63       20,  z -5.40

Every cut after 1 or 2 passes loses, 17 of 18 at |z| > 2; after 4 passes
it still loses or ties; only after 8 -- half the budget already spent evenly --
does it reach a tie, and no cell resolves in its favour.

WHY.  The true key survives a cut about as often as its climb has ALREADY
recovered the board.  At L=167 after one pass it survives 26 of 60 at f=10%,
roughly what one climb's success rate plus the 10% chance keeps would give;
at L=80 it survives 12 of 60, against the 6 chance alone would keep.  A
climb that failed at the true key does not leave a half-right board scoring
above 90% of wrong keys -- it scores like one of them.  So the cut keeps the
messages that were solved anyway and discards the slow ones that more
restarts would have rescued, which is the failure the design anticipated.
-F's problem was not only its cheap first tier: the full recipe's partial
climbs carry no rankable signal either.
"""
import argparse
import json
import math
import multiprocessing as mp
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ENIGMA = os.path.join(ROOT, "enigma")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-S", "k4f10", "-K", "-l", "wehrmacht", "-T", "1"]
DUMP = re.compile(r"^dumpall \S+ \S+ (\S+) (-?\d+\.\d+)", re.M)

PASSES = (1, 2, 4, 8)         # P: passes before the cut
FRACS = (0.10, 0.03, 0.01)    # f: fraction of keys kept
EPS = 1e-6


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def enigma(args, text):
    env = dict(os.environ, ENIGMA_SEED="0")
    p = subprocess.run([ENIGMA] + args, input=text, capture_output=True,
                       text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def rows(args, ct):
    """(start, score) per converged climb, in restart order under -T 1."""
    _, err = enigma(args + ["--dump-all"], ct)
    return [(m.group(1), float(m.group(2))) for m in DUMP.finditer(err)]


def by_key(rs):
    keys = {}
    for start, s in rs:
        keys.setdefault(start, []).append(s)
    return keys


def stats(xs):
    mu = sum(xs) / len(xs)
    sd = math.sqrt(sum((x - mu) ** 2 for x in xs) / (len(xs) - 1))
    return mu, sd


def quantile(xs, q):
    ys = sorted(xs)
    return ys[min(len(ys) - 1, int(q * len(ys)))]


def trial(job):
    L, seed, B, K, ntrue = job
    rng = random.Random(seed)
    corpus = trial.corpus
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    perm = rng.sample([1, 2, 3, 4, 5], 3)
    w = "".join(map(str, perm))
    r = "".join(rng.choice(LET) for _ in range(3))
    g = "".join(rng.choice(LET) for _ in range(3))
    ls = list(LET)
    rng.shuffle(ls)
    pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
    free = "".join(ls[20:])
    ct, _ = enigma(["-u", "B", "-w", w, "-r", r, "-g", g, "-s", pb], pt)

    key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    tb = rows(key + RECIPE + ["-s", pb, "--no-plug", free, "-R", "0"], ct)
    true_score = tb[0][1]
    tr = [s for _, s in rows(key + RECIPE + ["-R", str(ntrue)], ct)]

    # wrong keys: a different wheel order, so none of them is the true key
    while True:
        wp = rng.sample([1, 2, 3, 4, 5], 3)
        if wp != perm:
            break
    ww = "".join(map(str, wp))
    wr = "".join(rng.choice(LET) for _ in range(3))
    wg = rng.choice(LET) + ".."
    wkey = ["-u", "B", "-w", ww, "-r", wr]
    wrong = by_key(rows(wkey + ["-g", wg] + RECIPE + ["-R", str(B)], ct))
    wrong = {k: v for k, v in wrong.items() if len(v) == B}

    best_b = [max(v) for v in wrong.values()]
    mu_b, sd_b = stats(best_b)
    bar_even = mu_b + sd_b * math.sqrt(2 * math.log(K))
    found_b = max(tr[:B])
    rec = {"L": L, "seed": seed, "true": true_score,
           "even_recover": found_b >= true_score - EPS,
           "even_break": found_b >= true_score - EPS and found_b > bar_even,
           "even_bar": bar_even, "cells": {}}

    # which wrong keys survive which cut, and how deep each must be climbed
    need = {}
    cuts = {}
    for P in PASSES:
        best_p = {k: max(v[:P]) for k, v in wrong.items()}
        for f in FRACS:
            thr = quantile(list(best_p.values()), 1 - f)
            surv = [k for k, s in best_p.items() if s > thr]
            n = P + int(round((B - P) / f))
            cuts[(P, f)] = (thr, surv, n)
            for k in surv:
                need[k] = max(need.get(k, 0), n)
    deep = {}
    for k, n in need.items():
        deep[k] = [s for _, s in rows(wkey + ["-g", k] + RECIPE
                                      + ["-R", str(n)], ct)]

    for (P, f), (thr, surv, n) in cuts.items():
        survived = max(tr[:P]) > thr
        found = max(tr[:n])
        recover = found >= true_score - EPS
        cell = {"survive": survived, "recover": recover, "n": n,
                "nsurv": len(surv), "break": False, "bar": None}
        if len(surv) >= 3:
            mu_n, sd_n = stats([max(deep[k][:n]) for k in surv])
            bar = mu_n + sd_n * math.sqrt(2 * math.log(K * f))
            cell["bar"] = bar
            cell["break"] = survived and recover and found > bar
        rec["cells"][f"P{P}_f{f}"] = cell
    return rec


def init(corpus):
    trial.corpus = corpus


def mcnemar(a_only, b_only):
    n = a_only + b_only
    return 0.0 if n == 0 else (a_only - b_only) / math.sqrt(n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lengths", type=int, nargs="+", default=[80, 107, 167])
    ap.add_argument("--trials", type=int, default=60)
    ap.add_argument("--budget", type=int, default=16,
                    help="B: restarts per key in the even arm")
    ap.add_argument("--keys", type=float, default=2.47e6,
                    help="K: keys in the sweep (BYQMZ's stride-3 sweep)")
    ap.add_argument("--true-restarts", type=int, default=2048)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--out", default=os.path.join(
        HERE, "results-key-prune.jsonl"))
    args = ap.parse_args()
    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")
    need_n = max(P + int(round((args.budget - P) / f))
                 for P in PASSES for f in FRACS)
    if args.true_restarts < need_n:
        sys.exit(f"--true-restarts must be at least {need_n}")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(
                         HERE, "enigma-army-messages-1941.txt")))
    jobs = [(L, args.seed * 100003 + L * 1009 + t, args.budget, args.keys,
             args.true_restarts)
            for L in args.lengths for t in range(args.trials)]
    results = []
    with mp.Pool(args.workers, initializer=init, initargs=(corpus,)) as pool, \
            open(args.out, "w", encoding="utf-8") as fh:
        for rec in pool.imap_unordered(trial, jobs):
            fh.write(json.dumps(rec) + "\n")
            fh.flush()
            results.append(rec)
            print(f"  done L={rec['L']} seed={rec['seed']}", file=sys.stderr)

    print(f"B={args.budget} restarts per key even, K={args.keys:.3g} keys")
    for L in args.lengths:
        rs = [r for r in results if r["L"] == L]
        n = len(rs)
        ev = sum(r["even_break"] for r in rs)
        er = sum(r["even_recover"] for r in rs)
        print(f"\nL={L}  n={n}  even: recover {er}/{n}  break {ev}/{n}")
        print(f"  {'P':>2} {'f':>5} {'n':>5} {'survive':>8} {'recover':>8}"
              f" {'break':>6} {'only-prune':>11} {'only-even':>10} {'z':>6}")
        for P in PASSES:
            for f in FRACS:
                c = [r["cells"][f"P{P}_f{f}"] for r in rs]
                sv = sum(x["survive"] for x in c)
                rc = sum(x["survive"] and x["recover"] for x in c)
                br = sum(x["break"] for x in c)
                a = sum(x["break"] and not r["even_break"]
                        for x, r in zip(c, rs))
                b = sum(r["even_break"] and not x["break"]
                        for x, r in zip(c, rs))
                print(f"  {P:>2} {f:>5.2f} {c[0]['n']:>5} {sv:>8} {rc:>8}"
                      f" {br:>6} {a:>11} {b:>10} {mcnemar(a, b):>+6.2f}")


if __name__ == "__main__":
    main()
