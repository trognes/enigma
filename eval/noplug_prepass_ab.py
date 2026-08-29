#!/usr/bin/env python3
"""Does restricting the CAP-STAGE to the frequent half of the alphabet pay?

eval/results-freq-bias.txt established that a plug's index-of-coincidence
signal-to-noise grows as sqrt(m), m = count(a) + count(b) in the ciphertext --
so the low-order pre-pass can resolve plugs on frequent letters and essentially
cannot on rare ones.  The obvious action is to stop it looking at the rare
ones.  Its arithmetic is not obviously favourable (it cuts the reachable true
plugs ~4x to raise the per-plug hit rate ~1.5x), so it is measured end to end.

  arm A   kick and i4 pre-pass over all 26 letters
  arm B   kick and i4 pre-pass over the FREQUENT 26 - N only, the N rarest
          pinned empty with --no-plug (--exclude N, default 13)

In BOTH arms the f10 continuation runs with every letter free, so the
restriction applies to stage 0 alone.  The frequent set is chosen from the
CIPHERTEXT, so arm B is a realizable attack and not an oracle.

--exclude sets how strict the restriction is.  At N = 13 a true pair falls
entirely inside the allowed set only ~24% of the time, so most true plugs are
unreachable in stage 0; at N = 6 it is ~58%.  Arm B also explores less at
equal restarts, so --seed-ladder is what sets its matched restart count.

HOW, WITH NO CODE.  --no-plug pins for the whole run, so the two stages have to
be separate invocations -- which CLAUDE.md's own harness note already covers:
-S i4 is a legal terminal schedule, so --dump-all yields each restart's
converged stage-0 board, and `-c -R 0 --soft-plug <seed> -S f10` is exactly
"continue the full climb from this seed" with no kick to scatter it.  The
winner is picked by SCORE across the restarts, never by the plaintext, so the
selection is one a real attack could make.

TIMING IS NOT MEASURED HERE and must not be read off this harness: it pays
~0.1 s of process startup per invocation and makes 1 + R of them per trial, so
startup would be ~90% of it.  Use --timing, which fits the per-restart cost of
the stage-0 climb as a slope over several R and never subtracts a floor.

Usage:
  python3 eval/noplug_prepass_ab.py --trials 200 --length 100 --jobs 4
  python3 eval/noplug_prepass_ab.py --timing
"""

import argparse
import concurrent.futures
import os
import random
import re
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
    p = subprocess.run([ENIGMA] + [str(a) for a in args], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def dumped(err):
    """(score, canonical board) for every converged climb --dump-all printed."""
    out = []
    for line in err.splitlines():
        if not line.startswith("dumpall"):
            continue
        f = line.split()
        pairs = sorted("".join(sorted(p)) for p in f[5:] if len(p) == 2)
        out.append((float(f[4]), " ".join(pairs)))
    return out


def rare_n(ct, n):
    """The n least frequent letters of the ciphertext, ties by letter -- the
    ones arm B pins empty, so the pre-pass works on the other 26 - n."""
    return "".join(sorted(sorted(LET, key=lambda c: (ct.count(c), c))[:n]))


def mcnemar(only_a, only_b):
    n = only_a + only_b
    return (only_b - only_a) / sqrt(n) if n else 0.0


def corpus_text():
    return "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                   + decrypts(os.path.join(HERE,
                                           "enigma-army-messages-1941.txt")))


