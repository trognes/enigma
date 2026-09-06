#!/usr/bin/env python3
"""Milestone 3: key-climbs per second, GPU host against the CPU tool.

    python3 metal/throughput.py --host metal/enigma-metal
    python3 metal/throughput.py --host metal/enigma-metal --sustained 600
    python3 metal/throughput.py --fixtures 4 --restarts 64      # smoke test

metal/DESIGN.md 9. Cells are L x restarts-per-key, 24 fixtures each: an
authentic HG Nord excerpt, a random rotor key, a random 10-pair board,
the rotor key GIVEN and only the plugboard hidden -- the tier every tuning
number in the repo is measured on. Both arms run the identical command
line, so both climb by steepest ascent (the GPU has no -J/-K) and both
compare integer keys (--int).

WALL TIME, never a counter. score_iter prices neither the uploads nor the
host's per-item reporting walk, and DESIGN.md 9 says the transfers are
part of what is being measured.

THREE numbers per cell, because they answer different questions:

  device      the kernel alone, upload and download included -- what the
              GPU achieves, read from the host's own Device time line
  end-to-end  device plus the host's CPU-side work: building each key's
              rows, and walking every returned board through decode,
              score_report and score_components for the reporting the
              CPU tool does anyway. This is what a user gets.
  speedup     end-to-end against the CPU tool on the same fixtures

STARTUP IS SUBTRACTED FROM BOTH ARMS and that is not a detail: an
invocation loads its n-gram tables before doing any work, ~0.1 s, and at
-R 64 a whole run is a few hundred ms. CLAUDE.md's -S k entry records two
attempts at a wall-time number that were worthless for exactly this
reason -- 0.105 s of 0.111 s was the load. The baseline is one -R 1 run
per binary per length, which overstates startup by one climb: 1.5% of the
smallest cell and less everywhere else.

No pre-set bar (DESIGN.md decision 6): this prints the cells and the
interval, and the judgment comes after.

DO NOT READ A --host metal/enigma-ref RUN AS A RESULT. The reference
backend caps its threads at the KEY count (backend_cpu.cc), and this tier
gives one key per fixture, so it runs single-threaded against a CPU arm
using every core -- its speedup column is roughly 1/threads by
construction and says nothing about the port. It is here to check the
harness itself, and the script says so in its header when pointed at it.
"""

