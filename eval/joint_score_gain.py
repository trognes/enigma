#!/usr/bin/env python3
"""Does a SECOND message on the same day key rescue a SCORING failure?

THE QUESTION.  Two short messages (60-100 letters) share a day key and both
indicators are known, so a candidate board derives BOTH start positions and
message 2 can be scored at no cost in keyspace.  How often does that overturn a
scoring failure?

SCORING FAILURE, as this repo defines it (CLAUDE.md, `make crackquality
SPLIT=1`): the climb reaches a board that scores HIGHER than the true one.  It
is an information limit of the scoring model -- no search can cross it, because
the search is doing what it was asked and the answer is wrong.  It is also
exactly what more text erodes, which is why a second message is the right
instrument to point at it.

WHY THE IMPOSTOR MUST BE A CLIMBED BOARD.  A first version of this harness
scored random wrong ROTOR KEYS against the true board and found z = 9.6 at
L = 60 against a 5.3 bar, with the true key ranked first in 100% of trials --
i.e. no scoring failures at all.  That is not a null, it is a straw man: a
board that has not been fitted to the ciphertext is a hopeless competitor.  The
impostor has to be what the search actually produces, so this runs the real
climb (`./enigma -c --dump-all`) and takes the board it converges on.

THE MECHANISM UNDER TEST.  The true board derives the CORRECT start for message
2, so message 2 reads as German.  A climbed impostor derives a wrong start --
six plugboard lookups have to be right and it fails at least one (ENHANCEMENTS
3a) -- so it collects noise.  The asymmetry is the whole effect, and it should
be large rather than the sqrt(2) that a plain joint score gives.

Scores are summed UNNORMALISED (weighted by length, not per-symbol averages,
which would let a 60-letter message outvote a 100-letter one).

  python3 eval/joint_score_gain.py                    # 40/60/80/100
  python3 eval/joint_score_gain.py --trials 400 -R 16
"""
import argparse
import math
import os
import random
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "enigma")


def load_quadgrams(lang):
    path = os.path.join(ROOT, "ngrams", "%s_quadgrams.txt" % lang)
    table, total = {}, 0
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if len(p) == 2 and len(p[0]) == 4 and p[0].isalpha() and p[0].isascii():
            table[p[0].upper()] = int(p[1])
            total += int(p[1])
    floor = math.log10(1.0 / total)
    return {k: math.log10(v / total) for k, v in table.items()}, floor


