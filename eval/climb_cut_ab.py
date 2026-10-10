#!/usr/bin/env python3
"""Do cheaper restarts break more messages?  Two ways to cut a climb, A/B'd.

    git apply eval/climb_cut.patch && make && cp enigma /tmp/enigma-cut
    git checkout src && make
    python3 eval/climb_cut_ab.py --binary /tmp/enigma-cut

eval/results-probe-count.txt found that 45-47% of the decoding probes in a
restart's target stage only certify that the climb has converged, and that
another ~42% are spent before the last improving move.  eval/climb_cut.patch
adds two measurement-only switches, both off by default (off is byte-identical
to dev):

  ENIGMA_STALE_CAP=k  end a target-stage first-improvement sweep after k
                      consecutive non-improving moves instead of a full cycle
                      of 325.  Saves certificate probes; forfeits the
                      improvements that arrive later (12.3% of them after 100+
                      failures, ~6% after 200+).
  ENIGMA_RERANK=1     under -K, re-rank the remaining moves by IC after every
                      accepted move instead of walking the start-of-climb
                      order circularly.  Keeps the full certificate; aims at
                      the live probes.

Both change the climb's path, so neither can be judged on speed: a cheaper
restart is only worth having if more of them break more messages.  So:

  1. CALIBRATE.  Wall time per restart for each arm against the baseline, on
     the true key, with process startup subtracted (t at -R HI minus t at
     -R LO).  An arm that costs ratio r gets round(R / r) restarts.
  2. A/B.  Rotor key given, 10-pair board hidden, authentic HG Nord plaintext
     (the plugboard tier every tuning result here measures).  Each arm runs
     at the baseline's R -- the quality cost per restart -- and at its
     matched-time R, which is the comparison that decides.  Paired: the same
     ciphertext and the same kick seed per trial in every arm.  Judged on
     break50 (>= 50% of the letters), McNemar against the baseline.

Recipe: -c -S k4f10 -K --polish -l wehrmacht.  Nothing here uses hgnord.

MEASURED (1000 paired trials per length, baseline -R 16, results in
eval/results-climb-cut.txt).  break50 against the baseline's 275 / 535 / 915
at L = 80 / 107 / 167, McNemar z (negative = the arm loses):

                 cost per    same R              matched wall time
                 restart     80     107    167   80     107    167
    cap100       0.64-0.71  -2.96  -3.71  -0.82  -0.79  +0.69  +1.81
    cap200       0.74-0.89  -3.12  -3.39  -0.94  -2.02  +0.24  +2.41
    rerank       1.54-1.99  +0.09  +0.53  -1.21  -5.13  -4.30  -6.18

RERANK IS MEASURED DOWN.  It leaves the quality of a restart where it was
(every same-R cell inside |z| < 1.3) and costs 1.5-2x the time, because
re-scoring all 325 moves after each of the ~7 accepted moves costs about as
much as the decodes it could save.  At matched time it loses at every length.

THE CAP IS A WASH, NOT A WIN.  Capping the failed-probe run costs quality per
restart (z -3 to -3.7 at L = 80 and 107, at the same R), and the ~30% cheaper
restart buys about that much back: matched-time cells read -2.02 to +2.41.
The sign follows length -- losing at 80, level at 107, ahead at 167 -- and
the one cell past 2 (cap200 at L=167, +13 breaks of 1000 on a 91.5% base) is
one of six matched-time cells on one seed.  CLAUDE.md records two single-seed
cells past |z| = 3 that did not replicate; this is not evidence to build on.
The calibration also scatters (cap200's cost ratio reads 0.89 at L=80 and
0.74 at L=167 on similar work), which moves the matched-time R by a restart
or two.

REPLICATED at L=167 (eval/results-climb-cut-replicate.txt, seed 41, 3000
trials, 24 calibration fixtures): cap200 at the calibrated -R 21 breaks 2761
against 2729, z = +2.97; pooled with the first seed z = +3.74, ~+1.1pp.
L = 80 / 107 and wrong-key cost ratios are still unmeasured at that care.

THEN MEASURED (eval/results-dontlook.txt, seed 59, 3000 trials per length,
24-fixture calibration), with don't-look bits (ENIGMA_DONTLOOK=1) beside the
cap.  Matched-time z at L = 80 / 107 / 167: cap200 +0.30 / +2.61 / +3.40,
dontlook +0.86 / +2.70 / +1.71.  The cap's earlier L=80 loss was its
8-fixture calibration (0.89 against 0.80 here).  Wrong-key cost ratios match
the true-key ones (eval/results-climb-cut-wrongkey.txt).
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
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-S", "k4f10", "-K", "--polish", "-l", "wehrmacht",
          "-T", "1"]
ARMS = {"base": {},
        "cap100": {"ENIGMA_STALE_CAP": "100"},
        "cap200": {"ENIGMA_STALE_CAP": "200"},
        "rerank": {"ENIGMA_RERANK": "1"},
        "dontlook": {"ENIGMA_DONTLOOK": "1"}}


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(binary, env_extra, args, text):
    env = dict(os.environ, **env_extra)
    env.pop("ENIGMA_SEED", None)
    p = subprocess.run([binary] + args, input=text, capture_output=True,
                       text=True, check=False, env=env)
    return p.stdout.strip()


def fixture(corpus, L, seed):
    rng = random.Random(seed)
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    w = "".join(map(str, rng.sample([1, 2, 3, 4, 5], 3)))
    r = "".join(rng.choice(LET) for _ in range(3))
    g = "".join(rng.choice(LET) for _ in range(3))
    ls = list(LET)
    rng.shuffle(ls)
    pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
    key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    return pt, key, pb


def calibrate(binary, corpus, L, n, lo, hi, seed, arms):
    """Seconds per restart for each arm, startup subtracted, min of 2 reps,
    arm order rotated per fixture so drift is shared."""
    names = ["base"] + list(arms)
    per = {a: 0.0 for a in names}
    for f in range(n):
        pt, key, pb = fixture(corpus, L, seed + f)
        ct = run(binary, {}, key + ["-s", pb], pt)
        order = names[f % len(names):] + names[:f % len(names)]
        for a in order:
            best = None
            for _ in range(2):
                ts = []
                for R in (lo, hi):
                    t0 = time.perf_counter()
                    run(binary, ARMS[a], key + RECIPE
                        + ["-e", "1", "-R", str(R)], ct)
                    ts.append(time.perf_counter() - t0)
                d = (ts[1] - ts[0]) / (hi - lo)
                best = d if best is None else min(best, d)
            per[a] += best
    return {a: per[a] / per["base"] for a in names}


def trial(job):
    binary, L, seed, plan = job
    pt, key, pb = fixture(trial.corpus, L, seed)
    ct = run(binary, {}, key + ["-s", pb], pt)
    out = {"L": L, "seed": seed}
    for label, (arm, R) in plan.items():
        got = run(binary, ARMS[arm], key + RECIPE
                  + ["-e", str(seed), "-R", str(R)], ct)
        pct = sum(a == b for a, b in zip(got, pt)) / L
        out[label] = {"R": R, "pct": pct, "break": pct >= 0.5,
                      "exact": got == pt}
    return out


def init(corpus):
    trial.corpus = corpus


def mcnemar(a, b):
    return 0.0 if a + b == 0 else (a - b) / math.sqrt(a + b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--lengths", type=int, nargs="+", default=[80, 107, 167])
    ap.add_argument("--trials", type=int, default=1000)
    ap.add_argument("--restarts", type=int, default=16)
    ap.add_argument("--calib-fixtures", type=int, default=8)
    ap.add_argument("--calib-lo", type=int, default=64)
    ap.add_argument("--calib-hi", type=int, default=1024)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--seed", type=int, default=23)
    ap.add_argument("--arms", nargs="+", default=[a for a in ARMS
                                                  if a != "base"],
                    help="arms to run besides the baseline")
    ap.add_argument("--fixed", nargs="*", default=[],
                    help="extra ARM@R entries at a pinned restart count,"
                         " e.g. cap200@22 to replay an earlier cell")
    ap.add_argument("--out", default=os.path.join(
        HERE, "results-climb-cut.jsonl"))
    args = ap.parse_args()
    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(
                         HERE, "enigma-army-messages-1941.txt")))
    R = args.restarts
    report = []
    with open(args.out, "w", encoding="utf-8") as fh, \
            mp.Pool(args.workers, initializer=init,
                    initargs=(corpus,)) as pool:
        for L in args.lengths:
            ratio = calibrate(args.binary, corpus, L, args.calib_fixtures,
                              args.calib_lo, args.calib_hi,
                              args.seed * 7919 + L, args.arms)
            plan = {"base": ("base", R)}
            for a in args.arms:
                plan[a + "@R"] = (a, R)
                plan[a + "@time"] = (a, max(1, round(R / ratio[a])))
            for f in args.fixed:
                a, rr = f.split("@")
                plan[f] = (a, int(rr))
            fh.write(json.dumps({"L": L, "calibration": ratio,
                                 "plan": plan}) + "\n")
            jobs = [(args.binary, L, args.seed * 100003 + L * 1009 + t, plan)
                    for t in range(args.trials)]
            rs = list(pool.imap_unordered(trial, jobs, chunksize=4))
            for r in rs:
                fh.write(json.dumps(r) + "\n")
            fh.flush()
            report.append((L, ratio, plan, rs))
            print(f"  L={L} done", file=sys.stderr)

    print(f"recipe {' '.join(RECIPE)}; baseline -R {R}; break50 counts\n")
    for L, ratio, plan, rs in report:
        n = len(rs)
        base = sum(r["base"]["break"] for r in rs)
        print(f"L={L}  n={n}  base breaks {base} ({100 * base / n:.1f}%)")
        print("  wall time per restart vs base: "
              + ", ".join(f"{a} {ratio[a]:.3f}" for a in args.arms))
        print(f"  {'arm':<14}{'R':>5}{'breaks':>8}{'only-arm':>10}"
              f"{'only-base':>11}{'z':>7}{'exact':>7}{'mean%':>7}")
        for label, (arm, Rl) in plan.items():
            br = sum(r[label]["break"] for r in rs)
            a = sum(r[label]["break"] and not r["base"]["break"] for r in rs)
            b = sum(r["base"]["break"] and not r[label]["break"] for r in rs)
            ex = sum(r[label]["exact"] for r in rs)
            mean = 100 * sum(r[label]["pct"] for r in rs) / n
            print(f"  {label:<14}{Rl:>5}{br:>8}{a:>10}{b:>11}"
                  f"{mcnemar(a, b):>+7.2f}{ex:>7}{mean:>7.1f}")
        print()


if __name__ == "__main__":
    main()
