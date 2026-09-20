#!/usr/bin/env python3
"""Does a restart's STAGE-0 (k4) score predict whether its full k4f10 climb
reaches the truth?  The cheap check behind "successive halving over
restarts" -- ENHANCEMENTS.md 2b.

Two runs per trial with the same seed at -T 1, so --dump-all line i is
restart i in both: `-S k4` gives the stage-0 board and its k-score, `-S k4f10`
the final board.  The stage-0 board is a deterministic function of (key,
restart) -- the same fact --seed-dedup rests on -- and the first five trials
check it by comparing stage-0 duplicates against --seed-dedup's skip count
(a skip count one higher is a Bloom false positive, not a differing board).
Label = final board >= 50% correct.

    python3 eval/restart_halving_check.py RESTARTS TRIALS

About 10 s at -R 100 x 60 trials.  Read-only.
"""
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import (BIN, ROOT, load_plaintexts, rand_board,
                              rand_pos, rand_wheels)
from restart_ladder import core_table, pct_correct

L = 80
R = int(sys.argv[1]) if len(sys.argv) > 1 else 100
N = int(sys.argv[2]) if len(sys.argv) > 2 else 60
SEED = 23
ENV = dict(os.environ, ENIGMA_DATA="ngrams")


def run(ct, wheels, ring, start, seed, sched, extra=()):
    env = dict(ENV, ENIGMA_SEED=str(seed))
    model = [] if sched == "k4" else ["-f"]
    out = subprocess.run(
        [BIN, "-c", *model, "-S", sched, "-K", "-l", "wehrmacht", "-T", "1",
         "-u", "B", "-w", wheels, "-r", ring, "-g", start,
         "-R", str(R), "--dump-all", *extra],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT).stderr
    rows, skipped = [], None
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "dumpall" and len(f) >= 5:
            rows.append((float(f[4]), " ".join(sorted(f[5:]))))
        if line.startswith("Skipped"):
            skipped = int(f[1])
    return rows, skipped


rng = random.Random(SEED)
texts = load_plaintexts()
hits = []                     # percentile rank of each truth-reaching restart
broken = 0
kept = {1 / 3: 0, 1 / 2: 0, 2 / 3: 0}
check_ok = check_n = 0
for t in range(N):
    board = rand_board(rng)
    wheels, ring = rand_wheels(rng), rand_pos(rng)
    pt = rng.choice([p for p in texts if len(p) >= L])
    off = rng.randrange(0, len(pt) - L + 1)
    pt = pt[off:off + L]
    start = rand_pos(rng)
    ct = enigma_ref.decrypt(pt, wheels, ring, start, board)
    core = core_table(wheels, ring, start, L)
    ctn = [ord(c) - 65 for c in ct]
    ptn = [ord(c) - 65 for c in pt]
    seed = rng.randrange(1 << 30)
    s0, _ = run(ct, wheels, ring, start, seed, "k4")
    fin, _ = run(ct, wheels, ring, start, seed, "k4f10")
    if len(s0) != R or len(fin) != R:
        print("row count mismatch", len(s0), len(fin))
        continue
    if t < 5:
        _, sk = run(ct, wheels, ring, start, seed, "k4f10", ("--seed-dedup",))
        dup = R - len(set(b for _, b in s0))
        check_n += 1
        check_ok += (sk == dup)
    order = sorted(range(R), key=lambda i: -s0[i][0])
    rank = {i: k for k, i in enumerate(order)}
    good = [i for i in range(R)
            if pct_correct(fin[i][1], ctn, core, ptn) >= 50]
    if good:
        broken += 1
        for frac in kept:
            if any(rank[i] < frac * R for i in good):
                kept[frac] += 1
    for i in good:
        hits.append((rank[i] / R, pct_correct(s0[i][1], ctn, core, ptn)))
print(f"L={L} -R {R}, {N} trials: {broken} broke; {len(hits)} truth-reaching"
      f" restarts")
print(f"seed identity check (stage-0 dups == --seed-dedup skips): "
      f"{check_ok}/{check_n}")
print("percentile rank (0 = best stage-0 score) of the truth-reaching "
      "restarts:")
hs = sorted(h[0] for h in hits)
for q in (0.1, 0.25, 0.5, 0.75, 0.9):
    print(f"  {int(q * 100):3d}th pct: {hs[int(q * (len(hs) - 1))]:.2f}")
for frac in (1 / 3, 1 / 2, 2 / 3):
    print(f"  keep top {frac:.2f}: restarts kept "
          f"{sum(1 for h in hits if h[0] < frac)}/{len(hits)}, "
          f"trials still broken {kept[frac]}/{broken}")
print(f"stage-0 boards of truth-reaching restarts: mean %correct "
      f"{sum(h[1] for h in hits) / len(hits):.1f}")
