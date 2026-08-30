#!/usr/bin/env python3
"""Is -K (IC-ranked move order) as good as -J (target-model order)?

    python3 eval/jorder_ab.py --length 167 --trials 400 --seed 4242

WHAT IS BEING ASKED.  `-J` visits the 325 plugboard toggles best-first, and
builds that order once per restart by scoring every move from the perturbed
starting board.  With a fused or quad target each of those 325 probes is a
FULL DECODE, and the scan is 20-23% of the climb's scored plugboards (measured
at -R 64: 20 800 of 91 451 at L=100, of 98 081 at L=167).  Its share grows with
message length, because it is linear in L where the histogram form is flat.

`-K` is the same climb with that scan ranked by the index of coincidence,
computed from the co-occurrence table in O(26) per move.  Measured on one
167-letter fixture that takes the run from 7 535 643 plugboards scored to
5 546 450 -- a 26% cut in scoring work.

BUT IT IS A SEARCH CHANGE, NOT A SPEEDUP.  An IC order is not a target-model
order, so the climb visits moves differently and can converge somewhere else.
Cheaper per restart is worthless if it recovers less, and the two effects have
to be weighed against each other rather than one assumed.  Hence this harness:
paired trials, same excerpts, same keys, same boards, arms differing only in
-J against -K.

WHY WEHRMACHT BY DEFAULT.  The recommended recipe for real traffic is
`-f -l wehrmacht -S k4f10`, and CLAUDE.md records that scoring results do not
transfer between prose and telegraphic German (the mono-vs-IC pre-pass
ordering reverses).  So telegraphic is the case that matters operationally --
but it is exactly the transfer question that makes PROSE worth checking before
a recommendation is written for all languages, which is what --corpus and
--language are for:

    --corpus eval/corpus-tune-phase-ab.txt --language english

  That file is plain A-Z English prose; the default corpus is instead the
  authentic HG Nord decrypts, parsed out of the message databases.

MATCHED COMPUTE IS THE POINT.  The IC arm is cheaper per restart, so at equal
-R it is also getting less compute -- which would flatter the target arm.  The
harness therefore reports plugboards scored for both arms alongside recovery,
so a win can be read against what it cost.  If the IC arm is level on recovery
at less work, that is the win; if it is level at equal work, it is not.

--match-compute SPENDS THAT SAVING instead of banking it: it times both arms
once, single-threaded, before the trials, and gives -K the restart count whose
WALL TIME matches -J's.  Equal -R and matched compute are different questions
and the answer can differ, so the mode is explicit rather than a default.

  DO NOT MATCH ON plugboards scored.  That counter counts calls through the
  fused score loop; the IC ordering's O(26) work per move happens OUTSIDE it,
  so the counter UNDERSTATES -K's real cost and matching on it would hand -K
  extra compute under the name of matching.  Measured on one fixture at
  L=100: the counter says -25% on f10 where wall time says -15%, and -11% on
  k4f10 where wall says -3%.  On k4f10 the wall difference is at the ~4.5%
  self-control floor -- there is nothing there to spend, and a matched-compute
  run on that schedule is an equivalence check, not a test.

  STARTUP IS SUBTRACTED, via an -R 0 run of each arm.  A whole invocation at
  a low -R is mostly the n-gram load, and leaving that in dilutes the ratio
  toward 1 -- which would silently under-award the IC arm.

  CALIBRATE SERIAL, MEASURE PARALLEL.  --jobs runs the trials across
  processes, which is safe because they measure RECOVERY, not time.  The
  calibration is a timing measurement and always runs single-threaded, before
  the pool starts; run it on an otherwise idle box.

THE JUDGE IS BREAK50: the number of trials recovering at least half the
plaintext.  Exact recovery is near-zero at the short end and so is dominated
by trial noise; the mean is dragged around by catastrophic failures, where a
board that got 5% and one that got 45% are both simply "not broken".  Half the
letters is the point past which a reader has the message, and the count of
those is what CLAUDE.md's restart ladder already judges on.  Mean and exact
are still reported, as secondary.
"""

import argparse
import os
import random
import re
import multiprocessing
import subprocess
import sys
import time
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


