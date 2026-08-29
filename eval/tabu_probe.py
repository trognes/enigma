#!/usr/bin/env python3
"""Is there anything for a TABU visited-set to forbid, and would forbidding it
help?

--restart-tt once measured near-total basin diversity and took tabu off the
table, but that was an -R <~ 64 result: distinct converged boards per restart
fall to 0.79 at -R 100 and 0.49 at -R 1000, so most restarts at a high budget
DO rediscover a basin already found.  That makes the premise true.  It does not
make tabu useful -- the basins a visited-set would push the climb into may
simply be worse.

TABU'S CEILING IS A RESTART COUNT, which is what makes this cheap to bound
without implementing anything.  If a run of R restarts converges to D distinct
basins, then a PERFECT tabu set -- one that never revisits and whose fresh
basins are drawn from the same distribution -- turns every duplicate into a new
draw, giving R distinct basins instead of D.  That is exactly the search a plain
run of R * (R/D) restarts already performs.  So:

    tabu's best case at R  <=  the measured gain from R to R*(R/D) restarts

and both sides are measurable with the shipped binary.  If that gain is small,
tabu cannot pay however well it is implemented.

The probe also classifies each trial by what actually went wrong, because the
ceiling is only interesting if the failures are EXPLORATION failures:

  break    the true board converged AND scored top
  scoring  the true board converged but something else outscored it
           -- no amount of fresh exploration fixes this
  search   the true board never converged in any restart
           -- the only class tabu could convert

--seed-dedup is the shipped cousin and is subtracted where it applies: it skips
a climb when the stage-0 SEED repeats, which is a strictly smaller set than
"converged to a basin already seen" (two different seeds can reach one basin).

Usage:
  python3 eval/tabu_probe.py --trials 40 --length 100 --restarts 100 1000
"""

import argparse
import concurrent.futures
import os
import random
import re
import subprocess
import sys

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


def norm(board):
    """Canonical form of a plugboard so two spellings of one board compare
    equal: pairs sorted within and between."""
    pairs = sorted("".join(sorted(p)) for p in board.split() if len(p) == 2)
    return " ".join(pairs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--length", type=int, default=100)
    ap.add_argument("--restarts", type=int, nargs="+", default=[100, 1000])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length
    rng = random.Random(args.seed)
    specs = []
    for _ in range(args.trials):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
        specs.append((pt, w, r, g, pb))

    base = ["-c", "-J", "--polish", "-f", "-l", "wehrmacht", "-S", "k4f10",
            "-T", 1, "-e", "7"]

    def one(spec, R):
        pt, w, r, g, pb = spec
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)
        out, err = run(key + base + ["-R", R, "--dump-all"], ct)
        boards, scores = [], []
        for line in err.splitlines():
            if not line.startswith("dumpall"):
                continue
            f = line.split()
            # dumpall <refl+wheels> <ring> <start> <score> <plugs...>
            scores.append(float(f[4]))
            boards.append(norm(" ".join(f[5:])))
        if not boards:
            return None
        truth = norm(pb)
        distinct = len(set(boards))
        found = truth in boards
        best = boards[max(range(len(boards)), key=lambda i: scores[i])]
        broke = (best == truth)
        cls = "break" if broke else ("scoring" if found else "search")
        # how many of the R climbs would --seed-dedup have skipped?
        _, err2 = run(key + base + ["-R", R, "--seed-dedup"], ct)
        m = re.search(r"Skipped (\d+) full climbs on duplicate seeds of (\d+)",
                      err2)
        skipped = int(m.group(1)) if m else 0
        seeds = int(m.group(2)) if m else R
        return distinct, len(boards), cls, skipped, seeds

    print(f"# L={L}, {args.trials} trials, rotor key GIVEN, 10-pair board "
          f"hidden, -c -J --polish -f -l wehrmacht -S k4f10")
    print(f"{'R':>6} {'distinct/R':>11} {'seed-dup':>9}  "
          f"{'break':>6} {'scoring':>8} {'search':>7}   {'tabu ceiling':>13}")
    for R in args.restarts:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            res = [r for r in ex.map(lambda s: one(s, R), specs) if r]
        if not res:
            continue
        dsum = sum(d for d, _, _, _, _ in res)
        nsum = sum(n for _, n, _, _, _ in res)
        cls = {"break": 0, "scoring": 0, "search": 0}
        for _, _, c, _, _ in res:
            cls[c] += 1
        sk = sum(s for _, _, _, s, _ in res)
        sd = sum(t for _, _, _, _, t in res)
        ratio = dsum / nsum
        print(f"{R:>6} {ratio:>11.3f} {100.0 * sk / sd:>8.1f}%  "
              f"{cls['break']:>6} {cls['scoring']:>8} {cls['search']:>7}   "
              f"{'-R ' + str(round(R / ratio)):>13}", flush=True)


if __name__ == "__main__":
    main()
