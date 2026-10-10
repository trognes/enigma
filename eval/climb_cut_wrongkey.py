#!/usr/bin/env python3
"""What does each climb-cut arm cost per restart on WRONG keys?

    git apply eval/climb_cut.patch && make && cp enigma /tmp/enigma-cut
    git checkout src && make
    python3 eval/climb_cut_wrongkey.py --binary /tmp/enigma-cut

eval/climb_cut_ab.py prices each arm at the TRUE key, which is the regime of
the plugboard tier it measures.  A real sweep spends nearly all its climbs on
wrong keys, where a climb has no true basin to converge into and its stale
runs may be longer or shorter, so an arm's saving there need not match.

Per fixture: encipher an excerpt under a random key and 10-pair board, then
sweep 676 keys under a DIFFERENT wheel order (every key wrong), at two
restart counts, and take the difference so startup and the one-off work
cancel.  Min of 2 reps; arm order rotated per fixture so drift is shared.
"""
import argparse
import os
import random
import subprocess
import time

from climb_cut_ab import GATES, RECIPE, LET, arm_env, decrypts, fixture, run

HERE = os.path.dirname(os.path.abspath(__file__))


def sweep_time(binary, env, key, R, ct):
    t0 = time.perf_counter()
    run(binary, env, key + RECIPE + ["-e", "1", "-R", str(R)], ct)
    return time.perf_counter() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--lengths", type=int, nargs="+", default=[80, 107, 167])
    ap.add_argument("--fixtures", type=int, default=6)
    ap.add_argument("--arms", nargs="+", default=["cap200", "dontlook"])
    ap.add_argument("--lo", type=int, default=1)
    ap.add_argument("--hi", type=int, default=5)
    ap.add_argument("--seed", type=int, default=71)
    ap.add_argument("--gate", nargs="*", default=[],
                    help="L=T score gates for the gated arm")
    args = ap.parse_args()
    for g in args.gate:
        L, T = g.split("=")
        GATES[int(L)] = T
    # run() discards the binary's stderr, so a missing table reads as a
    # fast sweep rather than an error; pin the data directory.
    os.environ.setdefault("ENIGMA_DATA",
                          os.path.join(os.path.dirname(HERE), "ngrams"))
    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(
                         HERE, "enigma-army-messages-1941.txt")))
    names = ["base"] + args.arms
    print(f"recipe {' '.join(RECIPE)}; 676 wrong keys per sweep; "
          f"per-restart time from -R {args.hi} minus -R {args.lo}\n")
    for L in args.lengths:
        per = {a: 0.0 for a in names}
        for f in range(args.fixtures):
            pt, key, pb = fixture(corpus, L, args.seed * 1009 + L * 31 + f)
            ct = run(args.binary, {}, key + ["-s", pb], pt)
            rng = random.Random(f)
            w = key[3]
            while True:
                w2 = "".join(map(str, rng.sample([1, 2, 3, 4, 5], 3)))
                if w2 != w:
                    break
            wrong = ["-u", "B", "-w", w2, "-r", key[5],
                     "-g", rng.choice(LET) + ".."]
            order = names[f % len(names):] + names[:f % len(names)]
            for a in order:
                best = None
                for _ in range(2):
                    env = arm_env(a, L)
                    d = (sweep_time(args.binary, env, wrong, args.hi, ct)
                         - sweep_time(args.binary, env, wrong, args.lo,
                                      ct)) / (args.hi - args.lo)
                    best = d if best is None else min(best, d)
                per[a] += best
        print(f"L={L}  base {per['base'] / args.fixtures / 676 * 1e3:.3f} "
              "ms per key-restart;  vs base: "
              + ", ".join(f"{a} {per[a] / per['base']:.3f}"
                          for a in args.arms))


if __name__ == "__main__":
    main()
