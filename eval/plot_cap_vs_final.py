"""Scatter: the score at the cap against the score after the full climb.

Reads eval/results-experiment-f.csv (80 000 candidates: 500 keys x 40 kicks x
{4,10}-plug kick x -M off/on).  Each candidate is a k4 cap-4 climb followed by
the f10 continuation, so it carries both scores and the outcome.

This is the picture behind experiment F's central number.  If the cap score
predicted the final score well, promoting only the best-scoring seeds would be
a cheap win; it does not (AUC ~0.62-0.79 by arm), and selection loses at
matched budget.

TWO CHOICES WORTH KNOWING:

  x is the cap board RESCORED WITH f by default, not its own k score, so both
  axes are the same statistic and the y = x line means something.  --xk plots
  the k score the pre-pass actually optimises instead; the two rank almost
  equally well (0.687 vs 0.677 AUC overall), which was itself a finding.

  BROKEN TRIALS ARE DRAWN LAST AND OPAQUE.  They are 5-9% of the data, so
  painting them in draw order would bury them under 20 000 grey points and the
  plot would show only where candidates are, not where breaks are.

Usage:  python3 eval/plot_cap_vs_final.py [--xk] [-o FILE]
"""
import argparse, csv, statistics, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("--xk", action="store_true",
                help="x axis = the cap's own k score, not the f rescore")
ap.add_argument("-o", "--out", default="eval/cap-vs-final.png")
ap.add_argument("--csv", default="eval/results-experiment-f.csv")
args = ap.parse_args()

rows = list(csv.DictReader(open(args.csv)))
xcol = "k4_score_k" if args.xk else "k4_score_f"
xlab = ("cap-4 seed, k score (what the pre-pass optimises)" if args.xk
        else "cap-4 seed, rescored with f")

ARMS = [("10", "0"), ("10", "1"), ("4", "0"), ("4", "1")]
TITLE = {("10", "0"): "10-plug kick (default)", ("10", "1"): "10-plug kick, -M",
         ("4", "0"): "4-plug kick",             ("4", "1"): "4-plug kick, -M"}

def pearson(a, b):
    ma, mb = statistics.mean(a), statistics.mean(b)
    num = sum((p - ma) * (q - mb) for p, q in zip(a, b))
    den = (sum((p - ma) ** 2 for p in a) * sum((q - mb) ** 2 for q in b)) ** .5
    return num / den if den else float("nan")

def auc(score, label):
    """Rank AUC: P(a broken trial outscores an unbroken one)."""
    pos = sum(label); neg = len(label) - pos
    if not pos or not neg:
        return float("nan")
    tot = 0.0
    for rank, (_, lab) in enumerate(sorted(zip(score, label)), start=1):
        if lab:
            tot += rank
    return (tot - pos * (pos + 1) / 2) / (pos * neg)

# muted ink for misses, the status-critical step for breaks
MISS, HIT = "#8d8b84", "#d03b3b"
fig, axes = plt.subplots(2, 2, figsize=(11, 9.6), sharex=True, sharey=True)
fig.patch.set_facecolor("white")

xall = [float(r[xcol]) for r in rows]
yall = [float(r["f10_score_f"]) for r in rows]
xlo, xhi = min(xall), max(xall)
ylo, yhi = min(yall), max(yall)

for ax, arm in zip(axes.flat, ARMS):
    rs = [r for r in rows if (r["size"], r["M"]) == arm]
    x = [float(r[xcol]) for r in rs]
    y = [float(r["f10_score_f"]) for r in rs]
    lab = [1 if float(r["f10_pct"]) >= 50 else 0 for r in rs]

    ax.scatter([p for p, l in zip(x, lab) if not l],
               [q for q, l in zip(y, lab) if not l],
               s=2, c=MISS, alpha=.10, linewidths=0, rasterized=True)
    ax.scatter([p for p, l in zip(x, lab) if l],
               [q for q, l in zip(y, lab) if l],
               s=3.5, c=HIT, alpha=.55, linewidths=0, rasterized=True)

    if not args.xk:   # same statistic on both axes, so y = x is meaningful
        lo = min(xlo, ylo); hi = max(xhi, yhi)
        ax.plot([lo, hi], [lo, hi], color="#b8b6ae", lw=1,
                ls=(0, (4, 3)), zorder=0)

    ax.set_title(TITLE[arm], fontsize=11, pad=8, loc="left", color="#0b0b0b")
    # boxed: this line sits over the broken cloud in three of the four panels
    ax.text(.03, .955, "r = %.2f     AUC = %.2f     broke %.1f%%"
            % (pearson(x, y), auc(x, lab), 100 * sum(lab) / len(lab)),
            transform=ax.transAxes, fontsize=9, color="#52514e", va="top",
            bbox=dict(facecolor="white", edgecolor="none", alpha=.82,
                      boxstyle="round,pad=.35"))
    ax.set_xlim(xlo - .1, xhi + .1)
    ax.set_ylim(ylo - .1, yhi + .1)
    ax.grid(True, color="#e7e5df", lw=.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color("#c9c7c0")
    ax.tick_params(colors="#52514e", labelsize=9)

for ax in axes[1]:
    ax.set_xlabel(xlab, fontsize=10, color="#52514e")
for ax in axes[:, 0]:
    ax.set_ylabel("after the full f10 climb", fontsize=10, color="#52514e")

h = [plt.Line2D([], [], marker="o", ls="", ms=5, mfc=c, mec="none", label=t)
     for c, t in ((MISS, "not broken"), (HIT, "broken (≥50% of letters)"))]
if not args.xk:
    h.append(plt.Line2D([], [], color="#b8b6ae", lw=1, ls=(0, (4, 3)),
                        label="y = x (the climb never scores worse)"))
fig.legend(handles=h, loc="lower center", ncol=3, frameon=False,
           fontsize=9.5, bbox_to_anchor=(.5, .012), labelcolor="#52514e")

fig.suptitle("Does the seed's score predict the final climb?",
             fontsize=15, x=.055, ha="left", y=.975, color="#0b0b0b")
fig.text(.055, .945,
         "80 000 candidates, L=100 telegraphic German. Each point is one "
         "restart: its cap-4 seed, then the f10 continuation.",
         fontsize=10, color="#52514e", ha="left")
fig.tight_layout(rect=(0, .045, 1, .935))
fig.savefig(args.out, dpi=170)
print("wrote", args.out, file=sys.stderr)
