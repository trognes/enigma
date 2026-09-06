#!/usr/bin/env python3
"""DESIGN.md 17.6 step 1: where the ~900 lane-cycles per character go.

    make -C metal metal && make -C metal ablate
    python3 metal/ablate.py --host metal/enigma-metal

Six rows at ONE cell. Each removes one suspect from 17.2 and is timed
against the baseline, so the share it removes is what that suspect cost;
each also reports the register cap from the host's own `GPU:` line, which
is a property of the compiled pipeline and therefore differs per variant.
That second column is what tells thread memory (a latency cost, cap
unchanged) apart from arrays promoted to registers with select chains (an
ALU cost, cap rises when the array goes) -- the question 384 alone could
not settle.

    0  baseline, the shipping kernel
    1  no histogram in the fused stage         17.2(a)/(b), freq[26]
    2  no board lookups in the decode          17.2(a), the two steck[]
    3  32-bit accumulators                     17.2(b) -- ANSWER-PRESERVING,
                                               a candidate fix, verified
                                               identical on the CPU backend
    4  no all8 gather                          17.2(d), the 457 KB table
    5  pass counts, not a speed row            17.2(c), the divergence factor
    L  baseline at 128 lanes per threadgroup   occupancy, no kernel change

VARIANTS 1, 2 AND 4 RETURN A WRONG BOARD. That is the point of a cost
probe, and it is why the host prints a banner under $ENIGMA_GPU_ABLATE
and why nothing here reports a plaintext. Only rows 0, 3 and L are runs
whose answer means anything.

One cell, chosen to be the one the report is about: L=107, -R 256,
26 keys, which is a row of the M2/M1 tables. Repetitions are the min of
a few, as everywhere in this repo, and the device time is the host's own
timer so startup and the reporting walk are outside it.
"""

import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, os.pardir)
ENIGMA = os.path.join(TOP, "enigma")
NGRAMS = os.path.join(TOP, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-S", "k4f10", "-f", "-l", "wehrmacht", "--int"]

VARIANTS = [
    (0, "baseline", None, None),
    (1, "no histogram (freq[26])", 1, None),
    (2, "no board lookups", 2, None),
    (3, "32-bit accumulators", 3, None),
    (4, "no all8 gather", 4, None),
    (5, "pass counts (divergence)", 5, None),
    (0, "baseline, 128 lanes", None, "128"),
]


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def fixture(rng, corpus, L):
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
    r = "".join(rng.choice(LET) for _ in range(3))
    g = "".join(rng.choice(LET) for _ in range(3))
    ls = list(LET)
    rng.shuffle(ls)
    pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
    key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    p = subprocess.run([ENIGMA] + key + ["-s", pb], input=pt,
                       capture_output=True, text=True,
                       env=dict(os.environ, ENIGMA_SEED="0",
                                ENIGMA_DATA=NGRAMS))
    # 26 keys: the rightmost start position wildcarded, as throughput.py's
    # --keys 26 does, so this cell is the one the tables report.
    return ["-u", "B", "-w", w, "-r", r, "-g", g[0] + g[1] + "."], \
        p.stdout.strip()


def run(host, key, ct, R, threads, lib, lanes):
    env = dict(os.environ, ENIGMA_SEED="0", ENIGMA_DATA=NGRAMS)
    if lib:
        env["ENIGMA_METALLIB"] = lib
        env["ENIGMA_GPU_ABLATE"] = "1"
    if lanes:
        env["ENIGMA_GPU_LANES"] = lanes
    args = key + RECIPE + ["-R", str(R), "-T", str(threads)]
    p = subprocess.run([host] + args, input=ct, capture_output=True,
                       text=True, env=env)
    dev = re.search(r"Device time ([0-9.]+) s", p.stderr)
    cap = re.search(r"\((\d+) threads per threadgroup", p.stderr)
    div = re.search(r"max/mean = ([0-9.]+)", p.stderr)
    items = re.search(r"climbed (\d+) restart", p.stderr)
    if (p.returncode != 0) or (dev is None) or (items is None):
        sys.stderr.write(p.stderr[-800:])
        return None
    return {"secs": float(dev.group(1)), "items": int(items.group(1)),
            "cap": int(cap.group(1)) if cap else 0,
            "div": float(div.group(1)) if div else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.path.join(HERE, "enigma-metal"))
    ap.add_argument("--fixtures", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--length", type=int, default=107)
    ap.add_argument("--restarts", type=int, default=256)
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--seed", type=int, default=11)
    args = ap.parse_args()
    if not os.path.exists(args.host):
        sys.exit(f"build {args.host} first (make -C metal metal)")

    corpus = "".join(decrypts(os.path.join(TOP, "eval", "enigma-messages.txt"))
                     + decrypts(os.path.join(TOP, "eval",
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)
    fx = []
    while len(fx) < args.fixtures:
        key, ct = fixture(rng, corpus, args.length)
        if len(ct) == args.length:
            fx.append((key, ct))

    print(f"# {os.path.relpath(args.host, TOP)}, L={args.length}, "
          f"-R {args.restarts}, 26 keys, {args.fixtures} fixtures x "
          f"{args.reps} reps")
    print("# DESIGN.md 17.6 step 1. device climbs/s, the host's own timer.")
    print("# Variants 1, 2 and 4 return a WRONG board: cost probes.\n")
    print("  # variant                        climbs/s   vs base   cap  note")
    base = None
    for n, label, ab, lanes in VARIANTS:
        lib = (os.path.join(HERE, f"climb-a{ab}.metallib")
               if ab is not None else None)
        if lib and not os.path.exists(lib):
            print(f"  {n} {label:29s}  -- no {os.path.basename(lib)}; "
                  f"run `make -C metal ablate`")
            continue
        best = None
        cap = 0
        div = None
        for _ in range(args.reps):
            tot_s = tot_i = 0.0
            ok = True
            for key, ct in fx:
                r = run(args.host, key, ct, args.restarts, args.threads,
                        lib, lanes)
                if r is None:
                    ok = False
                    break
                tot_s += r["secs"]
                tot_i += r["items"]
                cap = r["cap"]
                if r["div"] is not None:
                    div = r["div"]
            if not ok:
                break
            rate = tot_i / tot_s if tot_s > 0 else 0.0
            best = rate if (best is None) else max(best, rate)
        if best is None:
            print(f"  {n} {label:29s}  (failed)")
            continue
        if base is None:
            base = best
        note = ""
        if div is not None:
            note = f"divergence {div:.2f}x"
        elif n == 3:
            note = "answer-preserving"
        elif n in (1, 2, 4):
            note = "wrong board"
        print(f"  {n} {label:29s} {best:9.0f}   {best / base:6.2f}x  "
              f"{cap:4d}  {note}")

    print("\nRead: a row far above 1.00x is what that suspect cost. A cap")
    print("above the baseline's says the array was in REGISTERS (an ALU")
    print("cost); a cap unchanged says thread memory (a latency cost).")


if __name__ == "__main__":
    main()
