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

Table C is occupancy: lanes per threadgroup on the shipping kernel,
answer-preserving and needing no kernel change. 64 is the peak, at
1.27x. (It briefly carried a second arm pairing each lane count with the
32-bit accumulators; see lane_sweep() for why that arm answered "no" and
was removed.)

Table D (--scaling, on its own) is the per-probe cost against message
length on the baseline kernel: fixed passes for the cost, natural for
context, at five lengths from 60 to 256. It fits t(L) = a + b*L and
prints, per length, an upper bound on the 17.4 redesign -- which spreads
the per-character slope across 32 lanes and leaves the per-probe
intercept where it is. See scaling().

COST, this time multiplied out: an invocation is ~3.3 s at this cell, so
tables A and B are 2 x 5 x fixtures x reps invocations and table C is
6 x fixtures x sweep-reps. The defaults (4 fixtures, 3 reps, 1 sweep
rep) come to ~150 invocations, about eight minutes. --skip-probes runs
only table C, --skip-lanes only A and B. --scaling is 5 x 2 x fixtures
x reps, ~120 invocations, the longer lengths slower each -- ten minutes
or so.

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


def cell(args, fx, lib, lanes, reps=None):
    """Min over reps of the pooled rate; the pass stats of the last rep."""
    best = None
    cap = 0
    mean = div = None
    for _ in range(reps if reps else args.reps):
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
    """Occupancy: lanes per threadgroup on the shipping kernel.

    The `grp` column is floor(cap / lanes): how many threadgroups of that
    size a core can hold, the cap being the compiler's register-derived
    limit that the host reports.

    This used to run a second arm with the 32-bit accumulators, on the
    reading that they lifted the cap 384 -> 448 and so could only pay
    where grp differed (32 and 64 lanes). It ran, and both arms read 384
    at every row: the 448 belongs to the FIXED-PASS build of variant 3,
    where try_repair is compiled out, and not to the accumulators -- the
    natural build reads 384 with them too, in every run. So grp never
    differed, the pairing measured 1.01-1.02x flat, and the arm is gone
    along with the half of table C's cost it was.
    """
    print("\nC. Lanes per threadgroup, shipping kernel, answer-preserving")
    print("  lanes   climbs/s  cap  grp   vs 256")
    rows = []
    ref = None
    for ln in LANES:
        c = cell(args, fx, None, ln, args.sweep_reps)
        if c is None:
            print(f"  {ln:5d}  (failed)")
            continue
        if ln == 256:
            ref = c["rate"]
        rows.append((ln, c))
    for ln, c in rows:
        vs = f"  {c['rate'] / ref:6.2f}x" if ref else ""
        print(f"  {ln:5d} {c['rate']:10.0f} {c['cap']:4d} {c['cap'] // ln:4d}"
              f"{vs}")
    print("  (grp = floor(cap/lanes), threadgroups a core can hold. Below")
    print("   the restart count a key spans several groups, each loading")
    print("   its own rows[]; that is the fall-off at the small end.)")


SCALING_L = [60, 107, 167, 214, 256]
PROBES_PER_PASS = 326   # mc_key for best_score, then the 325 toggles


