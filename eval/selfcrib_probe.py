#!/usr/bin/env python3
"""The SELF-CRIB deduction: does a doubled word reject wrong rotor settings?

    python3 eval/selfcrib_probe.py --selftest   # correctness anchor only
    python3 eval/selfcrib_probe.py              # the gate measurement

ENHANCEMENTS.md item 5's surviving idea.  A classic crib knows the plaintext
letter at a position; a SELF-crib knows only that two positions carry the SAME
letter, which a doubled word (ENGELMANN X ENGELMANN) supplies for free from the
ciphertext alone -- no correct decrypt needed, which is exactly what killed the
score-bonus form of the same evidence.

THE ALGEBRA.  Decryption is `p_i = steck[core_i[steck[c_i]]]`.  With p_i == p_j
and steck an involution it cancels from both sides:

    core_i[steck[c_i]] = core_j[steck[c_j]]

The plaintext letter has vanished.  core_j is an involution too, so

    steck[c_j] = core_j[core_i[steck[c_i]]]

-- a propagation rule computable from the rotor key alone.

WHAT THIS MEASURES, and why it is the only number that matters.  Rejection power
comes only from LOOPS: a tree is always satisfiable (guess the root, propagate,
never contradict).  If the 2L ciphertext letters of a doubling are distinct the
menu is L disjoint edges -- a forest, zero loops, zero rejection.  What is meant
to rescue it is the flanking X: the corpus pattern is not `W X W` but
`X W X W X` (96% carry an X left, 71% both sides), and those X's are REAL
known plaintext sharing one left-hand side, so guessing steck[X] deduces three
plugs at once and anchors the otherwise-floating equality edges.

The go/no-go is the per-alignment rejection rate p on a WRONG key, because the
alignment is unknown and a key survives unless EVERY alignment contradicts.
Rejections multiply, so what matters is p^H over H ~ 950 hypotheses:

    p = 0.999  -> 0.999^950  = 39% rejected   (dead: 61% of wrong keys survive)
    p = 0.9999 -> 0.9999^950 = 91% rejected   (marginal)

That compounding is documented, not hypothetical: it takes a 12-letter classic
crib from 99.9% rejection pinned to 5.3% swept (archived/cribs.md 4.2a).  So p
has to be extraordinarily close to 1, far closer than an ordinary crib needs.
Measure it; do not estimate it.
"""
import argparse
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import UNSET, core_rows, corpus, random_key   # noqa: E402
from ring_stride_geometry_probe import (                     # noqa: E402
    crypt, num, plugboard)

X = ord("X") - ord("A")


def doublings(t, minlen=6, maxlen=13, maxmm=0):
    """Every (start, L) whose word repeats across an X, longest first.

    maxmm=0 here: a mismatch would break the p_i == p_j premise the whole
    deduction rests on, so the probe uses exact repeats only.
    """
    out = []
    n = len(t)
    for j in range(minlen, n - minlen):
        if t[j] != "X":
            continue
        for L in range(minlen, min(maxlen, j, n - j - 1) + 1):
            w, v = t[j - L:j], t[j + 1:j + 1 + L]
            if "X" in w or "X" in v or w != v:
                continue
            out.append((j - L, L))
    return sorted(out, key=lambda p: -p[1])


class SelfMenu:
    """A doubled word at a known alignment, as constraints on the plugboard.

    Two edge kinds, and the difference between them is the whole experiment:

      anchor edges  (p, c, i)          classic -- the plaintext letter is known
                                       (the flanking X's)
      equal edges   (ci, i, cj, j)     p_i == p_j, plaintext letter UNKNOWN

    `at` is the first letter of the first copy, `L` its length; the separator
    sits at at+L and the second copy at at+L+1.
    """

    #  variant -> which positions the hypothesis asserts are X.  The separator
    #  is the only one that comes free with the doubling; the flanks are a
    #  GUESS, true 96% (left) and 71% (both) of the time in the corpus, so a
    #  real sweep has to enumerate them and a wrong guess rejects the TRUE key.
    VARIANTS = {"bare": (), "sep": ("s",), "sep+L": ("s", "l"),
                "sep+L+R": ("s", "l", "r")}

    def __init__(self, cipher, at, L, variant="sep+L+R"):
        c = num(cipher)
        self.valid = True
        self.variant = variant
        self.equal = [(int(c[at + t]), at + t,
                       int(c[at + L + 1 + t]), at + L + 1 + t)
                      for t in range(L)]
        want = self.VARIANTS[variant]
        pos_of = {"l": at - 1, "s": at + L, "r": at + 2 * L + 1}
        self.anchor_edges = []
        for k in want:
            pos = pos_of[k]
            if not (0 <= pos < len(c)):
                self.valid = False            # the flank is off the message
                continue
            # Enigma never encrypts a letter to itself, so ciphertext X where
            # the hypothesis says the plaintext is X kills this alignment.
            if int(c[pos]) == X:
                self.valid = False
            self.anchor_edges.append((X, int(c[pos]), pos))
        self.seed = X if self.anchor_edges else self.equal[0][0]


