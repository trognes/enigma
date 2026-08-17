#!/usr/bin/env python3
"""Does signature seeding still win when the ROTOR KEY IS UNKNOWN?

    python3 eval/seeded_sweep.py                # the sweep A/B
    python3 eval/seeded_sweep.py --trials 4     # a quick look

Every other measurement in this investigation (`eval/seeder_vs_restarts.py`)
gives the rotor key and hides only the plugboard, which is the tier this repo
tunes on -- and it is the seeder's BEST case.  A real sweep is different in the
one way that matters: the deduction runs at EVERY key and the seeded climbs are
paid at every key, so a seeded key costs `deduction + k climbs` against a
baseline key's `-R` climbs.  That multiplies across the keyspace in the opposite
direction from the win, and nothing so far reaches it.

This is the smallest honest test of it: `-g A..` with the wheels, reflector and
ring fixed, i.e. 676 start positions.  Small enough to run many trials, real
enough that the true key has to outscore 675 competitors.

THE ARMS

  A<N>  baseline   one process sweeping all 676 keys with -R N, as the tool
                   would actually be invoked
  S<k>  seeded     per key: run the terminal-signature deduction, IC-rank the
                   seeds, climb the top k with their cables pinned via -s (and
                   deduced no-cable letters via --no-plug) at -R 0 -- the
                   configuration `eval/results-seed-hedge.txt` settled on.
                   Best by score within a key, then across keys.

WHAT TO WATCH, and it is not only the headline.  Two effects pull opposite ways
and only a sweep shows either:

  * a wrong key's deduction pins WRONG plugs, which should depress its score and
    HELP discrimination -- seeding as an accidental filter;
  * or it pins plugs that happen to flatter a wrong key, PROMOTING it over the
    truth, which would be fatal and is invisible at a pinned key.

So the run reports the true key's rank under each arm, not just whether the
winner was right.

COST IS IN score_iter, as everywhere else here.  Wall time is meaningless: the
seeded arm spawns one process per (key, seed) and each pays the ~50 ms n-gram
load, which is ~95% of its wall time and an artefact of the harness rather than
of the method.  The binary has no seeded-sweep mode -- that is the point of
measuring before building one.
"""
import argparse
import math
import os
import random
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import selfcrib_probe as SC                                  # noqa: E402
from crib_menu import core_rows, corpus                       # noqa: E402
from ring_stride_geometry_probe import crypt, num, plugboard  # noqa: E402
from seeder_vs_restarts import (ALPHA, RECIPE, binom_two,     # noqa: E402
                                paired_ci, pct, run, seeds_for)


def sweep_baseline(binary, ct, wheels, refl, ring, restarts, lang, threads):
    """One process, all 676 keys, exactly as the tool would be invoked."""
    args = list(RECIPE) + [
        "-u", refl, "-w", wheels, "-r", ring, "-g", "A..",
        "-l", lang, "-T", str(threads), "-R", str(restarts)]
    return run(binary, args, ct)


def sweep_seeded(binary, ct, wheels, refl, ring, k, lang, tab, pool):
    """Per key: deduce, IC-rank, climb the top k pinned at -R 0.

    Returns (best_plaintext, total_score_iter, per_key_best_score) where the
    last is indexed by the (g1, g2) key so the true key's RANK can be read off.
    """
    n = len(ct)
    c = num(ct)
    jobs = []
    for g1 in range(26):
        for g2 in range(26):
            start = "A" + ALPHA[g1] + ALPHA[g2]
            rows = core_rows([int(x) - 1 for x in wheels], refl,
                             np.array([ord(x) - 65 for x in ring]),
                             np.array([0, g1, g2]), n)
            ranked, _ = seeds_for(ct, rows, np.arange(26), tab)
            for cables, noplug, _ in ranked[:k]:
                args = list(RECIPE) + [
                    "-u", refl, "-w", wheels, "-r", ring, "-g", start,
                    "-l", lang, "-T", "1", "-R", "0"]
                if cables:
                    args += ["-s", cables]
                if noplug:
                    args += ["--no-plug", noplug]
                jobs.append(((g1, g2), args))
    got = list(pool.map(lambda j: run(binary, j[1], ct), jobs))
    best = {}
    total = 0
    for (key, _), (out, it, _wl, sc) in zip(jobs, got):
        total += it or 0
        if sc is not None and (key not in best or sc > best[key][0]):
            best[key] = (sc, out)
    if not best:
        return "", total, {}
    win = max(best.items(), key=lambda kv: kv[1][0])
    return win[1][1], total, {kk: v[0] for kk, v in best.items()}