def run(args, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = os.path.join(HERE, os.pardir, "ngrams")
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


# One trial: encipher the excerpt under a random key and board, then hand the
# ciphertext to both arms.  Returns None if the encryption came back the wrong
# length (a corpus excerpt that lost characters), so the pair is dropped rather
# than scored against a truncated plaintext.
def trial(spec):
    pt, w, r, g, pb, base_tail, rj, rk = spec
    key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    ct, _ = run(key + ["-s", pb], pt)
    if len(ct) != len(pt):
        return None
    out = {}
    for arm, flag, R in (("target", "-J", rj), ("ic", "-K", rk)):
        o, err = run(key + ["-c", flag, "-R", R] + base_tail, ct)
        out[arm] = (pct_correct(o, pt), o == pt, scored(err))
    return out


# Seconds of SEARCH per run, startup subtracted -- see the docstring on why the
# -R 0 subtraction is load-bearing.  Min of `reps`, as make bench does.
def search_time(argv, ct, R, reps=5):
    def once(rr):
        best = 1e9
        for _ in range(reps):
            t0 = time.perf_counter()
            run(argv + ["-R", rr], ct)
            best = min(best, time.perf_counter() - t0)
        return best
    return once(R) - once(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=400)
    ap.add_argument("--length", type=int, default=167)
    ap.add_argument("--restarts", type=int, default=8)
    ap.add_argument("--schedule", default="k4f10")
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=4242)
    ap.add_argument("--language", default="wehrmacht")
    ap.add_argument("--corpus", default=None,
                    help="plain A-Z text file to draw excerpts from "
                         "(default: the authentic HG Nord decrypts)")
    ap.add_argument("--ic-restarts", type=int, default=0,
                    help="-R for the -K arm (0 = same as --restarts)")
    ap.add_argument("--match-compute", action="store_true",
                    help="time both arms first, then give -K the -R whose "
                         "wall time matches -J's")
    ap.add_argument("--jobs", type=int, default=1,
                    help="run trials across N processes (recovery only; the "
                         "calibration stays single-threaded)")
    ap.add_argument("--tsv", action="store_true",
                    help="one tab-separated row, for grid sweeps")
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    if args.corpus:
        # '#' lines are dropped BEFORE the A-Z strip: uppercasing a comment
        # would otherwise splice its letters into the corpus itself.
        text = "".join(ln for ln in open(args.corpus, encoding="utf-8")
                       if not ln.lstrip().startswith("#"))
        corpus = re.sub(r"[^A-Z]", "", text.upper())
    else:
        corpus = "".join(
            decrypts(os.path.join(HERE, "enigma-messages.txt"))
            + decrypts(os.path.join(HERE, "enigma-army-messages-1941.txt")))
    L = args.length
    rng = random.Random(args.seed)
    tail = ["-S", args.schedule, "-f", "-l", args.language, "-T", 1]

    # Every trial is drawn from the seed BEFORE anything runs, so the trial set
    # is identical whatever --jobs is.
    specs = []
    for _ in range(args.trials):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(args.plugs))
        specs.append([pt, w, r, g, pb, tail, args.restarts, args.restarts])

    ratio = None
    rk = args.ic_restarts or args.restarts
    if args.match_compute:
        # Calibrate on the first trial's ciphertext, single-threaded.
        pt, w, r, g, pb = specs[0][:5]
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)
        tj = search_time(key + ["-c", "-J"] + tail, ct, args.restarts)
        tk = search_time(key + ["-c", "-K"] + tail, ct, args.restarts)
        # A self-control on the SAME arm, so the ratio can be read against
        # this box's floor rather than against an assumed one.
        tj2 = search_time(key + ["-c", "-J"] + tail, ct, args.restarts)
        ratio = tk / tj if tj > 0 else 1.0
        if ratio > 0:
            rk = max(1, round(args.restarts / ratio))
        if not args.tsv:
            print(f"# calibration: -J {tj:.3f}s, -K {tk:.3f}s "
                  f"(ratio {ratio:.3f}); self-control -J/-J {tj2 / tj:.3f}")
            print(f"# matched compute: -J -R {args.restarts} "
                  f"vs -K -R {rk}")
    for sp in specs:
        sp[7] = rk

    # arm -> [mean %correct sum, exact count, break50 count, plugboards]
    tot = {"target": [0.0, 0, 0, 0], "ic": [0.0, 0, 0, 0]}
    only = {"target": 0, "ic": 0}          # discordant on break50
    only_ex = {"target": 0, "ic": 0}       # discordant on exact
    diffs = []                             # paired mean-%correct differences
    n = 0

    if args.jobs > 1:
        with multiprocessing.Pool(args.jobs) as pool:
            results = pool.map(trial, specs, chunksize=1)
    else:
        results = [trial(sp) for sp in specs]

    for res in results:
        if res is None:
            continue
        n += 1
        b50 = {}
        ex = {}
        for arm in ("target", "ic"):
            pc, exact, sc = res[arm]
            b50[arm] = pc >= 50.0
            ex[arm] = exact
            tot[arm][0] += pc
            tot[arm][1] += 1 if exact else 0
            tot[arm][2] += 1 if b50[arm] else 0
            tot[arm][3] += sc
        diffs.append(res["ic"][0] - res["target"][0])
        if b50["target"] and not b50["ic"]:
            only["target"] += 1
        elif b50["ic"] and not b50["target"]:
            only["ic"] += 1
        if ex["target"] and not ex["ic"]:
            only_ex["target"] += 1
        elif ex["ic"] and not ex["target"]:
            only_ex["ic"] += 1

    if n == 0:
        sys.exit("no usable trials")

    bt, bi = tot["target"][2], tot["ic"][2]
    st, si = tot["target"][3], tot["ic"][3]
    z = mcnemar_z(only["target"], only["ic"])
    dc = 100.0 * (si - st) / st if st else 0.0
    md = sum(diffs) / n
    # Paired 95% CI on the mean difference -- the continuous outcome has far
    # more power than the count, so it is what bounds a null result.
    var = sum((d - md) ** 2 for d in diffs) / (n - 1) if n > 1 else 0.0
    half = 1.96 * sqrt(var / n) if n > 1 else 0.0
    if args.tsv:
        # L sched R_J R_K n break50_t break50_i dz mean_t mean_i ex_t ex_i
        # compute% mean_diff ci_lo ci_hi
        print(f"{L}\t{args.language}\t{args.schedule}\t{args.restarts}\t"
              f"{rk}\t{n}\t{bt}\t"
              f"{bi}\t{z:+.2f}\t{tot['target'][0] / n:.2f}\t"
              f"{tot['ic'][0] / n:.2f}\t{tot['target'][1]}\t{tot['ic'][1]}\t"
              f"{dc:+.1f}\t{md:+.2f}\t{md - half:+.2f}\t{md + half:+.2f}")
        return
    print(f"# L={L}, {n} paired trials, -f -l {args.language} -c -J/-K "
          f"-S {args.schedule} -R {args.restarts}/{rk}, {args.plugs}-pair "
          f"board hidden, rotor key given, seed {args.seed}")
    print(f"{'arm':8s} {'BREAK50':>9s} {'mean %correct':>14s} {'exact':>10s} "
          f"{'plugboards':>13s}")
    for arm in ("target", "ic"):
        mn, ex_n, b5, sc = tot[arm]
        print(f"{arm:8s} {b5:5d}/{n:<3d} {mn / n:14.2f} {ex_n:6d}/{n:<4d} "
              f"{sc:13,d}")
    print(f"\nBREAK50 ic - target: {bi - bt:+d} of {n}  "
          f"(discordant: only target {only['target']}, only ic {only['ic']}, "
          f"McNemar z = {z:+.2f})")
    print(f"mean  ic - target: {md:+.2f}pp, 95% CI [{md - half:+.2f}, "
          f"{md + half:+.2f}]   exact discordant: {only_ex['target']} / "
          f"{only_ex['ic']}")
    if st:
        print(f"plugboards: ic is {dc:+.1f}% of the target arm "
              f"(NOT a compute ratio -- see the docstring)")


if __name__ == "__main__":
    main()
