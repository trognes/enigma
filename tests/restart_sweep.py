#!/usr/bin/env python3
#
# Restart-perturbation sweep: how strong should each random restart be (the -S r
# token, k random plug pairs injected), and how many restarts (-R) before the gain
# flattens? Measured on the hard SHORT lengths (40, 70) with mean %-correct (a
# graded, lower-variance signal than exact recovery).
#
# Held fixed: the model schedule is the shipped IC->quad recipe (-S ...iq, IC
# uncapped then quad), so only the perturbation strength k and the restart count R
# vary. k = -1 is the legacy full random involution (no r token: a uniform 1..13
# pair board); k >= 0 injects exactly k random pairs from the seed each restart.
#
# Trials are generated identically to tests/crack_quality.py (same SEED, same
# random.Random(SEED*1000003+length) stream), so every (k, R) cell solves the
# IDENTICAL problems -- a properly paired design. Per-trial %-correct is retained
# so a paired t-test can compare small-k vs the legacy full involution at fixed R.
#
# Output: tests/restart_sweep.csv (k,R,len,mean,lo95,hi95,exact,n) plus a printed
# paired summary. Tunables (env): TRIALS, SEED, KS, RS, LENGTHS.

import os
import random
import statistics
import string
import subprocess
import sys

BIN = "./enigma"
CLANG = "english"
MODEL = "q"
PAIRS = 10

CORPUS = ("THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEX"
          "OFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMO"
          "REOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINC"
          "OMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUR"
          "EOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDI"
          "NTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERAL"
          "YEARSANDSAVEDCOUNTLESSLIVES")


def env(name, default):
    v = os.environ.get(name)
    return v if v not in (None, "") else default


TRIALS = int(env("TRIALS", "200"))
SEED = int(env("SEED", "1"))
KS = [int(x) for x in env("KS", "-1 0 1 2 3 4 8").split()]
RS = [int(x) for x in env("RS", "1 2 4 8 16 32").split()]
LENGTHS = [int(x) for x in env("LENGTHS", "40 70").split()]
OUT = env("OUT", "tests/restart_sweep.csv")


def run(args, text):
    p = subprocess.run([BIN] + args, input=text,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)
    return p.stdout, p.stderr


def encrypt(key, plain):
    u, w, r, g, pb = key
    out, _ = run(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", pb], plain)
    return out.strip()


def climb(key, ct, opts):
    u, w, r, g, _ = key
    args = ["-" + MODEL, "-l", CLANG, "-u", u, "-w", w, "-r", r, "-g", g, "-c"] + opts
    out, _ = run(args, ct)
    return out.strip()


def pct_correct(rec, truth):
    if not truth:
        return 0.0
    same = sum(1 for a, b in zip(rec, truth) if a == b)
    return 100.0 * same / len(truth)


def gen_trials(length):
    rng = random.Random(SEED * 1000003 + length)
    trials = []
    for _ in range(TRIALS):
        off = rng.randrange(len(CORPUS) - length + 1)
        excerpt = CORPUS[off:off + length]
        u = rng.choice("ABC")
        w = "".join(str(d) for d in rng.sample(range(1, 9), 3))
        r = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        g = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        letters = rng.sample(string.ascii_uppercase, 2 * PAIRS)
        pb = " ".join(letters[2 * i] + letters[2 * i + 1] for i in range(PAIRS))
        trials.append((excerpt, (u, w, r, g, pb)))
    return trials


def schedule_opts(k, R):
    sched = "iq" if k < 0 else ("r%diq" % k)
    return ["-S", sched, "-R", str(R)]


def ci95(xs):
    n = len(xs)
    if n < 2:
        return (0.0, 0.0)
    m = statistics.mean(xs)
    se = statistics.pstdev(xs) / (n ** 0.5)
    return (m - 1.96 * se, m + 1.96 * se)


def paired_t(a, b):
    """paired t-stat of a-b (per-trial)."""
    d = [x - y for x, y in zip(a, b)]
    n = len(d)
    m = sum(d) / n
    sd = statistics.pstdev(d)
    if sd == 0:
        return m, 0.0
    return m, m / (sd / (n ** 0.5))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    if not os.access(BIN, os.X_OK):
        sys.exit("error: %s not built; run 'make' first" % BIN)

    print("restart-perturbation sweep  trials=%d seed=%d  model=-%s lang=%s pairs=%d"
          % (TRIALS, SEED, MODEL, CLANG, PAIRS))
    print("schedule = -S {iq | r<k>iq} -R <R>   (IC uncapped -> quad)")
    print("KS=%s  RS=%s  LENGTHS=%s\n" % (KS, RS, LENGTHS))

    # per-length deterministic trial set, shared by every cell
    trials_by_len = {L: gen_trials(L) for L in LENGTHS}
    cts_by_len = {L: [(encrypt(k, e), e, k) for e, k in trials_by_len[L]] for L in LENGTHS}

    # results[(L,k,R)] = list of per-trial pcts
    results = {}
    rows = []
    print("%4s %4s %4s  %7s  %15s  %7s" % ("len", "k", "R", "mean%", "95% CI", "exact%"))
    for L in LENGTHS:
        for k in KS:
            for R in RS:
                opts = schedule_opts(k, R)
                pcts = [pct_correct(climb(key, ct, opts), e) for ct, e, key in cts_by_len[L]]
                results[(L, k, R)] = pcts
                mean = statistics.mean(pcts)
                lo, hi = ci95(pcts)
                exact = 100.0 * sum(1 for p in pcts if p >= 99.95) / len(pcts)
                rows.append((L, k, R, mean, lo, hi, exact, len(pcts)))
                klabel = "leg" if k < 0 else str(k)
                print("%4d %4s %4d  %7.2f  [%5.1f,%5.1f]  %7.2f"
                      % (L, klabel, R, mean, lo, hi, exact))
                sys.stdout.flush()

    with open(OUT, "w") as f:
        f.write("len,k,R,mean,lo95,hi95,exact,n\n")
        for L, k, R, mean, lo, hi, exact, n in rows:
            f.write("%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%d\n" % (L, k, R, mean, lo, hi, exact, n))
    print("\nwrote %s" % OUT)

    # paired: at each R, small-k vs legacy full involution (k=-1)
    if -1 in KS:
        print("\npaired (k vs legacy full-random), per length & R:  mean(k-leg)  paired-t")
        for L in LENGTHS:
            for R in RS:
                if R == 1:
                    continue  # R=1 has no restart -> all k identical
                base = results[(L, -1, R)]
                for k in KS:
                    if k < 0:
                        continue
                    md, t = paired_t(results[(L, k, R)], base)
                    flag = ""
                    if t > 2:
                        flag = " k>leg"
                    elif t < -2:
                        flag = " leg>k"
                    print("  L%-3d R%-3d k%-2d  %+6.2f   t=%+5.2f%s" % (L, R, k, md, t, flag))


if __name__ == "__main__":
    main()
