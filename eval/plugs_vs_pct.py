#!/usr/bin/env python3
#
# Correct-plugs -> text-recovery relationship (PERFORMANCE.md section 6.11).
#
# Aggregates every plugboard-recovery row across all eval/results*.tsv shards and
# plots mean % letters correct as a function of the number of correctly recovered
# plugs (of 10 true). For each row:  #correct = |recovered_plugs ∩ true_plugs|,
# paired with letters_matched_pct. Also splits out boards with NO spurious plugs
# (recovered ⊆ true) to show that spurious plugs drag recovery down at fixed #correct.
#
# The curve is strongly CONVEX (bows below the linear chord): each correct plug is
# worth more than the last until near-complete, because the plugboard is applied
# TWICE on each letter's path -- a position decodes correctly only if both its
# contacts route right. This is the shape underneath the "6-8 correct-plug basin
# gap": text gain per plug is tiny at low #correct, so the (text-driven) climb score
# has little gradient until ~5-6 plugs are right.
#
# Usage:  python3 eval/plugs_vs_pct.py        # writes eval/plots/correct_plugs_vs_pct.png

import glob, csv, collections, statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def pset(s):
    return frozenset(frozenset(p) for p in s.split()) if s and s.strip() else frozenset()


by = collections.defaultdict(list)      # #correct -> [pct]
by_nospur = collections.defaultdict(list)
for f in glob.glob("eval/results*.tsv"):
    try:
        rdr = csv.DictReader(open(f), delimiter='\t')
    except OSError:
        continue
    for d in rdr:
        tp = d.get('true_plugs', '')
        if not tp:
            continue
        try:
            pct = float(d['letters_matched_pct'])
        except (ValueError, KeyError):
            continue
        rec, true = pset(d.get('recovered_plugs', '')), pset(tp)
        nc, nsp = len(rec & true), len(rec - true)
        by[nc].append(pct)
        if nsp == 0:
            by_nospur[nc].append(pct)

ks = list(range(11))
mean = [statistics.mean(by[k]) if by[k] else float('nan') for k in ks]
sd = [statistics.pstdev(by[k]) if by[k] else 0.0 for k in ks]
nsk = [k for k in ks if len(by_nospur.get(k, [])) >= 30]
nsm = [statistics.mean(by_nospur[k]) for k in nsk]

total = sum(len(by[k]) for k in ks)
print(f"{'#correct':>8} {'n':>8} {'mean%':>7} {'std':>6}")
for k in ks:
    print(f"{k:>8} {len(by[k]):>8} {mean[k]:7.1f} {sd[k]:6.1f}")
print(f"total rows: {total}")

fig, ax = plt.subplots(figsize=(9, 5.6))
ax.fill_between(ks, [m - s for m, s in zip(mean, sd)], [m + s for m, s in zip(mean, sd)],
                color='#1f5fa8', alpha=.12, label='±1 std (per-run spread)')
ax.plot(ks, mean, '-D', color='#1f5fa8', lw=2.2, ms=7, label='mean % letters correct (all runs)')
ax.plot(nsk, nsm, '--s', color='#2e8b57', lw=1.8, ms=6, label='mean, boards with NO spurious plugs')
ax.plot([0, 10], [mean[0], mean[10]], ':', color='#b0b0b0', lw=1.5, label='linear reference (chord)')
for k in ks:
    ax.annotate(f"{mean[k]:.0f}", (k, mean[k]), textcoords="offset points",
                xytext=(0, 8), ha='center', fontsize=8, color='#1f5fa8')
ax.set_xticks(ks)
ax.set_xlabel("number of correct plugs recovered (of 10 true)")
ax.set_ylabel("mean % letters correct")
ax.set_ylim(0, 104)
ax.grid(alpha=.3)
ax.legend(frameon=False, fontsize=9, loc='upper left')
ax.set_title(f"Correct plugs vs text recovery  ({total//1000}k plugboard-recovery runs, all lengths/langs/configs)\n"
             "strongly convex: the curve bows below the chord — the last few plugs carry most of the text",
             fontsize=10.5)
fig.tight_layout()
fig.savefig("eval/plots/correct_plugs_vs_pct.png", dpi=110)
print("wrote eval/plots/correct_plugs_vs_pct.png")
