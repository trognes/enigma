#!/usr/bin/env python3
"""ENHANCEMENTS.md item 5(e): sweep the gate T and the bonus multiplier M.

5(e) proposes adding the doubling evidence to the score:

    gate    apply only to candidates with z > T
    bonus   M x [ 5.02 + 1.2*(L-6) ] decades, on the LONGEST doubling

and records four things a sweep has to fix, all of which drove this design:

  1. THE RISK SIDE NEEDS REAL HIGH-SCORING WRONG KEYS.  The earlier numbers
     came from 48 RANDOM wrong keys per message, with the dangerous tail
     reached by Gaussian extrapolation.  Here every trial is a genuine
     17576-key sweep with `-c --dump-all`, so the competitors ARE the
     top-scoring keys, and a chance doubling among them is observed rather
     than modelled.
  2. DO NOT CHOOSE AND VALIDATE ON THE SAME CASES.  Trials are split into
     train/test halves by index; the grid is read on train and the chosen
     cell reported on test.
  3. OPERATIONAL LENGTH IS UNTESTED.  L spans 60..200.
  4. THE INDEPENDENCE ASSUMPTION IS UNMEASURED.  The doubling rate is
     reported AS A FUNCTION OF z, rather than assumed flat.

Scenario: the day key is known and the start position is not -- the case
`MODERN_BREAKING_NOTES.md` 5j actually met, and the one where a 26^3 sweep is
affordable.  Candidates are the 17576 starts; the true key is one of them.

    python3 eval/doubling_bonus_sweep.py            # generate, then analyse
    REUSE=1 python3 eval/doubling_bonus_sweep.py    # re-analyse only
    TRIALS=40 LENS=60,140 python3 ...               # smaller run
"""
import json
import math
import os
import random
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref                                        # noqa: E402

DB = "eval/enigma-army-messages-1941.txt"
OUT = "eval/results-doubling-bonus-sweep.jsonl"
BIN = "./enigma"
TRIALS = int(os.environ.get("TRIALS", 60))
LENS = [int(x) for x in os.environ.get("LENS", "60,100,140,200").split(",")]
THREADS = os.environ.get("THREADS", "8")
RESTARTS = os.environ.get("RESTARTS", "8")
A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
# how many candidates to decrypt per trial: the top ones (which are the only
# ones a bonus could lift into the lead) plus a random sample used only for
# the rate-vs-z curve.
TOPN, SAMPLE = 3000, 1500


def corpus():
    out = []
    for b in re.split(r"\n### ", open(DB, encoding="utf-8").read())[1:]:
        m = re.search(r"DECRYPT:\s+((?:.|\n)*?)\n[A-Z]", b)
        if m:
            d = "".join(m.group(1).split())
            if "-" not in d and "[" not in d:
                out.append(d)
    return out


def longest_doubling(t, maxmm=1, maxlen=16, k=6):
    """Longest qualifying W X V, or 0.  Item 5's rule, longest match only."""
    best = 0
    n = len(t)
    for j in range(k, n - k):
        if t[j] != "X":
            continue
        for L in range(k, min(maxlen, j, n - j - 1) + 1):
            w, v = t[j - L:j], t[j + 1:j + 1 + L]
            if "X" in w or "X" in v:
                continue
            mm = 0
            for a, b in zip(w, v):
                if a != b:
                    mm += 1
                    if mm > maxmm:
                        break
            else:
                best = max(best, L)
    return best


def bonus(L, M):
    return 0.0 if L < 6 else M * (5.02 + 1.2 * (L - 6))


