#!/usr/bin/env python3
"""Menu construction and closure deduction for cribs (archived/cribs.md §6; §12 step 2).

    python3 eval/crib_menu.py                 # regenerate §4.1's table
    python3 eval/crib_menu.py --selftest      # anchor model, check the logic
    python3 eval/crib_menu.py --vectors FILE  # test vectors for the C++ step

WHY THIS EXISTS, given that the deduction is going into `enigma.cc` anyway.

§4.1's table -- loops per crib length, and the fraction of rotor settings a crib
rejects -- is what the cost model in archived/cribs.md rests on, and nothing in the repo
regenerated it.  This does.  If the C++ later disagrees with §4.1 there is now a
second implementation to arbitrate, instead of two suspects and no judge.

It is NOT here to validate the C++ by comparison.  §10.1 and §10.2 are oracle
tests -- the corpus supplies message, key and plugboard, so the deduction is
checked against the answer key, not against another program.  This script runs
those same two checks on itself (the `true hyp` and `plugs` columns), which is
what makes its own numbers worth quoting.

WHAT A MENU IS.  Line the crib up against the ciphertext.  At message position i
the machine turned plaintext letter p into ciphertext letter c, so p and c are
joined by an edge labelled i; letters are the nodes.  A LOOP is a path through
those edges returning where it started, and loops are what let a crib reject a
wrong rotor setting: follow the forced plugs around the loop and it either comes
back consistent or contradicts itself.

THE DEDUCTION.  Decryption is `p = steck[core_i[steck[c]]]`, and the rotor core
is its own inverse, so that rearranges to

    steck[p] = core_i[steck[c]]

-- if you know what the ciphertext letter is plugged to, the core tells you what
the plaintext letter is plugged to.  Guess one plug, chain that rule along every
edge, and add reciprocity (`steck[x] = y` implies `steck[y] = x`: Welchman's
diagonal board, free here because the board is stored as an involution).  A
contradiction kills the guess; kill all 26 and the rotor setting is impossible.

The machine model mirrors `enigma.cc` and is shared with
`eval/ring_stride_geometry_probe.py`, whose `selftest()` anchors it against the
binary; `--selftest` re-runs that anchor and then checks the menu logic against
known plugboards.
"""
import argparse
import os
import random
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ring_stride_geometry_probe import (       # noqa: E402
    num, txt, subst_array, positions, plugboard, crypt)

FILES = ["enigma-messages.txt", "enigma-army-messages-1941.txt"]
LENGTHS = [8, 10, 12, 14, 16, 18, 20, 25]
UNSET = -1


# ------------------------------------------------------------------- corpus ---

def corpus():
    """Authentic plaintexts, split at the unrecorded letters: a crib may not
    span a '-', because we do not know what is there."""
    out = []
    for fn in FILES:
        body = open(os.path.join(HERE, fn), encoding="utf-8").read()
        for blk in re.split(r"^### Message", body, flags=re.M)[1:]:
            m = re.search(r"DECRYPT:\s+((?:.*\n)(?:             .*\n)*)", blk)
            if m:
                pt = re.sub(r"[^A-Z-]", "", m.group(1))
                out.extend(s for s in pt.split("-") if len(s) >= 45)
    return out


# --------------------------------------------------------------------- menu ---

