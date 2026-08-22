#!/usr/bin/env python3
"""Does a bigger -R open the JOINT-SCORING conversion door?

BACKGROUND.  eval/joint_score_gain.py measured that a second message on the
same day key overturns essentially every SCORING failure -- and that this does
not convert into breaks, because joint re-ranking can only promote a board the
search ACTUALLY PRODUCED, and at -R 8..100 the truth is almost never among the
produced boards while a different board wins.  One trial in 1200.

THE OPEN QUESTION.  More restarts means more converged boards, so the truth
should eventually be produced-but-mis-ranked rather than absent.  At -R 8..100
that population stayed empty.  This runs the ladder out to -R 1000.

WHAT IS MEASURED, per (trial, R):

  exact    the reported board IS the true board
  conv     it is not, but the true board IS among the converged restarts
           (the CONVERSION CANDIDATE population -- what joint scoring needs)
  absent   the true board was never reached by any restart

and, for every conv trial, whether joint re-ranking actually promotes the
truth.  That last step is the honest end-to-end test: it ranks the top-K
boards the search produced BY JOINT SCORE, message 2's start DERIVED per board
from the indicators, and asks whether the truth comes out on top.  Comparing
the truth against the winner alone would overstate it -- a third board can win
jointly.

PAIRED.  Every R in the ladder sees the same trial (same board, key,
plaintexts, indicators), so the columns are directly comparable.

  python3 eval/restart_ladder.py                       # L=60,80  R=8..1000
  python3 eval/restart_ladder.py --trials 100 -L 80 -R 100,1000
"""
import argparse
import multiprocessing
import os
import random
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import (BIN, ROOT, load_plaintexts, pair_set,
                              rand_board, rand_pos, rand_wheels, score_board)

# How many of the search's own boards enter the joint re-ranking.  The
# candidates are ordered by their message-1 score, so K is a depth into a list
# the search itself produces -- a real attacker has exactly this list.
TOPK = 8


def climb_dump(ct, wheels, ring, start, lang, restarts, seed):
    """Return (reported board, reported score, {converged: score}, {seen}).

    The reported answer comes off the last PROGRESS line, not the best dumpall
    line: --polish runs after all restarts and never appears in the dump (see
    joint_score_gain.climb_board).  The dump gives the CONVERGED boards, which
    is what "did the search produce the truth as a candidate" needs.

    The fourth return is every board echoed on a progress line -- boards the
    climb PASSED THROUGH.  They are a different thing from a converged board:
    a climb can visit the truth and keep going uphill past it, which is what
    "the truth is not the top of any hill" looks like from the inside.  This
    set UNDERCOUNTS such visits badly, because a progress line fires only on a
    board beating the run's global high-water mark, so a truth visited late in
    a good run is silent.  Read it as a lower bound, never as a rate.
    """
    env = dict(os.environ, ENIGMA_SEED=str(seed), ENIGMA_DATA="ngrams")
    out = subprocess.run(
        [BIN, "-c", "-f", "-S", "i4f10", "-J", "--polish", "-l", lang,
         "-u", "B", "-w", wheels, "-r", ring, "-g", start,
         "-R", str(restarts), "--dump-all"],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT).stderr
    final, dump, seen = ("", None), {}, set()
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "dumpall" and len(f) >= 5:
            b = " ".join(sorted(f[5:]))
            s = float(f[4])
            if b not in dump or s > dump[b]:
                dump[b] = s
            continue
        if len(f) < 5 or not re.match(r"^-?\d+\.\d+$", f[0]):
            continue
        plugs = []
        for tok in f[4:]:
            if len(tok) == 2 and tok.isalpha() and tok.isupper():
                plugs.append(tok)
            else:
                break
        final = (" ".join(sorted(plugs)), float(f[0]))
        seen.add(final[0])
    return final[0], final[1], dump, seen


def make_trial(rng, texts, length):
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
        msgs.append(dict(ct=ct, grund=grund, enc=enc, start=start))
    return dict(board=board, wheels=wheels, ring=ring, msgs=msgs,
                seed=rng.randrange(1 << 30))


