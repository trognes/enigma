#!/usr/bin/env python3
"""Which pre-pass does a FUSED target want on telegraphic traffic, mono or IC?

    python3 eval/prepass_ab.py --length 167 --trials 500 --seed 4242

WHY THIS EXISTS.  CLAUDE.md recommends `-S m4f10` -- a monogram pre-pass before
the fused target -- and cites a paired A/B in which mono beat IC on telegraphic
traffic by 2.2pp.  That measurement used `-a` as the TARGET (it is written
`m4a10` vs `i4a10`), and `-f` differs from `-a` precisely by folding the index
of coincidence into the target score.  So the recommendation had never been
checked against the model the tool actually recommends, and there are two
opposite predictions:

  * an IC pre-pass is now REDUNDANT (the target already sees IC), so mono should
    win by even more; or
  * an IC pre-pass is now ALIGNED with what the target optimises, so IC should
    win.

MEASURED: the second.  At L=167 on authentic HG Nord telegraphic German, over
2000 paired trials in five independent seeds, `i4f10` beats `m4f10` by 2.81pp
mean %-correct (95% CI [-4.80, -0.82], z=2.76) and 3.1pp of exact recovery
(72.2% -> 75.2%; McNemar over the 1800 trials with logged discordants
p=0.021).  All five seeds favour i4f10 and heterogeneity is Q=1.65 on 4 df, so
the runs scatter around one effect rather than disagreeing.

THE TARGET MATTERS ABOUT TWICE AS MUCH AS THE PRE-PASS.  --arms generalises
this to any two schedules; run with `--arms i4f10 i4a10` and the same L=167
telegraphic setup, 1000 paired trials in two seeds, the FUSED target beats the
weighted one by 5.20pp (95% CI [+3.29, +7.12], z=5.34; exact 77.4% vs 71.4%,
McNemar p=1.1e-07, Q=0.78 on 1 df).  That is above the +3.0..+4.4pp CLAUDE.md
records for `-f` over `-a`, which was measured with each model's OWN
recommended staging -- so pairing `-a` with an IC pre-pass suits it less well
than mono does, and the two knobs interact rather than being independent.

STILL OPEN: this is three cells of a 2x2 ({m4,i4} x {a,f}) at L=167; the
missing one is m4a10 vs m4f10 at this length.  And a single run at L=60 leaned
mono under `-f` (+1.77pp, CI spans 0), so a length component may coexist with
the interaction.

DESIGN NOTES.
  * PAIRED: each trial's ciphertext goes to both arms, so the excerpt, rotor key
    and hidden board are identical and the difference is the schedule alone.
  * The plaintext is drawn from the 69 AUTHENTIC decrypts, which are held out
    from the Appendix-C statistics the `wehrmacht` tables are built from -- so
    the scorer is not being tested on its own training text.
  * MATCHED COMPUTE is checked rather than assumed: the two arms' score_iter is
    reported and ran within 2% in every run.
  * Judge on mean %-correct (CLAUDE.md: the graded, lower-variance signal);
    exact recovery and McNemar are the secondary check.
"""
import argparse
import os
import random
import re
import statistics
import subprocess
import sys
from math import comb

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ENIGMA = os.path.join(ROOT, "enigma")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ARMS = ("m4f10", "i4f10")   # overridden by --arms


def decrypts(path):
    """The DECRYPT field only -- stop at the next FIELD: label, or EMENDED and
    TRANSLATION leak in and the 'corpus' stops being machine output."""
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(args, text):
    p = subprocess.run([ENIGMA] + args, input=text, capture_output=True,
                       text=True, check=False)
    it = re.search(r"scored (\d+) plugboards", p.stderr)
    return p.stdout.strip(), int(it.group(1)) if it else 0


def main():
    global ARMS
    ap = argparse.ArgumentParser()
    ap.add_argument("--length", type=int, default=167)
    ap.add_argument("--trials", type=int, default=500)
    ap.add_argument("--restarts", default="8")
    ap.add_argument("--seed", type=int, default=4242)
    ap.add_argument("--threads", default="4")
    ap.add_argument("--arms", nargs=2, default=list(ARMS),
                    metavar=("A", "B"),
                    help="two --score schedules to compare, e.g. i4f10 i4a10. "
                         "The TARGET is the schedule's last model token, so "
                         "changing it changes the model the run is scored by; "
                         "-f/-a on the command line would then conflict with "
                         "it, which is why neither is passed.")
    args = ap.parse_args()
    ARMS = tuple(args.arms)

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    if len(corpus) < args.length + 1:
        sys.exit("corpus too short")
    print(f"# corpus {len(corpus)} letters of authentic HG Nord decrypts",
          file=sys.stderr)

    rng = random.Random(args.seed)
    L = args.length
    res = {a: [] for a in ARMS}
    iters = {a: 0 for a in ARMS}

    for _ in range(args.trials):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
        ct, _ = run(["-u", "B", "-w", w, "-r", r, "-g", g, "-s", pb], pt)
        for arm in ARMS:
            out, it = run(["-u", "B", "-w", w, "-r", r, "-g", g, "-c", "-J",
                           "--polish", "-l", "wehrmacht", "-S", arm,
                           "-R", args.restarts, "-T", args.threads, "-e", "7"],
                          ct)
            res[arm].append(100.0 * sum(a == b for a, b in zip(out, pt)) / L)
            iters[arm] += it

    n = args.trials
    print(f"L={L}  n={n}  -R {args.restarts}  seed={args.seed}")
    for arm in ARMS:
        v = res[arm]
        print(f"  {arm}: mean %correct {statistics.mean(v):6.2f}   "
              f"exact {sum(1 for x in v if x > 99.99)}/{n}   "
              f"score_iter {iters[arm]:,}")
    ex = {a: [x > 99.99 for x in res[a]] for a in ARMS}
    b = sum(1 for x, y in zip(ex[ARMS[0]], ex[ARMS[1]]) if x and not y)
    c = sum(1 for x, y in zip(ex[ARMS[0]], ex[ARMS[1]]) if y and not x)
    nd = b + c
    p = (2 * sum(comb(nd, k) for k in range(0, min(b, c) + 1)) / 2 ** nd
         if nd else 1.0)
    print(f"  McNemar on exact: {ARMS[0]}-only {b}, {ARMS[1]}-only {c}, "
          f"discordant {nd}, p={min(p, 1.0):.3f}")
    d = [x - y for x, y in zip(res[ARMS[0]], res[ARMS[1]])]
    se = statistics.stdev(d) / len(d) ** 0.5
    print(f"  paired diff ({ARMS[0]}-{ARMS[1]}): {statistics.mean(d):+6.2f} pp"
          f"  95% CI [{statistics.mean(d) - 1.96 * se:+.2f}, "
          f"{statistics.mean(d) + 1.96 * se:+.2f}]   "
          f"(negative = {ARMS[1]} ahead)")


if __name__ == "__main__":
    main()