class Menu:
    """The crib as a graph: one edge per crib position, joining the plaintext
    letter to the ciphertext letter under it."""

    def __init__(self, crib, cipher, at):
        self.crib, self.at = crib, at
        p, c = num(crib), num(cipher[at:at + len(crib)])
        # Enigma never encrypts a letter to itself, so a position where the
        # crib letter equals the ciphertext letter proves the alignment wrong.
        self.valid = bool(len(p) == len(c) and not np.any(p == c))
        self.edges = [(int(p[j]), int(c[j]), at + j) for j in range(len(p))]

    def components(self):
        """Connected components, as letter sets, largest first."""
        adj = {}
        for a, b, _ in self.edges:
            adj.setdefault(a, set()).add(b)
            adj.setdefault(b, set()).add(a)
        seen, comps = set(), []
        for start in sorted(adj):
            if start in seen:
                continue
            stack, comp = [start], set()
            while stack:
                v = stack.pop()
                if v in comp:
                    continue
                comp.add(v)
                stack.extend(adj[v] - comp)
            seen |= comp
            comps.append(comp)
        return sorted(comps, key=len, reverse=True)

    def loops(self):
        """Cycle rank of the LARGEST component: edges - nodes + 1.

        Largest only, per archived/cribs.md §6.3: every component needs its own
        hypothesis, so a menu that falls into pieces multiplies the work rather
        than dividing it -- and the small pieces carry no loops anyway.
        """
        comps = self.components()
        if not comps:
            return 0
        big = comps[0]
        e = sum(1 for a, b, _ in self.edges if a in big)
        return max(0, e - len(big) + 1)

    def anchor(self):
        """The letter to hypothesise about: highest degree in the largest
        component, so one guess reaches as far as possible."""
        comps = self.components()
        if not comps:
            return None
        deg = {}
        for a, b, _ in self.edges:
            deg[a] = deg.get(a, 0) + 1
            deg[b] = deg.get(b, 0) + 1
        return max(sorted(comps[0]), key=lambda v: deg[v])


# ---------------------------------------------------------------- deduction ---

DIAGONAL = True     # Welchman's diagonal board; --no-diagonal turns it off


def deduce_all(menu, rows):
    """Run all 26 hypotheses at once.

    Returns (alive, board): alive[h] says the hypothesis "the anchor letter is
    plugged to h" survived, and board[h] holds its partial plugboard, UNSET
    where still unknown.  The 26 run together because they are independent and
    identically shaped, so the loop below is over EDGES, not hypotheses.
    """
    board = np.full((26, 26), UNSET, dtype=np.int64)
    alive = np.ones(26, dtype=bool)
    a = menu.anchor()
    if a is None:
        return alive, board
    h = np.arange(26)
    board[h, a] = h                            # the hypothesis ...
    if DIAGONAL:
        board[h, h] = a                        # ... and its reciprocal

    def assign(x, y, idx):
        """steck[x] = y for the hypotheses in idx (x one letter, y an array
        with one value per row).  Sets the reciprocal too, and kills any
        hypothesis where either half contradicts what is already known."""
        if idx.size == 0:
            return
        cur = board[idx, x]
        clash = (cur != UNSET) & (cur != y)
        alive[idx[clash]] = False
        r, v = idx[~clash], y[~clash]
        board[r, x] = v
        if not DIAGONAL:
            return
        back = board[r, v]                     # reciprocity: steck[y] = x
        clash2 = (back != UNSET) & (back != x)
        alive[r[clash2]] = False
        ok = ~clash2
        board[r[ok], v[ok]] = x

    changed = True
    while changed:
        changed = False
        for p, c, i in menu.edges:
            core = rows[i]
            idx = np.nonzero(alive & (board[:, c] != UNSET)
                             & (board[:, p] == UNSET))[0]
            if idx.size:
                assign(p, core[board[idx, c]], idx)
                changed = True
            idx = np.nonzero(alive & (board[:, p] != UNSET)
                             & (board[:, c] == UNSET))[0]
            if idx.size:
                assign(c, core[board[idx, p]], idx)
                changed = True
            # Both ends known: the edge must agree, or the hypothesis is dead.
            idx = np.nonzero(alive & (board[:, p] != UNSET)
                             & (board[:, c] != UNSET))[0]
            if idx.size:
                alive[idx[core[board[idx, c]] != board[idx, p]]] = False
    return alive, board


def rejects(menu, rows):
    """True when every hypothesis contradicts: the rotor setting is impossible
    and the search never has to decrypt under it."""
    if not menu.valid:
        return True
    return not deduce_all(menu, rows)[0].any()


# ------------------------------------------------------------------ machine ---

_CACHE = {}


def core_rows(wheels, refl, ring, start, n):
    """rows[i]: the rotor-stack-and-reflector permutation at message position
    i -- `enigma.cc`'s rows[] exactly, with the plugboard left off."""
    key = (tuple(wheels), refl)
    S = _CACHE.get(key)
    if S is None:
        S = _CACHE[key] = subst_array(wheels, refl)
    o = (positions(start, wheels, n) - np.asarray(ring)[None, :]) % 26
    return S[o[:, 0], o[:, 1], o[:, 2], :].astype(np.int64)


