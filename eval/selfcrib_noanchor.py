#!/usr/bin/env python3
"""Does a doubled word work as a self-crib WITHOUT the X between the copies?

    python3 eval/selfcrib_noanchor.py                # the measurement
    python3 eval/selfcrib_noanchor.py --trials 40

--self-crib-seeds requires the X.  Its 26 hypotheses are guesses for steck[X],
and self_crib_try seeds with exactly that; the equality edges (this position and
that one carry the SAME unknown plaintext letter) cannot start anything on their
own, because every board entry stays unset until an anchor propagates the guess
into the message.  So `SIEGFRIEDSIEGFRIED` -- a doubled word with no separator
at all -- forms no hypothesis and is invisible to the feature.

That is not rare enough to ignore: of the 66 corpus messages, THREE carry a 6+
doubled word with no separator and no X-separated one anywhere else
(SIEGFRIED, OSTROW, ROSENOW).

THE ALGEBRA DOES NOT NEED THE X.  An equality edge gives

    steck[c_j] = core_j[core_i[steck[c_i]]]

which relates two CIPHERTEXT letters' plugs and mentions no plaintext at all.
Guess steck[c_0] instead of steck[X] and the same closure runs.  What is lost is
not grounding -- both are a real claim about a real letter -- but EDGES: an
anchored hypothesis has L equality edges plus 1-3 anchor edges, an unanchored
one has L equality edges and nothing else.  Fewer constraints means fewer
contradictions, so more hypotheses survive and each deduces less.

WHAT THIS MEASURES, at the true key, on the same messages under the same keys:

  anchored    W X W, guess steck[X], separator asserted (what ships)
  unanchored  W W,   guess steck[c_0], no anchor at all

reported as hypotheses surviving, cables deduced, whether a CORRECT hypothesis
exists (every deduced plug agreeing with the true board), and where IC ranks it.
A hypothesis deducing no cable is not counted correct: it is consistent with any
board.
"""
import argparse
import collections
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import selfcrib_probe as SC                                    # noqa: E402
from crib_menu import UNSET, core_rows, corpus, random_key      # noqa: E402
from ring_stride_geometry_probe import crypt, num, plugboard    # noqa: E402

X = SC.X


class GapMenu:
    """A doubled word with an arbitrary gap between the copies.

    gap=1 with anchor=True is what --self-crib-seeds builds (W X W, separator
    asserted to be plaintext X).  gap=0 with anchor=False is the case it cannot
    see (W W).  Same edge shapes either way, so selfcrib_probe.deduce runs both.
    """

    def __init__(self, cipher, at, L, gap, anchor, flank=False):
        c = num(cipher)
        self.valid = True
        self.equal = [(int(c[at + t]), at + t,
                       int(c[at + L + gap + t]), at + L + gap + t)
                      for t in range(L)]
        # `anchor` asserts the SEPARATOR is plaintext X (only meaningful at
        # gap 1); `flank` asserts the letter BEFORE the first copy is.  A
        # tandem repeat has no separator but usually does have a left flank --
        # 4 of 4 in this corpus -- so it is not anchorless in practice.
        want = []
        if anchor:
            want.append(at + L)
        if flank:
            want.append(at - 1)
        self.anchor_edges = []
        for pos in want:
            if not (0 <= pos < len(c)):
                self.valid = False
            elif int(c[pos]) == X:
                self.valid = False            # no letter encrypts to itself
            else:
                self.anchor_edges.append((X, int(c[pos]), pos))
        self.seed = X if self.anchor_edges else self.equal[0][0]


def ic(seq):
    cc = collections.Counter(seq)
    n = len(seq)
    return sum(v * (v - 1) for v in cc.values()) / (n * (n - 1)) if n > 1 else 0.0


def decrypt_ic(c, rows, board):
    s = np.arange(26)
    for x in range(26):
        if board[x] != UNSET:
            s[x] = board[x]
    return ic(s[rows[np.arange(len(c)), s[c]]])


