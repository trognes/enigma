#!/usr/bin/env python3
"""How far do restarts go, and does a bigger -R open the joint-scoring door?

A BOARD COUNTS AS RECOVERED AT 50% OF THE PLAINTEXT, not at an exact match.
That is the operational criterion: a board missing two of ten plugs still
recovers most of the message, and that is a break.  Exact recovery is reported
alongside, because it is what every earlier sweep here measured, together with
the mean %-correct, which is the graded signal CLAUDE.md tells you to judge
search changes on.

BACKGROUND.  eval/joint_score_gain.py measured that a second message on the
same day key overturns essentially every SCORING failure -- and that this does
not convert into breaks, because joint re-ranking can only promote a board the
search ACTUALLY PRODUCED, and at -R 8..100 the truth is almost never among the
produced boards while a different board wins.  One trial in 1200.

THE OPEN QUESTION.  More restarts means more converged boards, so a good board
should eventually be produced-but-mis-ranked rather than absent.  This runs the
ladder well past where that sweep stopped.

WHAT IS MEASURED, per (trial, R):

  recov    the reported board decrypts >= 50% of message 1 correctly
  conv     it does not, but some CONVERGED restart board would
           (the CONVERSION CANDIDATE population -- what joint scoring needs)
  absent   no converged board reaches 50%

and, for every conv trial, whether joint re-ranking actually promotes one.
That is the honest end-to-end test: it ranks the top-K boards THE SEARCH
PRODUCED by joint score, message 2's start DERIVED per board from the
indicators, and asks whether the winner clears 50%.  The true board is NOT in
that set -- an attacker does not have it, and an earlier version of this
harness which did inject it reported a promotion rate that a real top-K list
could not have delivered (the truth ranked 60th and 87th of ~1600 converged
boards at -R 5000).  The rank of the best 50%-clearing board is reported so
the K window is visibly not doing the work.

WHERE THE GOOD BOARDS RANK.  Two ranks per trial, both against the search's
own ordering of the boards it converged on: the TRUE board's, and that of the
best board clearing 50%.  The second is the one that matters operationally --
recovering the board exactly is not required, so what counts is how near the
top the search puts something good enough.  Recorded for EVERY trial,
recovered ones included; restricting them to misses would answer a conditioned
version of the question.

AND WHAT THE WINNING BOARD LOOKS LIKE.  A scoring/search boolean alone cannot
answer the obvious follow-up: does a bigger -R find boards NEARER the truth, or
merely boards that SCORE better?  Each non-recovered trial records the winning
board, its score, the truth's score, the difference in log units over the whole
message, how many of the ten plugs it shares with the truth, its %-correct, and
whether it derives message 2's true start.  --out writes every record as JSONL
so a long run can be re-analysed without being re-run.

PAIRED.  Every R in the ladder sees the same trial (same board, key,
plaintexts, indicators), so the columns are directly comparable.

  python3 eval/restart_ladder.py
  python3 eval/restart_ladder.py -L 40,60,80,100 \\
      -R 8,32,100,316,1000,2000,5000 --out results-restart-ladder.jsonl
"""
import argparse
import json
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

# How many of the search's own boards enter the joint re-ranking.  Each costs
# two binary invocations, so this is a real budget; the reported rank of the
# best 50%-clearing board says whether it binds.
TOPK = 8

# A decrypt this correct counts as recovered.  NOT a claim about legibility:
# it is the operational bar this sweep was asked for, and half the letters
# wrong is well short of prose.  Everything below says "clears 50%", never
# "readable", so the threshold is never quietly upgraded into a claim.
RECOVER_PCT = 50.0