def deduce(menu, rows):
    """All 26 hypotheses at once: hypothesis h is "seed letter plugs to h".

    Returns (alive, board).  Mirrors crib_menu.deduce_all's shape -- the 26 run
    together because they are independent and identically shaped, so the loop
    is over EDGES rather than hypotheses -- with the equality edge added.
    """
    board = np.full((26, 26), UNSET, dtype=np.int64)
    alive = np.ones(26, dtype=bool)
    if not menu.valid:
        return np.zeros(26, dtype=bool), board
    h = np.arange(26)
    board[h, menu.seed] = h
    board[h, h] = menu.seed              # Welchman's diagonal board

    def assign(x, y, idx):
        if idx.size == 0:
            return
        cur = board[idx, x]
        clash = (cur != UNSET) & (cur != y)
        alive[idx[clash]] = False
        r, v = idx[~clash], y[~clash]
        board[r, x] = v
        back = board[r, v]               # reciprocity: steck[y] = x
        clash2 = (back != UNSET) & (back != x)
        alive[r[clash2]] = False
        ok = ~clash2
        board[r[ok], v[ok]] = x

    changed = True
    while changed:
        changed = False
        for p, c, i in menu.anchor_edges:
            core = rows[i]
            for a, b in ((p, c), (c, p)):
                idx = np.nonzero(alive & (board[:, a] != UNSET)
                                 & (board[:, b] == UNSET))[0]
                if idx.size:
                    assign(b, core[board[idx, a]], idx)
                    changed = True
            idx = np.nonzero(alive & (board[:, p] != UNSET)
                             & (board[:, c] != UNSET))[0]
            if idx.size:
                alive[idx[core[board[idx, c]] != board[idx, p]]] = False
        for ci, i, cj, j in menu.equal:
            # steck[c_j] = core_j[core_i[steck[c_i]]], and the mirror image.
            for src, si, dst, dj in ((ci, i, cj, j), (cj, j, ci, i)):
                idx = np.nonzero(alive & (board[:, src] != UNSET)
                                 & (board[:, dst] == UNSET))[0]
                if idx.size:
                    assign(dst, rows[dj][rows[si][board[idx, src]]], idx)
                    changed = True
            idx = np.nonzero(alive & (board[:, ci] != UNSET)
                             & (board[:, cj] != UNSET))[0]
            if idx.size:
                bad = (rows[i][board[idx, ci]] != rows[j][board[idx, cj]])
                alive[idx[bad]] = False
    return alive, board


def rejects(menu, rows):
    return not deduce(menu, rows)[0].any()


def trials(rng, texts, n, minlen=6):
    """Messages carrying an exact doubling, with their true key and board."""
    out = []
    while len(out) < n:
        pt = rng.choice(texts)
        ds = doublings(pt, minlen=minlen)
        if not ds:
            continue
        at, L = ds[0]
        wheels, refl, ring, start = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
        ct = crypt(pt, wheels, refl, ring, start, plug)
        out.append((pt, at, L, (wheels, refl, ring, start), plug, ct))
    return out


def true_variant(pt, at, L):
    """The richest variant the plaintext actually supports.  A hypothesis that
    asserts X where the plaintext has something else is simply WRONG, and
    rejects the true key -- which is how the first version of this probe scored
    37/40 instead of 40/40 (ROMANOVKA is flanked by N and G, not X)."""
    left = (at - 1 >= 0) and pt[at - 1] == "X"
    right = (at + 2 * L + 1 < len(pt)) and pt[at + 2 * L + 1] == "X"
    if left and right:
        return "sep+L+R"
    return "sep+L" if left else "sep"


