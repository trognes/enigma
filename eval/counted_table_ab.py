#!/usr/bin/env python3
"""A corpus-COUNTED supplement to the wehrmacht tables, held out by message,
against the stock tables -- ENHANCEMENTS.md 2b.  The first positive lever
at L=80 below -R 100.

The wehrmacht tables are prose German bent to Appendix C's marginals; no
message plaintext went into them, so they cannot know XSIGX or XLKWX.  This
mixes each order's stock table with the n-gram counts of the corpus messages
in the TRAINING folds, at weight w of the table's mass:

    mixed[g] = base[g] + w * base_total * corpus_count[g] / corpus_total

and scores a trial's message with the table built WITHOUT its fold, so no
message ever contributes to its own table.  Five folds by message index.  A
trial is a random 80-letter window, a random rotor key and a fresh 10-pair
board; both arms see the identical trial; break50 on the reported plaintext.

    python3 eval/counted_table_ab.py TRIALS SEED [L] [R] [W,W,...] [--garble]

--garble keeps the garble-flagged corpus messages in both training and test;
the default drops them.  Tables land under $TMPDIR (a few MB per fold and
weight).  About 0.2 s per trial per arm.
"""
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import (BIN, HERE, ROOT, rand_board, rand_pos,
                              rand_wheels)

N, SEED = int(sys.argv[1]), int(sys.argv[2])
args = [a for a in sys.argv[3:] if not a.startswith("--")]
L = int(args[0]) if len(args) > 0 else 80
R = args[1] if len(args) > 1 else "100"
W = [float(x) for x in args[2].split(",")] if len(args) > 2 else [0.2]
GARBLE = "--garble" in sys.argv
FOLDS = 5
ORD = {1: "monograms", 2: "bigrams", 3: "trigrams", 4: "quadgrams"}
TMP = tempfile.mkdtemp(prefix="counted_table_")


def corpus():
    text = open(os.path.join(HERE, "enigma-army-messages-1941.txt"),
                encoding="utf-8").read()
    out = []
    for block in re.split(r"(?=### Message No\.)", text)[1:]:
        m = re.search(r"DECRYPT:\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
        if not m:
            continue
        s = "".join(m.group(1).split()).replace("-", "")
        n = re.search(r"NOTES:\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
        notes = n.group(1) if n else ""
        flag = (re.search(r"garbl|corrupt|partial|poor|does not decrypt|"
                          r"heavily", notes, re.I) is not None
                or "-" in m.group(1))
        if len(s) >= L and (GARBLE or not flag):
            out.append(s)
    return out


msgs = corpus()
fold_of = {i: i % FOLDS for i in range(len(msgs))}
print(f"{len(msgs)} {'messages incl. garbled' if GARBLE else 'clean messages'}"
      f" >= {L}, {FOLDS} folds")

base = {}
for n, name in ORD.items():
    d = {}
    for line in open(os.path.join(ROOT, "ngrams", f"wehrmacht_{name}.txt")):
        g, c = line.split()
        d[g] = int(c)
    base[n] = (d, sum(d.values()))


def build(fold, w):
    out = os.path.join(TMP, f"f{fold}_w{w}")
    os.makedirs(out, exist_ok=True)
    train = [m for i, m in enumerate(msgs) if fold_of[i] != fold]
    for n, name in ORD.items():
        c = Counter()
        for t in train:
            for i in range(len(t) - n + 1):
                c[t[i:i + n]] += 1
        ctot = sum(c.values())
        d, btot = base[n]
        mixed = dict(d)
        for g, k in c.items():
            mixed[g] = mixed.get(g, 0) + int(round(w * btot * k / ctot))
        with open(os.path.join(out, f"wehrmacht_{name}.txt"), "w") as f:
            for g, k in sorted(mixed.items(), key=lambda kv: -kv[1]):
                f.write(f"{g} {k}\n")
    return out


t0 = time.time()
dirs = {(f, w): build(f, w) for f in range(FOLDS) for w in W}
print(f"tables built in {time.time() - t0:.0f}s under {TMP}")


def run(ct, wh, r, g, seed, datadir):
    env = dict(os.environ, ENIGMA_SEED=str(seed), ENIGMA_DATA=datadir)
    out = subprocess.run(
        [BIN, "-c", "-f", "-S", "k4f10", "-K", "--polish", "-l", "wehrmacht",
         "-T", "1", "-u", "B", "-w", wh, "-r", r, "-g", g, "-R", R],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT)
    return out.stdout.strip()


rng = random.Random(SEED)
arms = ["base"] + [f"w{w}" for w in W]
res = {a: [] for a in arms}
for t in range(N):
    board = rand_board(rng)
    wh, r = rand_wheels(rng), rand_pos(rng)
    mi = rng.randrange(len(msgs))
    pt = msgs[mi]
    off = rng.randrange(0, len(pt) - L + 1)
    pt = pt[off:off + L]
    g = rand_pos(rng)
    ct = enigma_ref.decrypt(pt, wh, r, g, board)
    seed = rng.randrange(1 << 30)

    def ok(dec):
        return len(dec) == L and sum(x == y for x, y in zip(dec, pt)) >= L / 2

    res["base"].append(ok(run(ct, wh, r, g, seed, "ngrams")))
    for w in W:
        res[f"w{w}"].append(ok(run(ct, wh, r, g, seed,
                                   dirs[(fold_of[mi], w)])))
pool = "ALL messages incl. garbled" if GARBLE else "clean messages"
print(f"L={L} -R {R}, {N} paired trials, seed {SEED}, {pool}, 5-fold counted "
      f"supplement")
b = res["base"]
print(f"  base    break50 {sum(b)}/{N}")
for w in W:
    v = res[f"w{w}"]
    ov = sum(1 for x, y in zip(v, b) if x and not y)
    ob = sum(1 for x, y in zip(v, b) if y and not x)
    print(f"  w={w:<5} break50 {sum(v)}/{N}   only-w {ov}  only-base {ob}")
