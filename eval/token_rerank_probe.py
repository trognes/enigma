#!/usr/bin/env python3
"""A sparse corpus-5-gram bonus as a RE-RANKER over the search's converged
boards, leave-one-message-out -- ENHANCEMENTS.md 2b.

L=80, rotor key given, 10-pair board hidden, -R 100, k4f10 -K.  The table
holds every 5-gram present in at least two OTHER corpus messages (support is
counted in messages, so a doubled name inside one message is not evidence).
Each of the top-K converged boards is decrypted in Python off the rotor core,
its matched 5-grams counted, and the winner re-picked by
`score + w * matches / L`.  Baseline = the top converged board by score, so
neither arm has --polish.  Ceiling = any of the top-K boards clearing 50%,
which is what bounds every re-ranker.

    python3 eval/token_rerank_probe.py TRIALS SEED

About 0.13 s per trial.  Read-only.
"""
import os
import random
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import (BIN, ROOT, load_plaintexts, rand_board,
                              rand_pos, rand_wheels)
from restart_ladder import core_table

L, R, K = 80, 100, 32
N = int(sys.argv[1]) if len(sys.argv) > 1 else 200
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 73
W = [0.5, 1, 2, 4, 8]
ENV = dict(os.environ, ENIGMA_DATA="ngrams")
texts = [p for p in load_plaintexts() if len(p) >= L]
msg_grams = [set(p[i:i + 5] for i in range(len(p) - 4)) for p in texts]


def table(exclude):
    c = Counter()
    for j, g in enumerate(msg_grams):
        if j != exclude:
            c.update(g)
    return {g for g, n in c.items() if n >= 2}


def dump(ct, w, r, g, seed):
    env = dict(ENV, ENIGMA_SEED=str(seed))
    out = subprocess.run(
        [BIN, "-c", "-f", "-S", "k4f10", "-K", "-l", "wehrmacht", "-T", "1",
         "-u", "B", "-w", w, "-r", r, "-g", g, "-R", str(R), "--dump-all"],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT).stderr
    d = {}
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "dumpall":
            b = " ".join(sorted(f[5:]))
            s = float(f[4])
            if b not in d or s > d[b]:
                d[b] = s
    return sorted(d.items(), key=lambda kv: -kv[1])[:K]


rng = random.Random(SEED)
base = ceil = 0
won = {w: 0 for w in W}
disc = {w: [0, 0] for w in W}
truth_bonus = imp_bonus = 0.0
nimp = 0
for t in range(N):
    board = rand_board(rng)
    wh, ri = rand_wheels(rng), rand_pos(rng)
    mi = rng.randrange(len(texts))
    pt = texts[mi]
    off = rng.randrange(0, len(pt) - L + 1)
    pt = pt[off:off + L]
    st = rand_pos(rng)
    ct = enigma_ref.decrypt(pt, wh, ri, st, board)
    seed = rng.randrange(1 << 30)
    tab = table(mi)
    core = core_table(wh, ri, st, L)
    ctn = [ord(c) - 65 for c in ct]
    ptn = [ord(c) - 65 for c in pt]
    cands = []
    for b, s in dump(ct, wh, ri, st, seed):
        sp = enigma_ref._plugboard(b)
        dec = [sp[core[i][sp[c]]] for i, c in enumerate(ctn)]
        pc = 100.0 * sum(1 for i in range(L) if dec[i] == ptn[i]) / L
        ds = "".join(chr(65 + x) for x in dec)
        bonus = sum(1 for i in range(L - 4) if ds[i:i + 5] in tab)
        cands.append((s, pc, bonus))
    if not cands:
        continue
    b_ok = cands[0][1] >= 50
    base += b_ok
    ceil += any(c[1] >= 50 for c in cands)
    truth_bonus += sum(1 for i in range(L - 4) if pt[i:i + 5] in tab)
    if not b_ok:
        imp_bonus += cands[0][2]
        nimp += 1
    for w in W:
        top = max(cands, key=lambda c: c[0] + w * c[2] / L)
        ok = top[1] >= 50
        won[w] += ok
        if ok and not b_ok:
            disc[w][0] += 1
        if b_ok and not ok:
            disc[w][1] += 1
print(f"L={L} -R {R}, {N} trials, seed {SEED}, top-{K} converged boards, "
      f"leave-one-message-out 5-gram table")
print(f"  baseline (top-1 by score) break50 {base}/{N};  ceiling (any of "
      f"top-{K}) {ceil}/{N}")
print(f"  matched 5-grams per window: truth {truth_bonus / N:.1f}, impostor "
      f"winners {imp_bonus / max(1, nimp):.1f}")
for w in W:
    print(f"  w={w:<4} break50 {won[w]:3d}/{N}   only-rerank {disc[w][0]:2d}"
          f"  only-base {disc[w][1]:2d}")
