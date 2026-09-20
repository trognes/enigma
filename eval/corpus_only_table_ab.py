#!/usr/bin/env python3
"""Tables with NO prose German in them, against the corpus-counted w=1000
tables of eval/counted_table_ab.py -- ENHANCEMENTS.md 2b.

At w = 1000 the stock wehrmacht tables are 0.1% of the mass, and their one
remaining job is to ORDER the grams the training fold never saw: a gram
found only in prose sits below every corpus gram but above the unseen
floor, in prose order.  This asks whether that ordering is worth anything,
and whether Appendix C can stand in for prose as the out-of-corpus prior.
Five arms, the same trials in each:

  base      the stock wehrmacht tables (prose German bent to Appendix C);
  w1000     stock + 1000x the training fold's counts, the reference;
  corpus    the training fold's counts alone, all four orders, scaled so
            the unseen floor sits as far below a once-seen gram as in
            w1000 (the loader floors an unseen gram at ONE count, so a
            raw 2 900-letter table cannot tell a hapax from nothing);
  shallow   the same counts scaled by 10 only -- the floor one decade
            under a hapax -- to price the depth of the penalty on its own;
  appc      corpus counts plus Appendix C at its natural size: Fig 17
            (monograms per 20 000 letters) into the mono order and
            Fig 18 (the 400 trigrams, counts per ~20 000 letters) into
            the tri order; bi and quad are corpus only; matched floor.

Rotor key given, 10-pair board hidden, -c -f -S k4f10 -K --polish, the
message's own fold held out as in counted_table_ab.py.  break50.

    python3 eval/corpus_only_table_ab.py TRIALS SEED [L] [R]

About 0.2 s per trial per arm.
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
from build_telegraphic_ngrams import FIG17, TELE3
from joint_score_gain import (BIN, HERE, ROOT, rand_board, rand_pos,
                              rand_wheels)

N, SEED = int(sys.argv[1]), int(sys.argv[2])
L = int(sys.argv[3]) if len(sys.argv) > 3 else 80
R = sys.argv[4] if len(sys.argv) > 4 else "100"
FOLDS = 5
W_REF = 1000.0
ORD = {1: "monograms", 2: "bigrams", 3: "trigrams", 4: "quadgrams"}
ARMS = ["base", "w1000", "corpus", "shallow", "appc"]
TMP = tempfile.mkdtemp(prefix="corpus_only_")


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
        if len(s) >= L and not flag:
            out.append(s)
    return out


msgs = corpus()
fold_of = {i: i % FOLDS for i in range(len(msgs))}
print(f"{len(msgs)} clean messages >= {L}, {FOLDS} folds")

base = {}
for n, name in ORD.items():
    d = {}
    for line in open(os.path.join(ROOT, "ngrams", f"wehrmacht_{name}.txt")):
        g, c = line.split()
        d[g] = int(c)
    base[n] = (d, sum(d.values()))

# Appendix C at its natural size, in letters: Fig 17 is percentages, Fig 18
# is counts per ~20 000 letters, so both scale to that sample.
APPC = {1: {k: v * 200.0 for k, v in FIG17.items()}, 3: dict(TELE3)}


def write(path, table):
    s = min(1.0, 4.0e9 / max(table.values()))
    with open(path, "w") as f:
        for g, k in sorted(table.items(), key=lambda kv: -kv[1]):
            k = int(round(k * s))
            if k > 0:
                f.write(f"{g} {k}\n")


def build(fold, arm):
    out = os.path.join(TMP, f"f{fold}_{arm}")
    os.makedirs(out, exist_ok=True)
    train = [m for i, m in enumerate(msgs) if fold_of[i] != fold]
    for n, name in ORD.items():
        c = Counter()
        for t in train:
            for i in range(len(t) - n + 1):
                c[t[i:i + n]] += 1
        ctot = sum(c.values())
        d, btot = base[n]
        if arm == "w1000":
            mixed = dict(d)
            for g, k in c.items():
                mixed[g] = mixed.get(g, 0) + W_REF * btot * k / ctot
        else:
            counts = dict(c)
            if arm == "appc":
                for g, k in APPC.get(n, {}).items():
                    counts[g] = counts.get(g, 0) + k
            # In w1000 a corpus hapax is p ~ 1/ctot and the floor is
            # 1/((1 + W_REF) * btot); the same depth here needs every
            # count multiplied by (1 + W_REF) * btot / ctot.
            s = 10.0 if arm == "shallow" else (1 + W_REF) * btot / ctot
            mixed = {g: k * s for g, k in counts.items()}
        write(os.path.join(out, f"wehrmacht_{name}.txt"), mixed)
    return out


t0 = time.time()
dirs = {(f, a): build(f, a) for f in range(FOLDS) for a in ARMS if a != "base"}
print(f"tables built in {time.time() - t0:.0f}s under {TMP}")


def run(ct, wh, r, g, seed, datadir):
    env = dict(os.environ, ENIGMA_SEED=str(seed), ENIGMA_DATA=datadir)
    out = subprocess.run(
        [BIN, "-c", "-f", "-S", "k4f10", "-K", "--polish", "-l", "wehrmacht",
         "-T", "1", "-u", "B", "-w", wh, "-r", r, "-g", g, "-R", R],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT)
    return out.stdout.strip()


rng = random.Random(SEED)
res = {a: [] for a in ARMS}
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

    for a in ARMS:
        d = "ngrams" if a == "base" else dirs[(fold_of[mi], a)]
        res[a].append(ok(run(ct, wh, r, g, seed, d)))

print(f"L={L} -R {R}, {N} paired trials, seed {SEED}, clean messages, "
      f"5-fold tables")
for a in ARMS:
    print(f"  {a:8s} break50 {sum(res[a])}/{N}")
for ref in ("base", "w1000"):
    b = res[ref]
    for a in ARMS:
        if a == ref:
            continue
        v = res[a]
        ov = sum(1 for x, y in zip(v, b) if x and not y)
        ob = sum(1 for x, y in zip(v, b) if y and not x)
        print(f"  {a:8s} vs {ref:6s} only-{a} {ov:3d}  only-{ref} {ob:3d}")
