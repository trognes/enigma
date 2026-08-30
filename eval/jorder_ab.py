#!/usr/bin/env python3
"""Is an IC move-order as good as a target-model move-order for -J?

    python3 eval/jorder_ab.py --length 167 --trials 400 --seed 4242

WHAT IS BEING ASKED.  `-J` visits the 325 plugboard toggles best-first, and
builds that order once per restart by scoring every move from the perturbed
starting board.  With a fused or quad target each of those 325 probes is a
FULL DECODE, and the scan is 20-23% of the climb's scored plugboards (measured
at -R 64: 20 800 of 91 451 at L=100, of 98 081 at L=167).  Its share grows with
message length, because it is linear in L where the histogram form is flat.

$ENIGMA_JORDER=ic ranks that scan by the index of coincidence instead,
computed from the co-occurrence table in O(26) per move.  Measured on one
167-letter fixture that takes the run from 7 535 643 plugboards scored to
5 546 450 -- a 26% cut in scoring work.

BUT IT IS A SEARCH CHANGE, NOT A SPEEDUP.  An IC order is not a target-model
order, so the climb visits moves differently and can converge somewhere else.
Cheaper per restart is worthless if it recovers less, and the two effects have
to be weighed against each other rather than one assumed.  Hence this harness:
paired trials, same excerpts, same keys, same boards, arms differing only in
the environment variable.

WHY WEHRMACHT.  The recommended recipe for real traffic is `-f -l wehrmacht
-S k4f10 -J`, and CLAUDE.md records that scoring results do not transfer
between prose and telegraphic German (the mono-vs-IC pre-pass ordering
reverses).  Measuring this on English prose would answer a question nobody
has.

MATCHED COMPUTE IS THE POINT.  The IC arm is cheaper per restart, so at equal
-R it is also getting less compute -- which would flatter the target arm.  The
harness therefore reports plugboards scored for both arms alongside recovery,
so a win can be read against what it cost.  If the IC arm is level on recovery
at ~26% less work, that is the win; if it is level at equal work, it is not.
"""

import argparse
import os
import random
import re
import subprocess
import sys
from math import sqrt

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(args, text, env_extra=None):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = os.path.join(HERE, os.pardir, "ngrams")
    if env_extra:
        env.update(env_extra)
    p = subprocess.run([ENIGMA] + [str(a) for a in args], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def scored(err):
    m = re.search(r"([0-9]+) plugboards", err)
    return int(m.group(1)) if m else 0


def pct_correct(a, b):
    if not a or len(a) != len(b):
        return 0.0
    return 100.0 * sum(1 for x, y in zip(a, b) if x == y) / len(b)


def mcnemar_z(only_a, only_b):
    n = only_a + only_b
    return (only_a - only_b) / sqrt(n) if n else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=400)
    ap.add_argument("--length", type=int, default=167)
    ap.add_argument("--restarts", type=int, default=8)
    ap.add_argument("--schedule", default="k4f10")
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=4242)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length
    rng = random.Random(args.seed)

    # arm label -> (mean %correct sum, exact count, plugboards scored)
    tot = {"target": [0.0, 0, 0], "ic": [0.0, 0, 0]}
    only = {"target": 0, "ic": 0}
    n = 0

    for _ in range(args.trials):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(args.plugs))

        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)
        if len(ct) != L:
            continue

        # Rotor key GIVEN, board hidden: this isolates the plugboard climb,
        # which is the only thing the move order touches.
        base = key + ["-c", "-J", "-S", args.schedule, "-f",
                      "-l", "wehrmacht", "-R", args.restarts, "-T", 1]
        got = {}
        for arm, env in (("target", None), ("ic", {"ENIGMA_JORDER": "ic"})):
            out, err = run(base, ct, env)
            got[arm] = out
            tot[arm][0] += pct_correct(out, pt)
            tot[arm][1] += 1 if out == pt else 0
            tot[arm][2] += scored(err)
        n += 1
        ea, eb = got["target"] == pt, got["ic"] == pt
        if ea and not eb:
            only["target"] += 1
        elif eb and not ea:
            only["ic"] += 1

    if n == 0:
        sys.exit("no usable trials")

    print(f"# L={L}, {n} paired trials, -f -l wehrmacht -c -J "
          f"-S {args.schedule} -R {args.restarts}, {args.plugs}-pair board "
          f"hidden, rotor key given, seed {args.seed}")
    print(f"{'arm':8s} {'mean %correct':>14s} {'exact':>10s} "
          f"{'plugboards scored':>19s}")
    for arm in ("target", "ic"):
        s, ex, sc = tot[arm]
        print(f"{arm:8s} {s / n:14.2f} {ex:6d}/{n:<4d} {sc:19,d}")
    dm = tot["ic"][0] / n - tot["target"][0] / n
    st, si = tot["target"][2], tot["ic"][2]
    print(f"\nic - target: {dm:+.2f}pp mean")
    z = mcnemar_z(only["target"], only["ic"])
    print(f"discordant exact: only target {only['target']}, only ic "
          f"{only['ic']}  (McNemar z = {z:+.2f})")
    if st:
        print(f"compute: ic is {100.0 * (si - st) / st:+.1f}% of the target "
              f"arm's plugboards scored")


if __name__ == "__main__":
    main()
