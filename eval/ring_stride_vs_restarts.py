#!/usr/bin/env python3
"""Settles the open question left by eval/ring_stride_wehrmacht_probe.py: --ring-stride
loses accuracy per key searched, but it also searches fewer keys -- so is the compute it
saves better spent on more -R restarts than on an exhaustive ring2 sweep?

That earlier probe compared at FIXED work per candidate (a plain scan, true plugboard
given via -s, no restarts), which measures the accuracy cost but says nothing about the
trade. This one compares at MATCHED WALL TIME with the plugboard HIDDEN, which is the
only setting where -R does anything:

    A (baseline) : no stride,        -R N
    B (stride)   : --ring-stride 2,  -R round(N * ratio)

The ratio is calibrated PER CELL by direct timing, not assumed. Two traps here. It is
not constant across keyspaces (the equal-R cost ratio is ~1.71 on the 17576-key space
but ~1.53 on the 676-key space, since fixed costs dilute the saving differently), and
the equal-R ratio is NOT the right multiplier anyway: the extra restarts it buys are
marginal-cost, so the correct matched-wall-time ratio is higher (on the 676-key cell
the equal-R ratio is 1.53 but R_stride=350 vs R_base=200, i.e. 1.75, is what actually
equalises the clock). The script also
accumulates real wall time per arm and prints both totals, so a drifted match shows up
in the output instead of quietly invalidating the comparison.

ALWAYS 10 PLUGS -- standard Wehrmacht, and the `make crackquality` default. Fewer plugs
makes recovery easier and so makes a cell "measurable" sooner, but it is not the regime
the tool is for, and a trade measured on an unrealistically weak plugboard need not hold
at 10. When a cell floors at 0% the fix is more restarts, a longer message, or a smaller
rotor keyspace -- never fewer plugs.

WHAT IS REACHABLE AT 10 PLUGS WITH THE BOARD HIDDEN (measured, 8-trial probes). This is
worth recording because most short-message cells simply cannot discriminate:

    keys   L    -R    exact
    17576  60   12    0/8      floored
    17576  100  12    0/8      floored
    676    100  12    1/8
    676    100  48    2/8      restarts do lift it -- a budget limit, not a floor
    676    100  200   4/8      usable
    676    80   200   1/8      near floor even at heavy -R
    676    60   200   0/8      floored
    17576  130  12    2/6      usable

So ~L=100 is the SHORTEST testable length once the plugboard is hidden, and only with a
reduced keyspace plus heavy restarts. Below that both arms score ~0 and the comparison
has no power at all -- the failure is joint rotor+plugboard recovery, not the stride.
(For contrast, with the board GIVEN the rotor key alone is recoverable at L=40-60 --
eval/ring_stride_wehrmacht_probe.py -- so it is the hidden 10-plug board that costs the
short lengths, not the rotor search.)

Cells are "L:plugs:R:ratio:keyspace", keyspace being `open` (start1 wildcarded, 17576
keys) or `pin1` (start1 given, 676 keys -- a partially-known-key scenario). ring0/start0
are pinned to A in both the generated keys and the searched space, since wheel 0's
ring x start collapses to a pure offset (§7.10) and leaving it open only adds a
redundant 26x to both arms. ring2 stays fully wildcarded in every cell -- that is the
dimension --ring-stride acts on, so the trade is exercised identically either way.

Paired: both arms see the identical key, plugboard and plaintext, so the difference is
reported as win/loss counts as well as rates.

Usage: python3 eval/ring_stride_vs_restarts.py
Env: TRIALS (80), SEED (0), THREADS (4),
     CELLS ("100:10:200:1.75:pin1 130:10:12:1.71:open")
"""
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

TRIALS = int(os.environ.get("TRIALS", "80"))
SEED = int(os.environ.get("SEED", "0"))
THREADS = os.environ.get("THREADS", "4")
DEFAULT_CELLS = "100:10:200:1.75:pin1 130:10:12:1.71:open"


def parse_cell(s):
    L, plugs, r, ratio, keys = s.split(":")
    return int(L), int(plugs), int(r), float(ratio), keys


CELLS = [parse_cell(c) for c in os.environ.get("CELLS", DEFAULT_CELLS).split()]


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


def trial(L, plugs, rbase, rstride, keyspace, rng):
    block = rng.choice([b for b in CORPUS if len(b) >= L])
    off = rng.randrange(0, len(block) - L + 1)
    pt = block[off:off + L]
    r2, g1, g2 = rng.choice(ALPHA), rng.choice(ALPHA), rng.choice(ALPHA)
    letters = list(ALPHA)
    rng.shuffle(letters)
    board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(plugs))
    ct = run(["-i", "-u", "B", "-w", "123", "-r", "AA" + r2,
              "-g", "A" + g1 + g2, "-s", board], pt)

    gpat = "A" + g1 + "." if keyspace == "pin1" else "A.."
    common = ["-c", "-J", "--polish", "-f", "-l", "wehrmacht", "-u", "B", "-w", "123",
              "-r", "AA.", "-g", gpat, "-T", THREADS]
    out, secs = {}, {}
    for name, extra in (("base", ["-R", str(rbase)]),
                        ("stride", ["-R", str(rstride), "--ring-stride", "2"])):
        t0 = time.time()
        out[name] = run(common + extra, ct)
        secs[name] = time.time() - t0
    return {k: (out[k] == pt) for k in out}, secs


def main():
    print("%5s %5s %6s %7s %6s %6s %8s %8s %7s %7s %9s %9s"
          % ("L", "plugs", "keys", "R_base", "R_str", "n", "base%", "stride%",
             "b-only", "s-only", "base_s", "stride_s"))
    for L, plugs, rbase, ratio, keyspace in CELLS:
        rstride = round(rbase * ratio)
        rng = random.Random(SEED * 1000 + L * 100 + plugs)
        nb = ns = bonly = sonly = 0
        tb = ts = 0.0
        for i in range(TRIALS):
            res, secs = trial(L, plugs, rbase, rstride, keyspace, rng)
            nb += res["base"]
            ns += res["stride"]
            bonly += (res["base"] and not res["stride"])
            sonly += (res["stride"] and not res["base"])
            tb += secs["base"]
            ts += secs["stride"]
            if (i + 1) % 10 == 0:
                print("    [L=%d %s] %d/%d  base %d  stride %d  (b-only %d, s-only %d)"
                      "  %.0fs/%.0fs" % (L, keyspace, i + 1, TRIALS, nb, ns,
                                         bonly, sonly, tb, ts),
                      file=sys.stderr, flush=True)
        print("%5d %5d %6s %7d %6d %6d %7.1f%% %7.1f%% %7d %7d %9.1f %9.1f"
              % (L, plugs, keyspace, rbase, rstride, TRIALS,
                 100 * nb / TRIALS, 100 * ns / TRIALS, bonly, sonly, tb, ts))
    print("\nbase_s/stride_s are TOTAL wall seconds per arm -- they must be close for the")
    print("cell to be matched-compute; a large gap invalidates that cell.")
    print("b-only/s-only are the paired win counts and are the signal to read; with these")
    print("sample sizes a near-even split means 'no detectable difference', not a tie win.")


if __name__ == "__main__":
    main()