def rank_of(scores, true_key):
    """1-based rank of the true key among all keys, by score."""
    if true_key not in scores:
        return None
    order = sorted(scores, key=lambda k: -scores[k])
    return order.index(true_key) + 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default="./enigma")
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--trials", type=int, default=12)
    ap.add_argument("--restarts", default="1,2,4,8",
                    help="-R levels for the baseline sweep")
    ap.add_argument("--ks", default="1,3,5", help="top-k for the seeded sweep")
    ap.add_argument("--wheels", default="231")
    ap.add_argument("--reflector", default="B")
    ap.add_argument("--ring", default="AAA")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=20260818)
    ap.add_argument("--out", default="eval/results-seeded-sweep.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    tab = SC.ngram_tables(a.lang)
    ends = [t for t in corpus()
            if any(len(t) - (s + 2 * L + 1) <= 1
                   for s, L in SC.doublings(t, minlen=4, maxlen=20))]
    rlevels = [int(x) for x in a.restarts.split(",")]
    ks = [int(x) for x in a.ks.split(",")]
    tags = ["A%d" % r for r in rlevels] + ["S%d" % k for k in ks]

    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    say(__doc__.split("\n")[0])
    say("\n676 keys (-g A.., -w %s -u %s -r %s fixed), %d trials, -l %s"
        % (a.wheels, a.reflector, a.ring, a.trials, a.lang))
    say("baseline -R %s; seeded top-k %s at -R 0, cables pinned\n"
        % (a.restarts, a.ks))

    rows_out = []
    pool = ThreadPoolExecutor(max_workers=a.threads)
    for t in range(a.trials):
        pt_text = ends[t % len(ends)]
        g1, g2 = rng.randrange(26), rng.randrange(26)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
        ct = crypt(pt_text, [int(x) - 1 for x in a.wheels], a.reflector,
                   np.array([ord(x) - 65 for x in a.ring]),
                   np.array([0, g1, g2]), plug)
        out = {}
        t0 = time.perf_counter()
        for tag in tags:
            if tag[0] == "A":
                rec, it, _wl, _sc = sweep_baseline(
                    a.binary, ct, a.wheels, a.reflector, a.ring,
                    int(tag[1:]), a.lang, a.threads)
                out[tag] = (pct(rec, pt_text), it, None)
            else:
                rec, it, scores = sweep_seeded(
                    a.binary, ct, a.wheels, a.reflector, a.ring,
                    int(tag[1:]), a.lang, tab, pool)
                out[tag] = (pct(rec, pt_text), it, rank_of(scores, (g1, g2)))
        rows_out.append((len(ct), out))
        say("  trial %2d  n=%-4d %s   [%.0f s]"
            % (t + 1, len(ct),
               "  ".join("%s %5.1f" % (tg, out[tg][0]) for tg in tags),
               time.perf_counter() - t0))
    pool.shutdown()

    say()
    means = {t: np.array([r[1][t][0] for r in rows_out]) for t in tags}
    exact = {t: means[t] > 99.999 for t in tags}
    iters = {t: np.array([r[1][t][1] or 0 for r in rows_out]) for t in tags}
    say("%-6s %-10s %-12s %-14s %s"
        % ("arm", "mean %", "exact", "score_iter", "true-key rank"))
    for t in tags:
        rk = [r[1][t][2] for r in rows_out if r[1][t][2] is not None]
        say("%-6s %-10.1f %-12s %-14.0f %s"
            % (t, means[t].mean(), "%d/%d" % (exact[t].sum(), len(rows_out)),
               iters[t].mean(),
               "-" if not rk else "median %d, top-1 %d/%d"
               % (int(np.median(rk)), sum(x == 1 for x in rk), len(rk))))

    say()
    for k in ks:
        s = "S%d" % k
        near = min(("A%d" % r for r in rlevels),
                   key=lambda x: abs(iters[x].mean() - iters[s].mean()))
        d, lo, hi = paired_ci(means[s] - means[near])
        x = int((exact[s] & ~exact[near]).sum())
        y = int((exact[near] & ~exact[s]).sum())
        say("%s (%.0f iters) vs %s (%.0f, %.2fx): mean %+.1fpp"
            " CI [%+.1f, %+.1f]"
            % (s, iters[s].mean(), near, iters[near].mean(),
               iters[near].mean() / max(1.0, iters[s].mean()), d, lo, hi))
        say("   exact %+d (only %s %d, only %s %d), McNemar p = %.4f"
            % (x - y, s, x, near, y, binom_two(x, y)))

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
