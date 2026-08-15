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
  3. OPERATIONAL LENGTH IS UNTESTED.  L spans 60..140.
  4. THE INDEPENDENCE ASSUMPTION IS UNMEASURED.  The doubling rate is
     reported AS A FUNCTION OF z, rather than assumed flat.

Scenario: the day key is known and the start position is not -- the case
`MODERN_BREAKING_NOTES.md` 5j actually met, and the one where a 26^3 sweep is
affordable.  Candidates are the 17576 starts; the true key is one of them.

THE TRUE KEY IS SCORED TWO WAYS, and the difference is the whole reason the
first pilot of this harness was uninformative:

  ORACLE   the true key with its TRUE plugboard -- the ceiling a perfect
           climb reaches.  Isolates SCORING from search: if the true
           plaintext still loses to a wrong key, no search effort can fix
           it, and this is the population 5(e) exists for.
  CLIMB    the true key with whatever board the sweep's own climb found at
           it, at the same budget every other candidate got.  The
           operational reading.

A pilot at `-R 8` produced neither population: at L>=140 the true key was
rank 1 of 17576 at z = +12.5 (nothing to rescue), and at L=60 its climb
recovered 5-15% of the letters (a search failure, which no plaintext-side
feature can touch).  Both arms are recorded here so a trial can be classified
rather than silently averaged into one number.

    python3 eval/doubling_bonus_sweep.py            # generate, then analyse
    REUSE=1 python3 eval/doubling_bonus_sweep.py    # re-analyse only
    TRIALS=40 LENS=60,100 python3 ...               # smaller run
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
OUT = os.environ.get("OUT", "eval/results-doubling-bonus-sweep.jsonl")
BIN = "./enigma"
TRIALS = int(os.environ.get("TRIALS", 60))
LENS = [int(x) for x in os.environ.get("LENS", "60,80,100,120,140").split(",")]
THREADS = os.environ.get("THREADS", "8")
RESTARTS = os.environ.get("RESTARTS", "8")
SEED = int(os.environ.get("SEED", 20250816))
A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
# how many candidates to decrypt per trial: the top ones (which are the only
# ones a bonus could lift into the lead) plus a random sample used only for
# the rate-vs-z curve.  The top slice is COMPLETE, so the high-z bands of that
# curve are exact and only the low-z bands are sampled.
TOPN = int(os.environ.get("TOPN", 3000))
SAMPLE = int(os.environ.get("SAMPLE", 1500))
GRID_T = (-9, 0, 1, 2, 3, 4, 5)
GRID_M = (1, 2, 3, 5, 8, 14)


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
    """The calibrated log-likelihood ratio, times M.  0 below the L=6 floor."""
    return 0.0 if L < 6 else M * (5.02 + 1.2 * (L - 6))


def alias_starts(wheels, start):
    """Grundstellungen that decode IDENTICALLY to `start`, itself excluded.

    The double-stepping anomaly makes two starts the same key: with the
    middle wheel ON its notch the first keypress steps the middle AND the
    left wheel, so (g0, N, g2) and (g0+1, N+1, g2) reach the same positions
    after one character and agree for the rest of the message.  It FAILS
    when the right wheel is also on its notch, where the two branches of
    the step rule diverge -- hence the guard.

    This matters because such a start is NOT a competitor: it is the true
    key under another name.  Left in the candidate list it ties the true
    key's score exactly, which (a) counts as a chance doubling when the
    plaintext carries a real one, and (b) makes a strict `>` win test
    report a loss on a trial the search actually got right.  Three of the
    first seven "chance doublings" seen here were this.

    Verified against an exhaustive 17576-start search: 0 mismatches.
    """
    w = [int(c) - 1 for c in wheels]
    g = [ord(c) - 65 for c in start]
    if chr(g[2] + 65) in enigma_ref.NOTCH[w[2]]:
        return []
    out = []
    if chr(g[1] + 65) in enigma_ref.NOTCH[w[1]]:
        out.append(A[(g[0] + 1) % 26] + A[(g[1] + 1) % 26] + start[2])
    if chr((g[1] - 1) % 26 + 65) in enigma_ref.NOTCH[w[1]]:
        out.append(A[(g[0] - 1) % 26] + A[(g[1] - 1) % 26] + start[2])
    return out


def competitors(rec):
    """The candidate list with the true key's double-step aliases removed."""
    bad = set(alias_starts(rec["wheels"], rec["start"]))
    return [c for c in rec["cands"] if c["g"] not in bad]


def score_of(stderr):
    m = re.findall(r"^\s*(-[0-9.]+) [A-Za-z]", stderr, re.M)
    return float(m[-1]) if m else None


def run(args, text, env):
    return subprocess.run([BIN] + args, input=text, capture_output=True,
                          text=True, env=env)


