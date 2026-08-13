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
print("                      mean %-correct    exact")
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

# The two metrics above can disagree, and when they do it is the SHAPE of the
# failures that explains it, not their number: arm B always holds the true rotor
# key in its keyspace, so a miss is a plugboard miss that still returns most of
# the letters, while arm A can settle on a wrong offset, which nothing
# downstream repairs.  A "catastrophic" miss is a non-exact trial under 20%
# correct -- comfortably below the 76-98% band a plugboard miss occupies, and
# well above the ~4% a wrong offset gives, so the split is not sensitive to the
# threshold.  §7.15 computed this table by hand; it is printed here so a re-run
# at another length reports it the same way.
CATASTROPHIC = 20.0
print()
print("failure shapes (non-exact trials; catastrophic = under %.0f%% correct)"
      % CATASTROPHIC)
print("                      misses   catastrophic   partial   partial mean")
for name, arm in (("B  restarts, full  ", b), ("A  --tune-phase 2  ", a)):
    miss = [x["pct"] for x in arm if not x["exact"]]
    cat = [x for x in miss if x < CATASTROPHIC]
    part = [x for x in miss if x >= CATASTROPHIC]
    print("  %s   %3d      %3d           %3d       %s"
          % (name, len(miss), len(cat), len(part),
             "%5.1f%%" % (sum(part) / len(part)) if part else "    --"))
