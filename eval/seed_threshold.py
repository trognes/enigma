"""Is a seed's score worth acting on?  The lift curve and what it costs.

Experiment F asked "generate k cheap seeds, promote the best m < k" and
measured it down at every k it tried.  That verdict is right for the rule it
tested and does NOT cover the whole design space: every cell F swept was
m = 1 (best of k = 2, 5, 10), which is the aggressive end, and the aggressive
end is where selection loses worst.  This script sweeps the MILD end.

Reads eval/results-experiment-f.csv, default arm only (10-plug kick, no -M).

THREE THINGS IT COMPUTES:

  LIFT -- P(break | cap-score bin).  Steep: the top 1% of seeds break 90% of
  the time against an 8.2% base.  This is what the scatter plot shows.

  COST -- breaks per unit compute, which inverts the lift at the top.  The
  cheap k4 stage is 33.6% of a candidate (ENHANCEMENTS 2a), so finding a
  top-1% seed costs 100 cheap stages: 0.026 breaks/unit against the base's
  0.082.  The most predictive seeds are the ones you cannot afford to find.

  WHERE THE GAIN COMES FROM -- an ABSOLUTE cut-off also declines to spend on
  hard keys, which is triage across keys rather than selection among a key's
  restarts (and is what -F already does one level up).  The per-key relative
  rule isolates the part that is really about choosing between restarts.

Usage:  python3 eval/seed_threshold.py [--boot N]
"""
import argparse, csv, random, statistics
from collections import defaultdict

ap = argparse.ArgumentParser()
ap.add_argument("--csv", default="eval/results-experiment-f.csv")
ap.add_argument("--boot", type=int, default=2000, help="bootstrap resamples")
ap.add_argument("--seed", type=int, default=7)
args = ap.parse_args()

# The cheap stage's share of one candidate, measured in-process (ENHANCEMENTS
# 2a).  Every verdict here depends on it: a cheaper pre-pass moves the optimum
# toward stronger selection, a dearer one toward none at all.
CHEAP, CONT = 0.336, 0.664

rows = [r for r in csv.DictReader(open(args.csv))
        if r["size"] == "10" and r["M"] == "0"]
per = defaultdict(list)
for r in rows:
    per[r["key"]].append((float(r["k4_score_f"]),
                          1 if float(r["f10_pct"]) >= 50 else 0))
keys = list(per)
flat = [t for s in per.values() for t in s]
base = sum(l for _, l in flat) / len(flat)
print("default arm: %d candidates over %d keys, base break rate %.2f%%"
      % (len(flat), len(keys), 100 * base))
print("cost model: cheap stage %.3f, continuation %.3f of a candidate\n"
      % (CHEAP, CONT))

# ---------------------------------------------------------------- lift
order = sorted(flat, key=lambda t: t[0])
n = len(order)
print("LIFT -- break rate by cap-score percentile (pooled over keys):")
print("  %-12s %8s %8s %7s" % ("percentile", "n", "break%", "lift"))
for lo, hi in ((0, 50), (50, 80), (80, 90), (90, 95), (95, 99), (99, 100)):
    seg = order[int(lo / 100 * n):int(hi / 100 * n)]
    br = sum(l for _, l in seg) / len(seg)
    print("  %-12s %8d %7.1f%% %6.1fx"
          % ("%d-%d" % (lo, hi), len(seg), 100 * br, br / base))

# ---------------------------------------------------- cost of acting on it
def eff_base(sample):
    b = c = 0.0
    for k in sample:
        b += sum(l for _, l in per[k]); c += len(per[k])
    return b / c

def eff_abs(sample, cut):
    """One cut-off for every key: also declines to spend on hard keys."""
    b = c = 0.0
    for k in sample:
        s = per[k]; kept = [l for v, l in s if v >= cut]
        b += sum(kept); c += len(s) * CHEAP + len(kept) * CONT
    return b / c

