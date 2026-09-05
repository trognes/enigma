#!/usr/bin/env python3
"""Does exhausting the FIRST plug over COMMON letters beat spending the same
compute on plain -R restarts?  (The Sullivan/Weierud instinct, measured.)

    python3 eval/common_exhaust_ab.py --trials 40 --lengths 107 167 --topk 6

Arms, paired on identical trials (rotor key given, 10-pair board hidden,
-c -K -S k4f10 -f -l wehrmacht, no --polish so the finisher cannot confound a
question about the SEED):

  E-common  exhaust the first plug over every pair touching the top-K letters
            of the WEHRMACHT monogram table (K=6 -> 135 pairs); one pinned
            -R 0 climb per pair via -s, best score wins.  This is --exhaust 1
            restricted to the high-sensitivity letters.
  E-all     native --exhaust 1: all 325 first pairs, -R 0 each.  Reproduces
            the repo's known "exhaust is dominated" result as a sanity check.
  C-common  plain -R, plugboards-scored matched to E-common (two-pass rescale)
  C-all     plain -R, plugboards-scored matched to E-all

The letters are taken from the plaintext model the search scores against,
not from prose German: the score-sensitivity argument is about what is
frequent in the text being scored, and in telegraphic traffic that includes
the X separator.

Judged on break50 (>=50% recovered), with mean %-correct and exact recovery
as secondary, and McNemar on the discordant pairs.  Compute is matched on the
binary's own "scored N plugboards" counter; wall time is NOT the axis here
because a pinned single climb is ~90% process startup.
"""

import argparse
import os
import random
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from math import comb

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-K", "-S", "k4f10", "-f", "-l", "wehrmacht"]


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(argv, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    p = subprocess.run([ENIGMA] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip(), p.stderr


def scored(stderr):
    m = re.search(r"scored (\d+) plugboards", stderr)
    return int(m.group(1)) if m else 0


def best_dump(stderr):
    best = None
    for ln in stderr.splitlines():
        m = re.match(r"dumpall \S+ \S+ \S+ ([-\d.]+)", ln)
        if m:
            v = float(m.group(1))
            best = v if best is None else max(best, v)
    return best


def top_letters(k):
    freq = {}
    for ln in open(os.path.join(NGRAMS, "wehrmacht_monograms.txt"),
                   encoding="utf-8"):
        p = ln.split()
        if len(p) == 2 and len(p[0]) == 1 and p[0].upper() in LET:
            freq[p[0].upper()] = int(p[1])
    return sorted(freq, key=lambda c: -freq[c])[:k]


def common_pairs(top):
    tops = set(top)
    return [a + b for i, a in enumerate(LET) for b in LET[i + 1:]
            if a in tops or b in tops]


def pct(out, pt):
    return 100.0 * sum(a == b for a, b in zip(out, pt)) / len(pt)


def control(key, ct, R):
    out, err = run(key + RECIPE + ["-R", R], ct)
    return out, scored(err)


def matched_control(key, ct, R0, target):
    """plain -R matched on plugboards scored: run at R0, rescale once."""
    out, s = control(key, ct, R0)
    R1 = max(1, round(R0 * target / s)) if s else R0
    if R1 != R0:
        out, s = control(key, ct, R1)
    return out, s, R1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--lengths", type=int, nargs="+", default=[107, 167])
    ap.add_argument("--topk", type=int, default=6)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--seed", type=int, default=17)
    args = ap.parse_args()
    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    top = top_letters(args.topk)
    pairs = common_pairs(top)
    assert len(pairs) == args.topk * 25 - comb(args.topk, 2)
    rng = random.Random(args.seed)
    print(f"# common-letter exhaust vs matched -R, {RECIPE}, 10 plugs, "
          f"rotor key given")
    print(f"# top-{args.topk} wehrmacht letters: {''.join(top)} -> "
          f"{len(pairs)} first-plug pairs; E-all = native --exhaust 1 (325)")
    print(f"# break50 = recovered >= 50%; compute matched on plugboards "
          f"scored\n")

    for L in args.lengths:
        stats = {a: [0, 0, 0.0] for a in ("E-common", "C-common",
                                          "E-all", "C-all")}
        disc = {"common": [0, 0], "all": [0, 0]}   # [only-E, only-C]
        comp = {a: 0 for a in stats}
        cover = 0
        n = 0
        for _ in range(args.trials):
            pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
            w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
            r = "".join(rng.choice(LET) for _ in range(3))
            g = "".join(rng.choice(LET) for _ in range(3))
            ls = list(LET)
            rng.shuffle(ls)
            true = [ls[2 * i] + ls[2 * i + 1] for i in range(10)]
            pb = " ".join(true)
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct, _ = run(key + ["-s", pb], pt)
            if len(ct) != L:
                continue
            truth = {"".join(sorted(p)) for p in true}
            if any(p in truth for p in pairs):
                cover += 1

            # E-common: pinned -R 0 climb per common pair, parallel
            def one(p):
                out, err = run(key + RECIPE + ["-R", 0, "-s", p,
                                               "--dump-all"], ct)
                return best_dump(err), out, scored(err)
            with ThreadPoolExecutor(max_workers=args.jobs) as ex:
                res = list(ex.map(one, pairs))
            res = [x for x in res if x[0] is not None]
            e_out = max(res, key=lambda x: x[0])[1]
            e_scored = sum(x[2] for x in res)

            # E-all: native --exhaust 1
            a_out, a_err = run(key + RECIPE + ["-R", 0, "--exhaust", 1], ct)
            a_scored = scored(a_err)

            # matched controls
            c_out, c_scored, _ = matched_control(key, ct, len(pairs),
                                                 e_scored)
            ca_out, ca_scored, _ = matched_control(key, ct, 325, a_scored)

            n += 1
            got = {"E-common": e_out, "C-common": c_out,
                   "E-all": a_out, "C-all": ca_out}
            for a, s in (("E-common", e_scored), ("C-common", c_scored),
                         ("E-all", a_scored), ("C-all", ca_scored)):
                comp[a] += s
            for a, o in got.items():
                p = pct(o, pt)
                stats[a][0] += p >= 50
                stats[a][1] += o == pt
                stats[a][2] += p
            for tag, ea, ca in (("common", "E-common", "C-common"),
                                ("all", "E-all", "C-all")):
                be = pct(got[ea], pt) >= 50
                bc = pct(got[ca], pt) >= 50
                if be and not bc:
                    disc[tag][0] += 1
                if bc and not be:
                    disc[tag][1] += 1

        print(f"L={L}  n={n}  (a true plug among the common pairs in "
              f"{cover}/{n} trials)")
        print(f"  {'arm':>9} {'break50':>8} {'exact':>6} {'mean%':>6} "
              f"{'plugboards/trial':>17}")
        for a in ("E-common", "C-common", "E-all", "C-all"):
            b, e, m = stats[a]
            print(f"  {a:>9} {b:>5}/{n:<3} {e:>6} {m/n:>6.1f} "
                  f"{comp[a]/n:>17,.0f}")
        for tag in ("common", "all"):
            oe, oc = disc[tag]
            print(f"  E-{tag} vs C-{tag}: only-E {oe}, only-C {oc}"
                  f"{mcnemar(oe, oc)}")
        print()


def mcnemar(a, b):
    """exact two-sided binomial on the discordant pairs."""
    nn = a + b
    if nn == 0:
        return "  (no discordant pairs)"
    k = min(a, b)
    p = sum(comb(nn, i) for i in range(k + 1)) / 2 ** nn * 2
    return f"  McNemar p = {min(1.0, p):.3f}"


if __name__ == "__main__":
    main()