def run_trial(job):
    trial, ladder, lang = job
    board, wheels, ring = trial["board"], trial["wheels"], trial["ring"]
    m1, m2 = trial["msgs"]
    truth = pair_set(board)
    s_true = score_board(m1["ct"], wheels, ring, m1["start"], board, lang)

    def msg2_score(b):
        """Message 2 under board b, its start DERIVED from b via the indicator.

        A wrong board mis-maps the indicator through six plugboard lookups and
        so lands on a wrong start; that is the asymmetry being tested.
        """
        st = enigma_ref.decrypt(m2["enc"], wheels, ring, m2["grund"], b)
        return score_board(m2["ct"], wheels, ring, st, b, lang)

    out = {}
    for R in ladder:
        got, s_got, dump, seen = climb_dump(m1["ct"], wheels, ring,
                                            m1["start"], lang, R,
                                            trial["seed"])
        if not got:
            out[R] = dict(state="err")
            continue
        if pair_set(got) == truth:
            out[R] = dict(state="exact")
            continue
        reached = any(pair_set(b) == truth for b in dump)
        rec = dict(state="conv" if reached else "absent",
                   scoring=(s_got >= s_true),
                   visited=any(pair_set(b) == truth for b in seen))
        if reached:
            # Joint re-ranking over the boards the SEARCH produced, deepest
            # first by message-1 score.  The reported answer is included
            # explicitly: --polish can move it off the dump.
            # How deep the truth sits in the search's OWN message-1 ranking.
            # The re-ranking below adds the true board explicitly, so without
            # this the promotion rate would silently assume a candidate list
            # deep enough to contain it.  Rank 1 = best-scoring converged
            # board; report it so TOPK is visibly not the binding constraint.
            order = sorted(dump.items(), key=lambda kv: -kv[1])
            rec["rank"] = next((i + 1 for i, (b, _) in enumerate(order)
                                if pair_set(b) == truth), None)
            rec["ncand"] = len(order)
            names = {b for b, _ in order[:TOPK]} | {got, board}
            best, best_s = None, None
            for b in names:
                j = score_board(m1["ct"], wheels, ring, m1["start"], b, lang)
                if j is None:
                    continue
                j += msg2_score(b)
                if best_s is None or j > best_s:
                    best, best_s = b, j
            rec["promoted"] = (best is not None
                               and pair_set(best) == truth)
        out[R] = rec
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=350)
    ap.add_argument("--lengths", "-L", default="60,80")
    ap.add_argument("--restarts", "-R", default="8,32,100,316,1000")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--language", default="wehrmacht")
    ap.add_argument("--jobs", "-j", type=int,
                    default=max(1, (os.cpu_count() or 2)))
    args = ap.parse_args()

    ladder = [int(x) for x in args.restarts.split(",")]
    texts = load_plaintexts()
    print("%d authentic telegraphic plaintexts, model '%s', %d trials/cell,\n"
          "recipe -c -f -S i4f10 -J --polish, rotor key GIVEN\n"
          % (len(texts), args.language, args.trials))

    for length in [int(x) for x in args.lengths.split(",")]:
        rng = random.Random(args.seed + length)
        jobs = [(make_trial(rng, texts, length), ladder, args.language)
                for _ in range(args.trials)]
        with multiprocessing.Pool(args.jobs) as pool:
            results = pool.map(run_trial, jobs)

        print("L = %d" % length)
        print("%6s %8s %8s %8s %10s %9s %9s" %
              ("-R", "exact", "conv", "absent", "promoted", "scoring",
               "visited"))
        for R in ladder:
            cells = [r[R] for r in results if r[R]["state"] != "err"]
            n = len(cells) or 1
            ex = sum(1 for c in cells if c["state"] == "exact")
            cv = [c for c in cells if c["state"] == "conv"]
            ab = sum(1 for c in cells if c["state"] == "absent")
            sc = sum(1 for c in cells if c["state"] != "exact"
                     and c.get("scoring"))
            vis = sum(1 for c in cells if c.get("visited"))
            miss = len(cells) - ex
            print("%6d %7.1f%% %7.1f%% %7.1f%% %10s %8.0f%% %8.1f%%"
                  % (R, 100.0 * ex / n, 100.0 * len(cv) / n, 100.0 * ab / n,
                     ("%d/%d" % (sum(1 for c in cv if c.get("promoted")),
                                 len(cv))) if cv else "-",
                     (100.0 * sc / miss) if miss else 0.0,
                     100.0 * vis / n))
            if cv:
                print("%6s truth's rank among the %s converged boards: %s"
                      % ("", "/".join(str(c.get("ncand")) for c in cv),
                         "/".join(str(c.get("rank")) for c in cv)))
        print()

    print("  exact    = the search reported the true board")
    print("  conv     = it did not, but some restart DID converge on the true")
    print("             board -- the population joint scoring can act on")
    print("  absent   = no restart ever reached the true board")
    print("  promoted = of the conv trials, how many joint re-ranking over the")
    print("             search's own top-%d boards actually puts the truth"
          % TOPK)
    print("             first (message 2's start DERIVED per board)")
    print("  scoring  = share of NON-EXACT trials whose winner outscores the")
    print("             truth (a scoring failure, not a search one)")
    print("  visited  = non-exact trials where the truth was echoed on a")
    print("             progress line, i.e. a climb PASSED THROUGH it and")
    print("             kept going uphill.  A LOWER BOUND -- progress lines")
    print("             fire only above the run's high-water mark")


if __name__ == "__main__":
    main()
