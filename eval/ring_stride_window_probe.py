#!/usr/bin/env python3
"""How wide does --ring-stride's refinement actually have to be?

The refinement used to test only the ring2 values within +-floor(K/2) of the coarse
winner; it now tests all 25 unconditionally (archived/PERFORMANCE.md 7.11, "the
refinement is ~26x cheaper per ring2 value"). This harness re-measures that choice
directly: for window radius w = 1, 2, 3, ... and for the full sweep, how often does
the tool still return the exact plaintext?

Method mirrors eval/ring_stride_wehrmacht_probe.py (the end-to-end harness the
widening decision was made on), differing only in what is swept:
  - a real excerpt of length L from the authentic 1941 message database,
  - a random key (reflector, 3 wheels from I-V, ring/start wildcarded) and a random
    10-pair plugboard given via -s, so this stays a ROTOR-KEY measurement,
  - recover with -f -l wehrmacht and --ring-stride K, once per window radius,
  - "exact" = recovered plaintext byte-identical to the excerpt.

The window is applied through the binary's measurement-only ENIGMA_REFINE_WINDOW
override (unset or >= 13 = the shipped full sweep), so every cell is the real
shipped code path with only the refinement's ring2 set changed.

K=1 (no stride) is run once per trial as the baseline: a trial that also fails at
K=1 is a pre-existing scoring-floor case, not a window effect, and is excluded from
the window-limited miss rate.

Usage: python3 eval/ring_stride_window_probe.py
Env: LENGTHS ("150"), KS ("2 3 5"), WINDOWS ("1 2 3 4 5 6 8 10 13"), TRIALS (40),
     SEED (0), THREADS (4)
"""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")

sys.path.insert(0, HERE)
from ring_stride_wehrmacht_probe import excerpt, random_key, encrypt   # noqa: E402

LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "150").split()]
KS = [int(x) for x in os.environ.get("KS", "2 3 5").split()]
WINDOWS = [int(x) for x in os.environ.get("WINDOWS", "1 2 3 4 5 6 8 10 13").split()]
TRIALS = int(os.environ.get("TRIALS", "40"))
SEED = int(os.environ.get("SEED", "0"))
THREADS = os.environ.get("THREADS", "4")


def recover(ct, u, w, board, K, window=None):
    args = [BIN, "-f", "-l", "wehrmacht", "-u", u, "-w", w, "-r", "...", "-g", "...",
            "-s", board, "-T", THREADS]
    if K > 1:
        args += ["--ring-stride", str(K)]
    env = dict(os.environ)
    if window is not None:
        env["ENIGMA_REFINE_WINDOW"] = str(window)
    else:
        env.pop("ENIGMA_REFINE_WINDOW", None)
    r = subprocess.run(args, input=ct, capture_output=True, text=True, cwd=ROOT, env=env)
    return r.stdout.strip()


def trial(L, rng):
    u, w, r, g, board = random_key(rng)
    pt = excerpt(L, rng)
    ct = encrypt(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", board], pt)
    out = {(1, None): recover(ct, u, w, board, 1) == pt}
    for K in KS:
        for win in WINDOWS:
            out[(K, win)] = recover(ct, u, w, board, K, win) == pt
    return out


def sweep():
    print("# trials=%d seed=%d threads=%s  (window 13 == the shipped full sweep)"
          % (TRIALS, SEED, THREADS))
    print("\t".join(["len", "K", "n", "K=1 base"]
                    + ["w=%d" % w for w in WINDOWS]
                    + ["lost@w1"]))
    for L in LENGTHS:
        rng = random.Random(SEED * 1000 + L)      # paired: same trials for every cell
        rows = [trial(L, rng) for _ in range(TRIALS)]
        base = sum(1 for row in rows if row[(1, None)])
        for K in KS:
            cells = ["%d%%" % round(100 * sum(1 for row in rows if row[(K, w)]) / TRIALS)
                     for w in WINDOWS]
            # window-limited misses: the full sweep recovers it, w=1 does not
            lost = sum(1 for row in rows
                       if row[(K, WINDOWS[-1])] and not row[(K, WINDOWS[0])])
            print("\t".join([str(L), str(K), str(TRIALS),
                             "%d%%" % round(100 * base / TRIALS)]
                            + cells + ["%d" % lost]))
            sys.stdout.flush()


if __name__ == "__main__":
    sweep()
