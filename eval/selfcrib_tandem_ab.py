#!/usr/bin/env python3
"""Does --self-crib-tandem pay on a real sweep, and what does it cost elsewhere?

    python3 eval/selfcrib_tandem_ab.py               # the A/B
    python3 eval/selfcrib_tandem_ab.py --trials 8    # a quick look

eval/selfcrib_noanchor.py measured the DEDUCTION, at the true key: a
separator-free doubling still yields a correct hypothesis (195 of 200 trials
against the separated case's 197), and asserting the left flank recovers most
of the ranking sharpness the missing separator costs.

This measures the SWEEP, which is what actually decides whether the flag is
worth using, and it asks two different questions of two different populations:

  TANDEM-ONLY   messages carrying a separator-free doubling and NO separated
  POOL          one.  The payoff case: the default cannot form the hypothesis
                at all, so anything the flag recovers here is new.  The
                exclusion matters -- of the four corpus messages with a tandem
                doubling, one also carries a separated `ZANDERS`, so the
                default already seeds it and the flag can add nothing there.

  SEPARATED     messages carrying an X-separated doubling and NO tandem one.
  POOL          THE RISK CASE, and the reason this script exists.  Here every
                tandem hypothesis is wrong by construction, and there are as
                many of them as real ones -- so they compete for the same K
                seed slots and can only crowd the right ones out.  If turning
                the flag on costs recoveries here, it must stay opt-in whatever
                it wins on the other pool.

Both arms sweep the same 676 keys with the board hidden, so the only difference
is the hypothesis set.  Cost is reported as plugboards scored (the
-T-independent counter) and wall time, since the deduction's own cost is
uncounted and this flag roughly doubles it.
"""
import argparse
import os
import random
import re
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import corpus, random_key                       # noqa: E402
from ring_stride_geometry_probe import crypt, plugboard         # noqa: E402

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def doubles(t, L, gap):
    """Doublings of L+ letters at this gap whose word holds no X."""
    for i in range(len(t)):
        for n in range(L, 20):
            j = i + n + gap
            if j + n > len(t):
                break
            if "X" in t[i:i + n]:
                continue
            if gap == 1 and t[i + n] != "X":
                continue
            if t[i:i + n] == t[j:j + n]:
                return True
    return False


def run(binary, args, ct):
    t0 = time.perf_counter()
    p = subprocess.run([binary] + args, input=ct, capture_output=True,
                       text=True, check=False)
    wall = time.perf_counter() - t0
    it = re.search(r"scored (\d+) plugboard", p.stderr)
    return p.stdout.strip(), int(it.group(1)) if it else 0, wall


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default="./enigma")
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--seeds", type=int, default=10)
    ap.add_argument("--minlen", type=int, default=6)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=20260821)
    ap.add_argument("--out", default="eval/results-selfcrib-tandem.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    texts = corpus()
    tandem = [t for t in texts
              if doubles(t, a.minlen, 0) and not doubles(t, a.minlen, 1)]
    sep_only = [t for t in texts
                if doubles(t, a.minlen, 1) and not doubles(t, a.minlen, 0)]
    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    say(__doc__.split("\n")[0])
    say("\n%d trials per pool, --self-crib-seeds %d, %d-pair board hidden,"
        % (a.trials, a.seeds, a.plugs))
    say("start position swept (676 keys), -l %s, doubling of %d+ letters"
        % (a.lang, a.minlen))
    say("pools: %d messages carry only a tandem doubling, %d only a separated"
        " one\n" % (len(tandem), len(sep_only)))

    results = {}
    for pool_name, pool in (("tandem-only", tandem),
                            ("separated-only", sep_only)):
        if not pool:
            continue
        ok = {"off": [], "on": []}
        it = {"off": [], "on": []}
        wl = {"off": [], "on": []}
        for _ in range(a.trials):
            pt = rng.choice(pool)
            w, r, ring, start = random_key(rng)
            plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)),
                             a.plugs)
            ct = crypt(pt, w, r, ring, start, plug)
            base = ["-c", "-f", "-l", a.lang, "-J",
                    "-u", r, "-w", "".join(str(x + 1) for x in w),
                    "-r", "".join(ALPHA[x] for x in ring),
                    "-g", ALPHA[start[0]] + "..",
                    "-R", "0", "-T", str(a.threads),
                    "--self-crib-seeds", str(a.seeds),
                    "--self-crib-length", str(a.minlen)]
            for tag, extra in (("off", []), ("on", ["--self-crib-tandem"])):
                out, n, s = run(a.binary, base + extra, ct)
                ok[tag].append(out == pt)
                it[tag].append(n)
                wl[tag].append(s)
        results[pool_name] = (ok, it, wl)

    say("%-16s %-6s %-12s %-14s %s" % ("pool", "arm", "exact", "plugboards",
                                       "wall/trial"))
    say("-" * 62)
    for pool_name, (ok, it, wl) in results.items():
        for tag in ("off", "on"):
            e = np.array(ok[tag])
            say("%-16s %-6s %-12s %-14.0f %.2f s"
                % (pool_name, tag, "%d/%d" % (e.sum(), e.size),
                   np.mean(it[tag]), np.mean(wl[tag])))

    say()
    for pool_name, (ok, it, wl) in results.items():
        off = np.array(ok["off"])
        on = np.array(ok["on"])
        gained = int((on & ~off).sum())
        lost = int((off & ~on).sum())
        say("%-16s only-on %d, only-off %d  (net %+d), wall %.2fx"
            % (pool_name, gained, lost, gained - lost,
               np.mean(wl["on"]) / max(np.mean(wl["off"]), 1e-9)))

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
