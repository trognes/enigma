#!/usr/bin/env python3
"""Does a HIGHER-ORDER coincidence guide the pre-pass better than IC?

ENHANCEMENTS 2a, experiment C step 1 -- the gate that has to pass before a
schedule token is worth building.  Pure Python, no binary change.

THE IDEA.  IC is Sum f^2, the pair-coincidence rate, and nothing forces order
2: Sum f^3 is the triple rate.  Over the corpus, Sum f^3 separates from a flat
null by 2.85x where IC manages 1.49x, which is why this looked promising.

WHY THAT TABLE IS NOT THE TEST, and why this probe exists.  Contrast against a
uniform null measures DETECTION -- is this text German?  A pre-pass does not
detect, it ORDERS BOARDS: at each step it must rank a board with one more
correct plug above a board with one more wrong plug.  A statistic can be
excellent at the first and useless at the second.  An earlier probe here
already showed the detection framing misleading in another way: `z` rises
monotonically with the moment order while actual separation peaks at k = 2-3,
because the null grows badly right-skewed.

WHAT IS MEASURED.  For a true 10-pair board, build candidate boards with n
plugs of which c are correct (n = 1..cap, c = 0..n), decrypt, and score with
each statistic.  Then, for every (n, c), the AUC of
    P(statistic ranks the (c+1)-correct board above the c-correct one)
over independent draws -- 0.5 is a coin flip, 1.0 is perfect.  That is exactly
the comparison the climb makes when choosing its next plug, so a statistic
that cannot win here cannot steer a pre-pass however well it detects German.

Three statistics, all read off the SAME 26-bin histogram:
  ic     Sum n(n-1) / N(N-1)          -- order 2, what -S i4... uses
  cc3    Sum n(n-1)(n-2) / N(N-1)(N-2) -- order 3, the candidate
  mono   Sum n_x log p(x)             -- what -S m4... uses, for reference

FALSIFICATION, fixed in advance (ENHANCEMENTS 2a): if cc3 fails to beat ic,
drop the token rather than tuning a blend until it wins.

  python3 eval/coincidence_order_probe.py
  python3 eval/coincidence_order_probe.py --lengths 60 --keys 400
"""
import argparse
import collections
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import load_plaintexts, rand_pos, rand_wheels
from restart_ladder import core_table

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load_monograms(lang):
    """log10 p(x) per letter, hapax-floored exactly as the binary does."""
    path = os.path.join(ROOT, "ngrams", "%s_monograms.txt" % lang)
    counts = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) == 2 and len(p[0]) == 1 and p[0].isalpha() and p[0].isascii():
            counts[p[0].upper()] = counts.get(p[0].upper(), 0) + int(p[1])
    total = sum(counts.values())
    return [math.log10(max(counts.get(chr(65 + i), 1), 1) / total)
            for i in range(26)]


def stats(hist, n, logp):
    """The three statistics, from one histogram.  Sum f^2 and Sum f^3 use
    FALLING factorials -- the unbiased estimators -- so they are comparable
    across lengths; within one length any monotone form would rank the same."""
    ic = sum(c * (c - 1) for c in hist) / (n * (n - 1))
    cc3 = (sum(c * (c - 1) * (c - 2) for c in hist)
           / (n * (n - 1) * (n - 2)))
    mono = sum(c * logp[i] for i, c in enumerate(hist)) / n
    return ic, cc3, mono


def decode_hist(board_perm, ct_nums, core):
    """Histogram of the decrypt under a board, via p_i = S[core_i[S[c_i]]]."""
    hist = [0] * 26
    for i, c in enumerate(ct_nums):
        hist[board_perm[core[i][board_perm[c]]]] += 1
    return hist


def perm_of(pairs):
    s = list(range(26))
    for a, b in pairs:
        s[a], s[b] = b, a
    return s


def make_board(rng, true_pairs, n, c):
    """A board with n plugs, exactly c of them from the true board.

    The wrong plugs are drawn from letters the chosen correct plugs do not
    already use, and are rejected if they happen to BE a true pair -- so `c`
    is exact, not an expectation.
    """
    correct = rng.sample(true_pairs, c)
    used = {x for p in correct for x in p}
    truth = {frozenset(p) for p in true_pairs}
    wrong = []
    guard = 0
    while len(wrong) < n - c and guard < 500:
        guard += 1
        free = [x for x in range(26) if x not in used]
        if len(free) < 2:
            return None
        a, b = rng.sample(free, 2)
        if frozenset((a, b)) in truth:
            continue
        wrong.append((a, b))
        used.update((a, b))
    if len(wrong) < n - c:
        return None
    return correct + wrong


