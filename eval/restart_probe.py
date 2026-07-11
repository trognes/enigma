#!/usr/bin/env python3
#
# Restart coverage curve (PERFORMANCE.md section 6.16).
#
# Exact-recovery % vs the restart count R (16/64/256) at hard lengths, recommended
# -a recipe, true rotor key fixed. Answers whether the short-message residual is a
# hard floor (curve saturates -> the true basin is unreachable, no kick crosses it)
# or compute-bound (curve still climbing -> more restarts keep recovering).
#
# Finding: still climbing ~+15-25pp per 4x R at R=256, no plateau -- compute-bound.
# The true basin IS reachable (a rare deep target hit stochastically), so raw compute
# (more restarts via -T) is the reliable lever. See also eval/basin_oracle.py.
#
# Run from the repo root:  python3 eval/restart_probe.py   (env: TR)

import os, subprocess, re
from concurrent.futures import ThreadPoolExecutor

LANGS = ["english", "german"]
RS = ["16", "64", "256"]
LENGTHS = "50 60"
TR = os.environ.get("TR", "120")
ROW = re.compile(r"^\s*(\d+)\s+([\d.]+)\s+([\d.]+)\s*$")   # len mean exact


def run_one(lang, R):
    env = dict(os.environ)
    env.update(ENIGMA_DATA="ngrams", CLANG=lang, MODEL="a", TRIALS=TR, SEED="0",
               LENGTHS=LENGTHS, RESTARTS=R, CRACKOPTS="-S m4a10 -J --gainfix-best3 -T 8")
    p = subprocess.run(["python3", "tests/crack_quality.py"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True, env=env)
    rows = [(lang, R) + m.groups() for line in p.stdout.splitlines() if (m := ROW.match(line))]
    return rows


def main():
    print("coverage curve  model=-a  trials/len=%s" % TR)
    print("%-8s %4s %4s %8s %8s" % ("lang", "R", "len", "mean%", "exact%"))
    jobs = [(l, R) for l in LANGS for R in RS]
    with ThreadPoolExecutor(max_workers=3) as ex:   # each job already uses -T 8
        for rows in ex.map(lambda j: run_one(*j), jobs):
            for lang, R, L, mean, exact in rows:
                print("%-8s %4s %4s %8s %8s" % (lang, R, L, mean, exact))


if __name__ == "__main__":
    main()