def make_specs(corpus, L, trials, seed):
    rng = random.Random(seed)
    specs = []
    for _ in range(trials):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
        specs.append((pt, w, r, g, pb))
    return specs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=200)
    ap.add_argument("--length", type=int, default=100)
    ap.add_argument("--restarts", type=int, default=8)
    ap.add_argument("--kick", type=int, default=4)
    ap.add_argument("--restarts-b", type=int, default=0,
                    help="if set, arm B runs this many restarts instead -- "
                         "for the matched-COST comparison once --timing has "
                         "said what B's saving buys")
    ap.add_argument("--exclude", type=int, default=13,
                    help="how many of the RAREST ciphertext letters arm B "
                         "pins empty during stage 0 (13 = the frequent half; "
                         "6 leaves the frequent 20)")
    ap.add_argument("--pre", default="i4")
    ap.add_argument("--target", default="f10")
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timing", action="store_true",
                    help="fit the per-restart cost of the stage-0 climb in "
                         "each arm; single-threaded, wants a quiet box")
    ap.add_argument("--exclude-sweep", type=int, nargs="+", default=None,
                    help="sweep the restriction strictness: arm B pins the N "
                         "rarest letters, for each N given. Arm A is computed "
                         "once per trial and the f10 continuations are "
                         "memoised by seed across arms, so the sweep costs far "
                         "less than one run per level. --exclude 0 is added as "
                         "a null control and must read 0 discordant.")
    ap.add_argument("--seed-ladder", type=int, nargs="+", default=None,
                    help="distinct stage-0 seeds per arm at each -R. Arm B "
                         "explores less at equal restarts, so the plain "
                         "comparison confounds 'restricting the letters' with "
                         "'reducing exploration'; this is what sets B's "
                         "restart count for the diversity-matched run.")
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = corpus_text()
    L = args.length

    if args.exclude_sweep:
        # Arm A is identical in every cell, so it is computed ONCE per trial;
        # and the f10 continuation depends only on (key, seed), so it is
        # memoised per trial across all arms -- neighbouring exclusion levels
        # share many seeds.  --exclude 0 is prepended as a null control: with
        # no letters pinned arm B is byte-identical to arm A, so that row must
        # read 0 discordant or the harness is wrong.
        levels = [0] + [n for n in args.exclude_sweep if n != 0]
        specs = make_specs(corpus, L, args.trials, args.seed)
        print(f"# L={L}, {args.trials} trials, -S {args.pre} then "
              f"{args.target}, --random {args.kick}, -R {args.restarts} both "
              f"arms, rotor key given, 10-pair board hidden", file=sys.stderr)

        def trial_sweep(spec):
            pt, w, r, g, pb = spec
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct, _ = run(key + ["-s", pb], pt)
            memo = {}

            def cont(s):
                if s not in memo:
                    cmd = key + ["-c", "-J", "-S", args.target, "-l",
                                 args.lang, "-T", 1, "-e", "7", "-R", 0,
                                 "--dump-all"]
                    if s:
                        cmd += ["--soft-plug", s.replace(" ", "")]
                    o, e = run(cmd, ct)
                    d = dumped(e)
                    if not d:
                        sys.exit("continuation produced no dumpall line:\n"
                                 + " ".join(str(x) for x in cmd) + "\n" + e)
                    memo[s] = (d[0][0], o)
                return memo[s]

            def arm(excl):
                cmd = key + ["-c", "-J", "-S", args.pre, "-l", args.lang,
                             "-T", 1, "-e", "7", "-R", args.restarts,
                             "--random", args.kick, "--dump-all"]
                if excl > 0:
                    cmd += ["--no-plug", rare_n(ct, excl)]
                _, err = run(cmd, ct)
                seeds = {b for _, b in dumped(err)}
                best = None
                for s in seeds:
                    v = cont(s)
                    if (best is None) or (v[0] > best[0]):
                        best = v
                c = sum(x == y for x, y in zip(best[1], pt)) if best else 0
                return (2 * c >= L, 100.0 * c / L, len(seeds))

            base = arm(0) if 0 in levels else None
            return [arm(n) if n != 0 else base for n in levels], base

        if args.jobs > 1:
            with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
                res = list(ex.map(trial_sweep, specs))
        else:
            res = [trial_sweep(sp) for sp in specs]

        n = len(res)
        aa = sum(1 for _, b in res if b[0])
        ap_ = sum(b[1] for _, b in res) / n
        print(f"{'excl':>5} {'letters':>8} {'>=50%':>10} {'mean%':>7} "
              f"{'seeds':>6} {'effect':>8} {'z':>7}")
        print(f"{'A':>5} {26:>8} {str(aa) + '/' + str(n):>10} {ap_:>7.1f} "
              f"{sum(b[2] for _, b in res) / n:>6.2f} {'--':>8} {'--':>7}")
        for i, lv in enumerate(levels):
            hit = sum(1 for r_, _ in res if r_[i][0])
            pc = sum(r_[i][1] for r_, _ in res) / n
            sd = sum(r_[i][2] for r_, _ in res) / n
            oa = sum(1 for r_, b in res if b[0] and not r_[i][0])
            ob = sum(1 for r_, b in res if r_[i][0] and not b[0])
            print(f"{lv:>5} {26 - lv:>8} {str(hit) + '/' + str(n):>10} "
                  f"{pc:>7.1f} {sd:>6.2f} {100.0 * (ob - oa) / n:>+7.1f}pp "
                  f"{mcnemar(oa, ob):>+7.2f}")
        return

    if args.seed_ladder:
        specs = make_specs(corpus, L, args.trials, args.seed)
        cases = []
        for pt, w, r, g, pb in specs:
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct, _ = run(key + ["-s", pb], pt)
            cases.append((key, ct, rare_n(ct, args.exclude)))
        print(f"# distinct stage-0 seeds, L={L}, -S {args.pre} "
              f"--random {args.kick}, {len(cases)} keys")
        lab = "B freq " + str(26 - args.exclude)
        print(f"{'R':>5} {'A all 26':>10} {lab:>11} {'B/A':>7}")

        def distinct(arm, R):
            tot = 0
            for key, ct, rare in cases:
                cmd = key + ["-c", "-J", "-S", args.pre, "-l", args.lang,
                             "-T", 1, "-e", "7", "-R", R,
                             "--random", args.kick, "--dump-all"]
                if arm == "B":
                    cmd += ["--no-plug", rare]
                _, err = run(cmd, ct)
                tot += len({b for _, b in dumped(err)})
            return tot / len(cases)

        for R in args.seed_ladder:
            a = distinct("A", R)
            b = distinct("B", R)
            print(f"{R:>5} {a:>10.2f} {b:>11.2f} {b / a if a else 0:>7.3f}")
        return

    if args.timing:
        # Slope over several R, never a floor subtraction: at -R 8 a whole
        # invocation is ~95% process startup, so a floor-subtracted number
        # would be measuring the n-gram load.
        specs = make_specs(corpus, L, 8, args.seed)
        ladder = [0, 128, 512, 1024]
        print(f"# stage-0 cost, L={L}, -S {args.pre} --random {args.kick}, "
              f"8 keys, min of 3 reps, single-threaded")
        print(f"{'arm':>4} " + " ".join(f"{'R=' + str(R):>9}" for R in ladder)
              + f" {'us/restart':>11}")
        # encrypt OUTSIDE the timed loop -- it is R-independent, so it would
        # only inflate the intercept, but there is no reason to pay for it
        cases = []
        for pt, w, r, g, pb in specs:
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct, _ = run(key + ["-s", pb], pt)
            cases.append((key, ct, rare_n(ct, args.exclude)))
        slope = {}
        for arm in ("A", "B"):
            times = []
            for R in ladder:
                best = None
                for _ in range(3):
                    t0 = time.perf_counter()
                    for key, ct, rare in cases:
                        cmd = key + ["-c", "-J", "-S", args.pre, "-l",
                                     args.lang, "-T", 1, "-e", "7",
                                     "-R", R, "--random", args.kick]
                        if arm == "B":
                            cmd += ["--no-plug", rare]
                        run(cmd, ct)
                    t = time.perf_counter() - t0
                    best = t if best is None else min(best, t)
                times.append(best)
            # least squares slope of time on R, in us per restart per key
            n = len(ladder)
            mx = sum(ladder) / n
            my = sum(times) / n
            num = sum((x - mx) * (y - my) for x, y in zip(ladder, times))
            den = sum((x - mx) ** 2 for x in ladder)
            slope[arm] = (num / den) * 1e6 / len(specs)
            print(f"{arm:>4} " + " ".join(f"{t:9.3f}" for t in times)
                  + f" {slope[arm]:11.1f}")
        if slope["B"] > 0:
            print(f"\nB is {slope['A'] / slope['B']:.2f}x cheaper per restart "
                  f"in stage 0.")
            print(f"At arm A's stage-0 cost, arm B affords "
                  f"-R {round(args.restarts * slope['A'] / slope['B'])} "
                  f"(stage 0 only -- the f10 continuation is unchanged, so "
                  f"the whole-run ratio is smaller).")
        return

    specs = make_specs(corpus, L, args.trials, args.seed)
    print(f"# L={L}, {args.trials} trials, -S {args.pre} then {args.target}, "
          f"--random {args.kick}, rotor key given, 10-pair board hidden",
          file=sys.stderr)

    def trial(spec):
        pt, w, r, g, pb = spec
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)
        rare = rare_n(ct, args.exclude)
        out = []
        for arm in ("A", "B"):
            R = args.restarts
            if (arm == "B") and args.restarts_b:
                R = args.restarts_b
            cmd = key + ["-c", "-J", "-S", args.pre, "-l", args.lang,
                         "-T", 1, "-e", "7", "-R", R,
                         "--random", args.kick, "--dump-all"]
            if arm == "B":
                cmd += ["--no-plug", rare]
            _, err = run(cmd, ct)
            seeds = {b for _, b in dumped(err)}
            best = None
            for s in seeds:
                cont = key + ["-c", "-J", "-S", args.target, "-l", args.lang,
                              "-T", 1, "-e", "7", "-R", 0, "--dump-all"]
                if s:
                    # --soft-plug takes CONTIGUOUS letters, no spaces; passing
                    # the spaced board is a fatal error, and one this harness
                    # used to swallow as a 0.0 rather than report
                    cont += ["--soft-plug", s.replace(" ", "")]
                o, e = run(cont, ct)
                d = dumped(e)
                if not d:
                    sys.exit("continuation produced no dumpall line:\n"
                             + " ".join(str(x) for x in cont) + "\n" + e)
                # pick by SCORE, never by the plaintext
                if (best is None) or (d[0][0] > best[0]):
                    best = (d[0][0], o)
            if best is None:
                out.append((False, 0.0, len(seeds)))
                continue
            c = sum(x == y for x, y in zip(best[1], pt))
            out.append((2 * c >= L, 100.0 * c / L, len(seeds)))
        return out

    if args.jobs > 1:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            res = list(ex.map(trial, specs))
    else:
        res = [trial(sp) for sp in specs]

    hit = {"A": 0, "B": 0}
    pct = {"A": 0.0, "B": 0.0}
    sds = {"A": 0, "B": 0}
    only_a = only_b = 0
    for a, b in res:
        hit["A"] += 1 if a[0] else 0
        hit["B"] += 1 if b[0] else 0
        pct["A"] += a[1]
        pct["B"] += b[1]
        sds["A"] += a[2]
        sds["B"] += b[2]
        if a[0] and not b[0]:
            only_a += 1
        if b[0] and not a[0]:
            only_b += 1

    n = len(res)
    rb = args.restarts_b or args.restarts
    print(f"{'arm':>4} {'restarts':>9} {'>=50% of letters':>17} "
          f"{'mean %correct':>14} {'distinct seeds':>15}")
    for arm, R in (("A", args.restarts), ("B", rb)):
        print(f"{arm:>4} {R:>9} {str(hit[arm]) + '/' + str(n):>17} "
              f"{pct[arm] / n:>14.1f} {sds[arm] / n:>15.2f}")
    z = mcnemar(only_a, only_b)
    print(f"\ndiscordant A/B {only_a}/{only_b}   z = {z:+.2f}   "
          f"(positive favours B)")


if __name__ == "__main__":
    main()