def score(menu, ct, rows, plug):
    """(survivors, cables of the best, correct-exists, IC rank of correct)."""
    if not menu.valid:
        return 0, 0, False, None
    alive, board = SC.deduce(menu, rows)
    c = num(ct)
    hyps = []
    for h in range(26):
        if not alive[h]:
            continue
        hyps.append((h, board[h].copy(), decrypt_ic(c, rows, board[h])))
    if not hyps:
        return 0, 0, False, None
    cables = [sum(1 for x in range(26)
                  if b[x] != UNSET and b[x] != x) // 2 for _, b, _ in hyps]
    order = sorted(range(len(hyps)), key=lambda i: -hyps[i][2])
    rank = None
    for pos, i in enumerate(order, 1):
        b = hyps[i][1]
        good, cable = True, False
        for x in range(26):
            if b[x] == UNSET:
                continue
            if b[x] != plug[x]:
                good = False
                break
            if b[x] != x:
                cable = True
        if good and cable:
            rank = pos
            break
    return len(hyps), int(np.mean(cables)), rank is not None, rank


def doubles(t, L, gap):
    """Every doubling of L+ letters at this gap whose word contains no X."""
    out = []
    for i in range(len(t)):
        for n in range(L, 20):
            j = i + n + gap
            if j + n > len(t):
                break
            w = t[i:i + n]
            if "X" in w:
                continue
            if gap == 1 and t[i + n] != "X":
                continue
            if t[i:i + n] == t[j:j + n]:
                out.append((i, n))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=60)
    ap.add_argument("--minlen", type=int, default=6)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260820)
    ap.add_argument("--out", default="eval/results-selfcrib-noanchor.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    texts = corpus()
    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    # ISOLATING THE ANCHOR needs the same word at the same alignment with only
    # the assertion removed -- not a different doubling in a different message.
    # Messages carrying BOTH kinds are too few for that (1 in this corpus), so
    # the controlled arm takes the W X W messages and simply declines to assert
    # the separator: identical edges, identical positions, anchor gone.
    wxw = [t for t in texts if doubles(t, a.minlen, 1)]
    ww = [t for t in texts if doubles(t, a.minlen, 0)]
    say(__doc__.split("\n")[0])
    say("\n%d trials, %d-pair board, true key, doubled word of %d+ letters"
        % (a.trials, a.plugs, a.minlen))
    say("corpus: %d messages carry W X W, %d carry a separator-free W W\n"
        % (len(wxw), len(ww)))

    rows_out = []
    for label, pool, gap, anchor, *fl in (
            ("W X W, separator asserted", wxw, 1, True),
            ("W X W, anchor withheld", wxw, 1, False),
            ("W W, separator-free", ww, 0, False),
            ("W W + left flank asserted", ww, 0, False, True)):
        if not pool:
            continue
        surv, cab, corr, ranks, top1, top5 = [], [], 0, [], 0, 0
        for _ in range(a.trials):
            pt = rng.choice(pool)
            d = doubles(pt, a.minlen, gap)
            at, L = rng.choice(d)
            w, r, ring, start = random_key(rng)
            plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                             a.plugs)
            ct = crypt(pt, w, r, ring, start, plug)
            rows = core_rows(w, r, ring, start, len(ct))
            n, c, ok, rk = score(
                GapMenu(ct, at, L, gap, anchor, bool(fl and fl[0])),
                ct, rows, plug)
            surv.append(n)
            cab.append(c)
            if ok:
                corr += 1
                ranks.append(rk)
                top1 += (rk == 1)
                top5 += (rk <= 5)
        rows_out.append((label, np.mean(surv), np.mean(cab), corr,
                         a.trials, top1, top5, ranks))

    say("%-30s %8s %8s %10s %7s %7s" % ("arm", "surv", "cables", "correct",
                                        "rank 1", "top-5"))
    say("-" * 76)
    for label, s, c, corr, tot, t1, t5, ranks in rows_out:
        say("%-30s %8.1f %8.1f %10s %7d %7d"
            % (label, s, c, "%d/%d" % (corr, tot), t1, t5))

    say()
    say("surv    = hypotheses of 26 surviving the deduction (fewer = sharper)")
    say("cables  = real plug pairs the average survivor deduces")
    say("correct = trials where a survivor's every plug matches the true board")
    say("          AND it deduces at least one cable")

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
