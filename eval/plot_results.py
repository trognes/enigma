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

print("done ->", OUT)