def generate():
    texts = corpus()
    rng = random.Random(SEED)
    env = dict(os.environ, ENIGMA_SEED="0")
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
            key = ["-u", "B", "-w", wheels, "-r", ring]
            ct = run(key + ["-g", start, "-s", plugs], pt, env).stdout.strip()
            if len(ct) != L:
                continue

            r = run(["-c", "-f", "-l", "wehrmacht", "-S", "i4f10", "-J",
                     "--polish", "-g", "...", "-R", RESTARTS, "-T", THREADS,
                     "--dump-all"] + key, ct, env)
            # --dump-all emits one line per (key, RESTART), so the same start
            # appears -R times.  A search ranks a key by its BEST restart, so
            # collapse to that -- otherwise mu/sd are over key-restart pairs
            # and the candidate count is inflated -R-fold.
            byk = {}
            for line in r.stderr.split("\n") + r.stdout.split("\n"):
                if not line.startswith("dumpall "):
                    continue
                f = line.split()
                st, sc_, pl = f[3], float(f[4]), " ".join(f[5:])
                if st not in byk or sc_ > byk[st][0]:
                    byk[st] = (sc_, pl)
            if len(byk) < 17000:
                continue
            sc = [v[0] for v in byk.values()]
            mu = sum(sc) / len(sc)
            sd = (sum((x - mu) ** 2 for x in sc) / len(sc)) ** 0.5

            # ORACLE arm: the true key with the true board, no climb at all.
            tr = run(["-f", "-l", "wehrmacht", "-g", start, "-s", plugs] + key,
                     ct, env)
            oracle = score_of(tr.stderr)
            if oracle is None or tr.stdout.strip() != pt:
                continue

            # CLIMB arm: what the sweep itself found at the true start.
            cs, cp = byk[start]
            cd = enigma_ref.decrypt(ct, wheels, ring, start, cp)
            rec = {"i": i, "L": L, "wheels": wheels, "ring": ring,
                   "start": start, "plugs": plugs, "pt": pt,
                   "mu": mu, "sd": sd, "n_cand": len(byk),
                   "true_dbl": longest_doubling(pt),
                   "oracle": {"s": oracle, "z": (oracle - mu) / sd,
                              "d": longest_doubling(pt)},
                   "climb": {"s": cs, "z": (cs - mu) / sd,
                             "d": longest_doubling(cd),
                             "pct": 100.0 * sum(a == b for a, b in
                                                zip(cd, pt)) / L},
                   "cands": []}
            wrong = [(st, v[0], v[1]) for st, v in byk.items() if st != start]
            order = sorted(range(len(wrong)), key=lambda k: -wrong[k][1])
            pick = set(order[:TOPN]) | set(rng.sample(range(len(wrong)),
                                                     min(SAMPLE, len(wrong))))
            for k in sorted(pick, key=lambda k: -wrong[k][1]):
                st, s_, pl = wrong[k]
                d = enigma_ref.decrypt(ct, wheels, ring, st, pl)
                rec["cands"].append({"g": st, "s": s_, "z": (s_ - mu) / sd,
                                     "d": longest_doubling(d)})
            # alias-filtered, or the printed gap reads +0.0 on the ~7% of
            # trials where a double-step twin of the true key is in the list
            rec["best_wrong"] = max(c["s"] for c in competitors(rec))
            fh.write(json.dumps(rec) + "\n")
            fh.flush()
            print("  %2d L=%-4d dbl=%-3d climb pct=%5.1f z=%+6.2f | "
                  "oracle z=%+6.2f gap=%+7.1f dec"
                  % (i, L, rec["true_dbl"], rec["climb"]["pct"],
                     rec["climb"]["z"], rec["oracle"]["z"],
                     (rec["best_wrong"] - oracle) * (L - 3)), flush=True)


def load():
    return [json.loads(l) for l in open(OUT, encoding="utf-8")]


def adj(c, rec, T, M):
    """Score with the gate+bonus applied, in the binary's per-symbol units."""
    if c["z"] <= T:
        return c["s"]
    return c["s"] + bonus(c["d"], M) / (rec["L"] - 3)


def wins(rec, T, M, arm):
    t = adj(rec[arm], rec, T, M)
    return t > max(adj(c, rec, T, M) for c in competitors(rec))


def steals(rec, T, M, arm):
    """A win the bonus DESTROYS: true key top without it, not with it.

    This is the risk side measured rather than modelled -- the earlier
    `1 in N` figures multiplied P(high score) by P(doubling) under an
    independence assumption this run also checks directly (section 4).
    """
    return wins(rec, 99, 0, arm) and not wins(rec, T, M, arm)


def grid(recs, arm, out=True):
    g = {}
    for T in GRID_T:
        for M in GRID_M:
            g[(T, M)] = sum(wins(r, T, M, arm) for r in recs)
    if out:
        print("   %-8s %s" % ("", " ".join("M=%-4g" % m for m in GRID_M)))
        for T in GRID_T:
            print("   %-8s %s"
                  % ("off" if T < -8 else "z>%g" % T,
                     " ".join("%-6d" % g[(T, M)] for M in GRID_M)))
    return g


