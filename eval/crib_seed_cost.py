#!/usr/bin/env python3
"""What does a climb cost when plugs are already known? (archived/cribs.md §7a's cost
table, which was arithmetic and never measured.)

    python3 eval/crib_seed_cost.py

§7a claims a climb seeded with 5 known plugs is 3.30x cheaper than one from an
empty board, and 10.74x at 8 plugs.  Those numbers are move-set combinatorics --
fixing a letter removes it from the 325-toggle scan, so the per-pass work falls
quadratically -- and nothing in them was ever run.  CLAUDE.md is explicit that
this is not good enough: judge cost on WALL TIME, not on a counter, because
`score_iter` misses per-symbol work outside the score loop.

MEASURED HERE, over a fixed keyspace so every arm solves exactly the same
problems:

  preset      known plugs given with -s, which pins them in plug_fixed exactly
              as the crib's deduced plugs are pinned -- so this isolates the
              CLIMB's cost from the deduction's
  climb rule  steepest ascent (default) against -J first-improvement, on the
              seeded climb, which archived/cribs.md §7b left unargued
  deduced     the same question on the REAL path -- plugs deduced by the crib
              rather than handed over -- since `-s` is only a proxy for it: its
              plugs are all correct, and it says nothing about the letters a
              deduction shows carry NO cable, which are pinned too

Each arm is the min of several repetitions.  The plugboard is otherwise hidden
and the rotor key wildcarded over its start positions, so the run is dominated
by climbs rather than by startup.
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

# A 10-cable board; the presets below are prefixes of it, so a preset is always
# a subset of the truth -- what a correct crib deduction hands over.
TRUE = "AB CD EF GH IJ KL MN OP QR ST"
PT = ("DASOBERKOMMANDODERWEHRMACHTGIBTBEKANNTXAACHENXISTGERETTETXDURCHDEN"
      "EINSATZDERHILFSKRAEFTEXMELDUNGFOLGTXSPAETERXNEUNXUHRXABENDS")
SCORED = re.compile(r"scored (\d+) plugboard")


def encipher(pt):
    p = subprocess.run([BIN, "-i", "-u", "B", "-w", "123", "-r", "AAA",
                        "-g", "QEW", "-s", TRUE],
                       input=pt, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, universal_newlines=True)
    return p.stdout.strip()


def timed(ct, extra, reps, lang, start="...", climb=True):
    """Min wall time over reps, plus the plugboards-scored counter."""
    cmd = [BIN, "-u", "B", "-w", "123", "-r", "AAA", "-g", start,
           "-f", "-l", lang, "-T", "1"] + (["-c"] if climb else []) + extra
    env = dict(os.environ, ENIGMA_SEED="0")
    best, scored = None, 0
    for _ in range(reps):
        t0 = time.time()
        p = subprocess.run(cmd, input=ct, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, universal_newlines=True,
                           env=env)
        dt = time.time() - t0
        best = dt if best is None else min(best, dt)
        m = SCORED.search(p.stderr)
        if m:
            scored = int(m.group(1))
    return best, scored


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--lang", default="wehrmacht")
    args = ap.parse_args()
    if not os.path.exists(BIN):
        sys.exit("build the binary first")
    ct = encipher(PT)
    presets = [(0, []), (5, ["-s", " ".join(TRUE.split()[:5])]),
               (8, ["-s", " ".join(TRUE.split()[:8])])]

    print("Climb cost with plugs already known (%d letters, 17576 keys, one\n"
          "deterministic climb each, min of %d reps, -T 1)\n"
          % (len(PT), args.reps))
    print("  %-8s %10s %8s %14s %10s"
          % ("preset", "wall", "speedup", "boards scored", "vs §7a"))
    base_t = base_s = None
    claim = {0: 1.00, 5: 3.30, 8: 10.74}
    for n, extra in presets:
        t, s = timed(ct, extra, args.reps, args.lang)
        if base_t is None:
            base_t, base_s = t, s
        print("  %-8d %9.2fs %7.2fx %14d %9s"
              % (n, t, base_t / t, s,
                 "%.2fx" % claim[n] if n else "-"))

    print("\nClimb rule on the seeded climb (5 plugs preset)\n")
    print("  %-18s %10s %14s" % ("rule", "wall", "boards scored"))
    five = presets[1][1]
    for label, extra in (("steepest (default)", five),
                         ("-J first-improve", five + ["-J"])):
        t, s = timed(ct, extra, args.reps, args.lang)
        print("  %-18s %9.2fs %14d" % (label, t, s))

    deduced(ct, args)


def deduced(ct, args):
    """The same question with the plugs REALLY deduced, not handed over.

    `-s` above is a proxy: its plugs are correct, come from the true board, and
    say nothing about letters that carry no cable.  A crib deduction differs on
    all three counts -- it also pins the letters it shows unplugged, and 25 of
    its 26 hypotheses pin plugs that are simply WRONG.  So the per-climb cost is
    measured here on the real path, as boards scored per SURVIVING HYPOTHESIS
    (each of which is one seeded climb) against boards per unseeded climb.
    """
    print("\nWith the plugs really deduced, per surviving hypothesis\n")
    # The 8-letter swept arm rejects nothing, so on the full key space it runs
    # ~2300 climbs per key (archived/cribs.md 4.2b).  Measured on 26 keys instead, with
    # its own baseline -- the ratio is per-climb, so the key space cancels.
    rows = [(8, "pinned", "..."), (8, "swept", "AA."), (12, "pinned", "..."),
            (12, "swept", "..."), (16, "pinned", "..."), (16, "swept", "..."),
            (20, "swept", "...")]
    base = {}
    for start in ("...", "AA."):
        keys = 17576 if start == "..." else 26
        _, s = timed(ct, [], 1, args.lang, start=start)
        base[start] = float(s) / keys
        print("  unseeded climb, %5d keys: %7.0f boards each"
              % (keys, base[start]))
    print("\n  %-18s %8s %9s %10s %9s"
          % ("crib", "hyps", "pinned", "boards", "vs full"))
    for n, mode, start in rows:
        crib = PT[3:3 + n]
        at = ["--crib-at", "3"] if mode == "pinned" else []
        # The dump is a plain scan, so it counts hypotheses without climbing.
        cmd = [BIN, "-u", "B", "-w", "123", "-r", "AAA", "-g", start,
               "-f", "-l", args.lang, "-T", "1", "--crib", crib,
               "--crib-dump"] + at
        p = subprocess.run(cmd, input=ct, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, universal_newlines=True,
                           env=dict(os.environ, ENIGMA_SEED="0"))
        stops = [ln.split() for ln in p.stderr.splitlines()
                 if ln.startswith("cribstop")]
        if not stops:
            print("  %-2d letters %-7s   no surviving hypothesis" % (n, mode))
            continue
        # Fields 7.. are the pinned letters: both ends of each deduced cable,
        # plus the self-steckered letters the deduction shows carry none.
        pinned = sum(len(f) - 7 for f in stops) / float(len(stops))
        _, s = timed(ct, ["--crib", crib] + at, 1, args.lang, start=start)
        per = float(s) / len(stops)
        print("  %-2d letters %-7s %8d %9.1f %10.0f %8.1fx"
              % (n, mode, len(stops), pinned, per, base[start] / per))


if __name__ == "__main__":
    main()