def random_key(rng, maxwheel=5):
    w = rng.sample(range(maxwheel), 3)
    return (w, rng.choice("ABC"),
            np.array([rng.randrange(26) for _ in range(3)]),
            np.array([rng.randrange(26) for _ in range(3)]))


def trial(rng, texts, L, plugs):
    """One planted crib: plaintext, alignment, true key, board, ciphertext,
    menu."""
    pt = rng.choice([t for t in texts if len(t) >= L + 20])
    at = rng.randrange(len(pt) - L)
    wheels, refl, ring, start = random_key(rng)
    plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), plugs)
    ct = crypt(pt, wheels, refl, ring, start, plug)
    return (pt, at, (wheels, refl, ring, start), plug, ct,
            Menu(pt[at:at + L], ct, at))


# ---------------------------------------------------------------- the table ---

def table(args):
    """Regenerate archived/cribs.md §4.1.

    Per crib length: plant a crib in an authentic plaintext enciphered under a
    random key and plugboard, then

      * check the TRUE rotor setting is not rejected (§10.2, zero tolerance),
      * check every plug the truth's hypothesis deduces is correct (§10.1),
      * count how many WRONG rotor settings the crib rejects.
    """
    texts = corpus()
    rng = random.Random(args.seed)
    print("Menu closure by crib length (%d cribs x %d wrong settings each, "
          "%d plugs, seed %d)\n"
          % (args.cribs, args.settings, args.plugs, args.seed))
    print("  %6s %7s %7s %9s %9s %9s %8s %10s"
          % ("length", "loops", "looped", "rej|no", "rej|loop", "rejected",
             "true hyp", "plugs"))
    for L in args.lengths:
        loops, tested = [], 0
        rej = {0: [0, 0], 1: [0, 0]}       # [rejected, tested] by has-a-loop
        truth_ok, plugs_ok, plugs_n = 0, 0, 0
        for _ in range(args.cribs):
            pt, at, key, plug, ct, menu = trial(rng, texts, L, args.plugs)
            wheels, refl, ring, start = key
            nl = menu.loops()
            loops.append(nl)
            bucket = rej[1 if nl else 0]

            rows = core_rows(wheels, refl, ring, start, len(pt))
            alive, board = deduce_all(menu, rows)
            a = menu.anchor()
            if menu.valid and a is not None and alive[int(plug[a])]:
                truth_ok += 1
                h = int(plug[a])
                for x in range(26):
                    if board[h, x] != UNSET:
                        plugs_n += 1
                        plugs_ok += int(board[h, x] == plug[x])

            for _ in range(args.settings):
                w2, r2, ring2, start2 = random_key(rng)
                rows2 = core_rows(w2, r2, ring2, start2, len(pt))
                bucket[1] += 1
                tested += 1
                bucket[0] += rejects(menu, rows2)
        looped = sum(1 for x in loops if x) / len(loops)
        both = rej[0][0] + rej[1][0]
        def pct(b):
            return ("%8.2f%%" % (100.0 * b[0] / b[1])) if b[1] else "       -"
        print("  %6d %7.2f %6.0f%% %s %s %8.2f%% %6d/%-3d %5d/%-5d"
              % (L, sum(loops) / len(loops), 100 * looped, pct(rej[0]),
                 pct(rej[1]), 100.0 * both / max(tested, 1),
                 truth_ok, args.cribs, plugs_ok, plugs_n))
    print("\n  loops     cycle rank of the largest menu component")
    print("  looped    cribs with at least one loop")
    print("  rej|no    rotor settings rejected by a crib with NO loop")
    print("  rej|loop  ... and by a crib with at least one")
    print("  rejected  the mixture of the two, which is what a run sees")
    print("  true hyp  the TRUE setting survived (must be all)")
    print("  plugs     deduced plugs matching the true board (must be all)")


