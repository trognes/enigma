#!/usr/bin/env python3
"""How often does --confidence report a positive margin on NOTHING?

    python3 eval/confidence_false_positive.py --trials 2000

WHY MEASURE WHAT THE TOOL ALREADY PRINTS.  report_confidence() prints a p-value
from the Gaussian upper tail, family-wise over K keys.  That is a MODEL, and the
statistic it models -- the maximum over K draws -- lives in the far tail of the
null, at around 4.4 sd, which is exactly where a central-limit approximation is
weakest.  The score is a sum over positions, so the CLT gives the centre of the
null quickly and the tail slowly; a best-of-K statistic reads only the tail.

So the printed p is checked here against the thing it predicts: run the search
on ciphertext with no plaintext behind it, many times, and count how often the
reported margin clears a threshold.  Every hit is by construction spurious.

WHAT IT FOUND (2000 trials, L=200, K=17576, --confidence 1000):

    P(margin >= +0.54) = 2.35% measured against 0.70% predicted

a factor of 3.4.  The real null's best-of-K sits +0.21 sd above a Gaussian of
the same mu/sd, and its upper tail is fatter -- 95th percentile +0.40 measured
against +0.11 predicted.  IC is worse again (its null is a quadratic form in the
letter histogram rather than a sum over positions, so it is right-skewed before
any tail approximation enters).

N DOES NOT FIX THIS.  At N=1000 the estimation error in mu-hat/sigma-hat is only
about 0.10 sd; nearly all the spread is the genuine fluctuation of the best of
K, which no amount of sampling removes.  Raising --confidence buys precision in
the null, not a narrower margin distribution.

NONE OF IT MATTERS FAR OUT.  A real break reads +15 to +17 sd on the same sweep,
where being wrong by a factor of three on 1e-98 changes nothing.  The correction
matters only near zero, which is why the tool now says so only near zero.
"""
import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ENIGMA = os.path.join(ROOT, "enigma")
MARGIN_RE = re.compile(r"margin ([+-][0-9.]+) sd")

# Keyspaces to sweep, smallest first.  K enters the margin through the bar it
# subtracts, but the SPREAD depends on K too (the Gumbel scale is 1/sqrt(2 ln K)
# and the location correction grows), so the false-positive rate is not one
# number -- it has to be reported per keyspace.
SHAPES = {
  "start": (["-u", "B", "-w", "123", "-r", "AAA", "-g", "..."], 17576),
  "wheels": (["-u", ".", "-w", "...", "-r", "AAA", "-g", "..."], 3163680),
}


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--trials", type=int, default=2000)
  ap.add_argument("--length", type=int, default=200)
  ap.add_argument("--confidence", type=int, default=1000)
  ap.add_argument("--shape", choices=sorted(SHAPES), default="start")
  ap.add_argument("--threads", default="1")
  ap.add_argument("--model", nargs="+", default=["-q", "-l", "english"])
  ap.add_argument("--seed", type=int, default=20260811)
  args = ap.parse_args()

  if not os.path.exists(ENIGMA):
    sys.exit("build the binary first (make)")

  keyspace, kcount = SHAPES[args.shape]
  rng = random.Random(args.seed)
  margins = []

  for i in range(args.trials):
    # Random letters, not an encryption of random letters: those are the same
    # thing, and generating directly keeps the arm free of any key at all.
    ct = "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
                 for _ in range(args.length))
    p = subprocess.run(
      [ENIGMA] + keyspace + args.model
      + ["--confidence", str(args.confidence), "-e", str(i + 1),
         "-T", args.threads],
      input=ct, capture_output=True, text=True, check=False)
    m = MARGIN_RE.search(p.stderr)
    if m:
      margins.append(float(m.group(1)))
    if (i + 1) % 500 == 0:
      print(f"# {i + 1}/{args.trials} trials", file=sys.stderr, flush=True)

  if len(margins) < 2:
    sys.exit("no margins reported -- did the runs calibrate?")
  margins.sort()
  n = len(margins)

  def pct(q):
    return margins[min(n - 1, int(q * n))]

  print(f"# shape={args.shape} K={kcount} L={args.length} "
        f"--confidence {args.confidence} n={n}")
  print(f"# mean {sum(margins) / n:+.3f}  min {margins[0]:+.2f}  "
        f"max {margins[-1]:+.2f}")
  print(f"# percentiles: 50th {pct(.50):+.2f}  95th {pct(.95):+.2f}  "
        f"99th {pct(.99):+.2f}")
  for t in (0.0, 0.25, 0.5, 0.54, 1.0, 1.5, 2.0):
    k = sum(1 for v in margins if v >= t)
    print(f"  P(margin >= {t:+.2f}) = {k:>5}/{n} = {k / n:7.3%}")
  print("# every one of these is a FALSE positive: there is no plaintext.")


if __name__ == "__main__":
  main()
