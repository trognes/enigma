#!/usr/bin/env python3
"""Can an ordinary crib's surviving hypotheses be ranked by IC, as self-cribs are?

    python3 eval/crib_ic_rank.py                 # the measurement
    python3 eval/crib_ic_rank.py --trials 40

WHY ASK.  `--self-crib-seeds` ranks its deduced boards by the index of
coincidence of their decrypt and climbs only the top K; that was decisive
(CHANGELOG, MODERN_BREAKING_NOTES).  `crib_unit()` in enigma.cc does the same
deduction for an ordinary crib but then runs a FULL plugboard climb on EVERY
surviving (alignment, hypothesis) pair.  So the same lever is available -- if
there is a population to rank, and if IC actually points at the right member.

THERE IS A POPULATION, AND IT IS THE INTERESTING REGIME.  Measured on a
172-letter message swept over 676 keys, surviving hypotheses per key:

    10-letter crib   95.7        <-- ~96 full climbs per key
    14-letter crib    0.98
    19-letter crib    0.001

Long cribs reject nearly everything and leave nothing to rank; SHORT ones leave
~100 climbs per key.  That is exactly the regime CLAUDE.md records as unusable
("16 letters is the swept floor; below it a crib can only seed a climb") -- and
short cribs are the ones most likely to be PRESENT (93% of messages carry an
8-letter crib against 3% for a 20-letter one).

WHAT THIS MEASURES.  At the TRUE rotor key, enumerate every surviving
hypothesis over every alignment, decrypt under its deduced partial board, and
rank by IC.  Then ask where the CORRECT hypothesis lands -- correct meaning
every plug it deduces agrees with the true board.  If it sits in the top few of
~100, climbing the top K is a ~10x saving for little loss, and short cribs
become usable.  If it scatters, the idea is dead and the current climb-them-all
is right.

A hypothesis deducing NO cable at all is not counted as correct: it is
consistent with any board and would make the recall figure meaningless.
"""
import argparse
import collections
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import Menu, UNSET, core_rows, corpus, deduce_all, random_key  # noqa: E402
from ring_stride_geometry_probe import crypt, num, plugboard                  # noqa: E402


def ic(seq):
    c = collections.Counter(seq)
    n = len(seq)
    return sum(v * (v - 1) for v in c.values()) / (n * (n - 1)) if n > 1 else 0.0


def decrypt_ic(c, rows, board):
    """IC of the decrypt under a PARTIAL board (unset letters self-steckered).

    p_i = steck[core_i[steck[c_i]]] -- exactly enigma.cc's decode_at.
    """
    s = np.arange(26)
    for x in range(26):
        if board[x] != UNSET:
            s[x] = board[x]
    return ic(s[rows[np.arange(len(c)), s[c]]])


def hypotheses(ct, rows, crib):
    """Every surviving (alignment, hypothesis), with its board and IC."""
    c = num(ct)
    out = []
    for at in range(0, len(ct) - len(crib) + 1):
        m = Menu(crib, ct, at)
        if not m.valid:
            continue
        alive, board = deduce_all(m, rows)
        for h in range(26):
            if not alive[h]:
                continue
            out.append((at, h, board[h].copy(),
                        decrypt_ic(c, rows, board[h])))
    return out


def is_correct(board, plug):
    """Every deduced plug agrees with the true board, and at least one is a
    real cable (a board deducing only self-steckers is consistent with
    anything)."""
    cable = False
    for x in range(26):
        if board[x] == UNSET:
            continue
        if board[x] != plug[x]:
            return False
        if board[x] != x:
            cable = True
    return cable


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=60)
    ap.add_argument("--cribs", default="8,10,12,14",
                    help="crib lengths to test")
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260818)
    ap.add_argument("--out", default="eval/results-crib-ic-rank.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    texts = [t for t in corpus() if len(t) >= 120]
    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    say(__doc__.split("\n")[0])
    say("\n%d trials per crib length, authentic German, %d-pair board,"
        % (a.trials, a.plugs))
    say("crib taken from the true plaintext, alignment SWEPT (not pinned).\n")
    say("%5s %8s %9s %9s %9s %9s %9s"
        % ("len", "hyps", "correct", "top-1", "top-5", "top-10", "median"))
    say("-" * 62)

    rows_out = []
    for L in [int(x) for x in a.cribs.split(",")]:
        ranks, counts, found = [], [], 0
        for _ in range(a.trials):
            pt = rng.choice(texts)
            w, r, ring, start = random_key(rng)
            plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                             a.plugs)
            ct = crypt(pt, w, r, ring, start, plug)
            rows = core_rows(w, r, ring, start, len(ct))
            at = rng.randrange(0, len(pt) - L)
            crib = pt[at:at + L]
            hs = hypotheses(ct, rows, crib)
            if not hs:
                continue
            counts.append(len(hs))
            order = sorted(range(len(hs)), key=lambda i: -hs[i][3])
            rk = None
            for pos, i in enumerate(order, 1):
                if is_correct(hs[i][2], plug):
                    rk = pos
                    break
            if rk is not None:
                found += 1
                ranks.append(rk)
        if not counts:
            say("%5d  (no surviving hypotheses)" % L)
            continue
        ranks_a = np.array(ranks) if ranks else np.array([0])
        say("%5d %8.1f %9s %9s %9s %9s %9.0f"
            % (L, np.mean(counts), "%d/%d" % (found, len(counts)),
               "%d" % int((ranks_a == 1).sum()),
               "%d" % int((ranks_a <= 5).sum()),
               "%d" % int((ranks_a <= 10).sum()),
               np.median(ranks_a)))
        rows_out.append((L, np.mean(counts), found, len(counts), ranks_a))

    say()
    say("hyps    = surviving (alignment, hypothesis) pairs at the TRUE key,")
    say("          i.e. how many full climbs crib_unit() runs there today")
    say("correct = trials where a hypothesis matching the true board exists")
    say("top-N   = of those, how many rank in the top N by IC")
    say()
    for L, mean, found, tot, ranks_a in rows_out:
        if found:
            keep = int((ranks_a <= 10).sum())
            say("L=%-3d climbing the top 10 instead of all %.0f keeps %d of %d "
                "correct" % (L, mean, keep, found))

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