def vectors(args):
    """Write test vectors for the C++ deduction: a known message, key, board,
    crib and alignment, with every plug the deduction must produce."""
    texts = corpus()
    rng = random.Random(args.seed)
    with open(args.vectors, "w", encoding="utf-8") as f:
        f.write("# Crib deduction test vectors, from eval/crib_menu.py.\n")
        f.write("# Each record gives the true machine setting, a crib at a\n")
        f.write("# known alignment, and every plug the deduction must\n")
        f.write("# recover from the hypothesis matching the true board. A\n")
        f.write("# vector PASSES only if all are deduced and nothing else.\n")
        n = 0
        while n < args.count:
            L = rng.choice(args.lengths)
            pt, at, key, plug, ct, menu = trial(rng, texts, L, args.plugs)
            wheels, refl, ring, start = key
            a = menu.anchor()
            if not menu.valid or a is None:
                continue
            rows = core_rows(wheels, refl, ring, start, len(pt))
            alive, board = deduce_all(menu, rows)
            h = int(plug[a])
            if not alive[h]:
                continue
            got = ["%s%s" % (chr(65 + x), chr(65 + int(board[h, x])))
                   for x in range(26) if board[h, x] != UNSET]
            f.write("\nwheels    %s\n" % "".join(str(w + 1) for w in wheels))
            f.write("reflector %s\n" % refl)
            f.write("ring      %s\n" % txt(ring))
            f.write("start     %s\n" % txt(start))
            f.write("plugs     %s\n"
                    % " ".join("%s%s" % (chr(65 + i), chr(65 + int(plug[i])))
                               for i in range(26) if plug[i] > i))
            f.write("cipher    %s\n" % ct)
            f.write("crib      %s\n" % pt[at:at + L])
            f.write("at        %d\n" % at)
            f.write("anchor    %s\n" % chr(65 + a))
            f.write("loops     %d\n" % menu.loops())
            f.write("deduced   %s\n" % " ".join(got))
            n += 1
    print("wrote %s (%d vectors)" % (args.vectors, args.count))
    return 0


def selftest(args):
    """Anchor the machine against the binary, then the menu logic against
    known plugboards."""
    from ring_stride_geometry_probe import selftest as model_selftest
    model_selftest()
    rng = random.Random(0)
    texts = corpus()
    n = args.cribs * 4
    for _ in range(n):
        L = rng.choice(LENGTHS)
        pt, at, key, plug, ct, menu = trial(rng, texts, L, args.plugs)
        wheels, refl, ring, start = key
        if not menu.valid:
            print("FAIL: a TRUE crib failed the self-encryption test")
            return 1
        rows = core_rows(wheels, refl, ring, start, len(pt))
        alive, board = deduce_all(menu, rows)
        h = int(plug[menu.anchor()])
        if not alive[h]:
            print("FAIL: the true hypothesis was killed at the true setting")
            return 1
        for x in range(26):
            if board[h, x] != UNSET and board[h, x] != plug[x]:
                print("FAIL: deduced %s%s, true board has %s%s"
                      % (chr(65 + x), chr(65 + int(board[h, x])),
                         chr(65 + x), chr(65 + int(plug[x]))))
                return 1
    print("menu selftest: ok -- %d planted cribs, the true hypothesis survives "
          "every\ntime and every plug it deduces matches the true board" % n)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cribs", type=int, default=12,
                    help="cribs sampled per length [12]")
    ap.add_argument("--settings", type=int, default=400,
                    help="wrong rotor settings tested per crib [400]")
    ap.add_argument("--plugs", type=int, default=10, help="plugboard size [10]")
    ap.add_argument("--lengths", type=int, nargs="+", default=LENGTHS)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--vectors", help="write C++ test vectors to this file")
    ap.add_argument("--count", type=int, default=20,
                    help="vectors to write [20]")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--no-diagonal", action="store_true",
                    help="drop Welchman's diagonal board (reciprocity), "
                    "leaving pure loop closure -- the control that shows how "
                    "much of the rejection it supplies")
    args = ap.parse_args()
    global DIAGONAL
    DIAGONAL = not args.no_diagonal
    if args.selftest:
        return selftest(args)
    if args.vectors:
        return vectors(args)
    table(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
