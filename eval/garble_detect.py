#!/usr/bin/env python3
"""Can a single-letter quad-score spike detect a garble?  -- measured by
injecting random garbles into TRUE Wehrmacht decrypts and asking whether the
garbled positions light up.

    python3 eval/garble_detect.py --trials 300 --lengths 107 167 \
            --garble 0.02 0.04 0.06 0.08 0.10

A garble is one corrupted plaintext letter (Enigma has no diffusion, so one
ciphertext error damages exactly one plaintext letter).  Quad windows are
width 4, so a wrong letter sits in <=4 overlapping quadgrams and drives each
toward the hapax floor.  The detector reads a CORRECT decrypt (the state after
a successful climb -- the binding precondition), and for every position scores
all 25 single-letter substitutions over the <=4 windows covering it:

    gain_i = max_letter  sum(quad windows covering i, with that letter)
             - sum(quad windows covering i, with the current letter)

gain_i > 0 means some substitution improves the local quad score; a real
garble's own true letter is one of the 25, so its gain is large.  The question
is whether genuine off-distribution German (names, telegraphic tokens) produces
the same spikes -- that is what bounds precision.

Reported per (length, garble rate):
  AUC       P(a garbled position outscores a random clean position) -- the
            threshold-free separation; 0.5 = useless, 1.0 = perfect
  recall    fraction of injected garbles flagged, at a threshold set on CLEAN
            text to a fixed false-positive budget (flags/100 clean letters)
  precision fraction of flags that are real garbles, at that threshold

No enigma binary is needed: a single-letter change is a local quad effect and
IC (the other half of -f) barely moves, so this scores the quad table
directly, in full float precision.
"""

import argparse
import math
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
IDX = {c: i for i, c in enumerate(LET)}


def load_quad(lang="wehrmacht"):
    """quad[a][b][c][d] = log10 prob, unseen floored at a hapax."""
    path = os.path.join(NGRAMS, f"{lang}_quadgrams.txt")
    counts = {}
    total = 0
    for ln in open(path, encoding="utf-8"):
        p = ln.split()
        if len(p) != 2:
            continue
        g = p[0].upper()
        if len(g) != 4 or any(c not in IDX for c in g):
            continue
        c = int(p[1])
        counts[g] = c
        total += c
    floor = math.log10(1.0 / total)
    tbl = [[[[floor] * 26 for _ in range(26)] for _ in range(26)]
           for _ in range(26)]
    for g, c in counts.items():
        a, b, cc, d = (IDX[x] for x in g)
        tbl[a][b][cc][d] = math.log10(c / total)
    return tbl, floor


def decrypts(path):
    out = []
    txt = open(path, encoding="utf-8").read()
    for blk in txt.split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1).upper()))
    return out


def win_sum(nums, i, tbl):
    """sum of quad logprobs over every width-4 window containing position i."""
    n = len(nums)
    s = 0.0
    for j in range(max(0, i - 3), min(n - 4, i) + 1):
        a, b, c, d = nums[j], nums[j + 1], nums[j + 2], nums[j + 3]
        s += tbl[a][b][c][d]
    return s


