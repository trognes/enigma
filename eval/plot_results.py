#!/usr/bin/env python3
"""Plot performance graphs from eval/results.tsv. Writes PNGs to eval/plots/.
Usage: python3 eval/plot_results.py"""
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TSV = os.path.join(ROOT, "eval", "results.tsv")
OUT = os.path.join(ROOT, "eval", "plots")
os.makedirs(OUT, exist_ok=True)

# CVD-safe Okabe-Ito categorical hues, fixed order (never cycled).
COL = {"english": "#0072B2", "german": "#009E73",
       "french": "#E69F00", "danish": "#CC79A7"}
LANG_ORDER = ["english", "french", "german", "danish"]

plt.rcParams.update({
    "figure.dpi": 150, "font.size": 11, "axes.spines.top": False,
    "axes.spines.right": False, "axes.grid": True, "grid.color": "#e6e6e6",
    "grid.linewidth": 0.8, "axes.axisbelow": True, "axes.edgecolor": "#888888",
    "text.color": "#222222", "axes.labelcolor": "#222222",
    "xtick.color": "#555555", "ytick.color": "#555555",
})

rows = list(csv.DictReader(open(TSV), delimiter="\t"))
for r in rows:
    r["length"] = int(r["length"])
    r["pct"] = float(r["letters_matched_pct"])
    r["exact"] = int(r["exact_match"])


def curve(pred, value):
    """Mean of `value` per length over rows matching pred -> (lengths, means)."""
    agg = defaultdict(list)
    for r in rows:
        if pred(r):
            agg[r["length"]].append(value(r))
    xs = sorted(agg)
    return xs, [sum(agg[x]) / len(agg[x]) for x in xs]


def line_chart(series, title, ylabel, fname, ylim=(0, 102), xlim=(35, 320)):
    """series: list of (label, color, xs, ys)."""
    fig, ax = plt.subplots(figsize=(8, 5))
    for label, color, xs, ys in series:
        ax.plot(xs, ys, "-o", color=color, lw=2, ms=5, label=label,
                markeredgecolor="white", markeredgewidth=0.6)
    # lines converge at 100%, so direct end-labels collide -> legend carries identity
    ax.set_xlabel("message length (letters)")
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=13, fontweight="bold", pad=12)
    ax.set_ylim(*ylim)
    ax.set_xlim(*xlim)
    ax.legend(frameon=False, loc="lower right", fontsize=9)
    ax.margins(x=0.12)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, fname), bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote", fname)


GREEDY = "-J-Si4q10-R10"

# 1. cross-language mean %-correct (quad greedy, main grid)
line_chart(
    [(lang, COL[lang], *curve(lambda r, L=lang: r["config_label"] == GREEDY
                              and r["language"] == L, lambda r: r["pct"]))
     for lang in LANG_ORDER],
    "Plugboard recovery by language (quad, greedy climb, 10 plugs)",
    "letters correct (mean %)", "recovery_by_language.png")

# 2. cross-language exact-recovery rate
line_chart(
    [(lang, COL[lang], *curve(lambda r, L=lang: r["config_label"] == GREEDY
                              and r["language"] == L, lambda r: 100.0 * r["exact"]))
     for lang in LANG_ORDER],
    "Exact recovery by language (quad, greedy climb, 10 plugs)",
    "messages fully recovered (%)", "exact_by_language.png")

# 3. genuine telegraphic German vs prose
genuine = "-J-Si4q10-R10.genuine"
line_chart(
    [("prose", COL["german"], *curve(lambda r: r["config_label"] == GREEDY
                                     and r["language"] == "german", lambda r: r["pct"])),
     ("Doenitz 1945", "#0072B2", *curve(lambda r: r["corpus"] == "doenitz1945", lambda r: r["pct"])),
     ("1930 manual", "#D55E00", *curve(lambda r: r["corpus"] == "manual1930", lambda r: r["pct"]))],
    "Genuine telegraphic German vs prose (quad, greedy, 10 plugs)",
    "letters correct (mean %)", "genuine_german.png")

