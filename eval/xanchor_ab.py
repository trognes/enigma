#!/usr/bin/env python3
"""Two last search levers at L=80, -R 100, rotor key given, against plain
restarts on the same trials as eval/vocab_seed_ab.py -- ENHANCEMENTS.md 2b.

  biased   -R 100 --biased-random 1, shipped and measured +9% of breaks at
           -R 1..5 but never at 100;
  xanchor  anchor the climb on X, the most distinctive telegraphic letter:
           26 runs pinning steck[X] to each partner via -s (25 pairs) plus
           --no-plug X, at -R 4 each (104 climbs), best by reported score.
           The self-crib deduction's 26 guesses on steck[X], without the
           doubling that makes them deduce anything.

    python3 eval/xanchor_ab.py TRIALS SEED

About 2 s per trial, nearly all of it the 26 process startups of xanchor.
"""
import os
import random
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import (BIN, ROOT, load_plaintexts, rand_board,
                              rand_pos, rand_wheels)

L = 80
N = int(sys.argv[1]) if len(sys.argv) > 1 else 70
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 41
ENV = dict(os.environ, ENIGMA_DATA="ngrams")


def run(ct, w, r, g, seed, extra):
    """(reported plaintext, score of the last progress line)."""
    env = dict(ENV, ENIGMA_SEED=str(seed))
    out = subprocess.run(
        [BIN, "-c", "-f", "-S", "k4f10", "-K", "--polish", "-l", "wehrmacht",
         "-T", "1", "-u", "B", "-w", w, "-r", r, "-g", g, *extra],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT)
    score = None
    for line in out.stderr.splitlines():
        f = line.split()
        if len(f) >= 5 and re.match(r"^-?\d+\.\d+$", f[0]):
            score = float(f[0])
    return out.stdout.strip(), score


rng = random.Random(SEED)
texts = load_plaintexts()
arms = ["R100", "biased", "xanchor"]
res = {a: [] for a in arms}
wall = {a: 0.0 for a in arms}
for t in range(N):
    board = rand_board(rng)
    w, r = rand_wheels(rng), rand_pos(rng)
    pt = rng.choice([p for p in texts if len(p) >= L])
    off = rng.randrange(0, len(pt) - L + 1)
    pt = pt[off:off + L]
    g = rand_pos(rng)
    ct = enigma_ref.decrypt(pt, w, r, g, board)
    seed = rng.randrange(1 << 30)

    def ok(dec):
        return len(dec) == L and sum(x == y for x, y in zip(dec, pt)) >= L / 2

    t0 = time.time()
    dec, _ = run(ct, w, r, g, seed, ["-R", "100"])
    wall["R100"] += time.time() - t0
    res["R100"].append(ok(dec))
    t0 = time.time()
    dec, _ = run(ct, w, r, g, seed, ["-R", "100", "--biased-random", "1"])
    wall["biased"] += time.time() - t0
    res["biased"].append(ok(dec))
    t0 = time.time()
    best = ("", -1e9)
    for p in "ABCDEFGHIJKLMNOPQRSTUVWYZ":
        dec, s = run(ct, w, r, g, seed, ["-R", "4", "-s", "X" + p])
        if s is not None and s > best[1]:
            best = (dec, s)
    dec, s = run(ct, w, r, g, seed, ["-R", "4", "--no-plug", "X"])
    if s is not None and s > best[1]:
        best = (dec, s)
    wall["xanchor"] += time.time() - t0
    res["xanchor"].append(ok(best[0]))
print(f"L={L}, {N} paired trials, seed {SEED}, k4f10 -K --polish, rotor key "
      f"given")
for a in arms:
    print(f"  {a:8s} break50 {sum(res[a]):3d}/{N}   wall/trial "
          f"{wall[a] / N:.3f}s")
b = res["R100"]
for a in arms[1:]:
    v = res[a]
    only_v = sum(1 for x, y in zip(v, b) if x and not y)
    only_b = sum(1 for x, y in zip(v, b) if y and not x)
    union = sum(1 for x, y in zip(v, b) if x or y)
    print(f"  {a} vs R100: only-{a} {only_v}, only-R100 {only_b}, "
          f"union {union}/{N}")
