#!/usr/bin/env python3
"""Does the X-doubling signal rescue REAL scoring failures?

ENHANCEMENTS.md item 5 measured the doubling feature over every message below
the detection bar and reported 1 of 9.  That was the wrong denominator: the
feature was proposed to fix SCORING failures, and a below-bar population is
overwhelmingly SEARCH failures -- 21 of 22 in that corpus.  On its actual target
it went 1 for 1, which is 100% of an n of 1.

Scoring failures are rare at operational length, so this manufactures a
population of them the way the rest of the repo manufactures hard instances:
short excerpts of authentic telegraphic German, random rotor key, random 10-plug
board.  Excerpts are cut to CONTAIN a known doubling, so the feature always has
something to find and the measurement is about whether it SURVIVES and
DISCRIMINATES, not about coverage (which doubling_probe.py measures separately).

THE CLASSIFICATION IS THE POINT, and it needs the climb and the score separated:

  climb recovers (>=90% of letters) and z clears the bar   -> break
  climb recovers but z does NOT clear the bar              -> SCORING failure
  climb does not recover                                   -> search failure

Only the middle row is this feature's target.  It is the FTNBK shape: the true
key's decrypt is the plaintext, and the n-gram model still ranks it below wrong
keys.  A search failure cannot be rescued by any plaintext-side feature, because
there is no plaintext in the decrypt to read.

  ./eval/scoring_failure_probe.py [lengths] [nsamp] [seeds]
  REUSE=1 ./eval/scoring_failure_probe.py    # re-analyse the saved trials
"""
import json
import math
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from doubling_probe import MAXLEN, decrypts, repeats       # noqa: E402

BIN = "./enigma"
RECIPE = ["-c", "-f", "-l", "wehrmacht", "-S", "i4f10", "-J", "--polish"]
RESTARTS = os.environ.get("Z_RESTARTS", "256")
THREADS = os.environ.get("Z_THREADS", "4")
OUT = "eval/results-scoring-failure.json"
BAR = math.sqrt(2 * math.log(160293120))
RECOVERED = 90.0          # %-correct above which the climb counts as succeeded
K, MM = 6, 1              # the operating point doubling_probe.py found dominant
# L=60 is kept as a CONTROL, not a hopeful cell: measured, the climb recovers
# 1 of 8 there even at -R 512 against 5 of 8 at L=100, so shortening the message
# buries scoring failures under search failures rather than exposing them.
LENGTHS = [int(x) for x in (sys.argv[1] if len(sys.argv) > 1
                            else "60,100,140").split(",")]
NSAMP = int(sys.argv[2]) if len(sys.argv) > 2 else 48
SEEDS = int(sys.argv[3]) if len(sys.argv) > 3 else 3
ORDERS = [a + b + c for a in "12345" for b in "12345" for c in "12345"
          if len({a, b, c}) == 3]
A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def run(text, w, r, g, plugs=None, climb=True):
    """Encrypt (plugs given, no climb) or attack (board hidden, climb)."""
    cmd = [BIN, "-u", "B", "-w", w, "-r", r, "-g", g]
    if climb:
        cmd += RECIPE + ["-R", RESTARTS, "-T", THREADS]
    if plugs:
        cmd += ["-s", plugs]
    p = subprocess.run(cmd, input=text, capture_output=True, text=True,
                       env={**os.environ, "ENIGMA_SEED": "0"})
    return p.stdout.strip(), p.stderr


def score_of(stderr):
    import re
    m = re.findall(r"^\s*(-[0-9.]+) B", stderr, re.M)
    return float(m[-1]) if m else None


def pct(a, b):
    n = min(len(a), len(b))
    return 100.0 * sum(1 for i in range(n) if a[i] == b[i]) / n if n else 0.0


def board(rng):
    ls = list(A)
    rng.shuffle(ls)
    return " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))


