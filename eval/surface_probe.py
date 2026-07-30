#!/usr/bin/env python3
#
# Climb-surface-smoothness probe (archived/PERFORMANCE.md section 6.15).
#
# Varies ONLY the -a target weighting's smoothness (via ENIGMA_LOGLIN, symmetric),
# holding the recommended recipe fixed (-c -R 10 -S m4q10 -J --polish, mono
# pre-pass, fixed R -> ~matched compute). Metric via SPLIT: search-fail% (the failures
# we attack) and exact%. Baseline weights = -a's (1,0.6,0.3,0.15).
#
# Question: is -a's weighting the sweet spot of surface smoothness, or does a smoother
# (wider-basin) / sharper (narrower-basin) surface reach more true basins?
#
# Finding: FLAT. An 8x sweep (sharp tri 0.3 -> smoothest tri 2.4) moves search-fail%
# by <1pp (within 300-trial noise); baseline sits at the shallow optimum. There is no
# interior smoothness to exploit and no smooth-vs-exact tradeoff to turn into a
# continuation -- the scoring surface shape is not the binding constraint.
#
# Run from the repo root:  python3 eval/surface_probe.py   (env: TR)

import os, subprocess, re
from concurrent.futures import ThreadPoolExecutor

LANGS = ["english", "german"]
# (name, weights quad,tri,bi,mono)  smoothness increases down the list
LEVELS = [("sharp",     "1,0.3,0.15,0.07"),
          ("baseline",  "1,0.6,0.3,0.15"),
          ("smooth",    "1,1.2,0.6,0.3"),
          ("smoothest", "1,2.4,1.2,0.6")]
LENGTHS = "40 50 60 70"
TR = os.environ.get("TR", "300")
ROW = re.compile(r"^\s*(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$")  # len mean exact sf cf


def run_one(lang, name, weights):
    env = dict(os.environ)
    env.update(ENIGMA_DATA="ngrams", CLANG=lang, MODEL="q", TRIALS=TR, SEED="0",
               SPLIT="1", LENGTHS=LENGTHS, RESTARTS="10",
               CRACKOPTS="-S m4q10 -J --polish",
               ENIGMA_LOGLIN=weights, ENIGMA_LOGLIN_SYM="1")
    p = subprocess.run(["python3", "tests/crack_quality.py"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True, env=env)
    return [(lang, name) + m.groups() for line in p.stdout.splitlines() if (m := ROW.match(line))]


def main():
    print("surface-smoothness sweep (search-fail% is the target metric)  trials/len=%s" % TR)
    print("%-8s %-9s %4s %8s %8s %10s" % ("lang", "level", "len", "mean%", "exact%", "search-f%"))
    jobs = [(l, nm, w) for l in LANGS for (nm, w) in LEVELS]
    with ThreadPoolExecutor(max_workers=6) as ex:
        for rows in ex.map(lambda j: run_one(*j), jobs):
            for lang, name, L, mean, exact, sf, _cf in rows:
                print("%-8s %-9s %4s %8s %8s %10s" % (lang, name, L, mean, exact, sf))


if __name__ == "__main__":
    main()
