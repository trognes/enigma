#!/usr/bin/env python3
"""How many -R restarts is a self-crib search worth, at a given doubling length?

    python3 eval/selfcrib_vs_restarts.py --lengths 6 9 13 --restarts 8 32 64 \
                                         --trials 20

WHAT THIS ANSWERS.  The self-crib measurements on record compare the seeder
against a single restart count (`-R 16`) over a mixed population, so they say
"beats R16" or "ties R16" and nothing more.  They also sweep the SEARCH
parameter `--self-crib-length` rather than the message's own doubling length,
over messages of 48-214 letters -- so the message property and the message
length move together with the sample.

Here the message length is FIXED at 167 and the doubling length is the axis, on
synthetic-but-authentic messages from make_doubling_messages.py.  Each trial
runs one ciphertext through the seeder and through several plain restart counts,
so the arms are paired and the equivalent `-R` can be read off directly.

READ THE RESTART ARM'S OWN COLUMN BEFORE COMPARING BUCKETS.  A longer doubling
is more repeated text in the same 167 letters, so it lifts the index of
coincidence and makes the message easier for EVERY method.  If the `-R 32`
column climbs across buckets too, the bucket got easier -- that is not the
seeder improving.  Comparisons WITHIN a bucket are clean; comparisons ACROSS
buckets need that column read alongside.

The seeder runs at the DEFAULT `--self-crib-length 6` in every bucket, not at
the bucket's own length: a real user does not know how long the doubling is, so
telling the search would flatter it.
"""
import argparse
import json
import os
import random
import re
import subprocess
import sys
import time
from math import comb

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import corpus, random_key                          # noqa: E402
from ring_stride_geometry_probe import crypt, plugboard           # noqa: E402
from make_doubling_messages import word_bank, make                # noqa: E402

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def run(binary, args, ct):
    t0 = time.perf_counter()
    p = subprocess.run([binary] + args, input=ct, capture_output=True,
                       text=True, check=False)
    wall = time.perf_counter() - t0
    m = re.search(r"scored (\d+) plugboard", p.stderr)
    return p.stdout.strip(), int(m.group(1)) if m else 0, wall


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default="./enigma")
    ap.add_argument("--lengths", type=int, nargs="+",
                    default=[4, 5, 6, 7, 9, 13])
    ap.add_argument("--restarts", type=int, nargs="+",
                    default=[16, 32, 64, 128])
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--total", type=int, default=167)
    ap.add_argument("--seeds", type=int, default=10)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=20260822)
    ap.add_argument("--out", default="eval/results-selfcrib-vs-restarts.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    texts = corpus()
    bank = word_bank(texts)
    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    say(__doc__.split("\n")[0])
    say("\nL=%d fixed, %d trials/bucket, 10-pair board hidden, 676-key sweep,"
        % (a.total, a.trials))
    say("-c -f -l %s -J -S i4f10, seeder at --self-crib-seeds %d -R 0"
        % (a.lang, a.seeds))
    say("(the seeder runs at the DEFAULT --self-crib-length 6 in every bucket)\n")

    arms = ["selfcrib"] + ["R%d" % r for r in a.restarts]
    results = {}
    for wlen in a.lengths:
        ok = {k: [] for k in arms}
        wl = {k: [] for k in arms}
        it = {k: [] for k in arms}
        for _ in range(a.trials):
            pt, _word = make(rng, texts, bank, wlen, a.total)
            if pt is None:
                continue
            w, r, ring, start = random_key(rng)
            plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                             a.plugs)
            ct = crypt(pt, w, r, ring, start, plug)
            base = ["-c", "-f", "-l", a.lang, "-J", "-S", "i4f10",
                    "-u", r, "-w", "".join(str(x + 1) for x in w),
                    "-r", "".join(ALPHA[x] for x in ring),
                    "-g", ALPHA[start[0]] + "..",
                    "-T", str(a.threads), "--no-preflight"]
            trials = [("selfcrib", ["--self-crib-seeds", str(a.seeds),
                                    "-R", "0"])]
            trials += [("R%d" % r2, ["-R", str(r2)]) for r2 in a.restarts]
            for tag, extra in trials:
                out, n, s = run(a.binary, base + extra, ct)
                ok[tag].append(out == pt)
                it[tag].append(n)
                wl[tag].append(s)
        results[wlen] = (ok, wl, it)
        say("doubling length %d done" % wlen)

    say()
    say("%-9s %-10s %-9s %-11s %s" % ("doubling", "arm", "exact", "wall/trial",
                                      "plugboards"))
    say("-" * 60)
    for wlen in a.lengths:
        ok, wl, it = results[wlen]
        for k in arms:
            e = np.array(ok[k])
            say("%-9d %-10s %-9s %-11.2f %.0f"
                % (wlen, k, "%d/%d" % (e.sum(), e.size), np.mean(wl[k]),
                   np.mean(it[k])))
        say("")

    # The arms are PAIRED -- same ciphertext through every one -- so McNemar on
    # the discordant trials is available and is the test that applies. Aggregate
    # counts alone cannot distinguish 19-vs-19 with 0 discordant (identical
    # behaviour) from 19-vs-19 with 8 discordant (both arms failing different
    # messages), and those mean very different things.
    say("PAIRED COMPARISON  seeder vs each restart arm (McNemar, exact)")
    say("%-9s %-7s %-9s %-9s %-11s %s"
        % ("doubling", "arm", "seeder+", "arm+", "discordant", "p"))
    say("-" * 62)
    for wlen in a.lengths:
        ok, _, _ = results[wlen]
        sc = np.array(ok["selfcrib"])
        for r in a.restarts:
            ar = np.array(ok["R%d" % r])
            b = int((sc & ~ar).sum())
            c = int((ar & ~sc).sum())
            n = b + c
            if n == 0:
                p = 1.0
            else:
                k = min(b, c)
                p = min(1.0, 2 * sum(comb(n, i) for i in range(k + 1)) / 2 ** n)
            say("%-9d %-7s %-9d %-9d %-11d %.3f"
                % (wlen, "R%d" % r, b, c, n, p))
        say("")

    say("EQUIVALENT -R  (the restart count whose break rate the seeder matches)")
    say("%-9s %-12s %-34s %s" % ("doubling", "seeder", "restart arms", "reading"))
    say("-" * 76)
    for wlen in a.lengths:
        ok, wl, _ = results[wlen]
        sc = int(np.sum(ok["selfcrib"]))
        cells = "  ".join("R%d:%d" % (r, int(np.sum(ok["R%d" % r])))
                          for r in a.restarts)
        below = [r for r in a.restarts if int(np.sum(ok["R%d" % r])) <= sc]
        above = [r for r in a.restarts if int(np.sum(ok["R%d" % r])) > sc]
        if not above:
            verdict = "beats R%d (top arm)" % max(a.restarts)
        elif not below:
            verdict = "below R%d (lowest arm)" % min(a.restarts)
        else:
            verdict = "between R%d and R%d" % (max(below), min(above))
        say("%-9d %-12s %-34s %s"
            % (wlen, "%d/%d" % (sc, a.trials), cells, verdict))

    say("\nCost note: compare wall/trial, not plugboards -- the deduction runs")
    say("outside the counted score loop, so the counter understates the seeder.")

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    # Per-trial outcomes, so a later reader can re-test without re-running the
    # hours this cost.
    jl = a.out.rsplit(".", 1)[0] + ".jsonl"
    with open(jl, "w", encoding="utf-8") as fh:
        for wlen in a.lengths:
            ok, wl, it = results[wlen]
            for i in range(len(ok["selfcrib"])):
                fh.write(json.dumps({
                    "doubling": wlen, "trial": i,
                    "exact": {k: bool(ok[k][i]) for k in arms},
                    "wall": {k: round(wl[k][i], 3) for k in arms},
                    "plugboards": {k: it[k][i] for k in arms}}) + "\n")
    print("per-trial data in %s" % jl)
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