def gain_at(nums, i, tbl):
    """max single-letter improvement at position i (>=0), and the margin to
    the second-best substitution (a real garble has one standout letter; an
    off-distribution name improves diffusely)."""
    base = win_sum(nums, i, tbl)
    orig = nums[i]
    best = second = base
    for x in range(26):
        if x == orig:
            continue
        nums[i] = x
        s = win_sum(nums, i, tbl)
        if s > best:
            second = best
            best = s
        elif s > second:
            second = s
    nums[i] = orig
    return best - base, best - second


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=300)
    ap.add_argument("--lengths", type=int, nargs="+", default=[107, 167])
    ap.add_argument("--garble", type=float, nargs="+",
                    default=[0.02, 0.04, 0.06, 0.08, 0.10])
    ap.add_argument("--fp-budget", type=float, default=2.0,
                    help="flags per 100 clean letters that set the threshold")
    ap.add_argument("--seed", type=int, default=13)
    ap.add_argument("--lang", default="wehrmacht")
    args = ap.parse_args()

    tbl, floor = load_quad(args.lang)
    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    if len(corpus) < max(args.lengths) + 10:
        sys.exit("corpus too short")
    rng = random.Random(args.seed)

    budgets = [1.0, 5.0, 10.0]   # flags per 100 clean letters
    print(f"# garble detection by single-letter quad-score spike, "
          f"-l {args.lang}")
    print(f"# AUC = P(garbled position outscores a random clean position), "
          f"threshold-free")
    print(f"# recall@B / prec@B: threshold set so clean text yields B flags "
          f"per 100 letters\n")
    hdr = (f"{'L':>4} {'grbl':>5}  {'cln-g':>6} {'grb-g':>6}  "
           f"{'AUCg':>5} {'AUCm':>5}  ")
    for b in budgets:
        hdr += f"{'rec@'+str(int(b)):>6} {'prc@'+str(int(b)):>6}  "
    print(hdr)

    for L in args.lengths:
        # collect the clean-position gain distribution once per length: it is
        # the false-positive source and the thresholds are set on it.
        clean_gains = []
        clean_marg = []
        excerpts = []
        for _ in range(args.trials):
            s = rng.randrange(0, len(corpus) - L)
            nums = [IDX[c] for c in corpus[s:s + L]]
            excerpts.append(nums)
            for i in range(L):
                gi, mi = gain_at(nums, i, tbl)
                clean_gains.append(gi)
                clean_marg.append(mi)
        clean_sorted = sorted(clean_gains)
        thr = {}
        for b in budgets:
            q = 1.0 - b / 100.0
            thr[b] = clean_sorted[min(len(clean_sorted) - 1,
                                      int(q * len(clean_sorted)))]
        clean_mean = sum(clean_gains) / len(clean_gains)

        for g in args.garble:
            gpos_gains = []
            gpos_marg = []
            tp = {b: 0 for b in budgets}
            fa = {b: 0 for b in budgets}
            inj_total = 0
            for nums0 in excerpts:
                nums = list(nums0)
                injected = set()
                for i in range(L):
                    if rng.random() < g:
                        x = rng.randrange(26)
                        if x != nums[i]:
                            nums[i] = x
                            injected.add(i)
                inj_total += len(injected)
                for i in range(L):
                    gi, mi = gain_at(nums, i, tbl)
                    if i in injected:
                        gpos_gains.append(gi)
                        gpos_marg.append(mi)
                    for b in budgets:
                        if gi > thr[b]:
                            fa[b] += 1
                            if i in injected:
                                tp[b] += 1
            auc = auc_rank(gpos_gains, clean_gains)
            auc_m = auc_rank(gpos_marg, clean_marg)
            gmean = sum(gpos_gains) / len(gpos_gains) if gpos_gains else 0.0
            row = (f"{L:>4} {g:>5.2f}  {clean_mean:>6.2f} {gmean:>6.2f}  "
                   f"{auc:>5.3f} {auc_m:>5.3f}  ")
            for b in budgets:
                rec = tp[b] / inj_total if inj_total else 0.0
                prc = tp[b] / fa[b] if fa[b] else 0.0
                row += f"{rec:>6.1%} {prc:>6.1%}  "
            print(row)
        print()


def auc_rank(pos, neg):
    """P(random pos > random neg) via midrank; O((n+m) log)."""
    if not pos or not neg:
        return float("nan")
    both = sorted([(v, 1) for v in pos] + [(v, 0) for v in neg])
    # assign midranks
    rank_sum_pos = 0.0
    i = 0
    n = len(both)
    r = 1
    while i < n:
        j = i
        while j < n and both[j][0] == both[i][0]:
            j += 1
        midrank = (r + (r + (j - i) - 1)) / 2.0
        for k in range(i, j):
            if both[k][1] == 1:
                rank_sum_pos += midrank
        r += (j - i)
        i = j
    npos = len(pos)
    nneg = len(neg)
    return (rank_sum_pos - npos * (npos + 1) / 2.0) / (npos * nneg)


if __name__ == "__main__":
    main()