def load_plaintexts():
    text = open(os.path.join(HERE, "enigma-army-messages-1941.txt"),
                encoding="utf-8").read()
    out = []
    for block in re.split(r"(?=### Message No\.)", text)[1:]:
        m = re.search(r"DECRYPT:\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
        if m:
            s = "".join(m.group(1).split()).replace("-", "")
            if len(s) >= 60:
                out.append(s)
    return out


def board_str(pairs):
    """NORMALISED: letters within a pair sorted, pairs sorted.

    The binary emits boards this way, and an unnormalised board here made
    `got != board` true for two spellings of the SAME board -- which showed up
    as 40% of the L=100 "scoring failures" having all ten true plugs. Compare
    boards through pair_set(), never as strings.
    """
    return " ".join(sorted(chr(65 + min(a, b)) + chr(65 + max(a, b))
                           for a, b in pairs))


def pair_set(b):
    return frozenset(tuple(sorted(p)) for p in b.split()) if b else frozenset()


def rand_board(rng):
    L = list(range(26))
    rng.shuffle(L)
    return board_str([(L[2 * i], L[2 * i + 1]) for i in range(10)])


def rand_pos(rng):
    return "".join(chr(65 + rng.randrange(26)) for _ in range(3))


def rand_wheels(rng):
    return "".join(str(d) for d in rng.sample([1, 2, 3, 4, 5], 3))


def climb_board(ct, wheels, ring, start, lang, restarts, seed):
    """Run the REAL climb and return the board it converges on, best first."""
    env = dict(os.environ, ENIGMA_SEED=str(seed), ENIGMA_DATA="ngrams")
    # THE RECOMMENDED RECIPE, not a bare climb. An earlier version of this
    # harness ran plain `-q` and recovered the board 0.5% of the time at
    # L = 80, against the 12.0% CLAUDE.md measures at L = 82 -- a fifteen-fold
    # weaker climb, which lands FAR from the truth and so makes the impostor
    # trivially easy for message 2 to reject. Measuring the rescue on that
    # would have flattered it. `-f -S i4f10 -J --polish` gets to 6.8%; the
    # residual gap to 12.0% is the plaintext pool (whole messages here, some
    # corpus-flagged garbled, against prepass_ab.py's concatenated corpus).
    out = subprocess.run(
        [BIN, "-c", "-f", "-S", "i4f10", "-J", "--polish", "-l", lang,
         "-u", "B", "-w", wheels, "-r", ring,
         "-g", start, "-R", str(restarts), "--dump-all"],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT).stderr
    best, bestboard = None, ""
    for line in out.splitlines():
        if not line.startswith("dumpall"):
            continue
        f = line.split(None, 5)
        sc = float(f[4])
        if best is None or sc > best:
            best, bestboard = sc, (f[5].strip() if len(f) > 5 else "")
    return bestboard


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=200)
    ap.add_argument("--lengths", default="40,60,80,100")
    ap.add_argument("--restarts", "-R", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--language", default="wehrmacht")
    args = ap.parse_args()

    LQ, floor = load_quadgrams(args.language)
    texts = load_plaintexts()

    def score(t):                      # UNNORMALISED: length is the vote
        return sum(LQ.get(t[i:i + 4], floor) for i in range(len(t) - 3))

    print("%d authentic telegraphic plaintexts, model '%s',\n"
          "real climb at -R %d\n"
          % (len(texts), args.language, args.restarts))
    # The three outcomes are reported together because "how common is a
    # scoring failure" has two defensible denominators and they differ by an
    # order of magnitude: as a share of ALL trials it is small because most
    # trials fail to climb at all, and as a share of NON-EXACT trials it is
    # the number that says how much of the residual is out of the search's
    # reach. Quoting one without the other is how a floor gets misread.
    print("%5s %7s %8s %9s %9s %8s %11s" %
          ("L", "trials", "exact", "search", "scoring", "of miss", "rescued"))

    for length in [int(x) for x in args.lengths.split(",")]:
        rng = random.Random(args.seed + length)
        fails = rescued = exact = searchfail = 0
        for _ in range(args.trials):
            board = rand_board(rng)
            wheels, ring = rand_wheels(rng), rand_pos(rng)
            msgs = []
            for _ in range(2):
                pt = rng.choice([p for p in texts if len(p) >= length])
                off = rng.randrange(0, len(pt) - length + 1)
                pt = pt[off:off + length]
                grund, start = rand_pos(rng), rand_pos(rng)
                enc = enigma_ref.decrypt(start, wheels, ring, grund, board)
                ct = enigma_ref.decrypt(pt, wheels, ring, start, board)
                msgs.append(dict(ct=ct, grund=grund, enc=enc,
                                 start=start, pt=pt))

            # The climb sees message 1 only, at its true rotor key: this
            # isolates SCORING from the rotor search entirely.
            got = climb_board(msgs[0]["ct"], wheels, ring, msgs[0]["start"],
                              args.language, args.restarts,
                              rng.randrange(1 << 30))
            if not got:
                continue
            if pair_set(got) == pair_set(board):
                exact += 1      # same board, possibly spelled differently
                continue

            def joint(b):
                """Both messages under board b; msg 2's start DERIVED."""
                s = score(enigma_ref.decrypt(msgs[0]["ct"], wheels, ring,
                                             msgs[0]["start"], b))
                st = enigma_ref.decrypt(msgs[1]["enc"], wheels, ring,
                                        msgs[1]["grund"], b)
                return s + score(enigma_ref.decrypt(msgs[1]["ct"], wheels,
                                                    ring, st, b))

            s_true = score(enigma_ref.decrypt(msgs[0]["ct"], wheels, ring,
                                              msgs[0]["start"], board))
            s_got = score(enigma_ref.decrypt(msgs[0]["ct"], wheels, ring,
                                             msgs[0]["start"], got))
            if s_got < s_true:
                searchfail += 1        # a SEARCH failure, not a scoring one
                continue
            fails += 1
            if joint(board) > joint(got):
                rescued += 1
        miss = searchfail + fails      # every non-exact trial
        n = args.trials
        print("%5d %7d %7.1f%% %8.1f%% %8.1f%% %7.0f%% %10s"
              % (length, n, 100.0 * exact / n, 100.0 * searchfail / n,
                 100.0 * fails / n,
                 (100.0 * fails / miss) if miss else 0.0,
                 ("%d/%d" % (rescued, fails)) if fails else "-"))

    print("\n  exact   = the climb recovered the true board")
    print("  search  = it did not, and what it found scores LOWER  (a search")
    print("            failure -- more restarts can fix it)")
    print("  scoring = it did not, and what it found scores HIGHER (a scoring")
    print("            failure -- no search can fix it)")
    print("  of miss = scoring failures as a share of NON-EXACT trials")
    print("  rescued = of those, how many the true board wins back once")
    print("            message 2 is added, its start DERIVED per board")


if __name__ == "__main__":
    main()
