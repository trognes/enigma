#!/usr/bin/env python3
"""Summarise the paired A/B: means, the paired difference, and a CI on it."""
import json
import math
import sys

rows = [json.loads(l) for l in open(sys.argv[1])]
n = len(rows)
b = [r["B_restarts"] for r in rows]
a = [r["A_tunephase"] for r in rows]

bp = [x["pct"] for x in b]
ap = [x["pct"] for x in a]
d = [x - y for x, y in zip(ap, bp)]            # A minus B
mean = sum(d) / n
var = sum((x - mean) ** 2 for x in d) / (n - 1) if n > 1 else 0.0
se = math.sqrt(var / n)
lo, hi = mean - 1.96 * se, mean + 1.96 * se

print("trials: %d   wall: B %.0fs  A %.0fs (per trial, mean)"
      % (n, sum(x["sec"] for x in b) / n, sum(x["sec"] for x in a) / n))
print()
print("                      mean %%-correct    exact")
print("  B  restarts, full     %6.1f          %2d/%d" %
      (sum(bp) / n, sum(x["exact"] for x in b), n))
print("  A  --tune-phase 2     %6.1f          %2d/%d" %
      (sum(ap) / n, sum(x["exact"] for x in a), n))
print()
print("paired difference A-B: %+.1f pp   95%% CI [%+.1f, %+.1f]" % (mean, lo, hi))

# discordant pairs: where exactly one arm recovered exactly (McNemar's view)
only_a = sum(1 for x, y in zip(a, b) if x["exact"] and not y["exact"])
only_b = sum(1 for x, y in zip(a, b) if y["exact"] and not x["exact"])
both = sum(1 for x, y in zip(a, b) if x["exact"] and y["exact"])
neither = sum(1 for x, y in zip(a, b) if not x["exact"] and not y["exact"])
print("exact recovery: both %d   only A %d   only B %d   neither %d"
      % (both, only_a, only_b, neither))
if only_a + only_b:
    # exact binomial two-sided p for the discordant split
    k, m = min(only_a, only_b), only_a + only_b
    p = 2 * sum(math.comb(m, i) for i in range(k + 1)) / 2 ** m
    print("McNemar exact p = %.3f (on %d discordant pairs)" % (min(p, 1.0), m))
