#!/usr/bin/env python3
"""How much compute does --tune-phase actually NEED at a given length?

tune_phase_vs_restarts.py answers "at matched wall time, which arm is better?".
Once both arms saturate -- which they do by L=450, where each recovers ~100% of
the letters -- that question stops discriminating, and the interesting one
becomes the other direction: the matched budget was chosen to equal the
exhaustive sweep's cost, so how far BELOW it can --tune-phase go and still
break the message?  That is where the 125x smaller keyspace would actually pay.

Sweeps -R over the SAME instances the paired harness drew, so the numbers are
directly comparable to the arm B column in the matching results file.  Pass the
same LEN and SEED; the draw sequence depends on nothing else, and each instance
is checked against the recorded row before it is used.

  ./eval/tune_phase_budget.py eval/results-tune-phase-L450.jsonl 2,4,8,16
"""
import hashlib
import json
import os
import random
import subprocess
import sys
import time

BIN = "./enigma"
PLUGS = 10
THREADS = "4"
CLIMB = ["-f", "-l", "english", "-J", "-S", "m4f10", "-c", "--polish"]
WHEELS = ["-u", "B", "-w", "231"]
ARM = ["-r", "A..", "-g", "A..", "--tune-phase", "2"]
CORPUS = "eval/corpus-tune-phase-ab.txt"

PAIRED = sys.argv[1] if len(sys.argv) > 1 else \
    "eval/results-tune-phase-L450.jsonl"
BUDGETS = [int(x) for x in (sys.argv[2] if len(sys.argv) > 2
                            else "2,4,8,16").split(",")]
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 12345


def run(args, stdin):
    p = subprocess.run([BIN] + args, input=stdin, capture_output=True,
                       text=True, env={**os.environ, "ENIGMA_SEED": "0"})
    return p.stdout.strip()


def pct_correct(got, want):
    if not got:
        return 0.0
    n = min(len(got), len(want))
    return 100.0 * sum(a == b for a, b in zip(got[:n], want[:n])) / len(want)


def main():
    rows = [json.loads(line) for line in open(PAIRED)]
    text = "".join(c for c in open(CORPUS, encoding="utf-8").read().upper()
                   if "A" <= c <= "Z")
    length = rows[0].get("len") or int(os.environ.get("AB_LEN", "0"))
    if not length:
        sys.exit("results file predates the len field -- set AB_LEN")
    rng = random.Random(SEED)
    A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

    got = {r: [] for r in BUDGETS}
    secs = {r: 0.0 for r in BUDGETS}
    for row in rows:
        # redraw in the harness's exact order, then VERIFY -- a corpus or a
        # draw order that has moved underneath makes every comparison below a
        # comparison of different problems (PERFORMANCE.md 7.15's own trap).
        off = rng.randrange(len(text) - length)
        plain = text[off:off + length]
        ring = "A" + rng.choice(A) + rng.choice(A)
        start = "A" + rng.choice(A) + rng.choice(A)
        letters = list(A)
        rng.shuffle(letters)
        board = "".join(letters[:2 * PLUGS])
        if (ring, start, board,
                hashlib.sha1(plain.encode()).hexdigest()[:12]) != (
                row["ring"], row["start"], row["board"], row["pt"]):
            sys.exit("trial %d does not match the paired file" % row["trial"])

        ct = run(["-i"] + WHEELS + ["-r", ring, "-g", start, "-s", board],
                 plain)
        line = "trial %2d " % row["trial"]
        for r in BUDGETS:
            t0 = time.time()
            out = run(CLIMB + WHEELS + ARM + ["-R", str(r), "-T", THREADS], ct)
            secs[r] += time.time() - t0
            got[r].append((pct_correct(out, plain), out == plain))
            line += " R%-3d %5.1f%%%s" % (r, got[r][-1][0],
                                          "*" if got[r][-1][1] else " ")
        print(line, flush=True)

    n = len(rows)
    print("\nL=%d, %d instances, --tune-phase 2" % (length, n))
    print("  -R    mean %-correct   exact    wall/trial")
    for r in BUDGETS:
        print("  %-4d     %6.1f       %2d/%d      %5.1f s"
              % (r, sum(p for p, _ in got[r]) / n,
                 sum(1 for _, e in got[r] if e), n, secs[r] / n))
    b = [row["B_restarts"] for row in rows]
    print("  (paired file's exhaustive arm B: %.1f  %d/%d  %5.1f s)"
          % (sum(x["pct"] for x in b) / n, sum(x["exact"] for x in b), n,
             sum(x["sec"] for x in b) / n))


if __name__ == "__main__":
    main()
