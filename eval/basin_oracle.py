#!/usr/bin/env python3
#
# Restart basin structure at the oracle level (PERFORMANCE.md section 6.16).
#
# Dumps the R converged plugboard boards per problem (--dump-restarts, true rotor
# key fixed, recommended -a recipe) and, against the KNOWN true plugboard, measures
# how many CORRECT plugs each restart recovers. This exposes what the exact-board
# distinct-count (which the harness DIVERSITY mode and --restart-tt report) hides:
#
#   dist_exact    -- distinct converged boards           (~60/64: looks diverse)
#   dist_correct  -- distinct sets of *correct* plugs     (~15/64: the real basins)
#   mean/max_cor  -- per-restart depth toward the 10-plug truth (~0.7 / ~5-7)
#   union_cor     -- correct plugs assembled across ALL restarts (~9/10: GA material)
#
# Finding: the ~60-way exact-board "diversity" is a 4x overcount -- 64 restarts land
# in only ~15 distinct correct-plug states, most of them shallow/wrong, with the
# truth assembled only in the union. The residual is a rare-deep-basin COVERAGE
# problem, not redundancy; and the union material is unselectable (per-plug consensus
# ~1.1/10 correct, section 3.10), so no truth-free kick/repel/recombination helps.
#
# Run from the repo root:  python3 eval/basin_oracle.py   (env: N, LEN)

import os, sys, random, statistics

sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
import eval as E  # noqa

os.environ["ENIGMA_DATA"] = "ngrams"
os.environ["ENIGMA_SEED"] = "0"
BIN = "./enigma"
PAIRS = 10
N = int(os.environ.get("N", "40"))
LEN = int(os.environ.get("LEN", "50"))
LANGS = ["english", "german"]
CRACK = ["-c", "--dump-restarts", "-R", "64", "-S", "m4a10", "-J", "--gainfix-best3", "-T", "8"]


def pairset(pb):
    return set(frozenset((p[0], p[1])) for p in pb.split() if len(p) == 2)


def main():
    print("len=%d  problems/lang=%d  R=64  model=-a\n" % (LEN, N))
    print("%-8s %10s %10s %8s %8s %9s" %
          ("lang", "dist_exact", "dist_corr", "mean_cor", "max_cor", "union_cor"))
    for lang in LANGS:
        rng = random.Random(0)
        corpora = E.load_corpora(lang)
        de, dc, mc, xc, uc = [], [], [], [], []
        for _ in range(N):
            _, excerpt, key = E.gen_problem(rng, corpora, LEN, PAIRS)
            u, w, r, g, pb = key
            true = pairset(pb)
            ct = E.encrypt(BIN, key, excerpt)
            args = ["-a", "-l", lang, "-u", u, "-w", w, "-r", r, "-g", g] + CRACK
            _, err, _ = E.run(BIN, args, ct)
            rs = [line.split(None, 2)[2] if len(line.split(None, 2)) > 2 else ""
                  for line in err.splitlines() if line.startswith("restart ")]
            if not rs:
                continue
            corr = [frozenset(pairset(b) & true) for b in rs]
            de.append(len(set(rs)))
            dc.append(len(set(corr)))
            mc.append(statistics.mean(len(c) for c in corr))
            xc.append(max(len(c) for c in corr))
            uc.append(len(set().union(*corr)))
        n = len(de)
        print("%-8s %10.1f %10.1f %8.2f %8.2f %9.2f" %
              (lang, sum(de) / n, sum(dc) / n, sum(mc) / n, sum(xc) / n, sum(uc) / n))


if __name__ == "__main__":
    main()