def scaling(args, corpus, rng):
    """Per-probe cost against message length: what 17.4 can and cannot
    shorten.

    The baseline kernel at fixed passes, so a climb is exactly
    16 x 326 scored boards and the device time divides into ns per probe.
    Fitting t(L) = a + b*L splits that into the per-CHARACTER part, b*L,
    which 17.4 spreads across 32 lanes, and the per-PROBE part, a --
    loop control, mc_plug_count, the move bookkeeping, mc_key's entry --
    which it does not touch. So t(L) / (a + b*L/32) is an upper bound on
    the redesign from a measured number, before a line of it exists. The
    natural climb runs beside it for the pass count and divergence at
    each length, which are context rather than input to the fit.
    """
    flib = os.path.join(HERE, "climb-f0.metallib")
    alib = os.path.join(HERE, "climb-a0.metallib")
    if not (os.path.exists(flib) and os.path.exists(alib)):
        sys.exit("run `make -C metal ablate` first")
    print(f"# {os.path.relpath(args.host, TOP)}, -R {args.restarts}, "
          f"26 keys, {args.fixtures} fixtures x {args.reps} reps per length")
    print("# DESIGN.md 17.6: per-probe cost vs L, baseline kernel.\n")
    print("D. Length scaling -- fixed passes for the cost, natural for "
          "context")
    print("     L   climbs/s   ns/probe   natural passes   div")
    pts = []
    for L in SCALING_L:
        fx = []
        while len(fx) < args.fixtures:
            key, ct = fixture(rng, corpus, L)
            if len(ct) == L:
                fx.append((key, ct))
        f = cell(args, fx, flib, None)
        a = cell(args, fx, alib, None)
        if (f is None) or (a is None):
            print(f"  {L:4d}  (failed)")
            continue
        probes = (f["mean"] or 16.0) * PROBES_PER_PASS
        ns = 1e9 / (f["rate"] * probes)
        pts.append((L, ns))
        print(f"  {L:4d} {f['rate']:10.0f} {ns:10.1f} {a['mean'] or 0:15.2f}"
              f" {a['div'] or 0:6.2f}")
    if len(pts) < 2:
        return
    n = len(pts)
    sx = sum(p[0] for p in pts)
    sy = sum(p[1] for p in pts)
    sxx = sum(p[0] * p[0] for p in pts)
    sxy = sum(p[0] * p[1] for p in pts)
    b = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    a = (sy - b * sx) / n
    print(f"\n  fit: ns/probe = {a:.1f} + {b:.3f} * L")
    print("  (intercept = the per-probe part, slope = per character)")
    print("     L   measured   fitted   resid   intercept share   17.4 bound")
    for L, ns in pts:
        fit = a + b * L
        bound = ns / (a + b * L / 32.0) if (a + b * L / 32.0) > 0 else 0.0
        print(f"  {L:4d} {ns:10.1f} {fit:8.1f} {ns - fit:7.1f} "
              f"{100.0 * a / fit:15.1f}%  {bound:9.1f}x")
    print("\n  Read: the bound keeps the intercept and divides only the")
    print("  slope by 32 -- what 17.4's decomposition can reach at best,")
    print("  before reduction overhead and before occupancy. A bound near")
    print("  the 4-6x parity needs is a stop; the redesign must clear it")
    print("  with room for the costs this fit cannot see.")


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
    ap.add_argument("--skip-probes", action="store_true",
                    help="only the lane sweep (table C)")
    ap.add_argument("--sweep-reps", type=int, default=1,
                    help="reps for table C; its rows repeat within 1%%")
    ap.add_argument("--scaling", action="store_true",
                    help="table D only: per-probe cost vs L and the 17.4 "
                         "bound")
    args = ap.parse_args()
    if not os.path.exists(args.host):
        sys.exit(f"build {args.host} first (make -C metal metal)")

    corpus = "".join(decrypts(os.path.join(TOP, "eval", "enigma-messages.txt"))
                     + decrypts(os.path.join(TOP, "eval",
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)
    if args.scaling:
        scaling(args, corpus, rng)
        return
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

    if not args.skip_probes:
        table(args, fx, "f", "B. FIXED passes -- every variant does the "
              "same work; this is the measurement",
              "read the vs-base column: that is what the suspect cost")
        table(args, fx, "a", "A. NATURAL climb -- the pass count moves with "
              "the variant, hence passes/s",
              "climbs/s here is confounded by the trajectory; passes/s is "
              "not")

    if not args.skip_lanes:
        lane_sweep(args, fx)

    print("\nRead: in table B a row far above 1.00x is what that suspect")
    print("cost. A cap above the baseline's says the array was in")
    print("REGISTERS (an ALU cost); a cap unchanged says thread memory (a")
    print("latency cost). div is the simdgroup max/mean of the pass count")
    print("-- 17.2(c), the share of lane-cycles masked off.")


if __name__ == "__main__":
    main()