def selftest(rng, texts, n=40):
    """The true key at the true alignment must SURVIVE, and the plugs it
    deduces must be a subset of the true board.  An involution or composition
    error shows up here rather than as a wrong number downstream."""
    bad_alive = bad_plug = 0
    for pt, at, L, key, plug, ct in trials(rng, texts, n):
        rows = core_rows(key[0], key[1], key[2], key[3], len(ct))
        m = SelfMenu(ct, at, L, variant=true_variant(pt, at, L))
        alive, board = deduce(m, rows)
        # the hypothesis that matches the TRUE board must be one of the
        # survivors: steck[X] is what the true plugboard says it is
        true_h = int(plug[X])
        if not alive[true_h]:
            bad_alive += 1
            continue
        row = board[true_h]
        known = np.nonzero(row != UNSET)[0]
        if np.any(row[known] != plug[known]):
            bad_plug += 1
    print("correctness anchor over %d messages (true key, true alignment):" % n)
    print("   true hypothesis survives      %d/%d" % (n - bad_alive, n))
    print("   deduced plugs match the board %d/%d" % (n - bad_plug, n))
    return bad_alive == 0 and bad_plug == 0


def gate(rng, texts, msgs=40, wrong=200, minlen=6):
    """p_reject on WRONG keys at a SINGLE alignment -- the go/no-go number."""
    print("\ngate: per-alignment rejection on wrong rotor keys")
    print("   %d messages x %d wrong keys each\n" % (msgs, wrong))
    print("   %-9s %-7s %-13s %-11s %s"
          % ("variant", "holds", "rejected", "p_reject", "survive 950 algns"))
    cases = trials(rng, texts, msgs, minlen=minlen)
    for variant in ("bare", "sep", "sep+L", "sep+L+R"):
        rej = tot = holds = 0
        for pt, at, L, key, plug, ct in cases:
            # how often this hypothesis is even TRUE of the plaintext -- a
            # variant that asserts more than the message supports rejects the
            # true key too, so its rejection rate is not free
            tv = true_variant(pt, at, L)
            holds += (variant in ("bare", "sep")
                      or (variant == "sep+L" and tv != "sep")
                      or (variant == "sep+L+R" and tv == "sep+L+R"))
            m = SelfMenu(ct, at, L, variant=variant)
            for _ in range(wrong):
                w2 = random_key(rng)
                rows = core_rows(w2[0], w2[1], w2[2], w2[3], len(ct))
                tot += 1
                rej += rejects(m, rows)
        p = rej / tot
        print("   %-9s %-7s %-13s %-11.6f %.3g"
              % (variant, "%d%%" % round(100 * holds / len(cases)),
                 "%d/%d" % (rej, tot), p, (1 - p) ** 950 if p < 1 else 0.0))
    print("\n   'survive 950 alignments' is (1-p)^950: the fraction of wrong")
    print("   keys the filter would NOT reject on a 150-letter message, where")
    print("   the alignment is unknown and every one must contradict.")


def swept(rng, texts, msgs=4, wrong=40, minlen=6):
    """The number that actually decides it: rejection with the alignment
    UNKNOWN, measured rather than extrapolated.

    (1-p)^H assumes the H hypotheses are independent, and they are not -- same
    key, same ciphertext, overlapping positions.  So run every hypothesis a real
    sweep would run and count the wrong keys that survive ALL of them.  A key
    survives if any single hypothesis stays satisfiable.

    The hypothesis set is every (alignment, length, variant) a sweep must try,
    which is why the flank guess is not free: asserting a flank that the message
    does not have would reject the true key, so both variants have to be tried
    and a key must contradict under each."""
    print("\nswept: rejection with the alignment UNKNOWN (measured, not"
          " extrapolated)")
    lens = range(minlen, 13)
    for pt, at, L, key, plug, ct in trials(rng, texts, msgs, minlen=minlen):
        n = len(ct)
        hyps = []
        for a in range(1, n):
            for ln in lens:
                if a + 2 * ln + 1 > n:
                    break
                for v in ("sep", "sep+L", "sep+L+R"):
                    m = SelfMenu(ct, a, ln, variant=v)
                    if m.valid:
                        hyps.append(m)
        surv = 0
        for _ in range(wrong):
            w2 = random_key(rng)
            rows = core_rows(w2[0], w2[1], w2[2], w2[3], n)
            if any(deduce(m, rows)[0].any() for m in hyps):
                surv += 1
        # the true key must NOT be rejected
        trows = core_rows(key[0], key[1], key[2], key[3], n)
        tok = any(deduce(m, trows)[0].any() for m in hyps)
        print("   L=%-3d n=%-4d %5d hypotheses   wrong keys surviving %3d/%-3d"
              "  true key survives %s"
              % (L, n, len(hyps), surv, wrong, "yes" if tok else "NO (BUG)"))


