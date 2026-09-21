#!/usr/bin/env python3
"""Vocabulary-seeded climbs against plain restarts at L=80 -- MEASURED DOWN,
ENHANCEMENTS.md 2b.

Rotor key given, 10-pair board hidden, the recommended recipe.  The seeded
arms sweep `eval/vocab-generic.cribs` -- fourteen X-fenced telegraphic tokens
chosen from convention, none taken from the corpus -- with --crib-list, IC-rank
the surviving hypotheses and climb the top K from the deduced pins (-R 0, as
--self-crib-seeds recommends).  Paired: every arm sees the same trials.  Wall
time includes process startup in every arm alike, so compare arms, not
absolutes.  --present splits the vocK10-vs-R100 outcome by whether any token
is actually in the plaintext.

    python3 eval/vocab_seed_ab.py TRIALS SEED [--present]

About 1.5 s per trial over all six arms.
"""
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import (BIN, HERE, ROOT, load_plaintexts, rand_board,
                              rand_pos, rand_wheels)

L = 80
N = int(sys.argv[1]) if len(sys.argv) > 1 else 70
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 41
PRESENT = "--present" in sys.argv
VOCAB = os.path.join(HERE, "vocab-generic.cribs")
TOKENS = [w.split()[0] for w in open(VOCAB, encoding="utf-8")
          if w.strip() and not w.startswith("#")]
ENV = dict(os.environ, ENIGMA_DATA="ngrams")
SEEDED = ["-R", "0", "--crib-list", VOCAB, "--crib-seeds"]
ARMS = {
    "R100": ["-R", "100"],
    "R300": ["-R", "300"],
    "R400": ["-R", "400"],
    "vocK10": SEEDED + ["10"],
    "vocK5": SEEDED + ["5"],
    "vocK3": SEEDED + ["3"],
}
if PRESENT:
    ARMS = {a: ARMS[a] for a in ("R100", "vocK10")}


def run(ct, w, r, g, seed, extra):
    env = dict(ENV, ENIGMA_SEED=str(seed))
    t0 = time.time()
    out = subprocess.run(
        [BIN, "-c", "-f", "-S", "k4f10", "-K", "--polish", "-l", "wehrmacht",
         "-T", "1", "-u", "B", "-w", w, "-r", r, "-g", g, *extra],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT)
    return out.stdout.strip(), time.time() - t0


rng = random.Random(SEED)
texts = load_plaintexts()
res = {a: [] for a in ARMS}
wall = {a: 0.0 for a in ARMS}
present = []
for t in range(N):
    board = rand_board(rng)
    w, r = rand_wheels(rng), rand_pos(rng)
    pt = rng.choice([p for p in texts if len(p) >= L])
    off = rng.randrange(0, len(pt) - L + 1)
    pt = pt[off:off + L]
    g = rand_pos(rng)
    ct = enigma_ref.decrypt(pt, w, r, g, board)
    seed = rng.randrange(1 << 30)
    present.append(any(v in pt for v in TOKENS))
    for a, extra in ARMS.items():
        dec, dt = run(ct, w, r, g, seed, extra)
        wall[a] += dt
        ok = len(dec) == L and sum(x == y for x, y in zip(dec, pt)) >= L / 2
        res[a].append(ok)
print(f"L={L}, {N} paired trials, seed {SEED}, k4f10 -K --polish, rotor key "
      f"given, {len(TOKENS)} vocabulary cribs")
for a in ARMS:
    print(f"  {a:8s} break50 {sum(res[a]):3d}/{N}   wall/trial "
          f"{wall[a] / N:.3f}s")
b = res["R100"]
for a in ARMS:
    if a == "R100":
        continue
    v = res[a]
    only_v = sum(1 for x, y in zip(v, b) if x and not y)
    only_b = sum(1 for x, y in zip(v, b) if y and not x)
    union = sum(1 for x, y in zip(v, b) if x or y)
    print(f"  {a} vs R100: only-{a} {only_v}, only-R100 {only_b}, "
          f"union {union}/{N}")
if PRESENT:
    npres = sum(present)
    for lab, want in (("PRESENT", True), ("ABSENT", False)):
        n = sum(1 for p in present if p == want)
        pv = sum(1 for p, x in zip(present, res["vocK10"]) if p == want and x)
        pb = sum(1 for p, x in zip(present, b) if p == want and x)
        print(f"  token {lab} ({n} trials): vocK10 {pv}, R100 {pb}")
    print(f"  trials with a token present: {npres}/{N}")
