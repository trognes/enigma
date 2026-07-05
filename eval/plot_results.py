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
ax.set_title("Compute per climb, by variant (english, quad, 10 plugs, R=10)",
             fontsize=13, fontweight="bold", pad=12)
ax.set_ylim(0, 102); ax.set_xlim(0, 62)
ax.legend(frameon=False, loc="upper left", fontsize=9)
ax.annotate("-I/-J run each climb at ~1/3-2/5 the compute\nof steepest. At fixed R=10 steepest recovers a\nbit MORE per length (see recovery-vs-length);\nthe -I/-J win needs those cycles spent on more R.",
            (24, 46), textcoords="data", fontsize=8.5, color="#555555")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "greedy_vs_steepest_compute.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote greedy_vs_steepest_compute.png")

# 7. climb variants: recovery vs length (english), ignoring compute
line_chart(
    [("-I  first-improvement", "#0072B2",
      *curve(lambda r: r["config_label"] == FI and r["language"] == "english", lambda r: r["pct"])),
     ("-J  (+ dynamic order)", "#009E73",
      *curve(lambda r: r["config_label"] == GREEDY and r["language"] == "english", lambda r: r["pct"])),
     ("steepest ascent", "#D55E00",
      *curve(lambda r: r["config_label"] == STEEP and r["language"] == "english", lambda r: r["pct"]))],
    "Climb variants: recovery vs length (english, quad, 10 plugs, R=10)",
    "letters correct (mean %)", "climb_variants_recovery.png")

# 8. MATCHED-COMPUTE recovery vs length: -I R32, -J R24, steepest R10 (all ~55k)
line_chart(
    [("-I  R=32", "#0072B2",
      *curve(lambda r: r["config_label"] == "i4q10.R32.I" and r["language"] == "english", lambda r: r["pct"])),
     ("-J  R=24", "#009E73",
      *curve(lambda r: r["config_label"] == "i4q10.R24.J" and r["language"] == "english", lambda r: r["pct"])),
     ("steepest  R=10", "#D55E00",
      *curve(lambda r: r["config_label"] == STEEP and r["language"] == "english", lambda r: r["pct"]))],
    "Recovery vs length at MATCHED compute (~55k score_iter; english, quad, 10 plugs)",
    "letters correct (mean %)", "matched_compute_recovery.png")

# 9. matched-compute recovery per language (2x2 small multiples, pooled 2 seeds)
MC = [("-I  R=32", "i4q10.R32.I", "#0072B2"),
      ("-J  R=24", "i4q10.R24.J", "#009E73"),
      ("steepest  R=10", "i4q10.R10.steepest", "#D55E00")]
MC_LENS = {50, 70, 90, 120, 160}
fig, axes = plt.subplots(2, 2, figsize=(10, 7.5), sharex=True, sharey=True)
for ax, lang in zip(axes.flat, LANG_ORDER):
    for clabel, cfg, color in MC:
        xs, ys = curve(lambda r, L=lang, C=cfg: r["config_label"] == C
                       and r["language"] == L and r["length"] in MC_LENS, lambda r: r["pct"])
        ax.plot(xs, ys, "-o", color=color, lw=2, ms=5, label=clabel,
                markeredgecolor="white", markeredgewidth=0.6)
    ax.set_title(lang, fontsize=11, fontweight="bold")
    ax.set_ylim(0, 102)
    ax.grid(True, color="#e6e6e6", linewidth=0.8)
    ax.set_axisbelow(True)
for ax in axes[1]:
    ax.set_xlabel("message length (letters)")
for ax in (axes[0, 0], axes[1, 0]):
    ax.set_ylabel("letters correct (mean %)")
axes[0, 0].legend(frameon=False, loc="lower right", fontsize=8.5)
fig.suptitle("Matched-compute recovery, per language (~55k score_iter, quad, 10 plugs, 2 seeds)",
             fontsize=13, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "matched_compute_by_language.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote matched_compute_by_language.png")

