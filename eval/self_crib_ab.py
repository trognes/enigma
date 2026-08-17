#!/usr/bin/env python3
"""--self-crib-seeds in the tool: the sweep A/B, and the --self-crib-length default.

    python3 eval/signature_seed_ab.py                # both measurements
    python3 eval/signature_seed_ab.py --trials 4     # a quick look

`eval/seeded_sweep.py` measured this method by driving the binary from Python, one
process per (key, seed).  That rig CHARGED THE SEEDED ARM ~10x TOO MUCH: `--polish`
is a once-per-run finisher costing ~10 700 score_iter, and the rig made every key
its own run, while the baseline arm swept all 676 keys in one process and paid it
once.  Now that the mode is in the tool both arms are a single process, so this
re-measures it honestly -- and the correction runs in the method's favour, which is
the direction to be most careful about.

Two questions:

  1. THE SWEEP A/B.  `-g A..` with wheels, reflector and ring fixed = 676 start
     positions, so the true key must outscore 675 competitors.  Baseline `-R N`
     against `--self-crib-seeds K`, compared at equal score_iter.

  2. THE --self-crib-length DEFAULT.  A floor drops the weak short hypotheses (an
     L=4 menu deduces almost nothing and rejects nothing) and makes the deduction
     cheaper, at the price of missing a message actually signed with a short name.
     The corpus says 10 of 66 messages end with a 4+ signature but only 7 with a 7+
     one, so the floor trades coverage for seed quality.  Measured, not guessed.
"""
import argparse
import os
import random
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import selfcrib_probe as SC                                  # noqa: E402
from crib_menu import corpus                                  # noqa: E402
from ring_stride_geometry_probe import crypt, plugboard        # noqa: E402

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-f", "-J", "--polish", "-S", "i4f10"]


def run(binary, args, text):
    p = subprocess.run([binary] + args, input=text, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    it = None
    for line in p.stderr.splitlines():
        m = re.search(r"scored (\d+) plugboards", line)
        if m:
            it = int(m.group(1))
    return p.stdout.strip(), it or 0


def pct(rec, truth):
    if not rec:
        return 0.0
    return 100.0 * sum(a == b for a, b in zip(rec, truth)) / len(truth)


def trials(rng, ends, n):
    out = []
    for i in range(n):
        pt = ends[i % len(ends)]
        g1, g2 = rng.randrange(26), rng.randrange(26)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
        ct = crypt(pt, [1, 2, 0], "B", np.array([0, 0, 0]),
                   np.array([0, g1, g2]), plug)
        out.append((pt, ct))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default="./enigma")
    ap.add_argument("--trials", type=int, default=30)
    ap.add_argument("--restarts", default="1,2,4,8,16")
    ap.add_argument("--ks", default="1,3,5")
    ap.add_argument("--lengths", default="4,5,6,7,8")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=20260819)
    ap.add_argument("--out", default="eval/results-self-crib.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    ends = [t for t in corpus()
            if any(len(t) - (s + 2 * L + 1) <= 1
                   for s, L in SC.doublings(t, minlen=4, maxlen=20))]
    cases = trials(rng, ends, a.trials)
    key = ["-u", "B", "-w", "231", "-r", "AAA", "-g", "A..",
           "-l", "wehrmacht", "-T", str(a.threads)]
    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    say(__doc__.split("\n")[0])
    say("\n676 keys (-g A.., -w 231 -u B -r AAA fixed), %d trials, -l wehrmacht"
        % len(cases))
    say("recipe %s\n" % " ".join(RECIPE))

    arms = [("R%s" % r, ["-R", r]) for r in a.restarts.split(",")]
    arms += [("K%s" % k, ["-R", "0", "--self-crib-seeds", k])
             for k in a.ks.split(",")]
    res = {t: [[], []] for t, _ in arms}
    for pt, ct in cases:
        for tag, extra in arms:
            rec, it = run(a.binary, RECIPE + key + extra, ct)
            res[tag][0].append(pct(rec, pt))
            res[tag][1].append(it)
    say("%-8s %-10s %-12s %-14s %s"
        % ("arm", "mean %", "exact", "score_iter", "per key"))
    for tag, _ in arms:
        m = np.array(res[tag][0])
        i = np.array(res[tag][1])
        say("%-8s %-10.1f %-12s %-14.0f %.0f"
            % (tag, m.mean(),
               "%d/%d" % ((m > 99.999).sum(), m.size), i.mean(),
               i.mean() / 676))

    say("\n--self-crib-length sweep (K=1), coverage vs seed quality:")
    say("%-8s %-10s %-12s %-14s %s"
        % ("length", "mean %", "exact", "score_iter", "per key"))
    for L in a.lengths.split(","):
        ms, its = [], []
        for pt, ct in cases:
            rec, it = run(a.binary, RECIPE + key +
                          ["-R", "0", "--self-crib-seeds", "1",
                           "--self-crib-length", L], ct)
            ms.append(pct(rec, pt))
            its.append(it)
        m = np.array(ms)
        say("%-8s %-10.1f %-12s %-14.0f %.0f"
            % (L, m.mean(), "%d/%d" % ((m > 99.999).sum(), m.size),
               np.mean(its), np.mean(its) / 676))

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
