#!/usr/bin/env python3
"""Does ordering a crib library cheapest-first find the message sooner?
(cribs.md 5 step 5, re-measured against MEASURED cost.)

    python3 eval/crib_order_probe.py --trials 6

WHY THIS IS RE-MEASURED.  cribs.md 5 step 5 already compared orderings and
concluded that ordering by anything except evidence of recurrence loses badly
(median time-to-first-hit 141 h against 6.7 h).  That measurement priced each
crib with build_cribs.py's COST model, which charges by LENGTH on the
assumption that sweep cost is roughly flat across lengths -- 4.1's table gives
100-117 s for every row.  4.2b showed the model has the wrong unit: under `-c`
a surviving key is climbed once per surviving HYPOTHESIS, and measured on one
message a 20-letter crib swept in 0.15 s where a 10-letter one took 13.65 s.

That is a ~90x spread the old model did not have, against a ~26x spread in how
often a crib is present (4.2: 93% of messages carry an 8-letter crib, 3% a
20-letter one).  When the cost spread is the larger of the two, cheapest-first
can win -- so the conclusion has to be re-derived rather than inherited.

WHAT IS MEASURED.  Time to the FIRST crib that recovers the message, which is
what a human watching progress lines actually waits for (there is deliberately
no automatic early exit -- cribs.md 6.7).  Each crib is run alone and timed, so
an ordering's time-to-hit is the sum of the runs before its first hit plus that
hit.  Measuring per crib once and summing is exact here and far cheaper than
running the whole library once per ordering.

THE ORDERING IS NOT ALLOWED TO CHEAT.  The tool orders by its own sampled
ESTIMATE, made before any sweep runs; sorting by the measured wall time would
give it knowledge it does not have.  So the cost order is read from the tool's
own table (`--crib-order cost`), and only the timing comes from this harness.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from crib_menu import corpus, random_key                  # noqa: E402
from ring_stride_geometry_probe import txt, plugboard, crypt   # noqa: E402

BIN = os.path.join(HERE, os.pardir, "enigma")
ROW = re.compile(r"^\s+(\d+)\s+([A-Z]+)\s")


def load_library(path):
    cribs = []
    seen = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if (not line) or line.startswith("#"):
                continue
            c = line.split()[0].upper()
            if c.isalpha() and (len(c) >= 2) and (c not in seen):
                seen.add(c)
                cribs.append(c)
    return cribs


def tool_order(ct, key, lib, args):
    """The order the tool itself would run the library in, from its own table.

    Read from the binary rather than recomputed here, so the comparison uses the
    estimate the tool actually has rather than hindsight.
    """
    wheels, refl, ring, start = key
    cmd = [BIN, "-u", refl, "-w", "".join(str(w + 1) for w in wheels),
           "-r", txt(ring), "-g", args.start, "-c", "-f", "-l", args.lang,
           "-T", str(args.threads), "--crib-list", lib, "--crib-order", "cost",
           "--crib-max-hyps", "1e-9"]      # estimate every crib, sweep none
    p = subprocess.run(cmd, input=ct, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, universal_newlines=True,
                       env=dict(os.environ, ENIGMA_SEED="0"))
    order = []
    for line in p.stderr.splitlines():
        m = ROW.match(line)
        if m:
            order.append(m.group(2))
    return order


def run_one(ct, key, crib, pt, args):
    """Run one crib; return (wall seconds, did it recover the message)."""
    wheels, refl, ring, start = key
    cmd = [BIN, "-u", refl, "-w", "".join(str(w + 1) for w in wheels),
           "-r", txt(ring), "-g", args.start, "-c", "-f", "-l", args.lang,
           "-T", str(args.threads), "--crib", crib]
    t0 = time.time()
    p = subprocess.run(cmd, input=ct, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, universal_newlines=True,
                       env=dict(os.environ, ENIGMA_SEED="0"))
    dt = time.time() - t0
    got = re.sub(r"[^A-Z]", "", p.stdout.strip().upper())
    if len(got) != len(pt):
        return dt, False
    pct = 100.0 * sum(a == b for a, b in zip(pt, got)) / len(pt)
    return dt, pct >= args.hit


def time_to_hit(order, timing):
    """Seconds until the first crib in `order` that recovers, or None."""
    total = 0.0
    for c in order:
        dt, ok = timing[c]
        total += dt
        if ok:
            return total
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=6)
    ap.add_argument("--library",
                    default=os.path.join(HERE, os.pardir, "cribs",
                                         "wehrmacht.cribs"))
    ap.add_argument("--length", type=int, default=120, help="message letters")
    ap.add_argument("--start", default="D..",
                    help="-g pattern: the keyspace swept per crib")
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--hit", type=float, default=95.0,
                    help="%%-correct counted as recovering the message")
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    lib = load_library(args.library)
    texts = [t for t in corpus() if len(t) >= args.length]
    if not texts:
        sys.exit("no corpus text of at least %d letters" % args.length)
    rng = random.Random(args.seed)

    print("Time to the first crib that recovers the message\n"
          "(%d cribs, %d-letter messages, %d plugs hidden, -g %s, -T %d,\n"
          "hit = %.0f%% of letters correct)\n"
          % (len(lib), args.length, args.plugs, args.start, args.threads,
             args.hit))
    print("  %-6s %10s %10s %10s   %s"
          % ("trial", "file", "cost", "speedup", "first hit (cost order)"))

    wins = fileT = costT = 0.0
    rows = 0
    for t in range(args.trials):
        src = rng.choice(texts)
        off = rng.randrange(len(src) - args.length + 1)
        pt = src[off:off + args.length]
        key = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                         args.plugs)
        wheels, refl, ring, start = key
        ct = crypt(pt, wheels, refl, ring, start, plug)

        # The swept keyspace pins the true start's first letter, so the
        # -g pattern has to be rebuilt from whichever key was drawn.
        args.start = txt(start)[0] + ".."

        order = tool_order(ct, key, args.library, args)
        usable = [c for c in lib if c in set(order)]
        timing = {}
        for c in usable:
            timing[c] = run_one(ct, key, c, pt, args)

        f = time_to_hit([c for c in lib if c in timing], timing)
        g = time_to_hit([c for c in order if c in timing], timing)
        if (f is None) or (g is None):
            print("  %-6d %10s %10s %10s   (no crib in the library recovers it)"
                  % (t + 1, "-", "-", "-"))
            continue
        first = next(c for c in order if timing[c][1])
        rows += 1
        fileT += f
        costT += g
        wins += 1 if g < f else 0
        print("  %-6d %9.1fs %9.1fs %9.2fx   %s"
              % (t + 1, f, g, f / g if g > 0 else float("inf"), first))

    if rows:
        print("\n  mean time to hit: file order %.1fs, cost order %.1fs "
              "-- %.2fx\n  cost order was faster in %d of %d trials"
              % (fileT / rows, costT / rows,
                 fileT / costT if costT > 0 else float("inf"),
                 int(wins), rows))


if __name__ == "__main__":
    main()