def terminal_hyps(ct):
    """The TERMINAL-SIGNATURE hypothesis set: the message ends with a doubled
    word, `... X NAME X NAME [X]`.

    Half of the corpus's doublings are exactly this -- a signed surname closing
    the message (RENNER, MATHIAT, STEINECKE, STUERZBAECHER, HENNING after GEZ)
    -- and the position is what makes it worth anything.  The swept self-crib
    dies on ~2800 hypotheses of which ~1200 always survive; pinning the
    alignment to the end leaves only the word's LENGTH unknown, so the set is
    ~20.  The left flank is an X by construction (the name follows a
    separator), and a trailing X is the right flank when present.
    """
    n = len(ct)
    out = []
    for tail in (0, 1):
        for L in range(4, 14):
            at = n - tail - 2 * L - 1
            if at < 1:
                continue
            m = SelfMenu(ct, at, L,
                         variant="sep+L+R" if tail else "sep+L")
            if m.valid:
                out.append(m)
    return out


def terminal(rng, texts, wrong=300):
    """Does the terminal-signature crib reject wrong keys -- and what does the
    assumption cost when the message does NOT end that way?"""
    print("\nterminal signature: the message ends with a doubled word")
    ends, others = [], []
    for t in texts:
        ds = [(a, L) for a, L in doublings(t, minlen=4, maxlen=20)
              if len(t) - (a + 2 * L + 1) <= 1]
        (ends if ds else others).append(t)
    print("   %d corpus messages end with a doubling, %d do not\n"
          % (len(ends), len(others)))
    print("   %-26s %-10s %s" % ("population", "hyps", "outcome"))
    for name, pool, expect in (("ends with a doubling", ends, "must survive"),
                               ("does NOT end with one", others, "cost")):
        surv = tot = h = 0
        for t in pool[:12]:
            wheels, refl, ring, start = random_key(rng)
            plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
            ct = crypt(t, wheels, refl, ring, start, plug)
            hyps = terminal_hyps(ct)
            h += len(hyps)
            rows = core_rows(wheels, refl, ring, start, len(ct))
            tot += 1
            surv += any(deduce(m, rows)[0].any() for m in hyps)
        print("   %-26s %-10.1f true key survives %d/%d   (%s)"
              % (name, h / max(1, tot), surv, tot, expect))

    # the filter's power, on messages the assumption is TRUE of
    print()
    for t in ends[:4]:
        wheels, refl, ring, start = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
        ct = crypt(t, wheels, refl, ring, start, plug)
        hyps = terminal_hyps(ct)
        surv = 0
        for _ in range(wrong):
            w2 = random_key(rng)
            rows = core_rows(w2[0], w2[1], w2[2], w2[3], len(ct))
            if any(deduce(m, rows)[0].any() for m in hyps):
                surv += 1
        print("   n=%-4d %2d hypotheses   wrong keys surviving %3d/%-4d"
              "  = %.3f kept" % (len(ct), len(hyps), surv, wrong, surv / wrong))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--messages", type=int, default=40)
    ap.add_argument("--wrong", type=int, default=200)
    ap.add_argument("--swept-msgs", type=int, default=4)
    ap.add_argument("--swept-wrong", type=int, default=40)
    ap.add_argument("--seed", type=int, default=20260816)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    texts = [t for t in corpus() if doublings(t)]
    print("%d corpus messages carry an exact doubling of 6+ letters\n"
          % len(texts))
    ok = selftest(rng, texts)
    if a.selftest:
        return 0 if ok else 1
    if not ok:
        print("\nSELFTEST FAILED -- not measuring the gate on a broken"
              " deduction.")
        return 1
    gate(rng, texts, msgs=a.messages, wrong=a.wrong)
    swept(rng, texts, msgs=a.swept_msgs, wrong=a.swept_wrong)
    terminal(rng, texts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
