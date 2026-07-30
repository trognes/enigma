#!/usr/bin/env python3
"""Main --gainfix-best result: matched-problem m4q10 L40 English R-sweep.

(A) mean %-correct gain over baseline; (B) exact-recovery gain before vs after the
finish-cap fix (the fix removes the high-R exact-loss). Scoring failures removed
(a non-exact trial whose recovered_score >= true_score -- the information floor no
search can cross; see archived/PERFORMANCE.md 1). The comparison is MATCHED-PROBLEM: the same
tsv m4q10 baselines were reused and only --gainfix-best re-run on the identical
problems, so the per-trial Delta is low-variance (unlike a cross-sample diff of the
committed fresh-sample .gfbest rows vs the baselines).

Data (committed alongside):
  eval/gainfix_best_matched_prefix.tsv  -- --gainfix-best BEFORE the finish-cap fix
  eval/gainfix_best_matched_capped.tsv  -- --gainfix-best AFTER  the finish-cap fix
Both carry per-trial: L R sf b_ex b_pct b_si g_ex g_pct g_si  (b_=baseline, g_=gfbest).

Usage: python3 eval/plot_gainfix_best_matched.py  ->  eval/plots/gainfix_best_matched.png
"""
import csv, os, collections, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "eval")
OUT = os.path.join(ROOT, "eval", "plots", "gainfix_best_matched.png")

plt.rcParams.update({
    "figure.dpi": 150, "font.size": 11, "axes.spines.top": False,
    "axes.spines.right": False, "axes.grid": True, "grid.color": "#e6e6e6",
    "grid.linewidth": 0.8, "axes.axisbelow": True, "axes.edgecolor": "#888888",
    "text.color": "#222222", "axes.labelcolor": "#222222",
    "xtick.color": "#555555", "ytick.color": "#555555",
})
BASE, GFB, PRE = "#0072B2", "#009E73", "#E69F00"   # Okabe-Ito: blue / green / orange


def load(fn):
    """R -> (mean base%, mean gfbest%, base exact%, gfbest exact%), scoring failures removed."""
    g = collections.defaultdict(lambda: {"bp": [], "gp": [], "bx": [], "gx": []})
    with open(os.path.join(DATA, fn)) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            if r["sf"] == "True":
                continue
            R = int(r["R"]); d = g[R]
            d["bp"].append(float(r["b_pct"])); d["gp"].append(float(r["g_pct"]))
            d["bx"].append(int(r["b_ex"])); d["gx"].append(int(r["g_ex"]))
    return {R: (st.mean(d["bp"]), st.mean(d["gp"]),
                100 * st.mean(d["bx"]), 100 * st.mean(d["gx"])) for R, d in g.items()}


cap = load("gainfix_best_matched_capped.tsv")
pre = load("gainfix_best_matched_prefix.tsv")
Rs = sorted(cap)

fig, (axA, axB) = plt.subplots(1, 2, figsize=(11.5, 4.6))
fig.suptitle("--gainfix-best vs baseline  —  matched problems, m4q10 L40 English "
             "(scoring failures removed)", fontsize=12.5, y=0.99)

axA.plot(Rs, [cap[R][0] for R in Rs], "-o", color=BASE, lw=2, ms=5, label="baseline")
axA.plot(Rs, [cap[R][1] for R in Rs], "-o", color=GFB, lw=2, ms=5,
         label="--gainfix-best (cap-fixed)")
axA.set_xscale("log", base=2)
axA.set_xlabel("restarts  -R  (log)")
axA.set_ylabel("mean % letters correct")
axA.set_title("(A) recovery vs restarts", fontsize=11)
axA.legend(frameon=False, loc="lower right")
for R in Rs:
    d = cap[R][1] - cap[R][0]
    if d >= 0.9:
        axA.annotate(f"+{d:.1f}", (R, cap[R][1]), textcoords="offset points",
                     xytext=(0, 7), ha="center", fontsize=8, color=GFB)

d_cap = [cap[R][3] - cap[R][2] for R in Rs]
d_pre = [pre[R][3] - pre[R][2] for R in Rs]
axB.axhspan(min(d_pre) - 1, 0, color="#f2c9a0", alpha=0.20, zorder=0)
axB.axhline(0, color="#999999", lw=1, ls="--", zorder=1)
axB.plot(Rs, d_pre, "-o", color=PRE, lw=2, ms=5, label="before cap fix (over-plugs)")
axB.plot(Rs, d_cap, "-o", color=GFB, lw=2, ms=5, label="after cap fix")
axB.set_xscale("log", base=2)
axB.set_xlabel("restarts  -R  (log)")
axB.set_ylabel("Δ exact recovery  (gfbest − base, pp)")
axB.set_title("(B) exact-recovery gain: the cap fix removes the high-R loss", fontsize=11)
axB.legend(frameon=False, loc="lower left")

fig.tight_layout(rect=(0, 0, 1, 0.96))
fig.savefig(OUT)
print("wrote", OUT)
