#!/usr/bin/env python3
"""Do the stage-0 seeds plug the FREQUENT ciphertext letters?

eval/proto_freqbias.cc established that a plug's index-of-coincidence signal
scales with m = count(a) + count(b): the noise as sqrt(m), the true-plug signal
as m, so SNR as sqrt(m).  A low-order pre-pass therefore CAN resolve plugs on
frequent letters and essentially cannot on rare ones.

If that is what actually happens, the seeds a -S k4 stage produces should plug
frequent letters preferentially -- and then two things follow: the pre-pass's
move set could be restricted to the high-m plugs (~81 of 325 at the top
quartile) for a cheaper stage, and the seed pool's narrowness has a named
mechanism.  If the seed letters are flat in ciphertext frequency the picture is
wrong and neither follows.

WHAT IS MEASURED, per key: the correlation across the 26 letters between a
letter's ciphertext count and how often it carries a cable in the seeds, plus
the plug-weighted mean ciphertext count against the unweighted one (the
directly interpretable form -- "a seed plug lands on a letter occurring N times
against N0 for a letter picked at random").

It also reports the seeds' PLUG COUNT, which is the thing that is easy to get
wrong: with the default 10-pair kick a k4 cap only blocks ADDS, so the seed is
not a 4-plug board and any arithmetic assuming it is will be about the wrong
object.

Usage:
  python3 eval/seed_letter_bias.py --keys 40 --length 100 --restarts 300
"""

import argparse
import os
import random
import re
import subprocess
import sys
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


def pearson(x, y):
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    sxx = sum((a - mx) ** 2 for a in x)
    syy = sum((b - my) ** 2 for b in y)
    return sxy / sqrt(sxx * syy) if sxx > 0 and syy > 0 else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", type=int, default=40)
    ap.add_argument("--length", type=int, default=100)
    ap.add_argument("--restarts", type=int, default=300)
    ap.add_argument("--schedule", default="k4")
    ap.add_argument("--seed", type=int, default=3)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    L = args.length
    rng = random.Random(args.seed)

    corrs, ratios, npl, ndist, nseed = [], [], [], [], []
    # pooled: ciphertext count of a letter, against times plugged in a seed
    tru_hi = tru_lo = hit_hi = hit_lo = 0

    for _ in range(args.keys):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pairs = [ls[2 * i] + ls[2 * i + 1] for i in range(10)]
        pb = " ".join(pairs)

        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
        ct, _ = run(key + ["-s", pb], pt)
        cnt = [ct.count(c) for c in LET]

        _, err = run(key + ["-c", "-S", args.schedule, "-l", "wehrmacht",
                            "-T", 1, "-e", "7", "-R", args.restarts,
                            "--dump-all"], ct)
        plugged = [0] * 26
        seeds, tot_pairs, nboards = set(), 0, 0
        for line in err.splitlines():
            if not line.startswith("dumpall"):
                continue
            f = line.split()
            bp = sorted("".join(sorted(p)) for p in f[5:] if len(p) == 2)
            seeds.add(" ".join(bp))
            nboards += 1
            tot_pairs += len(bp)
            for p in bp:
                plugged[LET.index(p[0])] += 1
                plugged[LET.index(p[1])] += 1
        if nboards == 0:
            continue

        corrs.append(pearson(cnt, plugged))
        tp = sum(plugged)
        wmean = sum(c * p for c, p in zip(cnt, plugged)) / tp if tp else 0.0
        umean = sum(cnt) / 26.0
        ratios.append(wmean / umean if umean else 0.0)
        npl.append(tot_pairs / nboards)
        ndist.append(len(seeds))
        nseed.append(nboards)

        # do the seeds recover the TRUE plugs on frequent letters more often?
        med = sorted(cnt)[13]
        for p in pairs:
            a, b = LET.index(p[0]), LET.index(p[1])
            hi = (cnt[a] + cnt[b]) >= 2 * med
            got = sum(1 for s in seeds
                      if "".join(sorted(p)) in s.split())
            if hi:
                tru_hi += 1
                hit_hi += 1 if got else 0
            else:
                tru_lo += 1
                hit_lo += 1 if got else 0

    n = len(corrs)
    print(f"# L={L}, {n} keys, -S {args.schedule} -R {args.restarts}, "
          f"10-pair board hidden, rotor key given")
    print(f"seed plugs per board      {sum(npl) / n:6.2f}   "
          f"(the KICK sets 10; a k4 cap only blocks adds)")
    print(f"distinct seeds / restarts {sum(ndist) / sum(nseed):6.3f}")
    print(f"corr(ciphertext count, times plugged in a seed) "
          f"{sum(corrs) / n:+6.3f}")
    print(f"plug-weighted mean ciphertext count / unweighted "
          f"{sum(ratios) / n:6.3f}   (1.000 = no bias)")
    print(f"true plugs appearing in ANY seed: "
          f"frequent {hit_hi}/{tru_hi} = {100.0 * hit_hi / max(tru_hi, 1):.1f}%"
          f"   rare {hit_lo}/{tru_lo} = "
          f"{100.0 * hit_lo / max(tru_lo, 1):.1f}%")


if __name__ == "__main__":
    main()
