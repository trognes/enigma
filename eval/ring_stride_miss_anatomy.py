#!/usr/bin/env python3
"""WHY does --ring-stride miss? Classifies each stride-specific miss by which part of the
key the coarse pass got wrong -- which decides whether the miss is fixable at all, and by
what.

The motivating observation (PERFORMANCE.md 7.11): widening the refinement from a +/-K/2
window to EVERY skipped ring2 moved K=3 by +4.5/+7.0pp but moved K=2 by nothing. K=2's
coarse grid is never more than 1 away from the true ring2, so if its misses were
window failures, widening would have fixed them. It did not. The hypothesis this script
tests is that the approximation instead corrupts the coarse pass badly enough that the
true WHEEL ORDER / REFLECTOR (or ring0/start0) loses outright -- and since the refinement
pins those to the coarse winner, no amount of ring2 sweeping can recover them.

That distinction is actionable, because the two classes have different fixes:

  wheels/reflector wrong -> refining only the BEST coarse candidate can never recover it.
                            Fix: refine the top-M coarse candidates (7.11's one untested
                            mitigation). Now affordable -- a refinement candidate costs
                            1/(tasks * rc[0] * gc[0]) of a coarse one.
  wheels right, ring/start wrong -> the refinement had the right row and still lost, so
                            the fix is in the refinement itself (what it re-opens), not
                            in how many candidates it takes.

Method mirrors eval/ring_stride_wehrmacht_probe.py exactly (same corpus, key generation,
scoring model and -s plugboard-given setup) so the miss populations are comparable; the
only addition is parsing the WINNING KEY out of the progress output and diffing it
against the truth component by component.

Two reporting caveats, both real and neither a bug:
  - ring0 is reported as 'A' always and ring1/start1 may be a CLASS REPRESENTATIVE
    (7.10, 7.12), so a ring/start string can differ from the true key while decoding
    identically. Ring/start are therefore only compared when the plaintext is wrong, and
    even then "ring/start differs" is reported as a residual class rather than a precise
    diagnosis. The wheels+reflector comparison has no such caveat and is the load-bearing
    number.
  - Trials where K=1 also fails are the pre-existing scoring/search floor and are excluded
    outright; they are not stride misses.

Usage: python3 eval/ring_stride_miss_anatomy.py
Env: LENGTHS ("40 60"), KS ("2 3"), TRIALS (200), SEED (0), THREADS (4)
"""
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "40 60").split()]
KS = [int(x) for x in os.environ.get("KS", "2 3").split()]
TRIALS = int(os.environ.get("TRIALS", "200"))
SEED = int(os.environ.get("SEED", "0"))
THREADS = os.environ.get("THREADS", "4")

# A progress line is "  <score> <refl+wheels> <ring> <start> [plugs...] <text>".
PROGRESS = re.compile(r"^\s*-?\d+\.\d+\s+(\S+)\s+([A-Z]{3})\s+([A-Z]{3})\s")


def load_corpus(minlen):
    text = ""
    for fname in ("enigma-messages.txt", "enigma-army-messages-1941.txt"):
        text += open(os.path.join(HERE, fname)).read()
    blocks = re.findall(r"^DECRYPT:\s*(.*(?:\n {13}\S.*)*)", text, re.M)
    clean = [re.sub(r"\s+", "", b) for b in blocks]
    return [b for b in clean if "-" not in b and len(b) >= minlen]


def run(args, inp):
    p = subprocess.run([BIN] + args, input=inp, capture_output=True, text=True, cwd=ROOT)
    key = None
    for line in p.stderr.splitlines():
        m = PROGRESS.match(line)
        if m:
            key = (m.group(1), m.group(2), m.group(3))   # last one wins = the winner
    return p.stdout.strip(), key


def trial(L, corpus, rng, ks):
    u = rng.choice("ABC")
    w = "".join(str(x) for x in rng.sample(range(1, 6), 3))
    r = "".join(rng.choice(ALPHA) for _ in range(3))
    g = "".join(rng.choice(ALPHA) for _ in range(3))
    letters = list(ALPHA)
    rng.shuffle(letters)
    board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(10))
    blk = rng.choice(corpus)
    off = rng.randrange(0, len(blk) - L + 1)
    pt = blk[off:off + L]
    ct, _ = run(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", board], pt)

    base = ["-f", "-l", "wehrmacht", "-u", u, "-w", w, "-r", "...", "-g", "...",
            "-s", board, "-T", THREADS]
    out = {}
    for K in [1] + ks:
        args = base + (["--ring-stride", str(K)] if K > 1 else [])
        out[K] = run(args, ct)
    return pt, (u + w), out


def main():
    print("Classifies stride-SPECIFIC misses (K failed where K=1 succeeded).")
    print("wheels-wrong = the coarse winner's reflector+wheel-order is not the true one,")
    print("so refining only the best coarse candidate cannot recover it.\n")
    print("%5s %4s %7s %9s %13s %13s"
          % ("L", "K", "misses", "of-n", "wheels-wrong", "wheels-right"))
    for L in LENGTHS:
        corpus = load_corpus(L)
        rng = random.Random(SEED * 1000 + L)     # same stream as the sibling probe
        rows = [trial(L, corpus, rng, KS) for _ in range(TRIALS)]
        for K in KS:
            miss = [(true_uw, out) for pt, true_uw, out in rows
                    if out[1][0] == pt and out[K][0] != pt]
            bad_w = sum(1 for true_uw, out in miss
                        if out[K][1] is None or out[K][1][0] != true_uw)
            n = len(miss)
            print("%5d %4d %7d %8.1f%% %12s %13s"
                  % (L, K, n, 100.0 * n / TRIALS,
                     "%d (%.0f%%)" % (bad_w, 100.0 * bad_w / n) if n else "-",
                     "%d (%.0f%%)" % (n - bad_w, 100.0 * (n - bad_w) / n) if n else "-"))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
