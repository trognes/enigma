#!/usr/bin/env python3
"""DESIGN.md 17.6 step 1: where the ~900 lane-cycles per character go.

    make -C metal metal && make -C metal ablate
    python3 metal/ablate.py --host metal/enigma-metal

Three tables. Each ablation removes one suspect from 17.2 and is timed
against the baseline, so the share it removes is what that suspect cost;
each also reports the register cap from the host's own `GPU:` line, which
is a property of the compiled pipeline and therefore differs per variant.
That column tells thread memory (a latency cost, cap unchanged) apart
from an array promoted to registers with select chains (an ALU cost, cap
rises when the array goes) -- the question 384 alone could not settle.

    0  baseline, the shipping kernel
    1  no histogram in the fused stage         17.2(a)/(b), freq[26]
    2  no board lookups in the decode          17.2(a), the two steck[]
    3  32-bit accumulators                     17.2(b) -- ANSWER-PRESERVING,
                                               a candidate fix, verified
                                               identical on the CPU backend
    4  no all8 gather                          17.2(d), the 457 KB table

VARIANTS 1, 2 AND 4 RETURN A WRONG BOARD. That is the point of a cost
probe, and it is why the host prints a banner under $ENIGMA_GPU_ABLATE
and why nothing here reports a plaintext.

WHY THERE ARE TWO TABLES OF THE SAME FIVE VARIANTS. The first run of this
instrument was confounded and read variant 2 at 20.4x. Changing the score
changes the climb's trajectory, hence the number of passes before it
converges, and climbs/s does not normalise for that -- variant 2 makes the
decode independent of the board, so no move ever improves and the climb
exits after ONE pass. Almost all of that 20.4x was work not done. Table A
is the natural climb, reporting the pass count each variant actually took
so its rate can be read in passes/s; table B pins every variant to the
same number of passes (MC_FIXED_PASSES), so climbs/s is comparable by
construction. B is the measurement; A says how far the confound reached.

Table C is occupancy: lanes per threadgroup crossed with the 32-bit
accumulators, both arms answer-preserving and neither needing a kernel
change. It exists because variant 3 was the only probe to move the
register cap (384 -> 448) while measuring just 1.04x at 256 lanes, where
one threadgroup is resident either way -- the cap can only convert at a
threadgroup size small enough for the extra group to fit. The table
prints floor(cap/lanes) so that reading is checkable rather than
asserted.

One cell, the one the M1/M2 Pro tables report: L=107, -R 256, 26 keys.
Repetitions are the min of a few, as everywhere in this repo, and the
device time is the host's own timer so startup and the reporting walk are
outside it.
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
    (0, "baseline"),
    (1, "no histogram (freq[26])"),
    (2, "no board lookups"),
    (3, "32-bit accumulators"),
    (4, "no all8 gather"),
]
NOTES = {0: "", 1: "wrong board", 2: "wrong board",
         3: "answer-preserving", 4: "wrong board"}
LANES = [32, 64, 96, 128, 192, 256]


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
        env["ENIGMA_GPU_LANES"] = str(lanes)
    args = key + RECIPE + ["-R", str(R), "-T", str(threads)]
    p = subprocess.run([host] + args, input=ct, capture_output=True,
                       text=True, env=env)
    dev = re.search(r"Device time ([0-9.]+) s", p.stderr)
    cap = re.search(r"\((\d+) threads per threadgroup", p.stderr)
    st = re.search(r"mean = ([0-9.]+), max/mean = ([0-9.]+)", p.stderr)
    items = re.search(r"climbed (\d+) restart", p.stderr)
    if (p.returncode != 0) or (dev is None) or (items is None):
        sys.stderr.write(p.stderr[-800:])
        return None
    return {"secs": float(dev.group(1)), "items": int(items.group(1)),
            "cap": int(cap.group(1)) if cap else 0,
            "mean": float(st.group(1)) if st else None,
            "div": float(st.group(2)) if st else None}


def cell(args, fx, lib, lanes):
    """Min over reps of the pooled rate; the pass stats of the last rep."""
    best = None
    cap = 0
    mean = div = None
    for _ in range(args.reps):
        tot_s = tot_i = 0.0
        for key, ct in fx:
            r = run(args.host, key, ct, args.restarts, args.threads,
                    lib, lanes)
            if r is None:
                return None
            tot_s += r["secs"]
            tot_i += r["items"]
            cap = r["cap"]
            mean = r["mean"]
            div = r["div"]
        if tot_s <= 0:
            return None
        rate = tot_i / tot_s
        best = rate if (best is None) else max(best, rate)
    return {"rate": best, "cap": cap, "mean": mean, "div": div}


def table(args, fx, prefix, title, note):
    print(f"\n{title}")
    print("  # variant                    climbs/s  passes/s  vs base"
          "   cap  passes  div   note")
    base = None
    for n, label in VARIANTS:
        lib = os.path.join(HERE, f"climb-{prefix}{n}.metallib")
        if not os.path.exists(lib):
            print(f"  {n} {label:26s}  -- no {os.path.basename(lib)}; "
                  f"run `make -C metal ablate`")
            continue
        c = cell(args, fx, lib, None)
        if c is None:
            print(f"  {n} {label:26s}  (failed)")
            continue
        # passes/s is the invariant-work rate: a pass is one full 325-toggle
        # scan, so variants doing different numbers of them compare here and
        # not in climbs/s.
        pps = c["rate"] * c["mean"] if c["mean"] else 0.0
        if base is None:
            base = pps if pps > 0 else c["rate"]
        ref = pps if pps > 0 else c["rate"]
        print(f"  {n} {label:26s} {c['rate']:9.0f} {pps:9.0f}  "
              f"{ref / base:6.2f}x  {c['cap']:4d}  "
              f"{c['mean'] or 0:6.2f}  {c['div'] or 0:.2f}  {NOTES[n]}")
    print(f"  ({note})")


def lane_sweep(args, fx):
    """Occupancy, with and without the 32-bit accumulators.

    Both arms are MC_PASSES builds so the pairing is like with like (the
    counter costs ~0.5%), and both are answer-preserving, so this table
    is about the shipping kernel rather than about a probe.

    The `grp` column is floor(cap / lanes): how many threadgroups of that
    size a core can hold, the cap being the compiler's register-derived
    limit that the host reports. It is what makes this a MECHANISM and
    not just two columns of numbers -- the 32-bit arm's cap was measured
    at 448 against 384, which changes grp at 32 and 64 lanes and nowhere
    else, so a gain confined to those two confirms the reading and a gain
    spread evenly refutes it.
    """
    base_lib = os.path.join(HERE, "climb-a0.metallib")
    acc_lib = os.path.join(HERE, "climb-a3.metallib")
    if not (os.path.exists(base_lib) and os.path.exists(acc_lib)):
        print("\nC. skipped -- run `make -C metal ablate`")
        return
    print("\nC. Lanes per threadgroup x accumulator width -- occupancy only,"
          " both answer-preserving")
    print("  lanes    base c/s  cap  grp     32-bit c/s  cap  grp   32/base"
          "   vs 256")
    ref = None
    rows = []
    for ln in LANES:
        b = cell(args, fx, base_lib, ln)
        a = cell(args, fx, acc_lib, ln)
        if (b is None) or (a is None):
            print(f"  {ln:5d}  (failed)")
            continue
        if ln == 256:
            ref = b["rate"]
        rows.append((ln, b, a))
    for ln, b, a in rows:
        bg = b["cap"] // ln if ln else 0
        ag = a["cap"] // ln if ln else 0
        vs = f"  {b['rate'] / ref:6.2f}x" if ref else ""
        print(f"  {ln:5d} {b['rate']:11.0f} {b['cap']:4d} {bg:4d} "
              f"{a['rate']:14.0f} {a['cap']:4d} {ag:4d} "
              f"{a['rate'] / b['rate']:8.2f}x{vs}")
    print("  (grp = floor(cap/lanes), threadgroups a core can hold; vs 256")
    print("   is the base arm against its own 256-lane row. The 32-bit arm")
    print("   should gain ONLY where grp differs; a gain where it does not")
    print("   is something other than occupancy.)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.path.join(HERE, "enigma-metal"))
    ap.add_argument("--fixtures", type=int, default=4)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--length", type=int, default=107)
    ap.add_argument("--restarts", type=int, default=256)
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--skip-lanes", action="store_true")
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
    print("# Variants 1, 2 and 4 return a WRONG board: cost probes.")

    table(args, fx, "f", "B. FIXED passes -- every variant does the same "
          "work; this is the measurement",
          "read the vs-base column: that is what the suspect cost")
    table(args, fx, "a", "A. NATURAL climb -- the pass count moves with the "
          "variant, hence passes/s",
          "climbs/s here is confounded by the trajectory; passes/s is not")

    if not args.skip_lanes:
        lane_sweep(args, fx)

    print("\nRead: in table B a row far above 1.00x is what that suspect")
    print("cost. A cap above the baseline's says the array was in")
    print("REGISTERS (an ALU cost); a cap unchanged says thread memory (a")
    print("latency cost). div is the simdgroup max/mean of the pass count")
    print("-- 17.2(c), the share of lane-cycles masked off.")


if __name__ == "__main__":
    main()