def windows():
    """Short excerpts of authentic plaintext, each CONTAINING a doubling."""
    out = []
    for kenn, d in decrypts():
        got = set()
        for i in range(len(d)):
            for L in range(K, MAXLEN + 1):
                if i + 2 * L + 1 > len(d):
                    break
                w, v = d[i:i + L], d[i + L + 1:i + 2 * L + 1]
                if d[i + L] != "X" or "X" in w or "X" in v:
                    continue
                if sum(1 for a, b in zip(w, v) if a != b) > 1 or w in got:
                    continue
                got.add(w)
                span = 2 * L + 1
                for ln in LENGTHS:
                    if ln < span or ln > len(d):
                        continue
                    # centre the window on the doubling, then clamp into range
                    lo = max(0, min(len(d) - ln, i - (ln - span) // 2))
                    ex = d[lo:lo + ln]
                    if repeats(ex, K, MM):
                        out.append((kenn, ln, w, ex))
    return out


def collect():
    trials = []
    for kenn, ln, w, pt in windows():
        for s in range(SEEDS):
            rng = random.Random(hash((kenn, ln, w, s)) & 0xffffffff)
            tw = rng.choice(ORDERS)
            tr = "A" + rng.choice(A) + rng.choice(A)
            tg = "".join(rng.choice(A) for _ in range(3))
            plugs = board(rng)
            ct, _ = run(pt, tw, tr, tg, plugs, climb=False)
            if len(ct) != len(pt):
                continue
            tt, te = run(ct, tw, tr, tg)
            st = score_of(te)
            if st is None:
                continue
            null, wtexts = [], []
            for _ in range(NSAMP):
                ww = rng.choice(ORDERS)
                while ww == tw:
                    ww = rng.choice(ORDERS)
                wt, we = run(ct, ww, "A" + rng.choice(A) + rng.choice(A),
                             "".join(rng.choice(A) for _ in range(3)))
                sc = score_of(we)
                if sc is not None:
                    null.append(sc)
                    wtexts.append(wt)
            if len(null) < 10:
                continue
            mu = sum(null) / len(null)
            sd = (sum((x - mu) ** 2 for x in null) / (len(null) - 1)) ** 0.5
            trials.append({
                "kenn": kenn, "len": ln, "word": w, "pt": pt,
                "correct": pct(pt, tt), "z": (st - mu) / sd if sd else 0.0,
                "true_score": st, "max_wrong": max(null),
                "fires_true": bool(repeats(tt, K, MM)),
                "fires_wrong": sum(bool(repeats(t, K, MM)) for t in wtexts),
                "n_wrong": len(wtexts),
                # Saved so a change to the matching rule can be re-scored
                # without re-climbing -- the omission that made the MAXLEN=12
                # blind spot expensive to check.
                "true_text": tt, "wrong_texts": wtexts})
            t = trials[-1]
            print("  %-7s L=%-3d %-12s %5.1f%% z=%6.2f  %s"
                  % (kenn[:7], ln, w[:12], t["correct"], t["z"],
                     "FIRES" if t["fires_true"] else "-"), flush=True)
            json.dump(trials, open(OUT, "w"))
    return trials


def analyse(trials):
    n = len(trials)
    brk = [t for t in trials if t["correct"] >= RECOVERED and t["z"] > BAR]
    scor = [t for t in trials if t["correct"] >= RECOVERED and t["z"] <= BAR]
    srch = [t for t in trials if t["correct"] < RECOVERED]
    print("\n%d trials (excerpts chosen to contain a doubling), %d wrong keys"
          " each" % (n, NSAMP))
    print("  break         (climb recovers, z > bar)   %3d (%4.1f%%)"
          % (len(brk), 100.0 * len(brk) / n))
    print("  SCORING fail  (climb recovers, z <= bar)  %3d (%4.1f%%) <-target"
          % (len(scor), 100.0 * len(scor) / n))
    print("  search fail   (climb does not recover)    %3d (%4.1f%%)"
          % (len(srch), 100.0 * len(srch) / n))

    print("\n  the feature on each population (fires on the true key):")
    for name, grp in (("break", brk), ("SCORING failure", scor),
                      ("search failure", srch)):
        if not grp:
            continue
        f = sum(t["fires_true"] for t in grp)
        print("    %-16s %3d of %3d (%3.0f%%)"
              % (name, f, len(grp), 100.0 * f / len(grp)))

    fw = sum(t["fires_wrong"] for t in trials)
    nw = sum(t["n_wrong"] for t in trials)
    print("\n  false positives: %d of %d wrong-key decrypts (%.3f%%)"
          % (fw, nw, 100.0 * fw / nw if nw else 0.0))
    if scor:
        r = sum(1 for t in scor if t["fires_true"] and not t["fires_wrong"])
        print("\n  RESCUE RATE on scoring failures: %d of %d (%.0f%%)"
              % (r, len(scor), 100.0 * r / len(scor)))
        print("  (fires on the true key and on none of that trial's wrong"
              " keys, so\n   the true key is identified outright)")
    by = {}
    for t in trials:
        b = by.setdefault(t["len"], [0, 0, 0, 0])
        b[0] += 1
        b[1] += t["correct"] >= RECOVERED and t["z"] <= BAR
        b[2] += t["correct"] < RECOVERED
        b[3] += t["correct"] >= RECOVERED and t["z"] > BAR
    print("\n  by length %6s %7s %8s %7s"
          % ("n", "break", "scoring", "search"))
    for ln in sorted(by):
        c = by[ln]
        print("    L=%-4d %6d %7d %8d %7d" % (ln, c[0], c[3], c[1], c[2]))


def main():
    trials = json.load(open(OUT)) if os.environ.get("REUSE") else collect()
    analyse(trials)
    return 0


if __name__ == "__main__":
    sys.exit(main())
