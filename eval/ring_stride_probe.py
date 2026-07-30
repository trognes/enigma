#!/usr/bin/env python3
# Measures whether skip-K ring sampling on the RIGHTMOST wheel (wheel 2) still finds
# a candidate close enough to the true ring/start for cheap local refinement to
# recover the exact key -- using the real binary, not a toy simulation. Companion to
# the leftmost-wheel exact offset collapse (archived/PERFORMANCE.md 7.10, shipped); this
# measures the riskier, approximate half of the same user idea for the wheel that
# lacks that collapse's unconditional exactness (archived/PERFORMANCE.md 7.11).
#
# Method per trial:
#   - Random true key (reflector, wheel order, ring0/1/2, start0/1/2), a fixed
#     (given, not hill-climbed) plugboard, a real English excerpt of length L.
#   - Baseline (K=1): full wildcard search over ring1/start1/ring2/start2 (ring0
#     already collapses via the shipped left-wheel fix) with the TRUE plugboard
#     given via -s -- isolates rotor-key discrimination from plugboard search.
#   - Strided (K>1): run the SAME wildcard search once per candidate ring2 value
#     in {0, K, 2K, ...}, each with ring2 FIXED to that value (still wildcarding
#     ring1/start1/start2 fully) -- the max score across those runs is what a
#     stride-K coarse scan would report as best. Record its ring2/start2 vs truth.
#   - "recoverable" = the best stride candidate's ring2 is within stride/2 of the
#     true ring2 (so checking the skipped neighbors around it would find the
#     exact true key).
#
# Usage: python3 eval/ring_stride_probe.py
# Env: LENGTHS ("60 90 150"), KS ("2 3 4"), TRIALS (30), SEED (0)
import os
import random
import re
import subprocess
import statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
CORPUS = ("THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCE"
          "THANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHEN"
          "WEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNS"
          "REPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUREOFTHEMESSAGEEVENAFTERITHASBEEN"
          "ENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDINTHEWARHISTORIANSBELIEVETHATBREAKING"
          "THISCIPHERSHORTENEDTHECONFLICTBYSEVERALYEARSANDSAVEDCOUNTLESSLIVESANDCHANGEDTHE"
          "COURSEOFHISTORYFOREVERINWAYSNOONECOULDHAVEPREDICTEDATTHETIMEWHENTHEWARBEGAN")

SCORE_RE = re.compile(r'^\s*(-?\d+\.\d+)\s+\S+\s+\S+\s+(\S+)', re.M)

LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "60 90 150").split()]
KS = [int(x) for x in os.environ.get("KS", "2 3 4").split()]
TRIALS = int(os.environ.get("TRIALS", "30"))
SEED = int(os.environ.get("SEED", "0"))


def encrypt(args, pt):
    r = subprocess.run([BIN] + args, input=pt, capture_output=True, text=True, cwd=ROOT)
    return r.stdout.strip()


def run(args, ct):
    r = subprocess.run([BIN] + args, input=ct, capture_output=True, text=True, cwd=ROOT)
    best_score, best_start = None, None
    for m in SCORE_RE.finditer(r.stderr):
        sc = float(m.group(1))
        if best_score is None or sc > best_score:
            best_score, best_start = sc, m.group(2)
    return best_score, best_start


def random_key(rng):
    u = rng.choice("ABC")
    w = rng.sample(range(1, 6), 3)
    r = [rng.choice(ALPHA) for _ in range(3)]
    g = [rng.choice(ALPHA) for _ in range(3)]
    letters = list(ALPHA)
    rng.shuffle(letters)
    board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(9))
    return u, "".join(str(x) for x in w), "".join(r), "".join(g), board


def trial(L, K, rng):
    u, w, r, g, board = random_key(rng)
    off = rng.randrange(0, len(CORPUS) - L)
    pt = CORPUS[off:off + L]
    ct = encrypt(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", board], pt)
    # baseline: full wildcard ring1/start1/ring2/start2 (ring0 auto-collapses -- 7.10)
    base_args = ["-q", "-l", "english", "-u", u, "-w", w, "-r", "...",
                 "-g", "...", "-s", board, "-T", "1"]
    base_score, base_start = run(base_args, ct)
    true_ring2 = r[2]

    if K == 1:
        return dict(base_score=base_score, strided_score=base_score, exact=True, dist=0)

    cands = list(range(0, 26, K))
    results = []
    for ring2_num in cands:
        ring2 = ALPHA[ring2_num]
        args = ["-q", "-l", "english", "-u", u, "-w", w, "-r", "..%s" % ring2,
                "-g", "...", "-s", board, "-T", "1"]
        sc, start = run(args, ct)
        results.append((sc, ring2_num, start))
    best_score, best_ring2_num, best_start = max(results, key=lambda x: x[0])
    # dist = how far the STRIDE's tested ring2 is from true ring2 (mod 26, shortest)
    dist = min((best_ring2_num - ALPHA.index(true_ring2)) % 26,
               (ALPHA.index(true_ring2) - best_ring2_num) % 26)
    recoverable = dist <= K // 2
    return dict(base_score=base_score, strided_score=best_score,
                exact=(best_score >= base_score - 1e-9), dist=dist, recoverable=recoverable)


def sweep(Ls, Ks, n_trials, seed):
    print("%6s %4s %8s %10s %10s" % ("L", "K", "n", "recov%", "mean_dist"))
    for L in Ls:
        for K in Ks:
            rng = random.Random(seed * 1000 + L)   # same seed per L -> paired across K
            recov, dists = [], []
            for _ in range(n_trials):
                res = trial(L, K, rng)
                if K == 1:
                    continue
                recov.append(res["recoverable"])
                dists.append(res["dist"])
            if K == 1:
                print("%6d %4d %8d %10s %10s" % (L, K, n_trials, "n/a(base)", "n/a"))
            else:
                print("%6d %4d %8d %9.1f%% %10.2f"
                      % (L, K, n_trials, 100 * sum(recov) / len(recov), st.mean(dists)))


if __name__ == "__main__":
    sweep(LENGTHS, KS, TRIALS, SEED)