def core_table(wheels, ring, start, length):
    """The rotor-stack permutation per position, plugboard removed.

    Decryption is p_i = S[core_i[S[c_i]]] (enigma_ref.decrypt), so with core_i
    in hand, applying a BOARD costs two array lookups per letter instead of a
    fresh Enigma run -- which is what makes it affordable to test all ~2000
    converged boards of an -R 5000 run rather than only the reported one.

    Recovered with 26 constant-letter decrypts under an EMPTY board, where S is
    the identity: decrypt(chr(x)*L, ...)[i] is exactly core_i[x].
    """
    cols = [enigma_ref.decrypt(chr(65 + x) * length, wheels, ring, start, "")
            for x in range(26)]
    return [[ord(cols[x][i]) - 65 for x in range(26)] for i in range(length)]


def pct_correct(board, ct_nums, core, pt_nums):
    """Percentage of message positions this board decrypts correctly."""
    s = enigma_ref._plugboard(board)      # reuse, never re-derive the machine
    hit = 0
    for i, c in enumerate(ct_nums):
        if s[core[i][s[c]]] == pt_nums[i]:
            hit += 1
    return 100.0 * hit / len(pt_nums)


def climb_dump(ct, wheels, ring, start, lang, restarts, seed):
    """Return (reported board, reported score, {converged: score}, {seen}).

    The reported answer comes off the last PROGRESS line, not the best dumpall
    line: --polish runs after all restarts and never appears in the dump (see
    joint_score_gain.climb_board).  The dump gives the CONVERGED boards, which
    is what "did the search produce a board clearing 50%" needs.

    The fourth return is every board echoed on a progress line -- boards the
    climb PASSED THROUGH.  They are a different thing from a converged board:
    a climb can visit a good board and keep going uphill past it.  This set
    UNDERCOUNTS such visits badly, because a progress line fires only on a
    board beating the run's global high-water mark, so one visited late in a
    good run is silent.  Read it as a lower bound, never as a rate.
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
        msgs.append(dict(ct=ct, pt=pt, grund=grund, enc=enc, start=start))
    return dict(board=board, wheels=wheels, ring=ring, msgs=msgs,
                length=length, seed=rng.randrange(1 << 30))


def run_trial(job):
    trial, ladder, lang, depths = job
    board, wheels, ring = trial["board"], trial["wheels"], trial["ring"]
    m1, m2 = trial["msgs"]
    truth = pair_set(board)
    s_true = score_board(m1["ct"], wheels, ring, m1["start"], board, lang)

    core = core_table(wheels, ring, m1["start"], trial["length"])
    ct_nums = [ord(c) - 65 for c in m1["ct"]]
    pt_nums = [ord(c) - 65 for c in m1["pt"]]

    def clears(b):
        return pct_correct(b, ct_nums, core, pt_nums)

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

        pct = clears(got)
        st2 = enigma_ref.decrypt(m2["enc"], wheels, ring, m2["grund"], got)
        got_pairs = pair_set(got)
        rec = dict(pct=pct, exact=(got_pairs == truth),
                   board=got, s_got=s_got, s_true=s_true,
                   lead=(s_got - s_true) * trial["length"],
                   correct=len(got_pairs & truth), nplugs=len(got_pairs),
                   start2=(st2 == m2["start"]))

        # WHERE THE TRUE BOARD SITS in the search's own ranking of the boards
        # it converged on, by the message-1 score it assigned them.  Computed
        # for EVERY trial, recovered ones included -- the question "when the
        # search reaches the truth, how near the top does it put it" is about
        # the whole population, and restricting it to misses would answer a
        # conditioned version of it.  None when the truth never converged.
        order = sorted(dump.items(), key=lambda kv: -kv[1])
        rec["ncand"] = len(order)
        rec["true_rank"] = next((i + 1 for i, (b, _) in enumerate(order)
                                 if pair_set(b) == truth), None)

        # And where the best board CLEARING 50% sits, which is the rank that
        # actually matters: recovering the true board exactly is not required,
        # so the question is how near the top the search puts something good
        # enough.  Computed for every trial too, for the same reason.
        good = [(i + 1, b) for i, (b, _) in enumerate(order)
                if clears(b) >= RECOVER_PCT]
        rec["ngood"] = len(good)
        rec["good_rank"] = good[0][0] if good else None

        if pct >= RECOVER_PCT:
            rec["state"] = "recov"
            # A recovered trial with NO converged board clearing 50% means
            # --polish produced the answer after the restarts: worth keeping
            # separable, since no re-ranking of converged boards could find it.
            rec["polish_only"] = not good
            out[R] = rec
            continue

        # SCORING vs SEARCH keeps the repo's definition -- measured against the
        # TRUE board, since the claim it supports is that the true board is not
        # the model's optimum.  Counted only among non-recovered trials.
        rec["scoring"] = (s_got >= s_true)

        rec["visited"] = any(clears(b) >= RECOVER_PCT for b in seen)
        rec["state"] = "conv" if good else "absent"
        if good:
            # JOINT RE-RANKING AT SEVERAL DEPTHS.  The true board is
            # deliberately NOT added: an attacker re-ranks the list the search
            # gave them, and injecting the answer would report a promotion no
            # real top-K list could deliver.
            #
            # Message-1 scores come from the DUMP, not from a fresh run --
            # verified identical to score_board() to four decimals on every
            # board checked, which is what makes a depth of 128 affordable at
            # all (it halves the subprocess count). Message 2 still needs the
            # binary, since its start is derived per board.
            #
            # The depths are ascending and share one cache, so a sweep of
            # 8/32/128 costs 128 evaluations, not 168.
            s2cache = {}

            def joint(b, s1):
                if b not in s2cache:
                    s2cache[b] = msg2_score(b)
                return None if s2cache[b] is None else s1 + s2cache[b]

            rec["promoted_at"] = {}
            for K in depths:
                best, best_j = None, None
                for b, s1 in list(order[:K]) + [(got, s_got)]:
                    j = joint(b, s1)
                    if j is None:
                        continue
                    if best_j is None or j > best_j:
                        best, best_j = b, j
                rec["promoted_at"][K] = (best is not None
                                         and clears(best) >= RECOVER_PCT)
            rec["promoted"] = rec["promoted_at"][depths[0]]
        out[R] = rec
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=350)
    ap.add_argument("--lengths", "-L", default="40,60,80,100")
    ap.add_argument("--restarts", "-R", default="8,32,100,316,1000,2000,5000")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--language", default="wehrmacht")
    ap.add_argument("--jobs", "-j", type=int,
                    default=max(1, (os.cpu_count() or 2)))
    ap.add_argument("--out", help="write per-trial records here as JSONL")
    ap.add_argument("--rerank-depth", default=str(TOPK),
                    help="candidate-list depths for the joint "
                         "re-ranking, ascending (default %d)" % TOPK)
    args = ap.parse_args()

    ladder = [int(x) for x in args.restarts.split(",")]
    depths = sorted(int(x) for x in args.rerank_depth.split(","))
    texts = load_plaintexts()
    out_fh = open(args.out, "w", encoding="utf-8") if args.out else None
    print("%d authentic telegraphic plaintexts, model '%s', %d trials/cell,\n"
          "recipe -c -f -S i4f10 -J --polish, rotor key GIVEN,\n"
          "a board counts as RECOVERED at >= %.0f%% of the plaintext\n"
          % (len(texts), args.language, args.trials, RECOVER_PCT))

    for length in [int(x) for x in args.lengths.split(",")]:
        rng = random.Random(args.seed + length)
        jobs = [(make_trial(rng, texts, length), ladder, args.language,
                 depths)
                for _ in range(args.trials)]
        with multiprocessing.Pool(args.jobs) as pool:
            results = pool.map(run_trial, jobs)

        if out_fh:
            for i, (job, res) in enumerate(zip(jobs, results)):
                for R, rec in res.items():
                    out_fh.write(json.dumps(dict(
                        rec, L=length, R=R, trial=i,
                        true_board=job[0]["board"])) + "\n")
            out_fh.flush()

        cells_by_r = {R: [r[R] for r in results if r[R]["state"] != "err"]
                      for R in ladder}

        print("L = %d" % length)
        print("%6s %9s %8s %8s %9s %9s"
              % ("-R", "recov50", "exact", "mean%", "scoring", "visited"))
        for R in ladder:
            cells = cells_by_r[R]
            n = len(cells) or 1
            rc = sum(1 for c in cells if c["state"] == "recov")
            ex = sum(1 for c in cells if c.get("exact"))
            sc = sum(1 for c in cells if c.get("scoring"))
            vis = sum(1 for c in cells if c.get("visited"))
            miss = len(cells) - rc
            print("%6d %8.1f%% %7.1f%% %8.1f %8.0f%% %8.1f%%"
                  % (R, 100.0 * rc / n, 100.0 * ex / n,
                     sum(c["pct"] for c in cells) / n,
                     (100.0 * sc / miss) if miss else 0.0,
                     100.0 * vis / n))

        print("\n%6s  of the trials that did NOT recover, did the search"
              " PRODUCE\n%6s  a board clearing 50%%, and does joint re-ranking"
              " find it?" % ("", ""))
        print("%6s %8s %8s %10s %9s %10s"
              % ("-R", "conv", "absent", "promoted", "n good", "best rank"))
        for R in ladder:
            cells = cells_by_r[R]
            n = len(cells) or 1
            cv = [c for c in cells if c["state"] == "conv"]
            ab = sum(1 for c in cells if c["state"] == "absent")
            print("%6d %7.1f%% %7.1f%% %10s %9s %10s"
                  % (R, 100.0 * len(cv) / n, 100.0 * ab / n,
                     ("%d/%d" % (sum(1 for c in cv if c.get("promoted")),
                                 len(cv))) if cv else "-",
                     ("%.1f" % (sum(c["ngood"] for c in cv) / len(cv)))
                     if cv else "-",
                     ("%.0f" % (sum(c["good_rank"] for c in cv) / len(cv)))
                     if cv else "-"))

        if len(depths) > 1:
            # promoted@K against the CEILING at K -- the share of conv trials
            # whose good board is even inside a top-K list.  Reporting the two
            # together is the point: a low promotion rate means something
            # different when the board is not in the window at all.
            print("\n%6s  joint re-ranking at DEPTH: promoted / of those whose"
                  "\n%6s  good board is inside the window at all" % ("", ""))
            hdr = "%6s %7s" % ("-R", "conv")
            for K in depths:
                hdr += " %13s" % ("K=%d" % K)
            print(hdr)
            for R in ladder:
                cv = [c for c in cells_by_r[R] if c["state"] == "conv"]
                if not cv:
                    continue
                row = "%6d %7d" % (R, len(cv))
                for K in depths:
                    p = sum(1 for c in cv if c.get("promoted_at", {}).get(K))
                    inw = sum(1 for c in cv if c["good_rank"] <= K)
                    row += " %5.0f%% /%5.0f%%" % (100.0 * p / len(cv),
                                                  100.0 * inw / len(cv))
                print(row)

        print("\n%6s  where a GOOD board sits in the search's own ranking of"
              " the boards\n%6s  it converged on -- the true one, and the"
              " best one clearing 50%%" % ("", ""))
        print("%6s %7s %9s %8s %8s %7s %7s %7s"
              % ("-R", "board", "present", "rank 1", "top 8", "median",
                 "p90", "of n"))
        for R in ladder:
            cells = cells_by_r[R]
            n = len(cells) or 1
            scale = sum(c["ncand"] for c in cells) / n
            for kind, key in (("true", "true_rank"), ("any50", "good_rank")):
                ranks = sorted(c[key] for c in cells if c.get(key))
                if not ranks:
                    print("%6d %7s %8.1f%% %8s %8s %7s %7s %7.0f"
                          % (R, kind, 0.0, "-", "-", "-", "-", scale))
                    continue
                k = len(ranks)
                # A p90 needs enough observations to BE a p90.  The estimator
                # is nearest-rank, so below k = 20 the index lands on the last
                # element and the column silently becomes "worst of k" -- a
                # single trial reading as a distributional tail.  Print it only
                # when it means what the header says.
                p90 = ("%7d" % ranks[min(k - 1, int(0.9 * k))]
                       if k >= 20 else "%7s" % "-")
                print("%6d %7s %8.1f%% %7.0f%% %7.0f%% %7d %s %7.0f"
                      % (R, kind, 100.0 * k / n,
                         100.0 * sum(1 for r in ranks if r == 1) / k,
                         100.0 * sum(1 for r in ranks if r <= TOPK) / k,
                         ranks[k // 2], p90,
                         scale))

        # What the winning board LOOKS like, split by class: a search failure
        # lost to the truth and a scoring failure beat it, and there is no
        # reason their plug overlap should move with -R the same way.
        for label, want in (("SCORING failures (it OUTSCORES the truth)", True),
                            ("SEARCH failures (the truth OUTSCORES it)",
                             False)):
            print("\n%6s  the winning board on %s" % ("", label))
            print("%6s %6s %9s %9s %9s %8s"
                  % ("-R", "n", "plugs/10", "pct", "lead", "start2"))
            for R in ladder:
                cells = [c for c in cells_by_r[R]
                         if c["state"] != "recov" and c.get("scoring") == want]
                if not cells:
                    print("%6d %6d %9s %9s %9s %8s"
                          % (R, 0, "-", "-", "-", "-"))
                    continue
                n = len(cells)
                print("%6d %6d %9.2f %8.1f %9.1f %7.1f%%"
                      % (R, n,
                         sum(c["correct"] for c in cells) / n,
                         sum(c["pct"] for c in cells) / n,
                         sum(c["lead"] for c in cells) / n,
                         100.0 * sum(1 for c in cells if c["start2"]) / n))
        print()

    if out_fh:
        out_fh.close()

    print("  recov50  = the reported board decrypts >= %.0f%% of message 1"
          % RECOVER_PCT)
    print("  exact    = it is the true board, plug for plug")
    print("  mean%    = mean %-correct over ALL trials, the graded signal")
    print("  scoring  = share of NON-RECOVERED trials whose winner outscores")
    print("             the true board (a scoring failure, not a search one)")
    print("  visited  = a board clearing 50%% was echoed on a progress line,")
    print("             i.e. a climb PASSED THROUGH one and kept going up.")
    print("             A LOWER BOUND -- progress lines fire only above the")
    print("             run's high-water mark")
    print("  conv     = the answer did not read, but a CONVERGED board would")
    print("  promoted = of those, how many joint re-ranking over the search's")
    print("             own top-%d boards puts one of them first" % TOPK)
    print("  n good   = converged boards clearing 50%%")
    print("  best rank= where the best-SCORING one of them sits in the")
    print("             search's own ranking (so top-%d is visibly enough,"
          % TOPK)
    print("             or visibly not)")
    print("  board    = which good board the rank is of: 'true' the true")
    print("             one, 'any50' the best-scoring one clearing 50%%")
    print("  present  = share of trials whose converged boards include such")
    print("             a board; the rank columns are over those trials only")
    print("  top 8    = share of them inside the re-ranking window above")
    print("  p90      = 90th percentile rank, nearest-rank; blank under 20")
    print("             observations, where it would just be the worst one")
    print("  of n     = mean distinct converged boards, the scale a rank is")
    print("             read against")
    print("  plugs/10 = mean plugs the winning board shares with the truth")
    print("  lead     = its score minus the truth's, in LOG UNITS over the")
    print("             whole message (per-symbol score x length)")
    print("  start2   = share deriving message 2's TRUE start from the")
    print("             indicator -- six plugboard lookups, all must be right")


if __name__ == "__main__":
    main()
