#!/usr/bin/env python3
"""The knobs around the corpus-counted table, on one trial set --
ENHANCEMENTS.md 2b.  eval/counted_table_ab.py swept one weight w over all
four orders and found a plateau from ~50 up; eval/corpus_only_table_ab.py
found that the w = infinity limit (no prose at all) reads 194 against
w = 1000's 219.  Ten arms on the same trials:

  base       stock wehrmacht tables
  w1000      stock + 1000x the fold's counts, all orders (the reference)
  w1e4       the same at 10 000     } where does the plateau turn down
  w1e5       the same at 100 000    } toward the corpus-only 194?
  q10        quad at 10, mono/bi/tri at 1000   } one weight for a sparse
  lo10       quad at 1000, mono/bi/tri at 10   } order and three dense ones?
  aw0.3      w1000 with -a order weights (1, .3, .09, .027), lambda pinned
  aw1.0      w1000 with order weights (1, 1, 1, 1), lambda pinned
  lam10      w1000 with the -f IC weight at 10 (the rule gives 20 at L=80)
  lam40      w1000 with it at 40

The lambda arms are fractions of the rule (lam10 = 0.125*L, half of it),
so they mean the same thing at every length; fix10 is the literal value 10,
for the other-length follow-up, and is left out of the default set with
lam5.  The order-weight arms pin ENIGMA_IC_BLEND=20 because any
coefficient override switches the lambda rule off (scoring.cc: an
overridden run keeps the language's flat lambda), and the point is to move
one knob at a time.

Rotor key given, 10-pair board hidden, -c -f -S k4f10 -K --polish, the
message's own fold held out as in counted_table_ab.py.  break50.

    python3 eval/counted_table_knobs.py TRIALS SEED [L] [R] [--arms=a,b,c]

About 0.2 s per trial per arm at L=80.  The lambda follow-up on a held-out
seed is --arms=w1000,lam5,lam10,lam40; the other-length one is
--arms=w1000,lam10,fix10 at L=60/100/167 with -R 8.
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
L = int(sys.argv[3]) if len(sys.argv) > 3 else 80
R = sys.argv[4] if len(sys.argv) > 4 else "100"
FOLDS = 5
ORD = {1: "monograms", 2: "bigrams", 3: "trigrams", 4: "quadgrams"}
LAMBDA = str(0.25 * L)
# arm -> ((w for mono/bi/tri, w for quad) or None for the stock tables, env)
ARMS = {
    "base":   (None, {}),
    "w1000":  ((1000.0, 1000.0), {}),
    "w1e4":   ((1e4, 1e4), {}),
    "w1e5":   ((1e5, 1e5), {}),
    "q10":    ((1000.0, 10.0), {}),
    "lo10":   ((10.0, 1000.0), {}),
    "aw0.3":  ((1000.0, 1000.0), {"ENIGMA_AW": "1,0.3,0.09,0.027",
                                  "ENIGMA_IC_BLEND": LAMBDA}),
    "aw1.0":  ((1000.0, 1000.0), {"ENIGMA_AW": "1,1,1,1",
                                  "ENIGMA_IC_BLEND": LAMBDA}),
    "lam5":   ((1000.0, 1000.0), {"ENIGMA_IC_BLEND": str(0.0625 * L)}),
    # lam10 is HALF THE RULE (0.125*L, i.e. 10 at L=80); fix10 is the
    # literal value 10 at any length, for the other-length follow-up.
    "lam10":  ((1000.0, 1000.0), {"ENIGMA_IC_BLEND": str(0.125 * L)}),
    "fix10":  ((1000.0, 1000.0), {"ENIGMA_IC_BLEND": "10"}),
    "lam40":  ((1000.0, 1000.0), {"ENIGMA_IC_BLEND": str(0.5 * L)}),
}
# --arms a,b,c restricts the run to those arms (the lambda follow-up on a
# held-out seed needs four of them, not ten); the default is every arm but
# lam5 and fix10, which were added for the follow-ups.
_sel = [a[7:] for a in sys.argv if a.startswith("--arms=")]
if _sel:
    ARMS = {a: ARMS[a] for a in _sel[0].split(",")}
else:
    ARMS = {a: v for a, v in ARMS.items() if a not in ("lam5", "fix10")}
TMP = tempfile.mkdtemp(prefix="counted_knobs_")


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


def build(fold, wlo, wq):
    out = os.path.join(TMP, f"f{fold}_lo{wlo:g}_q{wq:g}")
    os.makedirs(out, exist_ok=True)
    train = [m for i, m in enumerate(msgs) if fold_of[i] != fold]
    for n, name in ORD.items():
        w = wq if n == 4 else wlo
        c = Counter()
        for t in train:
            for i in range(len(t) - n + 1):
                c[t[i:i + n]] += 1
        ctot = sum(c.values())
        d, btot = base[n]
        mixed = dict(d)
        for g, k in c.items():
            mixed[g] = mixed.get(g, 0) + w * btot * k / ctot
        # The loader clamps a count at 2^32; log10(count / total) is scale
        # invariant, so scale the table to fit under it.
        s = min(1.0, 4.0e9 / max(mixed.values()))
        with open(os.path.join(out, f"wehrmacht_{name}.txt"), "w") as f:
            for g, k in sorted(mixed.items(), key=lambda kv: -kv[1]):
                k = int(round(k * s))
                if k > 0:
                    f.write(f"{g} {k}\n")
    return out


t0 = time.time()
tables = {}
for ws, _ in ARMS.values():
    if ws is not None:
        for f in range(FOLDS):
            if (f, ws) not in tables:
                tables[(f, ws)] = build(f, *ws)
print(f"tables built in {time.time() - t0:.0f}s under {TMP}")


def run(ct, wh, r, g, seed, datadir, extra_env):
    env = dict(os.environ, ENIGMA_SEED=str(seed), ENIGMA_DATA=datadir,
               **extra_env)
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

    for a, (ws, env) in ARMS.items():
        d = "ngrams" if ws is None else tables[(fold_of[mi], ws)]
        res[a].append(ok(run(ct, wh, r, g, seed, d, env)))

print(f"L={L} -R {R}, {N} paired trials, seed {SEED}, clean messages, "
      f"5-fold tables, lambda rule {LAMBDA} at this length")
for a in ARMS:
    print(f"  {a:7s} break50 {sum(res[a])}/{N}")
for ref in ("base", "w1000"):
    if ref not in res:
        continue
    b = res[ref]
    for a in ARMS:
        if a == ref:
            continue
        v = res[a]
        ov = sum(1 for x, y in zip(v, b) if x and not y)
        ob = sum(1 for x, y in zip(v, b) if y and not x)
        print(f"  {a:7s} vs {ref:6s} only-{a:7s} {ov:3d}  only-{ref:6s} "
              f"{ob:3d}")