import argparse
import math
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, os.pardir)
ENIGMA = os.path.join(TOP, "enigma")
NGRAMS = os.path.join(TOP, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-S", "k4f10", "-f", "-l", "wehrmacht", "--int"]


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(binary, argv, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    t = time.perf_counter()
    p = subprocess.run([binary] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return time.perf_counter() - t, p.returncode, p.stderr


def device_secs(stderr):
    """The host's own Device time line; None for the CPU tool."""
    m = re.search(r"Device time ([0-9.]+) s", stderr)
    return float(m.group(1)) if m else None


def fixture(rng, corpus, L):
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
    r = "".join(rng.choice(LET) for _ in range(3))
    g = "".join(rng.choice(LET) for _ in range(3))
    key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    ls = list(LET)
    rng.shuffle(ls)
    pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
    _, _, _ = run(ENIGMA, key + ["-s", pb], pt)
    p = subprocess.run([ENIGMA] + key + ["-s", pb], input=pt,
                       capture_output=True, text=True,
                       env=dict(os.environ, ENIGMA_SEED="0",
                                ENIGMA_DATA=NGRAMS))
    return key, p.stdout.strip()


def startup(binary, key, ct, threads):
    """One -R 1 run: essentially the n-gram load, which both arms pay."""
    best = None
    for _ in range(3):
        el, rc, _ = run(binary, key + RECIPE + ["-R", 1, "-T", threads], ct)
        if rc != 0:
            return None
        best = el if (best is None) else min(best, el)
    return best


def mean_ci(xs):
    n = len(xs)
    if n < 2:
        return (xs[0] if xs else float("nan")), float("nan")
    mu = sum(xs) / n
    sd = math.sqrt(sum((x - mu) ** 2 for x in xs) / (n - 1))
    return mu, 1.96 * sd / math.sqrt(n)


def cell(host, corpus, rng, L, R, fixtures, threads):
    """One L x R cell: returns the aggregate rates and the per-fixture
    speedups, which are what carry the interval."""
    fx = []
    for _ in range(fixtures):
        key, ct = fixture(rng, corpus, L)
        if len(ct) == L:
            fx.append((key, ct))
    if not fx:
        return None

    s_gpu = startup(host, fx[0][0], fx[0][1], threads)
    s_cpu = startup(ENIGMA, fx[0][0], fx[0][1], threads)
    if (s_gpu is None) or (s_cpu is None):
        return None

    dev = gpu = cpu = 0.0
    climbs = 0
    ratios = []
    for key, ct in fx:
        args = key + RECIPE + ["-R", R, "-T", threads]
        e1, rc1, err1 = run(host, args, ct)
        e0, rc0, _ = run(ENIGMA, args, ct)
        if (rc0 != 0) or (rc1 != 0):
            return None
        d = device_secs(err1)
        if d is None:
            return None
        g = max(e1 - s_gpu, 1e-9)
        c = max(e0 - s_cpu, 1e-9)
        dev += d
        gpu += g
        cpu += c
        climbs += R
        ratios.append(c / g)
    mu, ci = mean_ci(ratios)
    return {"L": L, "R": R, "n": len(fx), "climbs": climbs,
            "device": climbs / dev if dev > 0 else 0.0,
            "gpu": climbs / gpu, "cpu": climbs / cpu,
            "speedup": mu, "ci": ci}


def sustained(host, corpus, rng, L, R, threads, seconds):
    """DESIGN.md 9's thermal check: the same cell over and over, reported
    as the first minute against the last. A GPU that throttles shows up
    here and nowhere else."""
    key, ct = fixture(rng, corpus, L)
    args = key + RECIPE + ["-R", R, "-T", threads]
    t0 = time.perf_counter()
    marks = []
    while time.perf_counter() - t0 < seconds:
        el, rc, err = run(host, args, ct)
        d = device_secs(err)
        if (rc != 0) or (d is None):
            return None
        marks.append((time.perf_counter() - t0, R / d))
    if len(marks) < 4:
        return None
    first = [r for t, r in marks if t <= 60.0] or [marks[0][1]]
    last = [r for t, r in marks if t >= marks[-1][0] - 60.0]
    return {"runs": len(marks), "first": sum(first) / len(first),
            "last": sum(last) / len(last)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.path.join(HERE, "enigma-ref"))
    ap.add_argument("--fixtures", type=int, default=24)
    ap.add_argument("--lengths", type=int, nargs="+", default=[60, 107, 167])
    ap.add_argument("--restarts", type=int, nargs="+",
                    default=[64, 256, 1024])
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--sustained", type=int, default=0,
                    metavar="SECONDS")
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args()
    for b in (ENIGMA, args.host):
        if not os.path.exists(b):
            sys.exit(f"build {b} first")

    corpus = "".join(decrypts(os.path.join(TOP, "eval", "enigma-messages.txt"))
                     + decrypts(os.path.join(TOP, "eval",
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)
    print(f"# {os.path.relpath(args.host, TOP)} vs ./enigma -T "
          f"{args.threads}, {RECIPE}, rotor key given, 10 plugs hidden")
    print(f"# {args.fixtures} fixtures per cell, wall time with startup "
          f"subtracted, climbs per second")
    if os.path.basename(args.host) == "enigma-ref":
        print("# WARNING: the reference backend threads over KEYS and this "
              "tier has one\n#          key per fixture, so it is "
              "single-threaded here. Harness check only,\n#          not a "
              "result.")
    print()
    print("    L      R    n   device      gpu      cpu   speedup    95% CI")
    for L in args.lengths:
        for R in args.restarts:
            c = cell(args.host, corpus, rng, L, R, args.fixtures,
                     args.threads)
            if c is None:
                print(f" {L:>4} {R:>6}    -  (cell failed)")
                continue
            print(f" {c['L']:>4} {c['R']:>6} {c['n']:>4} "
                  f"{c['device']:>8.0f} {c['gpu']:>8.0f} {c['cpu']:>8.0f} "
                  f"{c['speedup']:>8.2f}x  +-{c['ci']:.2f}")

    if args.sustained > 0:
        L = args.lengths[-1]
        R = args.restarts[-1]
        print(f"\nsustained: L={L}, -R {R}, {args.sustained} s on the device")
        s = sustained(args.host, corpus, rng, L, R, args.threads,
                      args.sustained)
        if s is None:
            print("  (too few runs to compare; raise --sustained)")
        else:
            drop = 100.0 * (1.0 - s["last"] / s["first"]) if s["first"] else 0
            print(f"  {s['runs']} runs: first minute {s['first']:.0f} "
                  f"climbs/s, last minute {s['last']:.0f}, "
                  f"{drop:+.1f}% drift")

    print("\nNo pre-set bar (DESIGN.md decision 6): read the cells.")


if __name__ == "__main__":
    main()
