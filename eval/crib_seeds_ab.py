#!/usr/bin/env python3
"""Does --crib-seeds K keep the answer while cutting the climbs?

    python3 eval/crib_seeds_ab.py                # the A/B
    python3 eval/crib_seeds_ab.py --trials 8     # a quick look

eval/crib_ic_rank.py measured the RANKING, at the true key: where does the
correct hypothesis land when the survivors are ordered by the index of
coincidence of their decrypt?  It said the window is ~10 letters -- top-10
keeps 92.5% of correct hypotheses out of ~91 survivors -- and that at 8 the
population explodes to ~440 while the ranking degrades.

This measures the SWEEP instead, which is the thing that matters and the thing
that measurement could not see.  A sweep spends its time at WRONG keys, where
survivors are also climbed and also ranked, so two effects the true-key figure
misses are in play: the cut saves work at every key, and it can equally drop a
wrong key's flattering hypothesis or the true key's correct one.

ARMS.  Same message, same key, same board, same crib -- only K varies:

    all   climb every surviving (alignment, hypothesis) pair (the default)
    K=n   IC-rank the survivors, climb the best n

reported as exact recovery and plugboards scored (the -T-independent cost
counter, not wall time).
"""
import argparse
import os
import random
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import corpus, random_key                       # noqa: E402
from ring_stride_geometry_probe import crypt, plugboard         # noqa: E402

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def run(binary, args, ct):
    p = subprocess.run([binary] + args, input=ct, capture_output=True,
                       text=True, check=False)
    it = re.search(r"scored (\d+) plugboard", p.stderr)
    return p.stdout.strip(), int(it.group(1)) if it else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default="./enigma")
    ap.add_argument("--trials", type=int, default=30)
    ap.add_argument("--ks", default="1,3,10,30")
    ap.add_argument("--crib-len", type=int, default=10)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=20260819)
    ap.add_argument("--out", default="eval/results-crib-seeds.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    texts = [t for t in corpus() if len(t) >= 140]
    ks = [int(x) for x in a.ks.split(",")]
    arms = ["all"] + ["K=%d" % k for k in ks]
    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    say(__doc__.split("\n")[0])
    say("\n%d trials, %d-letter crib from the true plaintext, %d-pair board"
        % (a.trials, a.crib_len, a.plugs))
    say("HIDDEN, start position swept (676 keys), -l %s\n" % a.lang)

    ok = {t: [] for t in arms}
    it = {t: [] for t in arms}
    for _ in range(a.trials):
        pt = rng.choice(texts)
        w, r, ring, start = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), a.plugs)
        ct = crypt(pt, w, r, ring, start, plug)
        at = rng.randrange(0, len(pt) - a.crib_len)
        crib = pt[at:at + a.crib_len]
        base = ["-c", "-f", "-l", a.lang, "-S", "i4f10", "-J",
                "-u", r, "-w", "".join(str(x + 1) for x in w),
                "-r", "".join(ALPHA[x] for x in ring),
                "-g", ALPHA[start[0]] + "..",
                "-R", "0", "-T", str(a.threads), "--crib", crib]
        for tag in arms:
            extra = [] if tag == "all" else ["--crib-seeds", tag[2:]]
            out, n = run(a.binary, base + extra, ct)
            ok[tag].append(out == pt)
            it[tag].append(n)

    say("%-6s %-12s %-14s %s" % ("arm", "exact", "plugboards", "vs all"))
    say("-" * 48)
    ref = np.mean(it["all"]) or 1.0
    for tag in arms:
        e = np.array(ok[tag])
        n = np.mean(it[tag])
        say("%-6s %-12s %-14.0f %.2fx"
            % (tag, "%d/%d" % (e.sum(), e.size), n, ref / max(n, 1.0)))

    say()
    base_ok = np.array(ok["all"])
    for tag in arms[1:]:
        e = np.array(ok[tag])
        lost = int((base_ok & ~e).sum())
        gained = int((e & ~base_ok).sum())
        say("%-6s only-all %d, only-%s %d" % (tag, lost, tag, gained))

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
