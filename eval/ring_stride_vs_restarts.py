#!/usr/bin/env python3
"""Settles the open question left by eval/ring_stride_wehrmacht_probe.py: --ring-stride
loses accuracy per key searched, but it also searches fewer keys -- so is the compute it
saves better spent on more -R restarts than on an exhaustive ring2 sweep?

That earlier probe compared at FIXED work per candidate (a plain scan, true plugboard
given via -s, no restarts), which measures the accuracy cost but says nothing about the
trade. This one compares at MATCHED WALL TIME with the plugboard HIDDEN, which is the
only setting where -R does anything:

    A (baseline) : no stride,        -R N
    B (stride)   : --ring-stride K,  -R round(N * ratio)

THE KEYSPACE MUST HAVE ring1 WILDCARDED OR THE QUESTION IS MOOT. The refinement pass
costs 25 * rc[1] * gc[1] * 26 keys (times -R, like everything else), a FIXED cost the
coarse saving has to beat. With ring1 pinned -- which is the tool's own default, -r
"AA." -- there is not enough coarse pass left to beat it, and --ring-stride costs MORE
than it saves. Measured here at L=100, -R 20, K=2:

    -r AA. -g A..     17 576 keys unstrided ->  25 688 strided    stride LOSES
    -r A.. -g AK.     17 576 keys unstrided ->  25 688 strided    stride LOSES
    -r A.. -g A..    102 076 keys unstrided ->  69 913 strided    1.46x saving

so only the third shape can answer the question. (The binary warns on the first two.)
An earlier version of this harness ran every cell with -r "AA." and could therefore only
ever have measured a loss; it was written when the refinement was a +/-K/2 window of K-1
values, which was cheap enough not to need ring1 open. Widening the refinement to every
skipped ring2 changed that, and the harness had not caught up.

The ratio is calibrated PER CELL by direct timing, not assumed -- both the coarse pass
and the refinement scale with -R, so the equal-R wall-time ratio is R-independent and is
the right multiplier. Measured on the 102k-key shape at L=100: 1.40 (K=2) and 1.86
(K=3), against key-count ratios of 1.46 and 1.88. The script accumulates real wall time
per arm and prints both totals, so a drifted match shows up in the output instead of
quietly invalidating the comparison.

ALWAYS 10 PLUGS -- standard Wehrmacht, and the `make crackquality` default. Fewer plugs
makes recovery easier and so makes a cell "measurable" sooner, but it is not the regime
the tool is for, and a trade measured on an unrealistically weak plugboard need not hold
at 10. When a cell floors at 0% the fix is more restarts or a longer message -- never
fewer plugs, and no longer a smaller keyspace either, since shrinking the keyspace is
exactly what removes the stride's reason to exist.

WHAT IS REACHABLE AT 10 PLUGS WITH THE BOARD HIDDEN, on the 102k-key shape (measured,
6-trial probes). Most short-message cells simply cannot discriminate:

    L    -R    exact    s/run
    100  8     0/6      79       floored

The joint rotor+plugboard recovery, not the stride, is what fails in a floored cell.
(For contrast, with the board GIVEN the rotor key alone is recoverable at L=40-60 --
eval/ring_stride_wehrmacht_probe.py -- so it is the hidden 10-plug board that costs the
short lengths, not the rotor search.)

Cells are "L:plugs:R:ratio:keyspace[:K]", K defaulting to 2. Keyspace is `mid` (ring1
and start1 both wildcarded, ~102k keys -- the only shape where the stride pays) or
`pin1` / `open` (ring1 pinned, kept only to demonstrate the loss). ring0/start0 are
pinned to A everywhere, since wheel 0's ring x start collapses to a pure offset (7.10)
and leaving it open only adds a redundant 26x to both arms. ring2 stays fully wildcarded
in every cell -- that is the dimension --ring-stride acts on.

Paired: both arms see the identical key, plugboard and plaintext, so the difference is
reported as win/loss counts as well as rates.

Usage: python3 eval/ring_stride_vs_restarts.py
Env: TRIALS (80), SEED (0), THREADS (4), CELLS
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
DEFAULT_CELLS = "150:10:8:1.40:mid:2 150:10:8:1.86:mid:3"


def parse_cell(s):
    """L:plugs:R_base:ratio:keyspace[:K] -- K defaults to 2."""
    f = s.split(":")
    L, plugs, r, ratio, keys = f[:5]
    K = int(f[5]) if len(f) > 5 else 2
    return int(L), int(plugs), int(r), float(ratio), keys, K


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


def trial(L, plugs, rbase, rstride, keyspace, K, rng):
    block = rng.choice([b for b in CORPUS if len(b) >= L])
    off = rng.randrange(0, len(block) - L + 1)
    pt = block[off:off + L]
    r1 = rng.choice(ALPHA) if keyspace == "mid" else "A"
    r2, g1, g2 = rng.choice(ALPHA), rng.choice(ALPHA), rng.choice(ALPHA)
    letters = list(ALPHA)
    rng.shuffle(letters)
    board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(plugs))
    ct = run(["-i", "-u", "B", "-w", "123", "-r", "A" + r1 + r2,
              "-g", "A" + g1 + g2, "-s", board], pt)

    # ring1 open is what gives the coarse saving something to beat the refinement's
    # fixed cost with; see the ring1 note at the top.
    rpat = "A.." if keyspace == "mid" else "AA."
    gpat = "A" + g1 + "." if keyspace == "pin1" else "A.."
    common = ["-c", "-J", "--polish", "-f", "-l", "wehrmacht", "-u", "B", "-w", "123",
              "-r", rpat, "-g", gpat, "-T", THREADS]
    out, secs = {}, {}
    for name, extra in (("base", ["-R", str(rbase)]),
                        ("stride", ["-R", str(rstride), "--ring-stride", str(K)])):
        t0 = time.time()
        out[name] = run(common + extra, ct)
        secs[name] = time.time() - t0
    return {k: (out[k] == pt) for k in out}, secs


def main():
    print("%5s %5s %6s %7s %6s %6s %8s %8s %7s %7s %9s %9s"
          % ("L", "K", "keys", "R_base", "R_str", "n", "base%", "stride%",
             "b-only", "s-only", "base_s", "stride_s"))
    for L, plugs, rbase, ratio, keyspace, K in CELLS:
        rstride = round(rbase * ratio)
        rng = random.Random(SEED * 1000 + L * 100 + plugs)
        nb = ns = bonly = sonly = 0
        tb = ts = 0.0
        for i in range(TRIALS):
            res, secs = trial(L, plugs, rbase, rstride, keyspace, K, rng)
            nb += res["base"]
            ns += res["stride"]
            bonly += (res["base"] and not res["stride"])
            sonly += (res["stride"] and not res["base"])
            tb += secs["base"]
            ts += secs["stride"]
            if (i + 1) % 10 == 0:
                print("    [L=%d K=%d %s] %d/%d  base %d  stride %d  (b-only %d, s-only %d)"
                      "  %.0fs/%.0fs" % (L, K, keyspace, i + 1, TRIALS, nb, ns,
                                         bonly, sonly, tb, ts),
                      file=sys.stderr, flush=True)
        print("%5d %5d %6s %7d %6d %6d %7.1f%% %7.1f%% %7d %7d %9.1f %9.1f"
              % (L, K, keyspace, rbase, rstride, TRIALS,
                 100 * nb / TRIALS, 100 * ns / TRIALS, bonly, sonly, tb, ts))
    print("\nbase_s/stride_s are TOTAL wall seconds per arm -- they must be close for the")
    print("cell to be matched-compute; a large gap invalidates that cell.")
    print("b-only/s-only are the paired win counts and are the signal to read; with these")
    print("sample sizes a near-even split means 'no detectable difference', not a tie win.")


if __name__ == "__main__":
    main()
