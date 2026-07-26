#!/usr/bin/env python3
"""--gainfix-best vs baseline across the full L40 restart sweep (R=20..81920),
English and German, matched-problem (same m4q10 tsv baselines reused, only
--gainfix-best re-run on the identical problems -> low-variance Delta, not a
cross-sample diff of the fresh-sample .gfbest corpus rows). Scoring failures
removed (a non-exact trial whose recovered_score >= true_score -- the
information floor no search can cross; PERFORMANCE.md 1). Cap-fixed build.

2x2: columns = language, rows = {mean %-correct, exact recovery}, each panel
base vs --gainfix-best.

Data (committed alongside):
  eval/gainfix_best_matched_capped.tsv         -- English, cap-fixed
  eval/gainfix_best_matched_german_capped.tsv  -- German,  cap-fixed
per-trial cols: L R sf b_ex b_pct b_si g_ex g_pct g_si  (b_=baseline, g_=gfbest)

Usage: python3 eval/plot_gainfix_best_en_de.py -> eval/plots/gainfix_best_en_de.png
"""
import csv, os, collections, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "eval")
OUT = os.path.join(ROOT, "eval", "plots", "gainfix_best_en_de.png")

plt.rcParams.update({
    "figure.dpi": 150, "font.size": 11, "axes.spines.top": False,
    "axes.spines.right": False, "axes.grid": True, "grid.color": "#e6e6e6",
    "grid.linewidth": 0.8, "axes.axisbelow": True, "axes.edgecolor": "#888888",
    "text.color": "#222222", "axes.labelcolor": "#222222",
    "xtick.color": "#555555", "ytick.color": "#555555",
})
BASE, GFB = "#0072B2", "#009E73"   # Okabe-Ito: blue = baseline, green = --gainfix-best


def load(fn):
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


langs = [("English", load("gainfix_best_matched_capped.tsv")),
         ("German", load("gainfix_best_matched_german_capped.tsv"))]

fig, axes = plt.subplots(2, 2, figsize=(11.5, 8.0), sharex=True)
fig.suptitle("--gainfix-best vs baseline  —  L40 restart sweep, matched problems "
             "(scoring failures removed)", fontsize=13, y=0.995)

for col, (name, d) in enumerate(langs):
    Rs = sorted(d)
    # row 0: mean %-correct ; row 1: exact recovery
    for row, (bi, gi, ylab) in enumerate([(0, 1, "mean % letters correct"),
                                          (2, 3, "exact recovery (%)")]):
        ax = axes[row][col]
        ax.plot(Rs, [d[R][bi] for R in Rs], "-o", color=BASE, lw=2, ms=4.5, label="baseline")
        ax.plot(Rs, [d[R][gi] for R in Rs], "-o", color=GFB, lw=2, ms=4.5,
                label="--gainfix-best")
        ax.set_xscale("log", base=2)
        for R in Rs:                       # annotate the clearer gains
            delta = d[R][gi] - d[R][bi]
            if delta >= 1.5:
                ax.annotate(f"+{delta:.1f}", (R, d[R][gi]), textcoords="offset points",
                            xytext=(0, 7), ha="center", fontsize=7.5, color=GFB)
        if row == 0:
            ax.set_title(name, fontsize=12)
        if row == 1:
            ax.set_xlabel("restarts  -R  (log)")
        if col == 0:
            ax.set_ylabel(ylab)
        if row == 0 and col == 0:
            ax.legend(frameon=False, loc="lower right")

fig.tight_layout(rect=(0, 0, 1, 0.97))
fig.savefig(OUT)
print("wrote", OUT)
