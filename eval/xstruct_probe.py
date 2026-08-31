#!/usr/bin/env python3
"""Does X-DELIMITER STRUCTURE discriminate where n-grams cannot?

    python3 eval/xstruct_probe.py --trials 80 --lengths 40 60 100

THE IDEA.  Telegraphic German uses X as a word separator, so a correct decrypt
carries structure an n-gram model can only glimpse through a 4-character
window: the GAPS between consecutive X's are word lengths.  Measured on the 75
authentic messages, X is 6.2-6.7% of letters, the gaps have median 7.5-9, and
P(gap = 0) is 0.5%.  A wrong board scatters X at random, which at the same
rate gives geometric gaps with P(gap = 0) = 6.7% -- a factor of thirteen on
that bin alone.  The statistic needs no table beyond a gap histogram, and it
is garble-robust: one corrupted letter moves one gap, where a high-order
n-gram model loses a whole window (which is why the tool's lineage chose
trigrams over hexagrams).

IT IS A STRUCTURE TERM, NOT AN X-RATE TERM.  Scoring "how much X is there"
would duplicate the monogram model, which already knows X is frequent.  So the
score is the mean log-probability PER GAP, conditioned on however many X's the
candidate happens to have, and a text with fewer than two X's scores neutral
rather than badly.

TWO THINGS THE PREVIOUS PROBE GOT WRONG, FIXED HERE:

  EXCERPTS COME FROM ONE MESSAGE.  score_weight_probe.py concatenates the
  corpus and slices anywhere, which fabricates a gap across every message
  boundary -- harmless for n-grams, fatal for a statistic that IS the gap
  distribution.

  THE GAP MODEL IS FITTED LEAVE-ONE-OUT.  The plaintext is drawn from the same
  corpus the distribution is fitted on, so without holding its message out the
  probe scores a text against a model containing that text.  The histogram is
  built per message and the source message subtracted for each trial.

AND THE ONE IT GOT RIGHT, KEPT: two generator sets.  A weighting is
disadvantaged against decoys produced by a model resembling it, and measured
in the weight probe that alone moved lambda's apparent value by 24pp.  Any
verdict here must hold under BOTH.
"""

import argparse
import math
import os
import random
import re
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from score_weight_probe import (Model, decrypts, run, board_str,  # noqa: E402
                                random_board, LET)

MAXGAP = 40          # gaps past this share one tail bin
SHIPPED_W = (1.0, 0.6, 0.3, 0.15)
SHIPPED_LAM = 30.0


def gap_hist(text):
    """Histogram of letters between consecutive X, capped into a tail bin."""
    pos = [i for i, c in enumerate(text) if c == "X"]
    h = Counter()
    for a, b in zip(pos, pos[1:]):
        h[min(b - a - 1, MAXGAP)] += 1
    return h


