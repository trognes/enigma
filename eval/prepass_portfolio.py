#!/usr/bin/env python3
#
# Pre-pass PORTFOLIO probe (PERFORMANCE.md section 6.10).
#
# Tests whether *mixing* IC-pre-pass and mono-pre-pass restarts (a 50/50 algorithm
# portfolio) beats either pure pre-pass at matched compute. For each random problem
# it runs both `-S i4q10` and `-S m4q10` with --dump-restarts, then, per restart
# budget R, compares three strategies by the best-scoring board each finds:
#
#   IC   = best of the first R  i4q10 restarts
#   MONO = best of the first R  m4q10 restarts
#   PORT = best of (first R/2 i4q10 restarts  +  first R/2 m4q10 restarts)   [50/50 mix]
#
# All three see R restarts total, so PORT is matched-compute to IC and MONO.
#
# Finding (see section 6.10): PORT lands BETWEEN IC and MONO -- usually below pure
# MONO -- so the mix is dominated once mono is the stronger base. The portfolio is
# a documented dead end; going all-mono (or, cheaper, pure quad) beats mixing.
#
# Metric: exact plugboard recovery (recovered board == true board) and mean number
# of correct plugs. Plugboard-recovery tier, 10 plugs, quad ranking, PROSE corpora
# (via EVAL_CORPORA -- do NOT drop this; loading all corpora pulls in off-distribution
# telegraphic text and inflates the mono advantage). Reuses tests/eval.py helpers.
#
# Usage:  python3 eval/prepass_portfolio.py         # english+german, L40..70, N=40
#   env:  LANGS, LENS, NPROB, RMAX (default 2560), CUTS (comma-sep restart budgets)

import sys, os, random, collections
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tests"))
import eval as E

os.environ["ENIGMA_DATA"] = "ngrams"
BIN = "./enigma"
RMAX = int(os.environ.get("RMAX", "2560"))
CUTS = [int(x) for x in os.environ.get("CUTS", "1280,2560").split(",")]
NPROB = int(os.environ.get("NPROB", "40"))
LANGS = os.environ.get("LANGS", "english german").split()
LENS = [int(x) for x in os.environ.get("LENS", "40 45 50 55 60 65 70").split()]
CORP = {
    "english": "builtin,city,mountains,ocean",
    "german":  "builtin,wald,reise,wissenschaft",
    "french":  "builtin,mer,montagne,ville",
    "danish":  "builtin,hav,by,skov,danmark",
}


def dump(cfg, lang, key, ct, seed):
    """Run one plugboard climb with --dump-restarts; return [(score, board_set), ...]
    in restart order (board_set = frozenset of frozenset pairs)."""
    u, w, r, g, _ = key
    _, err, _ = E.run(BIN, ["-q", "-l", lang, "-u", u, "-w", w, "-r", r, "-g", g,
                            "-c", "-S", cfg, "-R", str(RMAX), "--random", "10",
                            "--dump-restarts", "-e", str(seed), "-T", "1"], ct)
    out = []
    for line in err.splitlines():
        if line.startswith("restart "):
            f = line.split()
            out.append((float(f[1]), frozenset(frozenset(p) for p in f[2:])))
    return out


def best(lst):
    return max(lst, key=lambda x: x[0])[1] if lst else frozenset()


for lang in LANGS:
    os.environ["EVAL_CORPORA"] = CORP[lang]          # prose only -- important
    corp = E.load_corpora(lang)
    for L in LENS:
        rng = random.Random(7000 + L)                # reproducible problem set
        agg = collections.defaultdict(lambda: [0, 0])  # (strat, R) -> [exact, sum_ncorrect]
        for i in range(NPROB):
            _, excerpt, key = E.gen_problem(rng, corp, L, 10)
            ct = E.encrypt(BIN, key, excerpt)
            true = frozenset(frozenset(p) for p in key[4].split())
            ic = dump("i4q10", lang, key, ct, i)
            mo = dump("m4q10", lang, key, ct, i)
            for R in CUTS:
                h = R // 2
                for strat, board in (("IC", best(ic[:R])),
                                     ("MONO", best(mo[:R])),
                                     ("PORT", best(ic[:h] + mo[:h]))):
                    nc = len(board & true)
                    a = agg[(strat, R)]
                    a[0] += int(nc == 10)
                    a[1] += nc
        for R in CUTS:
            cells = " ".join(
                f"{s}={100 * agg[(s, R)][0] / NPROB:.0f}% [{agg[(s, R)][1] / NPROB:.2f}]"
                for s in ("IC", "MONO", "PORT"))
            print(f"{lang:8} L{L:<2} R={R:<5} N={NPROB} | {cells}")
