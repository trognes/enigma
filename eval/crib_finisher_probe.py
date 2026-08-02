#!/usr/bin/env python3
"""Do --polish and -J help a CRIB-SEEDED climb? (cribs.md 7b and 12 step 5.)

    python3 eval/crib_finisher_probe.py --trials 60

TWO QUESTIONS LEFT OPEN WHEN THE SEEDING SHIPPED, both deferred deliberately
rather than forgotten.

  --polish  is a fixed-cost finisher that completes a near-solution board. A
            crib-seeded board is already near-solution, but from a different
            direction -- its plugs are arithmetic on the machine equation, not
            a climb that happened to get close -- so the finisher may have
            nothing left to add. cribs.md 12 step 5 records this as "an A/B to
            run once step 5 exists and there is something to measure it on".

  -J        is measured 1.8x cheaper on a seeded climb (7a), but COST ONLY.
            This is the known-few-plug regime where -J is documented to need a
            cap to win on quality, so the pairing worth testing is `-J --score
            f10` rather than -J alone -- which is exactly what this runs.

MATCHED COMPUTE IS NOT ASSUMED. Both the recovery and the plugboards scored are
reported per arm, so a cheaper arm that recovers as much is a win on both axes
and a dearer one has to earn its cost. The plugboard is hidden and the rotor
key given, which isolates what the finisher does to a seeded board from the
rotor search around it.
"""
import argparse
import os
import random
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from crib_menu import corpus, random_key, Menu               # noqa: E402
from ring_stride_geometry_probe import txt, plugboard, crypt  # noqa: E402

BIN = os.path.join(HERE, os.pardir, "enigma")
SCORED = re.compile(r"scored (\d+) plugboard")

ARMS = [
    ("baseline", []),
    ("--polish", ["--polish"]),
    ("-J", ["-J"]),
    ("-J --score f10", ["-J", "--score", "f10"]),
    ("-J --polish", ["-J", "--polish"]),
]


def run(ct, key, crib, at, arm, args):
    """Return (percent of letters recovered, plugboards scored)."""
    wheels, refl, ring, start = key
    cmd = [BIN, "-u", refl, "-w", "".join(str(w + 1) for w in wheels),
           "-r", txt(ring), "-g", txt(start), "-c", "-f", "-l", args.lang,
           "--crib", crib, "--crib-at", str(at + 1),   # --crib-at is 1-based
           "-T", str(args.threads)] + arm
    p = subprocess.run(cmd, input=ct, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True,
                       env=dict(os.environ, ENIGMA_SEED="0"))
    got = re.sub(r"[^A-Z]", "", p.stdout.strip().upper())
    m = SCORED.search(p.stderr)
    return got, int(m.group(1)) if m else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--crib", type=int, default=12, help="crib letters [12]")
    ap.add_argument("--length", type=int, default=90)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    texts = [t for t in corpus() if len(t) >= args.length]
    if not texts:
        sys.exit("no corpus text of at least %d letters" % args.length)
    rng = random.Random(args.seed)

    pct = {n: [] for n, _ in ARMS}
    boards = {n: 0 for n, _ in ARMS}
    exact = {n: 0 for n, _ in ARMS}
    done = 0
    for _ in range(args.trials):
        src = rng.choice(texts)
        off = rng.randrange(len(src) - args.length + 1)
        pt = src[off:off + args.length]
        at = rng.randrange(len(pt) - args.crib + 1)
        crib = pt[at:at + args.crib]
        key = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                         args.plugs)
        wheels, refl, ring, start = key
        ct = crypt(pt, wheels, refl, ring, start, plug)
        menu = Menu(crib, ct, at)
        if not menu.valid or menu.anchor() is None:
            continue                    # the crib cannot sit here at all
        done += 1
        for name, arm in ARMS:
            got, b = run(ct, key, crib, at, name and arm, args)
            hit = (100.0 * sum(a == c for a, c in zip(pt, got)) / len(pt)
                   if len(got) == len(pt) else 0.0)
            pct[name].append(hit)
            boards[name] += b
            exact[name] += 1 if hit == 100.0 else 0

    if not done:
        sys.exit("no usable trials")
    print("Finisher and climb rule on a CRIB-SEEDED board\n"
          "(%d trials, %d-letter messages, %d-letter crib pinned, %d cables\n"
          "hidden, rotor key given, -f -l %s)\n"
          % (done, args.length, args.crib, args.plugs, args.lang))
    print("  %-16s %10s %8s %14s" % ("arm", "mean %", "exact", "boards scored"))
    base = sum(pct["baseline"]) / done
    for name, _ in ARMS:
        m = sum(pct[name]) / done
        print("  %-16s %9.1f%% %6d/%-3d %14d%s"
              % (name, m, exact[name], done, boards[name],
                 "" if name == "baseline" else "  (%+.1fpp)" % (m - base)))


if __name__ == "__main__":
    main()