class GapModel:
    """log10 P(gap), add-one smoothed, fitted with one message held out."""

    def __init__(self, per_msg):
        self.per_msg = per_msg
        self.total = Counter()
        for h in per_msg:
            self.total.update(h)

    def fit_excluding(self, idx):
        h = Counter(self.total)
        h.subtract(self.per_msg[idx])
        n = sum(v for v in h.values() if v > 0)
        # add-one over the MAXGAP+1 bins: no gap value is impossible, and an
        # unseen one must not be an infinite penalty.
        denom = n + (MAXGAP + 1)
        return [math.log10((max(h[g], 0) + 1) / denom)
                for g in range(MAXGAP + 1)]

    @staticmethod
    def score(text, lg):
        """Log-likelihood RATIO of the gaps against random X placement at this
        candidate's OWN X rate, per symbol.

        A raw log-probability cannot be used here, and the failure is not
        subtle: a decrypt with fewer than two X's has no gap to score, and
        whatever constant stands in for it is then compared against real
        texts' genuine scores. Measured, a neutral of 0.0 against a truth
        scoring -1.59 made the term PENALISE the truth in 100% of trials.

        A ratio fixes it by construction: no X's gives 0 because there is no
        evidence either way, gaps that look like real word lengths give a
        positive score, and gaps that look randomly placed give ~0. It also
        keeps this a STRUCTURE term -- the null carries the candidate's own X
        rate, so being X-rich earns nothing here (the monogram model already
        prices that)."""
        n = len(text)
        nx = text.count("X")
        h = gap_hist(text)
        if sum(h.values()) < 1 or nx < 2 or n < 2:
            return 0.0
        p = nx / n
        tot = 0.0
        for g, c in h.items():
            if g < MAXGAP:
                lgeom = math.log10(p) + g * math.log10(1.0 - p)
            else:   # tail bin: P(gap >= MAXGAP) = (1-p)^MAXGAP
                lgeom = MAXGAP * math.log10(1.0 - p)
            tot += c * (lg[g] - lgeom)
        return tot / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=80)
    ap.add_argument("--lengths", type=int, nargs="+", default=[40, 60, 100])
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--language", default="wehrmacht")
    ap.add_argument("--restarts", type=int, default=64)
    ap.add_argument("--generators", nargs="+",
                    default=["i", "t", "q", "a", "f"])
    ap.add_argument("--mu", type=float, nargs="+",
                    default=[0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0])
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    if not os.path.exists(os.path.join(HERE, os.pardir, "enigma")):
        sys.exit("build the binary first (make)")
    ngram = Model(args.language)
    msgs = (decrypts(os.path.join(HERE, "enigma-messages.txt"))
            + decrypts(os.path.join(HERE, "enigma-army-messages-1941.txt")))
    gm = GapModel([gap_hist(m) for m in msgs])
    rng = random.Random(args.seed)

    wins = {mu: 0 for mu in args.mu}
    per_len = {L: {mu: 0 for mu in args.mu} for L in args.lengths}
    per_len_n = dict.fromkeys(args.lengths, 0)
    ntr = 0
    gap_true, gap_dec = [], []

    for L in args.lengths:
        pool = [i for i, m in enumerate(msgs) if len(m) >= L]
        for _ in range(args.trials):
            mi = rng.choice(pool)
            src = msgs[mi]
            off = rng.randrange(0, len(src) - L + 1)
            pt = src[off:off + L]
            w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
            r = "".join(rng.choice(LET) for _ in range(3))
            g = "".join(rng.choice(LET) for _ in range(3))
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct = run(key + ["-s", board_str(random_board(rng, args.plugs))], pt)
            if len(ct) != L:
                continue

            texts = []
            for gl in args.generators:
                a = key + ["-c", "-K", "--polish", "-" + gl.lstrip("-"),
                           "-R", args.restarts, "-T", 1]
                if gl.lstrip("-") != "i":
                    a += ["-l", args.language]
                t = run(a, ct)
                if len(t) == L and t != pt:
                    texts.append(t)
            if not texts:
                continue

            lg = gm.fit_excluding(mi)          # the source message held out
            base_t = Model.score(ngram.parts(pt), SHIPPED_W, SHIPPED_LAM)
            xs_t = GapModel.score(pt, lg)
            dec = [(Model.score(ngram.parts(t), SHIPPED_W, SHIPPED_LAM),
                    GapModel.score(t, lg)) for t in texts]
            gap_true.append(xs_t)
            gap_dec.append(max(d[1] for d in dec))

            ntr += 1
            per_len_n[L] += 1
            for mu in args.mu:
                s_true = base_t + mu * xs_t
                if s_true > max(b + mu * x for b, x in dec):
                    wins[mu] += 1
                    per_len[L][mu] += 1

    if ntr == 0:
        sys.exit("no usable trials")
    print(f"# {ntr} trials, L={args.lengths}, {args.plugs} plugs, "
          f"generators {' '.join(args.generators)}, seed {args.seed}")
    print(f"# shipped mixture {SHIPPED_W} lambda={SHIPPED_LAM}, "
          f"plus mu * (mean log10 P per X-gap)")
    print(f"# gap model add-one smoothed, fitted LEAVE-ONE-MESSAGE-OUT\n")
    print(f"{'mu':>6} {'truth wins':>11}   " +
          "  ".join(f"L={L}" for L in args.lengths))
    for mu in args.mu:
        cells = "  ".join(
            f"{100 * per_len[L][mu] / (per_len_n[L] or 1):5.1f}%"
            for L in args.lengths)
        tag = "   <-- mu=0 is the shipped model" if mu == 0 else ""
        print(f"{mu:>6.2f} {100 * wins[mu] / ntr:>10.1f}%   {cells}{tag}")
    sep = sum(1 for a, b in zip(gap_true, gap_dec) if a > b)
    print(f"\nX-gap statistic ALONE ranks the truth above every decoy in "
          f"{100 * sep / ntr:.1f}% of trials")
    print(f"  mean log10 P per gap: truth "
          f"{sum(gap_true) / ntr:+.3f}, best decoy "
          f"{sum(gap_dec) / ntr:+.3f}")


if __name__ == "__main__":
    main()