def report_arm(recs, arm, label):
    base = sum(wins(r, 99, 0, arm) for r in recs)
    print("\n=== %s ARM: %d trials, true key already top in %d\n"
          % (label, len(recs), base))
    tr = [r for i, r in enumerate(recs) if i % 2 == 0]
    te = [r for i, r in enumerate(recs) if i % 2 == 1]
    print("2. GRID on the TRAIN half (%d trials); test half held out" % len(tr))
    g = grid(tr, arm)
    b0 = sum(wins(r, 99, 0, arm) for r in tr)
    print("   baseline (no bonus) %d of %d" % (b0, len(tr)))
    if not g:
        return
    best = max(g, key=lambda k: (g[k], -k[1], -k[0]))
    print("\n   best on train: %s M=%g -> %d of %d (baseline %d)"
          % ("gate off" if best[0] < -8 else "z>%g" % best[0], best[1],
             g[best], len(tr), b0))
    print("   HELD-OUT test: %d of %d (baseline %d)"
          % (sum(wins(r, best[0], best[1], arm) for r in te), len(te),
             sum(wins(r, 99, 0, arm) for r in te)))
    print("\n2b. RESCUED vs STOLEN over all %d trials (the two sides"
          " separately)" % len(recs))
    print("   %-8s %s" % ("", " ".join("M=%-9g" % m for m in GRID_M)))
    for T in GRID_T:
        cells = []
        for M in GRID_M:
            r_ = sum(wins(r, T, M, arm) and not wins(r, 99, 0, arm)
                     for r in recs)
            s_ = sum(steals(r, T, M, arm) for r in recs)
            cells.append("%-11s" % ("+%d/-%d" % (r_, s_)))
        print("   %-8s %s" % ("off" if T < -8 else "z>%g" % T,
                              " ".join(cells)))
    print("\n3. BY LENGTH at that cell")
    print("   %-6s %-6s %-11s %-11s %s"
          % ("L", "n", "baseline", "with bonus", "median gap (dec)"))
    for L in sorted({r["L"] for r in recs}):
        sub = [r for r in recs if r["L"] == L]
        gaps = sorted((max(c["s"] for c in competitors(r)) - r[arm]["s"])
                      * (r["L"] - 3) for r in sub)
        print("   %-6d %-6d %-11s %-11s %+.1f"
              % (L, len(sub),
                 "%d of %d" % (sum(wins(r, 99, 0, arm) for r in sub), len(sub)),
                 "%d of %d" % (sum(wins(r, best[0], best[1], arm) for r in sub),
                               len(sub)),
                 gaps[len(gaps) // 2]))


def main():
    if os.environ.get("REUSE") != "1" or not os.path.exists(OUT):
        print("generating %d trials, L in %s, -R %s ..."
              % (TRIALS, LENS, RESTARTS))
        generate()
    recs = load()
    if not recs:
        print("no trials")
        return 1
    print("\n%d trials, %d candidates swept each, -R %s"
          % (len(recs), recs[0]["n_cand"], RESTARTS))

    print("\n1. CLASSIFICATION (the climb arm decides which population a"
          " trial is in)")
    print("   %-6s %-5s %-9s %-9s %s"
          % ("L", "n", "recovered", "scoring", "search"))
    for L in sorted({r["L"] for r in recs}):
        sub = [r for r in recs if r["L"] == L]
        rec_ = [r for r in sub if r["climb"]["pct"] >= 90]
        scor = [r for r in rec_ if not wins(r, 99, 0, "climb")]
        print("   %-6d %-5d %-9d %-9d %d"
              % (L, len(sub), len(rec_), len(scor), len(sub) - len(rec_)))

    print("\n4. DOUBLING RATE AS A FUNCTION OF z (the independence check)")
    band = {}
    for r in recs:
        for c in competitors(r):
            b = min(int(math.floor(c["z"])), 6)
            n, h = band.get(b, (0, 0))
            band[b] = (n + 1, h + (c["d"] >= 6))
    print("   %-10s %10s %8s %10s" % ("z band", "candidates", "hits", "rate"))
    for b in sorted(band):
        n, h = band[b]
        print("   %-10s %10d %8d %10.2e"
              % ("%+d..%+d" % (b, b + 1), n, h, h / n if n else 0))

    report_arm(recs, "oracle", "ORACLE (true board -- the scoring ceiling)")
    ok = [r for r in recs if r["climb"]["pct"] >= 90]
    if ok:
        report_arm(ok, "climb", "CLIMB (recovered trials only)")
    else:
        print("\n=== CLIMB ARM: no trial recovered >=90%% of letters at -R %s;"
              "\n    every trial is a SEARCH failure, which this feature"
              " cannot address." % RESTARTS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
