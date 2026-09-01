#!/usr/bin/env python3
"""Does the LAGGED-COINCIDENCE PROFILE discriminate beyond IC?  The screen.

    python3 eval/kappa_probe.py --trials 80 --lengths 40 60 100 \
            [--generators <letters>]

THE STATISTIC.  kappa(k) is the coincidence rate of the text with itself at
lag k.  IC is (a weighted average of) kappa over ALL lags, so the tool's most
trusted statistic is the mean of a curve it never looks at.  The curve's
signal is SPARSE -- each repeated token spikes exactly one lag -- so the
combiner is the SUM OF THE TOP 3 per-lag z-scores, measured better than the
mean (which dilutes and re-derives IC), the max (noisy, and the easiest for a
climb to fake), and fixed thresholds (often zero at L=40) at every length
tried (60 paired trials per length per combiner).

The per-lag z is taken against the TEXT'S OWN expected match rate q = sum of
squared letter frequencies, so the term carries lag STRUCTURE and not IC over
again.  Equality-only, hence invariant to the exit plugboard: t[i] == t[i+k]
iff the pre-exit letters match, so no exit plug can create or destroy a peak.
The ENTRY application still shapes it, which is why a climb can in principle
game it -- one manufactured repeat fakes one lag -- and why this screen's
verdict is not the last word.

NO FITTED TABLE AND NO HELD-OUT SPLIT NEEDED: the null is computed from the
candidate itself, so unlike the X-gap model there is nothing to leak.
Excerpts still come from ONE message -- a repeat straddling a message
boundary would be fabricated structure.

THE STANDING CAVEAT, sharpened by the X-structure result: these decoys are
strong climb winners under n-gram and IC models, none of which optimises lag
structure, so the term faces a pool never selected against it.  X-structure
passed exactly this screen under both generator sets and then INVERTED end to
end (results-xstruct-ab.txt).  A pass here therefore licenses an
implementation and a paired break50 A/B, and nothing more; a fail here kills
the idea cheaply, which is what screens are for.
"""

import argparse
import math
import os
import random
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from score_weight_probe import (Model, decrypts, run, board_str,  # noqa: E402
                                random_board, LET)

SHIPPED_W = (1.0, 0.6, 0.3, 0.15)
SHIPPED_LAM = 30.0
KMAX = 30
TOPM = 3


def kappa_top(text):
    """Sum of the top TOPM per-lag coincidence z-scores, lags 1..KMAX."""
    n = len(text)
    if n < 12:
        return 0.0
    f = Counter(text)
    q = sum(v * v for v in f.values()) / (n * n)
    if q <= 0.0 or q >= 1.0:
        return 0.0
    zs = []
    for k in range(1, min(KMAX, n - 10) + 1):
        nn = n - k
        m = sum(text[i] == text[i + k] for i in range(nn))
        sd = math.sqrt(nn * q * (1.0 - q))
        zs.append((m - nn * q) / sd if sd > 0 else 0.0)
    return sum(sorted(zs)[-TOPM:])


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
                    default=[0.0, 0.01, 0.03, 0.1, 0.3, 1.0])
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    if not os.path.exists(os.path.join(HERE, os.pardir, "enigma")):
        sys.exit("build the binary first (make)")
    ngram = Model(args.language)
    msgs = (decrypts(os.path.join(HERE, "enigma-messages.txt"))
            + decrypts(os.path.join(HERE, "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)

    wins = {mu: 0 for mu in args.mu}
    per_len = {L: {mu: 0 for mu in args.mu} for L in args.lengths}
    per_len_n = dict.fromkeys(args.lengths, 0)
    ntr = 0
    alone = 0
    k_true, k_dec = [], []

    for L in args.lengths:
        pool = [m for m in msgs if len(m) >= L]
        for _ in range(args.trials):
            src = rng.choice(pool)
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

            base_t = Model.score(ngram.parts(pt), SHIPPED_W, SHIPPED_LAM)
            kt = kappa_top(pt)
            dec = [(Model.score(ngram.parts(t), SHIPPED_W, SHIPPED_LAM),
                    kappa_top(t)) for t in texts]
            k_true.append(kt)
            k_dec.append(max(d[1] for d in dec))
            if kt > max(d[1] for d in dec):
                alone += 1
            ntr += 1
            per_len_n[L] += 1
            for mu in args.mu:
                if base_t + mu * kt > max(b + mu * x for b, x in dec):
                    wins[mu] += 1
                    per_len[L][mu] += 1

    if ntr == 0:
        sys.exit("no usable trials")
    print(f"# {ntr} trials, L={args.lengths}, {args.plugs} plugs, "
          f"generators {' '.join(args.generators)}, seed {args.seed}")
    print(f"# score = shipped fused + mu * (top-{TOPM} lagged-kappa z sum, "
          f"lags 1..{KMAX})\n")
    print(f"{'mu':>6} {'truth wins':>11}   "
          + "  ".join(f"L={L}" for L in args.lengths))
    for mu in args.mu:
        cells = "  ".join(
            f"{100 * per_len[L][mu] / (per_len_n[L] or 1):5.1f}%"
            for L in args.lengths)
        tag = "   <-- mu=0 is the shipped model" if mu == 0 else ""
        print(f"{mu:>6.2f} {100 * wins[mu] / ntr:>10.1f}%   {cells}{tag}")
    print(f"\nkappa statistic ALONE ranks truth above every decoy in "
          f"{100 * alone / ntr:.1f}% of trials")
    print(f"  top-{TOPM} z sum: truth {sum(k_true) / ntr:+.2f}, "
          f"best decoy {sum(k_dec) / ntr:+.2f}")


if __name__ == "__main__":
    main()