def auc(better, worse):
    """P(a random `better` draw outranks a random `worse` one), ties at 0.5.

    Computed by merge-counting rather than the O(n^2) double loop, which
    matters at 40k draws per cell.
    """
    if not better or not worse:
        return None
    allv = sorted([(v, 1) for v in better] + [(v, 0) for v in worse])
    wins = ties = 0
    seen_worse = 0
    i = 0
    while i < len(allv):
        j = i
        while j < len(allv) and allv[j][0] == allv[i][0]:
            j += 1
        blk_b = sum(1 for k in range(i, j) if allv[k][1] == 1)
        blk_w = j - i - blk_b
        wins += blk_b * seen_worse
        ties += blk_b * blk_w
        seen_worse += blk_w
        i = j
    return (wins + 0.5 * ties) / (len(better) * len(worse))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lengths", default="40,60,100")
    ap.add_argument("--keys", type=int, default=250)
    ap.add_argument("--boards", type=int, default=24,
                    help="candidate boards per (key, n, c) cell")
    ap.add_argument("--cap", type=int, default=4,
                    help="plug cap the pre-pass runs under")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--language", default="wehrmacht")
    args = ap.parse_args()

    logp = load_monograms(args.language)
    texts = load_plaintexts()
    names = ("ic", "cc3", "mono")

    print("%d plaintexts, %s monograms, %d keys x %d boards per cell, cap %d\n"
          % (len(texts), args.language, args.keys, args.boards, args.cap))
    print("AUC: P(the statistic ranks c+1 correct plugs above c), 0.5 = chance")

    for length in [int(x) for x in args.lengths.split(",")]:
        rng = random.Random(args.seed + length)
        # samples[(n, c)][stat] -> list of values
        samples = collections.defaultdict(lambda: collections.defaultdict(list))
        for _ in range(args.keys):
            perm = list(range(26))
            rng.shuffle(perm)
            true_pairs = [(perm[2 * i], perm[2 * i + 1]) for i in range(10)]
            wheels, ring = rand_wheels(rng), rand_pos(rng)
            pt = rng.choice([p for p in texts if len(p) >= length])
            off = rng.randrange(0, len(pt) - length + 1)
            pt = pt[off:off + length]
            start = rand_pos(rng)
            board_str = " ".join(chr(65 + a) + chr(65 + b)
                                 for a, b in true_pairs)
            ct = enigma_ref.decrypt(pt, wheels, ring, start, board_str)
            ct_nums = [ord(x) - 65 for x in ct]
            core = core_table(wheels, ring, start, length)
            for n in range(1, args.cap + 1):
                for c in range(0, n + 1):
                    for _ in range(args.boards):
                        pairs = make_board(rng, true_pairs, n, c)
                        if pairs is None:
                            continue
                        h = decode_hist(perm_of(pairs), ct_nums, core)
                        for nm, v in zip(names, stats(h, length, logp)):
                            samples[(n, c)][nm].append(v)

        print("\nL = %d" % length)
        print("%6s %6s %8s %8s %8s %9s"
              % ("plugs", "c -> c+1", "ic", "cc3", "mono", "n each"))
        pooled = collections.defaultdict(list)
        for n in range(1, args.cap + 1):
            for c in range(0, n):
                row = []
                for nm in names:
                    a = auc(samples[(n, c + 1)][nm], samples[(n, c)][nm])
                    row.append(a)
                    if a is not None:
                        pooled[nm].append(a)
                if any(x is None for x in row):
                    continue
                print("%6d %8s %8.3f %8.3f %8.3f %9d"
                      % (n, "%d->%d" % (c, c + 1), row[0], row[1], row[2],
                         len(samples[(n, c)][names[0]])))
        print("%6s %8s %8.3f %8.3f %8.3f"
              % ("", "mean", sum(pooled["ic"]) / len(pooled["ic"]),
                 sum(pooled["cc3"]) / len(pooled["cc3"]),
                 sum(pooled["mono"]) / len(pooled["mono"])))

        # BY c, averaged over the board sizes n that can reach it.  Which
        # correct plug is being added matters more than how many wrong ones
        # sit beside it, and the per-n table buries that.  The AUCs are
        # averaged rather than the draws pooled: a c=1 board with n=1 has no
        # wrong plugs and one with n=4 has three, so they are different
        # objects and pooling their values would blur two effects together.
        print("%6s %8s %8s %8s %8s %9s"
              % ("", "-> c =", "ic", "cc3", "mono", "over n"))
        for target in range(1, args.cap + 1):
            per = collections.defaultdict(list)
            for n in range(target, args.cap + 1):
                for nm in names:
                    a = auc(samples[(n, target)][nm],
                            samples[(n, target - 1)][nm])
                    if a is not None:
                        per[nm].append(a)
            if not per["ic"]:
                continue
            print("%6s %8d %8.3f %8.3f %8.3f %9d"
                  % ("", target, sum(per["ic"]) / len(per["ic"]),
                     sum(per["cc3"]) / len(per["cc3"]),
                     sum(per["mono"]) / len(per["mono"]), len(per["ic"])))

    print("\n  ic   = Sum f^2, the index of coincidence (-S i4...)")
    print("  cc3  = Sum f^3, the triple-coincidence rate, the candidate")
    print("  mono = monogram log-probability (-S m4...), for reference")
    print("  A statistic must beat 0.5 to steer a climb at all, and must beat")
    print("  `ic` to justify a new schedule token.")


if __name__ == "__main__":
    main()