# 4. transliteration: multi vs single letter (german + danish)
line_chart(
    [("german multi (ae)", COL["german"],
      *curve(lambda r: r["corpus"] in ("wald", "reise", "wissenschaft")
             and r["config_label"].endswith("translit"), lambda r: r["pct"])),
     ("german single (a)", "#8fd3bf",
      *curve(lambda r: r["corpus"] in ("wald_sl", "reise_sl", "wissenschaft_sl"), lambda r: r["pct"])),
     ("danish multi (aa)", COL["danish"],
      *curve(lambda r: r["corpus"] == "danmark", lambda r: r["pct"])),
     ("danish single (a)", "#e6c3d8",
      *curve(lambda r: r["corpus"] == "danmark_sl", lambda r: r["pct"]))],
    "Transliteration convention: multi- vs single-letter (quad, greedy)",
    "letters correct (mean %)", "transliteration.png", xlim=(45, 100))

# 5. quad vs trigram, small multiples per language (same greedy climb, 5 shared lengths)
TRIG = "i4t10.R10.J"
TRI_LENS = {50, 90, 120, 160, 200}
MODELCOL = {"quad": "#0072B2", "trigram": "#E69F00"}
fig, axes = plt.subplots(2, 2, figsize=(10, 7), sharex=True, sharey=True)
for ax, lang in zip(axes.flat, LANG_ORDER):
    for mlabel, cfg in [("quad", GREEDY), ("trigram", TRIG)]:
        xs, ys = curve(lambda r, L=lang, C=cfg: r["config_label"] == C
                       and r["language"] == L and r["length"] in TRI_LENS, lambda r: r["pct"])
        ax.plot(xs, ys, "-o", color=MODELCOL[mlabel], lw=2, ms=5, label=mlabel,
                markeredgecolor="white", markeredgewidth=0.6)
    ax.set_title(lang, fontsize=11, fontweight="bold")
    ax.set_ylim(0, 102)
    ax.grid(True, color="#e6e6e6", linewidth=0.8)
    ax.set_axisbelow(True)
axes[1, 0].set_xlabel("message length (letters)")
axes[1, 1].set_xlabel("message length (letters)")
axes[0, 0].set_ylabel("letters correct (mean %)")
axes[1, 0].set_ylabel("letters correct (mean %)")
axes[0, 0].legend(frameon=False, loc="lower right", fontsize=9)
fig.suptitle("Quad vs trigram model, by language (greedy climb, 10 plugs)",
             fontsize=13, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "quad_vs_trigram.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote quad_vs_trigram.png")

# 6. climb variants at matched compute: mean %-correct vs score_iter (english)
STEEP = "i4q10.R10.steepest"
FI = "i4q10.R10.I"
fig, ax = plt.subplots(figsize=(8, 5))
for clabel, cfg, color, labelpts in [
        ("-I  first-improvement", FI, "#0072B2", False),
        ("-J  (+ dynamic order)", GREEDY, "#009E73", False),
        ("steepest ascent", STEEP, "#D55E00", True)]:
    agg = defaultdict(lambda: [0.0, 0.0, 0])  # length -> [sum score_iter, sum pct, n]
    for r in rows:
        if r["config_label"] == cfg and r["language"] == "english" and r["score_iter"]:
            a = agg[r["length"]]
            a[0] += float(r["score_iter"]); a[1] += r["pct"]; a[2] += 1
    pts = sorted((a[1] / a[2], a[0] / a[2], L) for L, a in agg.items())  # by mean%
    ys = [p[0] for p in pts]; xs = [p[1] / 1000 for p in pts]
    ax.plot(xs, ys, "-o", color=color, lw=2, ms=5, label=clabel,
            markeredgecolor="white", markeredgewidth=0.6)
    if labelpts:  # length labels on one band orient the whole chart
        for pct, si, L in pts:
            if L in (50, 90, 160, 300):
                ax.annotate("L%d" % L, (si / 1000, pct), xytext=(6, -3),
                            textcoords="offset points", color=color, fontsize=8)
ax.set_xlabel("compute — mean score_iter per message (thousands)")
ax.set_ylabel("letters correct (mean %)")
ax.set_title("Climb variants at matched compute (english, quad, 10 plugs)",
             fontsize=13, fontweight="bold", pad=12)
ax.set_ylim(0, 102); ax.set_xlim(0, 62)
ax.legend(frameon=False, loc="upper left", fontsize=9)
ax.annotate("first-improvement (-I/-J): same\nrecovery at ~1/3 the compute",
            (20, 72), (33, 52), textcoords="data", fontsize=9, color="#555555",
            arrowprops=dict(arrowstyle="->", color="#999999"))
fig.tight_layout()
fig.savefig(os.path.join(OUT, "greedy_vs_steepest_compute.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote greedy_vs_steepest_compute.png")

print("done ->", OUT)
