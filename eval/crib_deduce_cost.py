#!/usr/bin/env python3
"""What does the deduction itself cost? (cribs.md §4.1's cost table, the last
piece of it still unmeasured.)

    python3 eval/crib_deduce_cost.py

Every cost figure in cribs.md prices the CLIMB.  The deduction was assumed
negligible -- "one table lookup" -- but it runs 26 hypotheses at every viable
alignment before any climb starts, and a swept short crib has 70 to 90 of them.
That is up to ~2300 chained propagations per rotor setting, which is not
obviously negligible against a single climb.

METHOD.  A plain scan (no `-c`) over a fixed keyspace, so the only per-key work
is `setup_mapping`, the deduction, and -- for keys the crib does not reject --
one score.  Comparing arms against the no-crib baseline gives the deduction's
NET effect, which is what a user actually experiences: it costs propagations and
saves scores.

Reading the arms takes one asymmetry into account.  `crib_first_stop()` returns
at the FIRST surviving hypothesis, so

  * a crib that rejects nearly everything (16+ letters swept) pays the full
    alignments x 26 sweep on almost every key and scores almost nothing, so its
    wall time is essentially pure deduction -- that arm is what prices a single
    propagation;
  * a crib that rejects nothing (8 letters swept) exits early on almost every
    key and still pays for every score, so its overhead is a lower bound.

The two together bracket the cost, and the net column says whether it is worth
paying at all.
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")

TRUE = "AB CD EF GH IJ KL MN OP QR ST"
PT = ("DASOBERKOMMANDODERWEHRMACHTGIBTBEKANNTXAACHENXISTGERETTETXDURCHDEN"
      "EINSATZDERHILFSKRAEFTEXMELDUNGFOLGTXSPAETERXNEUNXUHRXABENDS")
ALIGN = re.compile(r"^Crib: (\d+) alignment.*rejected (\d+) of (\d+)", re.M)
KEYS = re.compile(r"Analysed (\d+) rotor")


def encipher(pt):
    p = subprocess.run([BIN, "-i", "-u", "B", "-w", "123", "-r", "AAA",
                        "-g", "QEW", "-s", TRUE],
                       input=pt, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, universal_newlines=True)
    return p.stdout.strip()


def timed(ct, extra, reps, lang):
    """Min wall time over reps, with the crib diagnostics parsed out."""
    cmd = [BIN, "-u", "B", "-w", "123", "-r", "AAA", "-g", "...",
           "-f", "-l", lang, "-T", "1"] + extra
    env = dict(os.environ, ENIGMA_SEED="0")
    best, aligns, rej, keys = None, 1, 0, 0
    for _ in range(reps):
        t0 = time.time()
        p = subprocess.run(cmd, input=ct, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, universal_newlines=True,
                           env=env)
        dt = time.time() - t0
        best = dt if best is None else min(best, dt)
        m = ALIGN.search(p.stderr)
        if m:
            aligns, rej = int(m.group(1)), int(m.group(2))
        k = KEYS.search(p.stderr)
        if k:
            keys = int(k.group(1))
    return best, aligns, rej, keys


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--lengths", type=int, nargs="+", default=[8, 12, 16, 20])
    args = ap.parse_args()
    if not os.path.exists(BIN):
        sys.exit("build the binary first")
    ct = encipher(PT)

    base, _, _, keys = timed(ct, [], args.reps, args.lang)
    print("Deduction cost, plain scan over %d keys, %d-letter message\n"
          "(min of %d reps, -T 1, no -c so no climb is involved)\n"
          % (keys, len(PT), args.reps))
    print("  no crib: %.2f s -- setup_mapping and one score per key\n" % base)
    print("  %-18s %8s %8s %9s %9s %11s"
          % ("crib", "aligns", "rejected", "wall", "net", "per hyp"))
    for n in args.lengths:
        crib = PT[3:3 + n]
        for label, extra in (("pinned", ["--crib", crib, "--crib-at", "3"]),
                             ("swept", ["--crib", crib])):
            t, a, r, k = timed(ct, extra, args.reps, args.lang)
            # Upper bound on propagations: every key sweeping every alignment.
            # Real runs exit at the first survivor, so this is exact only when
            # almost everything is rejected.
            hyps = float(k) * a * 26
            print("  %-2d letters %-7s %8d %7.1f%% %8.2fs %+8.2fs %9.0f ns"
                  % (n, label, a, 100.0 * r / max(k, 1), t, t - base,
                     1e9 * max(t - base, 0.0) / hyps))
    # The scan arm above prices the deduction against a SCORE. What a crib run
    # actually skips is a CLIMB, which costs three orders of magnitude more, so
    # the same deduction can lose on a scan and win overwhelmingly with -c.
    print("\nAgainst a climb (-c), which is what a crib run actually skips\n")
    cbase, _, _, k = timed(ct, ["-c"], args.reps, args.lang)
    print("  %-22s %9s %9s" % ("", "wall", "vs no crib"))
    print("  %-22s %8.2fs %9s" % ("no crib", cbase, "-"))
    for n in args.lengths:
        crib = PT[3:3 + n]
        t, a, r, k = timed(ct, ["-c", "--crib", crib], args.reps, args.lang)
        print("  %-2d letters swept      %8.2fs %8.2fx  (%.1f%% rejected)"
              % (n, t, cbase / t, 100.0 * r / max(k, 1)))

    print("\n  net      wall time against the no-crib baseline: the deduction")
    print("           costs propagations and saves the scores it rejects")
    print("  per hyp  net / (keys x alignments x 26). Exact only where almost")
    print("           everything is rejected -- elsewhere the early exit at the")
    print("           first surviving hypothesis makes it an overestimate of")
    print("           the true per-propagation cost")


if __name__ == "__main__":
    main()
