#!/usr/bin/env python3
"""The SELF-CRIB deduction: does a doubled word reject wrong rotor settings?

    python3 eval/selfcrib_probe.py --selftest        # correctness anchor only
    python3 eval/selfcrib_probe.py                   # every measurement
    python3 eval/selfcrib_probe.py --check-scorers   # anchor eval/ vs ./enigma

Two questions, and they came out opposite ways.  As a FILTER the deduction is
dead -- measured here, and the last section below has the numbers.  As a SEEDER
it is alive, but only in the TERMINAL-SIGNATURE form, where the alignment is
pinned to the end of the message (`... X RENNER X RENNER`) and the hypothesis
set collapses from ~2 800 to ~19: 200/200 trials then have exactly one fully
correct seed, and ranking the ~28 survivors by their decrypt's index of
coincidence puts it first 150 times.  `signature_seeder()`.

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
import math
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import UNSET, core_rows, corpus, random_key   # noqa: E402
from ring_stride_geometry_probe import (                     # noqa: E402
    crypt, jlog, load_counts, num, plugboard, score_table)

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


MODELS = ("i", "m", "b", "t", "q", "a", "f")


def binom_two(x, y):
    """Exact two-sided McNemar over the discordant pairs (x, y)."""
    n = x + y
    if n == 0:
        return 1.0
    tail = sum(math.comb(n, k) for k in range(min(x, y) + 1))
    return min(1.0, tail / 2 ** (n - 1))


def ngram_tables(lang):
    """log10 tables for every n-gram model, as `enigma.cc` loads them."""
    return {"m": jlog(load_counts(1, lang)),
            "b": jlog(load_counts(2, lang)).reshape(26, 26),
            "t": jlog(load_counts(3, lang)).reshape(26, 26, 26),
            "q": score_table("quad", lang),
            "a": score_table("all", lang)}


def model_scores(pt, tab):
    """(k, n) decrypts -> per-model per-symbol score, normalised as the tool
    normalises: mono by n, bi by n-1, tri by n-2, quad/all by n-3, and -f as
    the all-order score plus lambda=30 times the index of coincidence."""
    k, n = pt.shape
    f = np.zeros((k, 26), dtype=np.int64)
    for j in range(26):
        f[:, j] = (pt == j).sum(axis=1)
    ic = (f * (f - 1)).sum(axis=1) / float(n * (n - 1))
    out = {"i": ic,
           "m": tab["m"][pt].sum(1) / n,
           "b": tab["b"][pt[:, :-1], pt[:, 1:]].sum(1) / (n - 1),
           "t": tab["t"][pt[:, :-2], pt[:, 1:-1], pt[:, 2:]].sum(1) / (n - 2)}
    for key in ("q", "a"):
        out[key] = tab[key][pt[:, :-3], pt[:, 1:-2],
                            pt[:, 2:-1], pt[:, 3:]].sum(1) / (n - 3)
    out["f"] = out["a"] + 30.0 * ic
    return out


def check_scorers(rng, texts, lang="wehrmacht", binary="./enigma"):
    """Anchor `model_scores` against the binary: pin the rotor key, hand it a
    board with `-s`, and compare the one score it prints.  The ranking below is
    only worth anything if these seven agree with what the tool computes."""
    import re
    import subprocess
    alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    print("\nscorer anchor: eval/ vs %s" % binary)
    tab = ngram_tables(lang)
    pt_text = rng.choice([t for t in texts if len(t) > 120])
    wheels, refl, ring, start = random_key(rng)
    plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
    ct = crypt(pt_text, wheels, refl, ring, start, plug)
    c = num(ct)
    rows = core_rows(wheels, refl, ring, start, len(ct))
    mine = model_scores(plug[rows[np.arange(len(c)), plug[c]]][None, :], tab)
    w = "".join(str(x + 1) for x in wheels)
    rg = "".join(alpha[x] for x in ring)
    gg = "".join(alpha[x] for x in start)
    sp = "".join(alpha[a] + alpha[int(plug[a])]
                 for a in range(26) if plug[a] > a)
    worst = 0.0
    for m in MODELS:
        r = subprocess.run([binary, "-" + m, "-u", refl, "-w", w, "-r", rg,
                            "-g", gg, "-l", lang, "-s", sp],
                           input=ct, capture_output=True, text=True)
        tool = None
        for line in r.stderr.splitlines():
            hit = re.match(r"^\s*(-?[0-9]+\.[0-9]+)\s+[A-Za-z]", line)
            if hit:
                tool = float(hit.group(1))
        if tool is None:
            print("   -%s  NO SCORE from the binary" % m)
            return False
        worst = max(worst, abs(mine[m][0] - tool))
        print("   -%-3s eval %12.6f   tool %12.6f   d %.4f"
              % (m, mine[m][0], tool, abs(mine[m][0] - tool)))
    print("   worst disagreement %.4f -- the uint8 quantisation "
          "(ngram_scale=32) is +-0.016 per gram." % worst)
    return worst < 0.05


def seed_from_row(row):
    """A deduced board row as a full involution (unassigned = self-steckered),
    plus the cable pairs it asserts."""
    s = np.arange(26)
    known = np.nonzero(row != UNSET)[0]
    s[known] = row[known]
    pairs = tuple(sorted((a, int(s[a])) for a in known if s[a] != a
                         and a < s[a]))
    return s, known, pairs


def signature_seeder(rng, texts, msgs=10, reps=1, lang="wehrmacht"):
    """Does the TERMINAL-SIGNATURE deduction produce a seed worth pinning?

    The swept seeder died on precision, not recall: its top-ranked seeds pinned
    ~20 plugs and got ~none of them right, and a wrongly pinned plug is worse
    than no pin because the climb cannot undo it.  Pinning the alignment to the
    end of the message changes that arithmetic completely -- ~20 hypotheses
    against ~2 800 -- so the two questions are asked again here:

      1. does a fully CORRECT seed exist among the survivors?
      2. can it be found without knowing the truth -- i.e. where does it RANK
         when the seeds are ordered by their decrypt's score?

    Question 2 is asked of every scoring model, because the ranking signal is
    free to be a different one from the search's target model.

    Only ~10 corpus messages end with a doubling, so `reps` fresh keys and
    boards per message supply the sample size; the ranks move enough between
    single runs that one pass over the ten says nothing about which model wins.
    """
    print("\nsignature seeder: rank of a fully correct seed, by model")
    tab = ngram_tables(lang)
    ends = [t for t in texts
            if any(len(t) - (a + 2 * L + 1) <= 1
                   for a, L in doublings(t, minlen=4, maxlen=20))]
    print("   %d corpus messages end with a doubling; %d x %d keys, -l %s\n"
          % (len(ends), min(msgs, len(ends)), reps, lang))
    if reps == 1:
        print("   %-5s %-6s %-8s %-5s %-6s %s"
              % ("n", "seeds", "correct", "pins", "cables", "  ".join(
                  "%3s" % ("-" + m) for m in MODELS)))
    ranks = {m: [] for m in MODELS}
    missing = 0
    stats = []
    for pt_text in ends[:msgs] * reps:
        wheels, refl, ring, start = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
        ct = crypt(pt_text, wheels, refl, ring, start, plug)
        n = len(ct)
        rows = core_rows(wheels, refl, ring, start, n)
        c = num(ct)
        seen, boards, pins, pairs, ok = set(), [], [], [], []
        for menu in terminal_hyps(ct):
            alive, board = deduce(menu, rows)
            for h in np.nonzero(alive)[0]:
                s, known, pr = seed_from_row(board[h])
                key = s.tobytes()
                if key in seen:
                    continue
                seen.add(key)
                boards.append(s)
                pins.append(len(known))
                pairs.append(pr)
                # correct = every assignment agrees with the true board.  Both
                # counts matter: `pins` is what a --crib-style hybrid would fix
                # (a deduced NO-cable is a finding too), `cables` the subset
                # that is an actual plug.
                ok.append(bool(np.all(board[h][known] == plug[known])))
        if not boards:
            continue
        # p[k, i] = steck_k[core_i[steck_k[c_i]]]
        S = np.asarray(boards)                           # (k, 26)
        k = np.arange(len(S))[:, None]
        mid = rows[np.arange(n)[None, :], S[:, c]]       # (k, n)
        sc = model_scores(S[k, mid], tab)
        good = [i for i, o in enumerate(ok) if o]
        if not good:
            missing += 1
        cells = []
        for m in MODELS:
            order = np.argsort(-sc[m], kind="stable")
            pos = {int(i): r for r, i in enumerate(order)}
            r = min((pos[i] for i in good), default=None)
            cells.append("%3s" % ("-" if r is None else r))
            if r is not None:
                ranks[m].append(r)
        stats.append((len(boards), max((pins[i] for i in good), default=0),
                      max((len(pairs[i]) for i in good), default=0)))
        if reps == 1:
            print("   %-5d %-6d %-8d %-5s %-6s %s"
                  % (n, len(boards), len(good),
                     max((pins[i] for i in good), default="-"),
                     max((len(pairs[i]) for i in good), default="-"),
                     "  ".join(cells)))
    tot = len(stats)
    sd, sp, sc_ = (np.array([s[i] for s in stats]) for i in range(3))
    print("   %d/%d trials have a fully correct seed; it pins %.1f assignments"
          % (tot - missing, tot, sp[sp > 0].mean() if (sp > 0).any() else 0))
    print("   (%d-%d) of which %.1f (%d-%d) are cables, among %.1f seeds.\n"
          % (sp[sp > 0].min() if (sp > 0).any() else 0, sp.max(),
             sc_[sc_ > 0].mean() if (sc_ > 0).any() else 0,
             sc_[sc_ > 0].min() if (sc_ > 0).any() else 0, sc_.max(),
             sd.mean()))
    print("   %-8s %-9s %-9s %-9s %-10s %s"
          % ("signal", "top-1", "top-3", "mean rank", "vs -i", "McNemar"))
    base = np.array(ranks[MODELS[0]])
    for m in MODELS:
        r = np.array(ranks[m])
        if m == MODELS[0]:
            cmp_, p = "", ""
        else:
            x = int(((base == 0) & (r != 0)).sum())
            y = int(((base != 0) & (r == 0)).sum())
            cmp_, p = "%d / %d" % (x, y), "p = %.4f" % binom_two(x, y)
        print("   %-8s %-9s %-9s %-9.1f %-10s %s"
              % ("-" + m, "%d/%d" % ((r == 0).sum(), r.size),
                 "%d/%d" % ((r < 3).sum(), r.size), r.mean(), cmp_, p))
    print("\n   'vs -i' counts the trials where only -i put the correct seed")
    print("   top / only that model did, paired over the same trials.")
    print("\n   'correct' = every assignment the seed makes agrees with the")
    print("   true board; 'pins'/'cables' are how many it makes and how many")
    print("   of those are actual plugs.  The rank is that seed's position")
    print("   when the message's seeds are sorted by their decrypt's score --")
    print("   0 means the correct seed is the one a search would have picked.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--messages", type=int, default=40)
    ap.add_argument("--wrong", type=int, default=200)
    ap.add_argument("--swept-msgs", type=int, default=4)
    ap.add_argument("--swept-wrong", type=int, default=40)
    ap.add_argument("--seed", type=int, default=20260816)
    ap.add_argument("--seeder-msgs", type=int, default=10)
    ap.add_argument("--seeder-reps", type=int, default=20)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--check-scorers", action="store_true",
                    help="anchor the eval scorers against ./enigma first")
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
    if a.check_scorers and not check_scorers(rng, texts, lang=a.lang):
        print("\nSCORER ANCHOR FAILED -- not ranking seeds with these.")
        return 1
    signature_seeder(rng, texts, msgs=a.seeder_msgs, reps=a.seeder_reps,
                     lang=a.lang)
    return 0


if __name__ == "__main__":
    sys.exit(main())
