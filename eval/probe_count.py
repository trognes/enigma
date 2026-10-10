#!/usr/bin/env python3
"""Where do a restart's plugboard probes go?  A probe count, not a timing.

    git apply eval/probe_count.patch && make && cp enigma /tmp/enigma-probe
    git checkout src && make
    python3 eval/probe_count.py --binary /tmp/enigma-probe

The patch adds counters to the climb's probe sites and prints them at exit.
It is kept as a patch rather than merged because the counters sit in the
hottest loop in the program and are wanted only for this question.

A PROBE is one scoring of one candidate plugboard move.  In the target stage
(the f10 of -S k4f10) each probe is a full decode of the message; in the
pre-pass (k4) it is an O(26) histogram update and costs ~10x less.  So the
target-stage columns are where the time is.

Categories, per stage:
  order scan   -J's ranking of all 325 moves at the start of a sweep
  IC order     -K's replacement for it, O(26) per move, not a decode
  sweep live   first-improvement probes up to the last accepted move
  sweep tail   probes after it: certifying that nothing improves (<= 325)
  failed probes before each accepted move -- the stale run a sweep had built
               up when it found the move; what a cap on that run would lose
  try_repair   the 2-plug re-pair barrier cross at each convergence
  steep live / steep tail   the same split under steepest ascent

Run two ways per length: at the TRUE key (one key, -R 100) and over 676
WRONG keys (-R 2), since a real sweep spends nearly all its time on wrong
keys and their climbs need not look like the true key's.
"""
import argparse
import os
import random
import re
import subprocess
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
CATS = ["order scan", "IC order", "sweep live", "sweep tail", "try_repair",
        "steep live", "steep tail"]
LINE = re.compile(r"^PROBES   (.{11}) +(\d+)", re.M)
GAP = re.compile(r"^GAP stage=(\w+) below=(\d+) (\d+)", re.M)
HEAD = re.compile(r"^PROBES stage=(\w+) sweeps=(\d+) accepted=(\d+) "
                  r"repair_hits=(\d+)", re.M)


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(binary, args, text):
    env = dict(os.environ, ENIGMA_SEED="0")
    p = subprocess.run([binary] + args, input=text, capture_output=True,
                       text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def parse(err, acc):
    for m in GAP.finditer(err):
        acc[m.group(1)][f"gap<{m.group(2)}"] += int(m.group(3))
    stage = None
    for line in err.splitlines():
        h = HEAD.match(line)
        if h:
            stage = h.group(1)
            acc[stage]["sweeps"] += int(h.group(2))
            acc[stage]["accepted"] += int(h.group(3))
            acc[stage]["repair hits"] += int(h.group(4))
            continue
        m = LINE.match(line)
        if m and stage:
            acc[stage][m.group(1).strip()] += int(m.group(2))


def report(title, acc):
    print(title)
    for stage in ("target", "prepass"):
        a = acc[stage]
        tot = sum(a[c] for c in CATS if c != "IC order")
        if tot == 0:
            continue
        sw = max(a["sweeps"], 1)
        print(f"  {stage:<8} probes {tot:>9}  sweeps {a['sweeps']:>7}"
              f"  accepted/sweep {a['accepted'] / sw:5.1f}"
              f"  repair hits {a['repair hits']}")
        for c in CATS:
            if a[c]:
                share = "" if c == "IC order" else f"{100 * a[c] / tot:5.1f}%"
                print(f"      {c:<11} {a[c]:>9} {share:>7}"
                      f"  {a[c] / sw:7.1f}/sweep")
        gaps = sorted((int(k[4:]), v) for k, v in a.items()
                      if k.startswith("gap<"))
        acc_n = sum(v for _, v in gaps)
        if acc_n:
            lo, cum = 0, 0
            print("      failed probes before each accepted move:")
            for hi, v in gaps:
                cum += v
                print(f"        {lo:>3}-{hi - 1:<3} {100 * v / acc_n:5.1f}%"
                      f"   (>= {hi}: {100 * (acc_n - cum) / acc_n:5.1f}%)")
                lo = hi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--lengths", type=int, nargs="+", default=[80, 107, 167])
    ap.add_argument("--fixtures", type=int, default=4)
    ap.add_argument("--rule", default="-K", choices=["-K", "-J"],
                    help="climb rule; write --rule=-J, since argparse "
                         "reads a bare -J as an option")
    ap.add_argument("--seed", type=int, default=5)
    args = ap.parse_args()
    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(
                         HERE, "enigma-army-messages-1941.txt")))
    recipe = ["-c", "-S", "k4f10", args.rule, "--polish", "-l", "wehrmacht",
              "-T", "1"]
    rng = random.Random(args.seed)
    print(f"recipe: {' '.join(recipe)}; {args.fixtures} fixtures per length;"
          " 10-pair board hidden\n")
    for L in args.lengths:
        true_acc = defaultdict(lambda: defaultdict(int))
        wrong_acc = defaultdict(lambda: defaultdict(int))
        for _ in range(args.fixtures):
            pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
            perm = rng.sample([1, 2, 3, 4, 5], 3)
            w = "".join(map(str, perm))
            r = "".join(rng.choice(LET) for _ in range(3))
            g = "".join(rng.choice(LET) for _ in range(3))
            ls = list(LET)
            rng.shuffle(ls)
            pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
            ct, _ = run(args.binary,
                        ["-u", "B", "-w", w, "-r", r, "-g", g, "-s", pb], pt)
            _, err = run(args.binary, ["-u", "B", "-w", w, "-r", r, "-g", g,
                                       "-R", "100"] + recipe, ct)
            parse(err, true_acc)
            while True:
                wp = rng.sample([1, 2, 3, 4, 5], 3)
                if wp != perm:
                    break
            _, err = run(args.binary,
                         ["-u", "B", "-w", "".join(map(str, wp)), "-r", r,
                          "-g", rng.choice(LET) + "..", "-R", "2"]
                         + recipe, ct)
            parse(err, wrong_acc)
        report(f"L={L}  TRUE key, -R 100", true_acc)
        report(f"L={L}  WRONG keys, 676 x -R 2", wrong_acc)
        print()


if __name__ == "__main__":
    main()