# 10. EQUAL-restart recovery per language: steepest / -I / -J all at R=10.
# Same restart budget for all three (NOT matched compute): steepest costs the
# most score_iter per climb, -I ~2.8x less, -J ~1.24x -I. Both solver seeds
# pooled (all three configs share a matched 2-seed grid at these lengths).
EQ = [("steepest  R=10", "i4q10.R10.steepest", "#D55E00"),
      ("-I  R=10", "i4q10.R10.I", "#0072B2"),
      ("-J  R=10", "-J-Si4q10-R10", "#009E73")]
EQ_LENS = {50, 70, 90, 120, 160}
fig, axes = plt.subplots(2, 2, figsize=(10, 7.5), sharex=True, sharey=True)
for ax, lang in zip(axes.flat, LANG_ORDER):
    for clabel, cfg, color in EQ:
        xs, ys = curve(lambda r, L=lang, C=cfg: r["config_label"] == C
                       and r["language"] == L and r["length"] in EQ_LENS,
                       lambda r: r["pct"])
        ax.plot(xs, ys, "-o", color=color, lw=2, ms=5, label=clabel,
                markeredgecolor="white", markeredgewidth=0.6)
    ax.set_title(lang, fontsize=11, fontweight="bold")
    ax.set_ylim(0, 102)
    ax.grid(True, color="#e6e6e6", linewidth=0.8)
    ax.set_axisbelow(True)
for ax in axes[1]:
    ax.set_xlabel("message length (letters)")
for ax in (axes[0, 0], axes[1, 0]):
    ax.set_ylabel("letters correct (mean %)")
axes[0, 0].legend(frameon=False, loc="lower right", fontsize=8.5)
fig.suptitle("Equal-restart recovery, per language (R=10 all three, quad, 10 plugs, 2 seeds)",
             fontsize=13, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "equal_restart_by_language.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote equal_restart_by_language.png")

# 11. IC pre-pass cap sweep: recovery vs N for -J -S iNq10 -R 10, N=1..8.
# Ordered magnitude (message length) -> single-hue sequential ramp, light->dark;
# lines are flat and vertically separated by length, so direct end-labels
# (no legend). All four languages pooled (the effect is language-independent);
# compute is flat across N (~5%), so this is a matched-compute sweep.
IC_LENS = [40, 50, 60, 70, 80, 90, 120, 160]
IC_NS = list(range(1, 9))
ic = defaultdict(list)
for r in rows:
    cl = r["config_label"]
    if len(cl) > 1 and cl[0] == "i" and cl[1:2].isdigit() and cl.endswith("q10.R10.J"):
        ic[(int(cl[1]), r["length"])].append(r["pct"])
ramp = [plt.cm.Blues(v) for v in
        [0.30, 0.42, 0.53, 0.63, 0.73, 0.82, 0.90, 1.0]]
fig, ax = plt.subplots(figsize=(8.5, 5.5))
for L, color in zip(IC_LENS, ramp):
    ys = [sum(ic[(N, L)]) / len(ic[(N, L)]) for N in IC_NS]
    ax.plot(IC_NS, ys, "-o", color=color, lw=2, ms=5,
            markeredgecolor="white", markeredgewidth=0.6)
    ax.annotate(f"L{L}", (IC_NS[-1], ys[-1]), xytext=(6, 0),
                textcoords="offset points", va="center", fontsize=8.5,
                color=color, fontweight="bold")
ax.set_xlabel("IC pre-pass plug cap  N   (-S iNq10)")
ax.set_ylabel("letters correct (mean %)")
ax.set_title("IC pre-pass cap is inert: recovery vs cap N\n"
             "(-J -S iNq10 -R 10, quad, 10 plugs, all languages pooled, 2 seeds)",
             fontsize=12, fontweight="bold", pad=10)
ax.set_xticks(IC_NS)
ax.set_xlim(0.7, 8.8)
ax.set_ylim(0, 102)
ax.margins(x=0.05)
fig.tight_layout()
fig.savefig(os.path.join(OUT, "ic_cap_sweep.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote ic_cap_sweep.png")

print("done ->", OUT)