def generate():
    texts = corpus()
    rng = random.Random(20250816)
    trials = []
    with open(OUT, "w") as fh:
        for i in range(TRIALS):
            L = LENS[i % len(LENS)]
            # an excerpt CONTAINING a doubling -- the bonus is inert otherwise,
            # so conditioning here is what concentrates the sample where the
            # parameters actually act.  Report is conditional on that.
            for _ in range(400):
                t = rng.choice(texts)
                if len(t) < L:
                    continue
                s = rng.randrange(0, len(t) - L + 1)
                pt = t[s:s + L]
                if longest_doubling(pt) >= 6:
                    break
            else:
                continue
            wheels = "".join(rng.sample("12345", 3))
            ring = "".join(rng.choice(A) for _ in range(3))
            start = "".join(rng.choice(A) for _ in range(3))
            ls = rng.sample(A, 20)
            plugs = " ".join(ls[j] + ls[j + 1] for j in range(0, 20, 2))
            ct = subprocess.run(
                [BIN, "-u", "B", "-w", wheels, "-r", ring, "-g", start,
                 "-s", plugs], input=pt, capture_output=True,
                text=True).stdout.strip()

            env = dict(os.environ, ENIGMA_SEED="0")
            r = subprocess.run(
                [BIN, "-c", "-f", "-l", "wehrmacht", "-S", "i4f10", "-J",
                 "--polish", "-u", "B", "-w", wheels, "-r", ring, "-g", "...",
                 "-R", RESTARTS, "-T", THREADS, "--dump-all"],
                input=ct, capture_output=True, text=True, env=env)
            cand = []
            for line in r.stderr.split("\n") + r.stdout.split("\n"):
                if not line.startswith("dumpall "):
                    continue
                f = line.split()
                cand.append((f[3], float(f[4]), " ".join(f[5:])))
            if len(cand) < 17000:
                continue
            sc = [c[1] for c in cand]
            mu = sum(sc) / len(sc)
            sd = (sum((x - mu) ** 2 for x in sc) / len(sc)) ** 0.5
            order = sorted(range(len(cand)), key=lambda k: -cand[k][1])
            pick = set(order[:TOPN]) | set(rng.sample(range(len(cand)),
                                                      min(SAMPLE, len(cand))))
            rec = {"i": i, "L": L, "wheels": wheels, "ring": ring,
                   "start": start, "plugs": plugs, "pt": pt,
                   "mu": mu, "sd": sd, "n_cand": len(cand),
                   "true_dbl": longest_doubling(pt), "cands": []}
            for k in sorted(pick, key=lambda k: -cand[k][1]):
                st, s_, pl = cand[k]
                d = enigma_ref.decrypt(ct, wheels, ring, st, pl)
                c = {"g": st, "s": s_, "z": (s_ - mu) / sd,
                     "d": longest_doubling(d), "true": st == start}
                if c["true"]:
                    # did the CLIMB work here?  A true key whose plugboard was
                    # not recovered is a SEARCH failure and says nothing about
                    # the scoring question this sweep is about.
                    c["pct"] = 100.0 * sum(a == b for a, b in zip(d, pt)) / len(pt)
                rec["cands"].append(c)
            rec["pct"] = max([c.get("pct", 0.0) for c in rec["cands"]] + [0.0])
            if not any(c["true"] for c in rec["cands"]):
                # the true key did not even make the sampled set; record its
                # own score so the trial is still usable
                for st, s_, pl in cand:
                    if st == start:
                        d = enigma_ref.decrypt(ct, wheels, ring, st, pl)
                        rec["pct"] = 100.0 * sum(
                            a == b for a, b in zip(d, pt)) / len(pt)
                        rec["cands"].append(
                            {"g": st, "s": s_, "z": (s_ - mu) / sd,
                             "d": longest_doubling(d), "true": True,
                             "pct": rec["pct"]})
                        break
            fh.write(json.dumps(rec) + "\n")
            fh.flush()
            trials.append(rec)
            print("  trial %2d L=%-4d dbl=%-3d pct=%5.1f true z=%+6.2f rank %d"
                  % (i, L, rec["true_dbl"], rec["pct"],
                     [c["z"] for c in rec["cands"] if c["true"]][0],
                     1 + sum(1 for c in rec["cands"]
                             if c["s"] > [x["s"] for x in rec["cands"]
                                          if x["true"]][0])), flush=True)
    return trials


def load():
    return [json.loads(l) for l in open(OUT, encoding="utf-8")]


def wins(rec, T, M):
    """Does the true key end up strictly top after gate+bonus?"""
    best_true = best_other = None
    for c in rec["cands"]:
        s = c["s"] + (bonus(c["d"], M) / (rec["L"] - 3) if c["z"] > T else 0.0)
        if c["true"]:
            best_true = s if best_true is None else max(best_true, s)
        else:
            best_other = s if best_other is None else max(best_other, s)
    return best_true is not None and best_other is not None \
        and best_true > best_other


def main():
    if os.environ.get("REUSE") != "1" or not os.path.exists(OUT):
        print("generating %d trials, L in %s ..." % (TRIALS, LENS))
        generate()
    recs = load()
    print("\n%d trials, %d candidates each\n" % (len(recs), recs[0]["n_cand"]))

    print("4. DOUBLING RATE AS A FUNCTION OF z (the independence check)")
    band = {}
    for r in recs:
        for c in r["cands"]:
            if c["true"]:
                continue
            b = min(int(math.floor(c["z"])), 6)
            n, h = band.get(b, (0, 0))
            band[b] = (n + 1, h + (c["d"] >= 6))
    print("   %-10s %10s %8s %10s" % ("z band", "candidates", "hits", "rate"))
    for b in sorted(band):
        n, h = band[b]
        print("   %-10s %10d %8d %10.2e"
              % ("%+d..%+d" % (b, b + 1), n, h, h / n if n else 0))

    base = [r for r in recs if wins(r, 99, 0)]
    print("\n   baseline: true key already top in %d of %d trials"
          % (len(base), len(recs)))

    tr = [r for i, r in enumerate(recs) if i % 2 == 0]
    te = [r for i, r in enumerate(recs) if i % 2 == 1]
    print("\n1-3. GRID on the TRAIN half (%d trials); test half held out\n"
          % len(tr))
    print("   %-6s %s" % ("", "  ".join("M=%-5g" % m for m in (1, 2, 3, 5, 8))))
    grid = {}
    for T in (0, 1, 2, 3, 4, 5):
        row = []
        for M in (1, 2, 3, 5, 8):
            w = sum(wins(r, T, M) for r in tr)
            grid[(T, M)] = w
            row.append("%-7d" % w)
        print("   T>%-4d %s" % (T, " ".join(row)))
    b0 = sum(wins(r, 99, 0) for r in tr)
    print("   %-6s %s" % ("none", "baseline %d of %d" % (b0, len(tr))))
    best = max(grid, key=lambda k: (grid[k], -k[1]))
    print("\n   best on train: T>%g M=%g -> %d of %d (baseline %d)"
          % (best[0], best[1], grid[best], len(tr), b0))
    print("   HELD-OUT test: %d of %d (baseline %d)"
          % (sum(wins(r, *best) for r in te), len(te),
             sum(wins(r, 99, 0) for r in te)))

    print("\n3. BY LENGTH, at the chosen cell")
    for L in sorted({r["L"] for r in recs}):
        sub = [r for r in recs if r["L"] == L]
        print("   L=%-5d baseline %2d of %-3d -> %2d of %-3d"
              % (L, sum(wins(r, 99, 0) for r in sub), len(sub),
                 sum(wins(r, *best) for r in sub), len(sub)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
