#!/usr/bin/env python3
"""What decides an L=80 trial: the truth's own score against the IMPOSTOR
FLOOR -- ENHANCEMENTS.md 2b.

Rotor key given, 10-pair board hidden, the recommended recipe.  Per trial the
floor is the best CONVERGED board decrypting under 50% of the letters right,
and the truth is the true plaintext scored as itself under `-f -l wehrmacht`.
Then, from the other side, every 80-letter window of the corpus is scored as
itself and split by whether its source message is flagged garbled, since a
transcription garble is a window no method can win.

    python3 eval/impostor_floor_probe.py RESTARTS TRIALS

Seconds at -R 300 x 30 trials.  Read-only.
"""
import os
import random
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref
from joint_score_gain import (BIN, HERE, ROOT, load_plaintexts, rand_board,
                              rand_pos, rand_wheels)
from restart_ladder import core_table, pct_correct

L = 80
R = int(sys.argv[1]) if len(sys.argv) > 1 else 300
N = int(sys.argv[2]) if len(sys.argv) > 2 else 30
SEED = 11
ENV = dict(os.environ, ENIGMA_DATA="ngrams")
RECIPE = ["-c", "-f", "-S", "k4f10", "-K", "--polish", "-l", "wehrmacht"]


def climb(ct, wheels, ring, start, seed):
    """Every converged board of a -R run, best score per board."""
    env = dict(ENV, ENIGMA_SEED=str(seed))
    out = subprocess.run(
        [BIN, *RECIPE, "-u", "B", "-w", wheels, "-r", ring, "-g", start,
         "-R", str(R), "--dump-all"],
        input=ct, capture_output=True, text=True, env=env, cwd=ROOT).stderr
    dump = {}
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "dumpall" and len(f) >= 5:
            b = " ".join(sorted(f[5:]))
            s = float(f[4])
            if b not in dump or s > dump[b]:
                dump[b] = s
    return dump


def score_text(pt):
    """The per-letter -f score of a text, read off a no-search decrypt."""
    ct = enigma_ref.decrypt(pt, "123", "AAA", "AAA", "")
    out = subprocess.run(
        [BIN, "-f", "-l", "wehrmacht", "-u", "B", "-w", "123", "-r", "AAA",
         "-g", "AAA"],
        input=ct, capture_output=True, text=True, env=ENV, cwd=ROOT).stderr
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 4 and f[0].lstrip("-").replace(".", "").isdigit():
            return float(f[0])
    return None


def trials():
    rng = random.Random(SEED)
    texts = load_plaintexts()
    print(f"== {N} trials, L={L}, -R {R}: truth vs impostor floor ==")
    rows = []
    for _ in range(N):
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
        dump = climb(ct, wheels, ring, start, rng.randrange(1 << 30))
        st = score_text(pt)
        floor, good = None, None
        for b, s in sorted(dump.items(), key=lambda kv: -kv[1]):
            pc = pct_correct(b, ctn, core, ptn)
            if pc < 50 and floor is None:
                floor = s
            if pc >= 50 and good is None:
                good = s
            if floor is not None and good is not None:
                break
        broke = good is not None and (floor is None or good >= floor)
        rows.append((st, floor, good, broke))
        fl = "-" if floor is None else f"{floor:.3f}"
        mg = "-" if floor is None else f"{st - floor:+.3f}"
        print(f"  {'OK ' if broke else 'IMP'} truth {st:.3f}  floor {fl:>7}"
              f"  margin {mg:>7}  {pt[:40]}")
    fl = [r[1] for r in rows if r[1] is not None]
    print(f"floor: mean {sum(fl) / len(fl):.3f}  max {max(fl):.3f}  "
          f"min {min(fl):.3f}   truth: mean {sum(r[0] for r in rows) / N:.3f}")
    below = sum(1 for r in rows if r[1] is not None and r[0] < r[1])
    print(f"truth below the trial's own floor: {below} of {N}; "
          f"broke {sum(1 for r in rows if r[3])} of {N}")


def windows():
    text = open(os.path.join(HERE, "enigma-army-messages-1941.txt"),
                encoding="utf-8").read()
    rows = []
    for block in re.split(r"(?=### Message No\.)", text)[1:]:
        hdr = block.splitlines()[0][4:60]
        m = re.search(r"DECRYPT:\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
        if not m:
            continue
        s = "".join(m.group(1).split()).replace("-", "")
        if len(s) < L:
            continue
        n = re.search(r"NOTES:\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
        notes = n.group(1) if n else ""
        flag = (re.search(r"garbl|corrupt|partial|poor|does not decrypt|"
                          r"heavily", notes, re.I) is not None
                or "-" in m.group(1))
        for off in range(0, len(s) - L + 1, 40):
            rows.append((score_text(s[off:off + L]), flag, hdr,
                         s[off:off + L]))
    rows.sort()
    print(f"\n== every {L}-letter window of the corpus (step 40), scored as "
          f"itself ==")
    mean = sum(r[0] for r in rows) / len(rows)
    print(f"{len(rows)} windows, mean {mean:.3f}; "
          f"{sum(1 for r in rows if r[1])} from garble-flagged messages")
    for thr in (-9.4, -9.2, -9.0):
        sub = [r for r in rows if r[0] < thr]
        print(f"  below {thr}: {len(sub)}, of which garble-flagged "
              f"{sum(1 for r in sub if r[1])}")
    print("lowest windows (G = garble-flagged source):")
    for r in rows[:14]:
        print(f"  {r[0]:.3f} {'G' if r[1] else ' '} {r[3]}")


trials()
windows()
