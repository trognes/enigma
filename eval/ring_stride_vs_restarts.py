#!/usr/bin/env python3
"""Settles the open question left by eval/ring_stride_wehrmacht_probe.py: --ring-stride
loses accuracy per key searched, but it also searches fewer keys -- so is the compute it
saves better spent on more -R restarts than on an exhaustive ring2 sweep?

That earlier probe compared at FIXED work per candidate (a plain scan, true plugboard
given via -s, no restarts), which measures the accuracy cost but says nothing about the
trade. This one compares at MATCHED WALL TIME with the plugboard HIDDEN, which is the
only setting where -R does anything:

    A (baseline) : no stride,        -R N
    B (stride)   : --ring-stride 2,  -R round(N * 1.71)

1.71 is the measured cost ratio (K=1 vs K=2 at equal R on this keyspace; it converges to
the predicted 26/15 = 1.73 once R is large enough to swamp startup cost). The script does
not TRUST that match -- it accumulates real wall time per config and reports it, so a
drifted match is visible rather than silently invalidating the comparison.

Keyspace: reflector+wheels given, ring "AA." and start "A.." wildcarded -> 26 ring2 x
26 start1 x 26 start2 = 17576 keys. (start0/ring0 are pinned to A and the true keys are
generated to match, because wheel 0's ring x start collapses to a pure offset -- §7.10 --
so leaving it open would only add a redundant 26x factor to both arms.)

ALWAYS 10 PLUGS -- standard Wehrmacht, and the `make crackquality` default. Fewer plugs
makes recovery easier and so makes a cell "measurable" sooner, but it is not the regime
the tool is for, and a trade measured on an unrealistically weak plugboard need not hold
at 10. When a cell floors at 0% the fix is more restarts or a longer message, never
fewer plugs.

Cell choice: with the plugboard hidden, L=60 floors at 0% for both arms and discriminates
nothing; L=130 at -R 12 lands near 33%, off both floor and ceiling, which is where a
paired comparison has power. Note the corpus caps this -- only 13 authentic blocks reach
130 letters and 4 reach 190, so pushing length for signal would draw every excerpt from a
tiny correlated pool. Raising -R is the better lever, though it saturates too: 12 -> 24
restarts did not move recovery in a spot check.

Paired: both arms see the identical key, plugboard and plaintext, so the difference is
reported as win/loss counts as well as rates.

Usage: python3 eval/ring_stride_vs_restarts.py
Env: TRIALS (100), SEED (0), THREADS (4), RBASE (12), CELLS ("130:10")
"""
import os
import random
import re
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

TRIALS = int(os.environ.get("TRIALS", "100"))
SEED = int(os.environ.get("SEED", "0"))
THREADS = os.environ.get("THREADS", "4")
RBASE = int(os.environ.get("RBASE", "12"))
CELLS = [tuple(int(x) for x in c.split(":"))
         for c in os.environ.get("CELLS", "130:10").split()]

COST_RATIO = 1.71          # measured K=1 / K=2 wall-time ratio at equal R
RSTRIDE = round(RBASE * COST_RATIO)


def load_corpus():
    text = ""
    for fname in ("enigma-messages.txt", "enigma-army-messages-1941.txt"):
        text += open(os.path.join(HERE, fname)).read()
    blocks = re.findall(r"^DECRYPT:\s*(.*(?:\n {13}\S.*)*)", text, re.M)
    clean = [re.sub(r"\s+", "", b) for b in blocks]
    return [b for b in clean if "-" not in b]


CORPUS = load_corpus()


def run(args, inp):
    return subprocess.run([BIN] + args, input=inp, capture_output=True,
                          text=True, cwd=ROOT).stdout.strip()


def trial(L, plugs, rng):
    block = rng.choice([b for b in CORPUS if len(b) >= L])
    off = rng.randrange(0, len(block) - L + 1)
    pt = block[off:off + L]
    # ring0/ring1 = A and start0 = A, matching the searched "AA." / "A.." space
    r2, g1, g2 = rng.choice(ALPHA), rng.choice(ALPHA), rng.choice(ALPHA)
    letters = list(ALPHA)
    rng.shuffle(letters)
    board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(plugs))
    ct = run(["-i", "-u", "B", "-w", "123", "-r", "AA" + r2,
              "-g", "A" + g1 + g2, "-s", board], pt)

    common = ["-c", "-J", "--polish", "-f", "-l", "wehrmacht", "-u", "B", "-w", "123",
              "-r", "AA.", "-g", "A..", "-T", THREADS]
    out = {}
    secs = {}
    for name, extra in (("base", ["-R", str(RBASE)]),
                        ("stride", ["-R", str(RSTRIDE), "--ring-stride", "2"])):
        t0 = time.time()
        out[name] = run(common + extra, ct)
        secs[name] = time.time() - t0
    return {k: (out[k] == pt) for k in out}, secs


def main():
    print("matched-compute A/B: base -R %d  vs  stride(K=2) -R %d   (ratio %.2f)"
          % (RBASE, RSTRIDE, COST_RATIO))
    print("%6s %6s %7s %9s %9s %8s %8s %10s %10s"
          % ("L", "plugs", "n", "base%", "stride%", "b-only", "s-only", "base_s", "stride_s"))
    for L, plugs in CELLS:
        rng = random.Random(SEED * 1000 + L * 100 + plugs)
        nb = ns = bonly = sonly = 0
        tb = ts = 0.0
        for i in range(TRIALS):
            res, secs = trial(L, plugs, rng)
            nb += res["base"]
            ns += res["stride"]
            bonly += (res["base"] and not res["stride"])
            sonly += (res["stride"] and not res["base"])
            tb += secs["base"]
            ts += secs["stride"]
            # running tally to stderr: a cell takes ~an hour, so a status check
            # mid-run should show something more useful than an empty table
            if (i + 1) % 10 == 0:
                print("    [L=%d p=%d] %d/%d  base %d  stride %d  (b-only %d, s-only %d)"
                      " %.0fs/%.0fs" % (L, plugs, i + 1, TRIALS, nb, ns, bonly, sonly, tb, ts),
                      file=__import__("sys").stderr, flush=True)
        print("%6d %6d %7d %8.1f%% %8.1f%% %8d %8d %9.1f %9.1f"
              % (L, plugs, TRIALS, 100 * nb / TRIALS, 100 * ns / TRIALS,
                 bonly, sonly, tb, ts))
    print("\nbase_s/stride_s are TOTAL wall seconds per arm -- they must be close for")
    print("the comparison to be matched-compute; a large gap invalidates the cell.")


if __name__ == "__main__":
    main()
