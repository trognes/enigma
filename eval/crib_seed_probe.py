#!/usr/bin/env python3
"""Does a WRONG crib hypothesis ever out-score the right one? (cribs.md §7a
caution 1, §12 step 5a.)

    python3 eval/crib_seed_probe.py --trials 200 --crib 12

THE RISK THIS MEASURES.  A crib pins one letter's plug only by guessing it, so
the solver tries all 26 guesses and keeps whichever board scores best.  Exactly
one of those 26 is the truth; the other 25 seed the climb with plugs that are
simply wrong.  If a wrong one ever converges above the correctly-seeded climb,
the run returns a confident answer that is not the message -- and nothing in the
output says so.  §7a calls this the one thing that could undo the seeding mode,
and it was never measured.

HOW IT IS MEASURED.  Plant a crib in an authentic plaintext, encipher under a
random key and a random 10-cable board, then hand the tool the rotor key and the
crib but NOT the board.  The winner's plugboard is read back from the last
progress line, and the anchor letter -- the one the 26 hypotheses are about --
decides which hypothesis won:

    winner[anchor] == truth[anchor]   ->  the right hypothesis won
    otherwise                         ->  a wrong hypothesis out-scored it

That test is exact and needs no new diagnostic: the anchor's partner IS the
hypothesis.  Reporting recovery beside it shows what the failure costs when it
happens, which is the part that decides whether the mode is usable.
"""
import argparse
import os
import random
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from crib_menu import corpus, random_key, Menu           # noqa: E402
from ring_stride_geometry_probe import txt, plugboard, crypt   # noqa: E402

BIN = os.path.join(HERE, os.pardir, "enigma")
SCORE = re.compile(r"^\s*-?\d+\.\d+\s")


def run_crib(ct, key, crib, at, args):
    """Run the tool with the rotor key and the crib given, the board hidden.
    Returns (plaintext, plugboard dict) from the winning board."""
    wheels, refl, ring, start = key
    cmd = [BIN, "-u", refl, "-w", "".join(str(w + 1) for w in wheels),
           "-r", txt(ring), "-g", txt(start), "-c",
           "--crib", crib, "--crib-at", str(at + 1),   # --crib-at is 1-based
           "-f", "-l", args.lang, "--score", "f10", "-T", str(args.threads)]
    env = dict(os.environ, ENIGMA_SEED="0")
    p = subprocess.run(cmd, input=ct, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True, env=env)
    board, last = {}, None
    for line in p.stderr.splitlines():
        if SCORE.match(line):
            last = line
    if last:
        # score W R G S... A text -- the plug pairs are the two-letter fields
        for f in last.split()[4:]:
            if len(f) == 2 and f.isalpha() and f.isupper():
                board[f[0]] = f[1]
                board[f[1]] = f[0]
    return re.sub(r"[^A-Z]", "", p.stdout.strip().upper()), board


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=100)
    ap.add_argument("--crib", type=int, default=12, help="crib length [12]")
    ap.add_argument("--length", type=int, default=90, help="message letters")
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    texts = [t for t in corpus() if len(t) >= args.length]
    if not texts:
        sys.exit("no corpus text of at least %d letters" % args.length)
    rng = random.Random(args.seed)

    right, wrong, skipped = [], [], 0
    for _ in range(args.trials):
        src = rng.choice(texts)
        off = rng.randrange(len(src) - args.length + 1)
        pt = src[off:off + args.length]
        at = rng.randrange(len(pt) - args.crib + 1)
        crib = pt[at:at + args.crib]
        key = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                         args.plugs)
        wheels, refl, ring, start = key
        ct = crypt(pt, wheels, refl, ring, start, plug)
        menu = Menu(crib, ct, at)
        anchor = menu.anchor()
        if not menu.valid or anchor is None:
            skipped += 1                 # crib cannot sit here at all
            continue

        got, board = run_crib(ct, key, crib, at, args)
        pct = 100.0 * sum(a == b for a, b in zip(pt, got)) / len(pt)
        a_letter = chr(65 + anchor)
        truth = chr(65 + int(plug[anchor]))
        won = board.get(a_letter, a_letter)     # absent = self-steckered
        (right if won == truth else wrong).append(pct)

    n = len(right) + len(wrong)
    if n == 0:
        sys.exit("no usable trials")
    print("crib %d letters, message %d, %d plugs, %d trials (%d skipped: the "
          "crib\ncannot sit at the drawn position)\n"
          % (args.crib, args.length, args.plugs, n, skipped))
    print("  the RIGHT hypothesis won:  %3d / %d = %.0f%%"
          % (len(right), n, 100.0 * len(right) / n))
    print("  a WRONG hypothesis won:    %3d / %d = %.0f%%"
          % (len(wrong), n, 100.0 * len(wrong) / n))
    print()
    for label, v in (("right", right), ("wrong", wrong)):
        if v:
            v = sorted(v)
            print("  recovery when the %-5s hypothesis won: mean %5.1f%%, "
                  "median %5.1f%%, exact %d"
                  % (label, sum(v) / len(v), v[len(v) // 2],
                     sum(1 for x in v if x == 100.0)))
    allv = right + wrong
    print("\n  overall mean recovery %.1f%%, exact %d/%d"
          % (sum(allv) / len(allv), sum(1 for x in allv if x == 100.0), n))


if __name__ == "__main__":
    main()
