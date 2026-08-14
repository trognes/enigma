#!/usr/bin/env python3
"""Is a REAL message breakable ciphertext-only?  Measured by length.

CLAUDE.md's "unknown-key break rate" is built from synthetic trials: authentic
plaintext, but a random excerpt, a random key and a freshly drawn 10-pair board.
This probe asks the same question of the real messages themselves -- real key,
real board, real garbles, real length -- for which this repo holds ground truth.

A break needs the true key's score to clear what the best of K keys reaches by
chance, mu + sd*sqrt(2 ln K).  So: climb the true key, climb a sample of wrong
keys drawn from the same space, and report z = (true - mu)/sd against that bar.
No sweep is needed, which is what makes it cheap.

  ./eval/real_traffic_z.py            # default lengths, 96 null samples
  ./eval/real_traffic_z.py 74,140 200

ENIGMA_SEED is pinned to 0: without it the true key's own climb varies run to
run, and at a small sample count that moves z visibly (the 69-letter cell read
2.20 at 96 samples and 1.38 at 32).  Use at least ~96 samples; the null's sd is
what z divides by, and it is the noisy term.

THE TRAP THIS PROBE FELL INTO FIRST, because it silently inverts the result:
passing the true plugboard (-s) to BOTH arms.  That is not the attack scenario
-- the board is exactly what an attacker does not have -- and with a 10-pair
board given, the schedule's cap is already met, so the climb does nothing and
both arms are scored as plain scans.  The first run of this probe did that and
reported z = 0.32 / -0.35 / 2.37 at 76 / 113 / 174 letters, i.e. that nothing is
ever breakable, contradicting a well-measured claim.  The board must be HIDDEN
on both arms.
"""
import os
import random
import re
import subprocess
import sys

BIN = "./enigma"
DB = "eval/enigma-army-messages-1941.txt"
RECIPE = ["-c", "-f", "-l", "wehrmacht", "-S", "i4f10", "-J", "--polish"]
RESTARTS = os.environ.get("Z_RESTARTS", "64")
THREADS = os.environ.get("Z_THREADS", "4")
# K for the sweep a real attempt needs: -u B, wheels I-V, -r A.. -g ...
KEYSPACE = 160293120
BAR = (2 * __import__("math").log(KEYSPACE)) ** 0.5

LENGTHS = [int(x) for x in (sys.argv[1] if len(sys.argv) > 1
                            else "74,111,172").split(",")]
NSAMP = int(sys.argv[2]) if len(sys.argv) > 2 else 96
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 5
ORDERS = [a + b + c for a in "12345" for b in "12345" for c in "12345"
          if len({a, b, c}) == 3]


def records():
    out = []
    for b in re.split(r"\n### ", open(DB, encoding="utf-8").read())[1:]:
        def f(k):
            return re.search(k + r":\s+(.*)", b).group(1).strip()
        ct = "".join(re.search(r"CIPHERTEXT:\s+((?:.|\n)*?)\n[A-Z]",
                               b).group(1).split())
        if "-" in ct:          # unrecorded letters would need placeholders
            continue
        out.append((b.split("\n")[0].split("(")[-1].rstrip(") "),
                    f("WHEELS").split("(-w ")[1].rstrip(")"),
                    f("RING"), f("START"), ct))
    return out


def climb(ct, w, r, g):
    """Score the best board this key can reach.  NOTE: no -s -- the board is
    hidden, for the true key and for the null alike."""
    p = subprocess.run([BIN] + RECIPE + ["-u", "B", "-w", w, "-r", r, "-g", g,
                                         "-R", RESTARTS, "-T", THREADS],
                       input=ct, capture_output=True, text=True,
                       env={**os.environ, "ENIGMA_SEED": "0"})
    m = re.findall(r"^\s*(-[0-9.]+) B", p.stderr, re.M)
    return float(m[-1]) if m else None


def main():
    recs = records()
    A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    print("real-traffic true-key z, board hidden on both arms, -R %s, %d null "
          "samples" % (RESTARTS, NSAMP))
    print("chance best of K = %d keys is %.2f sd\n" % (KEYSPACE, BAR))
    print("  length  msg      true key      null mu        z    verdict")
    for target in LENGTHS:
        cand = [x for x in recs if abs(len(x[4]) - target) <= 15]
        if not cand:
            print("   %3d    (no message of this length in the database)"
                  % target)
            continue
        kenn, w, r, st, ct = min(cand, key=lambda x: abs(len(x[4]) - target))
        rng = random.Random(SEED)
        null = []
        for _ in range(NSAMP):
            ww = rng.choice(ORDERS)
            while ww == w:
                ww = rng.choice(ORDERS)
            v = climb(ct, ww, "A" + rng.choice(A) + rng.choice(A),
                      "".join(rng.choice(A) for _ in range(3)))
            if v is not None:
                null.append(v)
        mu = sum(null) / len(null)
        sd = (sum((x - mu) ** 2 for x in null) / (len(null) - 1)) ** 0.5
        t = climb(ct, w, r, st)
        z = (t - mu) / sd
        print("   %3d    %-7s  %8.4f    %8.4f   %6.2f   %s"
              % (len(ct), kenn, t, mu, z,
                 "breakable" if z > BAR else "SHORT of the bar"))


if __name__ == "__main__":
    main()