def eff_rel(sample, m):
    """Top m of THIS key's seeds: selection among restarts, nothing else."""
    b = c = 0.0
    for k in sample:
        s = sorted(per[k], key=lambda t: -t[0])[:m]
        b += sum(l for _, l in s); c += len(per[k]) * CHEAP + m * CONT
    return b / c

def eff_pairs(sample, rng):
    """Experiment F's rule at k = 2: promote the better of each PAIR."""
    b = c = 0.0
    for k in sample:
        sh = per[k][:]; rng.shuffle(sh)
        for i in range(0, len(sh) - 1, 2):
            b += max(sh[i], sh[i + 1], key=lambda t: t[0])[1]
            c += 2 * CHEAP + CONT
    return b / c

allv = sorted((v for v, _ in flat), reverse=True)
rng = random.Random(args.seed)

def boot(fn, *a):
    out = []
    for _ in range(args.boot):
        smp = [rng.choice(keys) for _ in keys]
        out.append(fn(smp, *a) / eff_base(smp) - 1)
    out.sort()
    return out[int(.025 * len(out))], out[int(.975 * len(out))]

print("\nCOST -- breaks per unit compute.  ABSOLUTE cut-off:")
print("  %-10s %9s %12s %9s %s" % ("keep", "P(brk|)", "breaks/unit",
                                   "vs base", "95% CI"))
for frac in (1.0, .5, .25, .1, .05, .01):
    cut = allv[min(len(allv) - 1, int(frac * len(allv)))]
    kept = [l for v, l in flat if v >= cut]
    e = eff_abs(keys, cut)
    lo, hi = boot(eff_abs, cut)
    print("  %-10s %8.1f%% %12.4f %+8.0f%%  [%+.0f%%, %+.0f%%]"
          % ("top %g%%" % (100 * frac), 100 * sum(kept) / len(kept), e,
             100 * (e / base - 1), 100 * lo, 100 * hi))

print("\nPER-KEY relative cut -- the part that is really seed selection:")
print("  %-10s %12s %9s %s" % ("keep m of 40", "breaks/unit", "vs base",
                               "95% CI"))
for m in (40, 20, 12, 8, 4, 1):
    e = eff_rel(keys, m)
    lo, hi = boot(eff_rel, m)
    print("  %-10s %12.4f %+8.0f%%  [%+.0f%%, %+.0f%%]"
          % (m, e, 100 * (e / base - 1), 100 * lo, 100 * hi))

# ------------------------------------------- why F's k=2 read differently
e = eff_pairs(keys, random.Random(args.seed))
print("\nF's RULE AT THE SAME KEEP-RATIO, for comparison:")
print("  best of each pair (k=2)  %.4f  %+.0f%%" % (e, 100 * (e / base - 1)))
print("  top HALF of the key      %.4f  %+.0f%%"
      % (eff_rel(keys, 20), 100 * (eff_rel(keys, 20) / base - 1)))
print("  Pairing wastes selection: two strong seeds in one pair promote only")
print("  one, two weak ones promote one anyway.")

# --------------------------------------------- pooled vs within-key AUC
def auc(s):
    pos = sum(l for _, l in s); neg = len(s) - pos
    if not pos or not neg:
        return None
    tot = 0.0
    for rank, (_, l) in enumerate(sorted(s, key=lambda t: t[0]), start=1):
        if l:
            tot += rank
    return (tot - pos * (pos + 1) / 2) / (pos * neg)

within = [a for a in (auc(s) for s in per.values()) if a is not None]
gm = statistics.mean(v for v, _ in flat)
betw = sum(len(s) * (statistics.mean(v for v, _ in s) - gm) ** 2
           for s in per.values())
tot = sum((v - gm) ** 2 for v, _ in flat)
print("\nWHERE THE DISCRIMINATION LIVES:")
print("  pooled AUC                %.3f" % auc(flat))
print("  within-key AUC            %.3f   (%d keys)"
      % (statistics.mean(within), len(within)))
print("  cap-score variance that is between keys: %.0f%%" % (100 * betw / tot))
