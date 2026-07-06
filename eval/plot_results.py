#!/usr/bin/env python3
"""Plot performance graphs from the eval log. Writes PNGs to eval/plots/.
Reads eval/results.tsv AND every eval/results-<timestamp>.tsv shard (new eval
batches are written to their own timestamped file to keep any one file under
GitHub's 100MB limit -- see eval/README.md). Usage: python3 eval/plot_results.py"""
import csv
import glob
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

def _load(path):
    with open(path) as f:
        return list(csv.DictReader(f, delimiter="\t"))


# results.tsv + all timestamped shards (eval/results-YYYYMMDD-HHMMSS.tsv)
shards = [TSV] + sorted(glob.glob(os.path.join(ROOT, "eval", "results-*.tsv")))
rows = [r for p in shards if os.path.exists(p) for r in _load(p)]
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

# 11. IC pre-pass cap sweep: recovery vs N for -J -S iNq10 -R 10, N=1..10.
# Ordered magnitude (message length) -> single-hue sequential ramp, light->dark;
# lines are flat and vertically separated by length, so direct end-labels
# (no legend). All four languages pooled (the effect is language-independent);
# compute is flat across N (~5%), so this is a matched-compute sweep. Each line
# carries a +/-SE band: the bands overlap across N, so the per-length wiggles
# are sampling noise, not a real cap effect (pooled, every N is within ~1pp).
import math as _m11
IC_LENS = [40, 50, 60, 70, 80, 90, 120, 160]
IC_NS = list(range(1, 11))
ic = defaultdict(list)
for r in rows:
    cl = r["config_label"]
    if len(cl) > 1 and cl[0] == "i" and cl[1:].split("q")[0].isdigit() \
            and cl.endswith("q10.R10.J"):
        ic[(int(cl[1:].split("q")[0]), r["length"])].append(r["pct"])
ramp = [plt.cm.Blues(v) for v in
        [0.30, 0.42, 0.53, 0.63, 0.73, 0.82, 0.90, 1.0]]
fig, ax = plt.subplots(figsize=(8.5, 5.5))
for L, color in zip(IC_LENS, ramp):
    ys, ses = [], []
    for N in IC_NS:
        v = ic[(N, L)]
        mu = sum(v) / len(v)
        ys.append(mu)
        ses.append(_m11.sqrt(sum((x - mu) ** 2 for x in v) / (len(v) - 1)) / _m11.sqrt(len(v)))
    ax.fill_between(IC_NS, [y - s for y, s in zip(ys, ses)],
                    [y + s for y, s in zip(ys, ses)], color=color, alpha=0.13, linewidth=0)
    ax.plot(IC_NS, ys, "-o", color=color, lw=2, ms=5,
            markeredgecolor="white", markeredgewidth=0.6)
    ax.annotate(f"L{L}", (IC_NS[-1], ys[-1]), xytext=(6, 0),
                textcoords="offset points", va="center", fontsize=8.5,
                color=color, fontweight="bold")
ax.set_xlabel("IC pre-pass plug cap  N   (-S iNq10)")
ax.set_ylabel("letters correct (mean %)")
ax.set_title("IC pre-pass cap is inert: recovery vs cap N (+/-SE bands)\n"
             "per-length wiggles are noise -- pooled, every N is within ~1pp "
             "(-J -S iNq10 -R 10, quad, 10 plugs, all langs, 2 seeds)",
             fontsize=10.5, fontweight="bold", pad=10)
ax.set_xticks(IC_NS)
ax.set_xlim(0.7, 10.9)
ax.set_ylim(0, 102)
ax.margins(x=0.05)
fig.tight_layout()
fig.savefig(os.path.join(OUT, "ic_cap_sweep.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote ic_cap_sweep.png")

# 12. Pre-pass MODEL sweep: IC vs mono vs bigram pre-pass, recovery vs cap N.
# -J -S {i,m,b}Nq10 -R 10. Three models = categorical hues (Okabe-Ito, fixed
# order). All langs pooled at short lengths (L40-90, where the pre-pass matters);
# +/-SE band. Compute is matched across all three (~20-24k score_iter).
import math as _math
PP_SHORT = [40, 45, 50, 55, 60, 65, 70, 75, 80, 90]
PP_MODELS = [("IC  (-S iNq10)", "i", range(1, 11), "#0072B2"),
             ("mono  (-S mNq10)", "m", range(1, 11), "#E69F00"),
             ("bigram  (-S bNq10)", "b", range(1, 11), "#009E73")]
pp = defaultdict(list)
noprepass = []   # pure q10, no pre-pass stage
for r in rows:
    mo = r["config_label"]
    if r["length"] not in PP_SHORT:
        continue
    if mo == "q10.R10.J":
        noprepass.append(r["pct"])
    elif len(mo) > 4 and mo[0] in "imb" and mo[1:].split("q")[0].isdigit() \
            and mo.endswith("q10.R10.J"):
        pp[(mo[0], int(mo[1:].split("q")[0]))].append(r["pct"])


def _mse(vals):
    m = sum(vals) / len(vals)
    se = _math.sqrt(sum((x - m) ** 2 for x in vals) / (len(vals) - 1)) / _math.sqrt(len(vals))
    return m, se


fig, ax = plt.subplots(figsize=(8.5, 5.5))
# "no pre-pass" (pure q10) reference: a horizontal band -- the level the climb
# reaches with NO staging, so every pre-pass line sits above it.
npm, npse = _mse(noprepass)
ax.axhspan(npm - npse, npm + npse, color="#999999", alpha=0.18, linewidth=0)
ax.axhline(npm, color="#666666", lw=1.6, ls="--")
ax.annotate(f"no pre-pass (-S q10): {npm:.0f}%", (10.2, npm), xytext=(0, -12),
            textcoords="offset points", ha="right", va="top",
            fontsize=8.5, color="#555555", fontweight="bold")
for label, key, nrange, color in PP_MODELS:
    ns = list(nrange)
    ms, ses = zip(*[_mse(pp[(key, N)]) for N in ns])
    ax.fill_between(ns, [m - s for m, s in zip(ms, ses)],
                    [m + s for m, s in zip(ms, ses)], color=color, alpha=0.15, linewidth=0)
    ax.plot(ns, ms, "-o", color=color, lw=2, ms=5, label=label,
            markeredgecolor="white", markeredgewidth=0.6)
ax.set_xlabel("pre-pass plug cap  N")
ax.set_ylabel("letters correct (mean %)")
ax.set_title("Pre-pass MODEL sets the level; cap N is inert: IC > mono > bigram > none\n"
             "(-J -S <model>Nq10 -R 10, quad target, 10 plugs, all langs, L40-90, 2 seeds)",
             fontsize=11.5, fontweight="bold", pad=10)
ax.set_xticks(range(1, 11))
ax.set_xlim(0.7, 10.3)
ax.set_ylim(0, 60)
ax.legend(frameon=False, loc="upper right", fontsize=9.5)
fig.tight_layout()
fig.savefig(os.path.join(OUT, "prepass_model_sweep.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote prepass_model_sweep.png")

# 13. Intermediate-stage sweep judged at MATCHED COMPUTE (english, L40-90).
# An extra middle climb stage (i3 <mid>B q10) costs more score_iter, so it is
# only a real win if it sits ABOVE the baseline i3q10 recovery-vs-compute curve
# (i3q10 swept over -R 10..18). x-axis = compute, not restarts.
IS_SHORT = [40, 45, 50, 55, 60, 65, 70, 75, 80, 90]
is_agg = defaultdict(list); is_si = defaultdict(list)
for r in rows:
    cl = r["config_label"]
    if r["language"] != "english" or r["length"] not in IS_SHORT:
        continue
    is_agg[cl].append(r["pct"]); is_si[cl].append(int(r["score_iter"]))
if any(k.startswith("i3m4") for k in is_agg):
    def _pt(cl):
        m, se = _mse(is_agg[cl]); return sum(is_si[cl]) / len(is_si[cl]), m, se
    fig, ax = plt.subplots(figsize=(8.5, 5.5))
    # baseline i3q10 recovery-vs-compute curve (-R 10..18)
    bx, by, bse = zip(*[_pt(f"i3q10.R{R}.J") for R in (10, 12, 14, 16, 18)])
    ax.plot(bx, by, "-", color="#444444", lw=2, zorder=3, label="baseline i3q10  (-R 10..18)")
    ax.errorbar(bx, by, yerr=bse, fmt="o", color="#444444", ms=5, capsize=3,
                markeredgecolor="white", markeredgewidth=0.6, zorder=4)
    # middle-stage points, colored by model
    IS_MODELS = [("mono middle", "m", "#E69F00"),
                 ("bigram middle", "b", "#009E73"),
                 ("trigram middle", "t", "#0072B2")]
    for label, key, color in IS_MODELS:
        pts = [_pt(f"i3{key}{B}q10.R10.J") for B in range(4, 9)]
        xs, ys, ses = zip(*pts)
        ax.errorbar(xs, ys, yerr=ses, fmt="o", color=color, ms=7, capsize=3,
                    markeredgecolor="white", markeredgewidth=0.8, label=label, zorder=5)
    ax.annotate("i3m8q10", (_pt("i3m8q10.R10.J")[0], _pt("i3m8q10.R10.J")[1]),
                xytext=(6, 4), textcoords="offset points", fontsize=8.5,
                color="#E69F00", fontweight="bold")
    ax.set_xlabel("compute  (mean score_iter per key)")
    ax.set_ylabel("letters correct (mean %)")
    ax.set_title("An intermediate mono/bigram stage beats the baseline at MATCHED compute\n"
                 "(points above the i3q10 curve = real staging win; english, quad, 10 plugs, L40-90)",
                 fontsize=11, fontweight="bold", pad=10)
    ax.legend(frameon=False, loc="lower right", fontsize=9)
    ax.margins(0.08)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "intermediate_stage_compute.png"), bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote intermediate_stage_compute.png")

# 14. Intermediate mono-stage at matched compute, PER LANGUAGE (firming run).
# 2x2: each panel = that language's baseline i3q10 recovery-vs-compute curve
# (-R 10..18) + the mono-middle points (i3m4..m8 q10). Mono above the curve =
# matched-compute win. Shows the english/french win does NOT reproduce in
# german/danish -> the effect is a hard-language one, not universal.
il_agg = defaultdict(list); il_si = defaultdict(list)
for r in rows:
    if r["length"] in IS_SHORT:
        il_agg[(r["language"], r["config_label"])].append(r["pct"])
        il_si[(r["language"], r["config_label"])].append(int(r["score_iter"]))
if any(k[1] == "i3m8q10.R10.J" and k[0] == "danish" for k in il_agg):
    def _ilpt(lg, cl):
        v = il_agg[(lg, cl)]; m, se = _mse(v)
        return sum(il_si[(lg, cl)]) / len(il_si[(lg, cl)]), m, se
    fig, axes = plt.subplots(2, 2, figsize=(10, 8), sharex=True)
    for ax, lang in zip(axes.flat, LANG_ORDER):
        bx, by, bse = zip(*[_ilpt(lang, f"i3q10.R{R}.J") for R in (10, 12, 14, 16, 18)])
        ax.plot(bx, by, "-", color="#444444", lw=2, zorder=3, label="baseline i3q10 (-R 10..18)")
        ax.errorbar(bx, by, yerr=bse, fmt="o", color="#444444", ms=4, capsize=2,
                    markeredgecolor="white", markeredgewidth=0.5, zorder=4)
        mx, my, mse_ = zip(*[_ilpt(lang, f"i3m{B}q10.R10.J") for B in range(4, 9)])
        ax.errorbar(mx, my, yerr=mse_, fmt="o", color="#E69F00", ms=7, capsize=2,
                    markeredgecolor="white", markeredgewidth=0.8, zorder=5,
                    label="mono middle i3m{4..8}q10")
        ax.set_title(lang, fontsize=11, fontweight="bold")
        ax.grid(True, color="#e6e6e6", linewidth=0.8); ax.set_axisbelow(True)
    for ax in axes[1]:
        ax.set_xlabel("compute (mean score_iter per key)")
    for ax in (axes[0, 0], axes[1, 0]):
        ax.set_ylabel("letters correct (mean %)")
    axes[0, 0].legend(frameon=False, loc="lower right", fontsize=8)
    fig.suptitle("Intermediate mono stage at matched compute, per language "
                 "(mono above baseline curve = win)\nreal only for english/french; "
                 "german/danish within noise (quad, 10 plugs, L40-90, 2 seeds)",
                 fontsize=11.5, fontweight="bold")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "intermediate_stage_by_language.png"), bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote intermediate_stage_by_language.png")

# 15. Recovery vs length, per language (the recommended config -J -S i4q10 -R 10).
# 2x2 small multiples: graded mean %-correct (colored, +/-SE band) plus the
# exact full-message recovery rate (gray) for context. Shows the tool's
# short-message cracking curve for each language.
RL_CFG = "-J-Si4q10-R10"
rl = defaultdict(list); rl_exact = defaultdict(list)
for r in rows:
    if r["config_label"] == RL_CFG:
        rl[(r["language"], r["length"])].append(r["pct"])
        rl_exact[(r["language"], r["length"])].append(r["exact"])
fig, axes = plt.subplots(2, 2, figsize=(10, 8), sharex=True, sharey=True)
for ax, lang in zip(axes.flat, LANG_ORDER):
    xs = sorted({L for (lg, L) in rl if lg == lang})
    means, ses, exacts = [], [], []
    for L in xs:
        v = rl[(lang, L)]; mu = sum(v) / len(v)
        means.append(mu)
        ses.append(_m11.sqrt(sum((x - mu) ** 2 for x in v) / (len(v) - 1)) / _m11.sqrt(len(v)))
        e = rl_exact[(lang, L)]; exacts.append(100.0 * sum(e) / len(e))
    col = COL[lang]
    ax.fill_between(xs, [m - s for m, s in zip(means, ses)],
                    [m + s for m, s in zip(means, ses)], color=col, alpha=0.15, linewidth=0)
    ax.plot(xs, means, "-o", color=col, lw=2, ms=5, markeredgecolor="white",
            markeredgewidth=0.6, label="mean % letters correct", zorder=4)
    ax.plot(xs, exacts, "--s", color="#888888", lw=1.6, ms=4, markeredgecolor="white",
            markeredgewidth=0.5, label="exact full-message rate", zorder=3)
    ax.set_title(lang, fontsize=11, fontweight="bold")
    ax.set_ylim(0, 102)
    ax.grid(True, color="#e6e6e6", linewidth=0.8); ax.set_axisbelow(True)
for ax in axes[1]:
    ax.set_xlabel("message length (letters)")
for ax in (axes[0, 0], axes[1, 0]):
    ax.set_ylabel("recovery (%)")
axes[0, 0].legend(frameon=False, loc="lower right", fontsize=8.5)
fig.suptitle("Recovery vs message length, per language "
             "(-J -S i4q10 -R 10, quad, 10 plugs, prose corpora)",
             fontsize=12.5, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "recovery_vs_length_by_language.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote recovery_vs_length_by_language.png")

# 16. i3m8q10 (mono intermediate stage) vs baseline i4q10, recovery vs length,
# per language. NOT matched compute (i3m8q10 ~32k vs i4q10 ~22k score_iter) --
# a "does the fancier config beat the default" view. Two configs = categorical
# hues; +/-SE bands. Short-length win is english/french only (see #75).
CMP = [("baseline i4q10", "i4q10.R10.J", "#444444"),
       ("i3m8q10 (mono middle)", "i3m8q10.R10.J", "#E69F00")]
cmp_d = defaultdict(list)
for r in rows:
    for _, cl, _ in CMP:
        if r["config_label"] == cl:
            cmp_d[(cl, r["language"], r["length"])].append(r["pct"])
fig, axes = plt.subplots(2, 2, figsize=(10, 8), sharex=True, sharey=True)
for ax, lang in zip(axes.flat, LANG_ORDER):
    for label, cl, color in CMP:
        xs = sorted({L for (c, lg, L) in cmp_d if c == cl and lg == lang})
        means, ses = [], []
        for L in xs:
            v = cmp_d[(cl, lang, L)]; mu = sum(v) / len(v)
            means.append(mu)
            ses.append(_m11.sqrt(sum((x - mu) ** 2 for x in v) / (len(v) - 1)) / _m11.sqrt(len(v)))
        ax.fill_between(xs, [m - s for m, s in zip(means, ses)],
                        [m + s for m, s in zip(means, ses)], color=color, alpha=0.15, linewidth=0)
        ax.plot(xs, means, "-o", color=color, lw=2, ms=5, markeredgecolor="white",
                markeredgewidth=0.6, label=label)
    ax.set_title(lang, fontsize=11, fontweight="bold")
    ax.set_ylim(0, 102)
    ax.grid(True, color="#e6e6e6", linewidth=0.8); ax.set_axisbelow(True)
for ax in axes[1]:
    ax.set_xlabel("message length (letters)")
for ax in (axes[0, 0], axes[1, 0]):
    ax.set_ylabel("mean % letters correct")
axes[0, 0].legend(frameon=False, loc="lower right", fontsize=8.5)
fig.suptitle("i3m8q10 (mono intermediate) vs baseline i4q10, per language "
             "(NOT matched compute: ~32k vs ~22k score_iter; quad, 10 plugs, 2 seeds)",
             fontsize=11.5, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "i3m8q10_vs_i4q10_by_language.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote i3m8q10_vs_i4q10_by_language.png")

# 17. i3m8q10 vs its exact paired baseline i3q10, recovery vs length, per lang.
# Both share the i3 pre-pass, so the ONLY difference is the added mono middle
# stage -- the cleanest isolation of its effect. Equal restarts (R=10), so NOT
# matched compute (i3m8q10 ~32k vs i3q10 ~23k score_iter).
CMP2 = [("baseline i3q10", "i3q10.R10.J", "#444444"),
        ("i3m8q10 (+ mono middle)", "i3m8q10.R10.J", "#E69F00")]
cmp2 = defaultdict(list)
for r in rows:
    for _, cl, _ in CMP2:
        if r["config_label"] == cl:
            cmp2[(cl, r["language"], r["length"])].append(r["pct"])
fig, axes = plt.subplots(2, 2, figsize=(10, 8), sharex=True, sharey=True)
for ax, lang in zip(axes.flat, LANG_ORDER):
    for label, cl, color in CMP2:
        xs = sorted({L for (c, lg, L) in cmp2 if c == cl and lg == lang})
        means, ses = [], []
        for L in xs:
            v = cmp2[(cl, lang, L)]; mu = sum(v) / len(v)
            means.append(mu)
            ses.append(_m11.sqrt(sum((x - mu) ** 2 for x in v) / (len(v) - 1)) / _m11.sqrt(len(v)))
        ax.fill_between(xs, [m - s for m, s in zip(means, ses)],
                        [m + s for m, s in zip(means, ses)], color=color, alpha=0.15, linewidth=0)
        ax.plot(xs, means, "-o", color=color, lw=2, ms=5, markeredgecolor="white",
                markeredgewidth=0.6, label=label)
    ax.set_title(lang, fontsize=11, fontweight="bold")
    ax.set_ylim(0, 102)
    ax.grid(True, color="#e6e6e6", linewidth=0.8); ax.set_axisbelow(True)
for ax in axes[1]:
    ax.set_xlabel("message length (letters)")
for ax in (axes[0, 0], axes[1, 0]):
    ax.set_ylabel("mean % letters correct")
axes[0, 0].legend(frameon=False, loc="lower right", fontsize=8.5)
fig.suptitle("Effect of the mono middle stage: i3m8q10 vs paired baseline i3q10, per language "
             "(equal restarts, NOT matched compute: ~32k vs ~23k score_iter)",
             fontsize=11, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "i3m8q10_vs_i3q10_by_language.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote i3m8q10_vs_i3q10_by_language.png")

# 18. i3m8q10 vs i3q10 at MATCHED COMPUTE, recovery vs length, per language.
# i3m8q10 (~31k score_iter) is compared to the plain baseline given equal
# compute via more restarts: i3q10 @ -R 14 (~30-31k, ratio 1.00-1.02). This is
# the fair head-to-head -- any gap here is the staging, not the compute.
CMP3 = [("i3q10 @R14 (matched compute)", "i3q10.R14.J", "#444444"),
        ("i3m8q10 (mono middle)", "i3m8q10.R10.J", "#E69F00")]
cmp3 = defaultdict(list)
for r in rows:
    for _, cl, _ in CMP3:
        if r["config_label"] == cl:
            cmp3[(cl, r["language"], r["length"])].append(r["pct"])
fig, axes = plt.subplots(2, 2, figsize=(10, 8), sharex=True, sharey=True)
for ax, lang in zip(axes.flat, LANG_ORDER):
    for label, cl, color in CMP3:
        xs = sorted({L for (c, lg, L) in cmp3 if c == cl and lg == lang})
        means, ses = [], []
        for L in xs:
            v = cmp3[(cl, lang, L)]; mu = sum(v) / len(v)
            means.append(mu)
            ses.append(_m11.sqrt(sum((x - mu) ** 2 for x in v) / (len(v) - 1)) / _m11.sqrt(len(v)))
        ax.fill_between(xs, [m - s for m, s in zip(means, ses)],
                        [m + s for m, s in zip(means, ses)], color=color, alpha=0.15, linewidth=0)
        ax.plot(xs, means, "-o", color=color, lw=2, ms=5, markeredgecolor="white",
                markeredgewidth=0.6, label=label)
    ax.set_title(lang, fontsize=11, fontweight="bold")
    ax.set_ylim(0, 102)
    ax.grid(True, color="#e6e6e6", linewidth=0.8); ax.set_axisbelow(True)
for ax in axes[1]:
    ax.set_xlabel("message length (letters)")
for ax in (axes[0, 0], axes[1, 0]):
    ax.set_ylabel("mean % letters correct")
axes[0, 0].legend(frameon=False, loc="lower right", fontsize=8.5)
fig.suptitle("i3m8q10 vs i3q10 at MATCHED compute (~31k score_iter), per language "
             "-- any gap is the staging, not the compute",
             fontsize=11.5, fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "i3m8q10_vs_i3q10_matched_by_language.png"), bbox_inches="tight", facecolor="white")
plt.close(fig)
print("wrote i3m8q10_vs_i3q10_matched_by_language.png")

# 19. Ordering experiment (§4.6): influence-order vs -J vs plain -I at MATCHED
# compute (~55k score_iter). Data in the timestamped shard, collision-free
# labels ord.{J,F,I}.*. --infl-order beats plain -I but is dominated by -J.
ORD = [("-J  R=24", "ord.J.R24", "#D55E00"),
       ("--infl-order  R=30", "ord.F.R30", "#009E73"),
       ("-I  R=32", "ord.I.R32", "#0072B2")]
ORD_LENS = [40, 50, 60, 70, 90]
od = defaultdict(list)
for r in rows:
    if r["config_label"] in ("ord.J.R24", "ord.F.R30", "ord.I.R32") and r["length"] in ORD_LENS:
        od[(r["config_label"], r["length"])].append(r["pct"])
if od:
    fig, ax = plt.subplots(figsize=(8.5, 5.5))
    for label, cfg, color in ORD:
        ys = [sum(od[(cfg, L)]) / len(od[(cfg, L)]) for L in ORD_LENS]
        ax.plot(ORD_LENS, ys, "-o", color=color, lw=2, ms=6, label=label,
                markeredgecolor="white", markeredgewidth=0.7)
    ax.set_xlabel("message length (letters)")
    ax.set_ylabel("mean % letters correct")
    ax.set_title("Ordering at MATCHED compute (~55k score_iter, quad, 10 plugs, 4 langs, 2 seeds)\n"
                 "influence-order beats plain -I but is dominated by -J's score-order",
                 fontsize=10.5, fontweight="bold", pad=10)
    ax.legend(frameon=False, loc="upper left", fontsize=9.5)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "infl_order_matched.png"), bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote infl_order_matched.png")

# 20. Ordering experiment, dense short range L40-60 (§4.6). Same matched-compute
# configs, with L45/L55 filled in, +/-SE bands -- resolves where influence-order
# crosses from competitive (short) to dominated by -J.
import math as _m20
ORD_SHORT = [40, 45, 50, 55, 60]
os_d = defaultdict(list)
for r in rows:
    if r["config_label"] in ("ord.J.R24", "ord.F.R30", "ord.I.R32") and r["length"] in ORD_SHORT:
        os_d[(r["config_label"], r["length"])].append(r["pct"])
if all((("ord.F.R30", L) in os_d) for L in ORD_SHORT):
    fig, ax = plt.subplots(figsize=(8, 5.5))
    for label, cfg, color in ORD:
        ms_, se_ = [], []
        for L in ORD_SHORT:
            v = os_d[(cfg, L)]; mu = sum(v) / len(v)
            ms_.append(mu)
            se_.append(_m20.sqrt(sum((x - mu) ** 2 for x in v) / (len(v) - 1)) / _m20.sqrt(len(v)))
        ax.fill_between(ORD_SHORT, [m - s for m, s in zip(ms_, se_)],
                        [m + s for m, s in zip(ms_, se_)], color=color, alpha=0.15, linewidth=0)
        ax.plot(ORD_SHORT, ms_, "-o", color=color, lw=2, ms=6, label=label,
                markeredgecolor="white", markeredgewidth=0.7)
    ax.set_xlabel("message length (letters)")
    ax.set_ylabel("mean % letters correct")
    ax.set_xticks(ORD_SHORT)
    ax.set_title("Ordering at matched compute, dense short range (~55k score_iter, 4 langs, 2 seeds)\n"
                 "influence-order ties -J through L55 (L50 blip is noise), falls behind from L60",
                 fontsize=10.5, fontweight="bold", pad=10)
    ax.legend(frameon=False, loc="upper left", fontsize=9.5)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "infl_order_short.png"), bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote infl_order_short.png")

# 21. Restart-gain sweep: recovery vs -R at short lengths (german, -J -S i4q10).
# One line per length; x = restart budget (categorical, since R=0 has no log
# point). Shows recovery is restart-limited and keeps climbing through R=80.
RSW_RS = [0, 1, 2, 5, 10, 20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 20480, 40960, 81920]
RSW_LENS = [40, 45, 50, 55, 60, 65, 70]
rsw = defaultdict(list)
for r in rows:
    cl = r["config_label"]
    if cl.startswith("rsw.J.R") and r["language"] == "german":
        try:
            R = int(cl[len("rsw.J.R"):])
        except ValueError:
            continue
        if R in RSW_RS and r["length"] in RSW_LENS:
            rsw[(R, r["length"])].append(r["pct"])
# x positions span every R present for ANY length; each length is drawn only
# where it has data (the high-R tail was run for the hardest lengths only).
RSW_HAVE = [R for R in RSW_RS if any(rsw[(R, L)] for L in RSW_LENS)]
if RSW_HAVE:
    xpos = {R: i for i, R in enumerate(RSW_HAVE)}
    ramp = [plt.cm.Blues(v) for v in
            [0.30, 0.40, 0.50, 0.60, 0.72, 0.84, 1.0]]
    fig, ax = plt.subplots(figsize=(9.5, 5.5))
    for L, color in zip(RSW_LENS, ramp):
        pts = [(xpos[R], sum(rsw[(R, L)]) / len(rsw[(R, L)])) for R in RSW_HAVE if rsw[(R, L)]]
        if not pts:
            continue
        lx, ly = zip(*pts)
        ax.plot(lx, ly, "-o", color=color, lw=2, ms=5, markeredgecolor="white",
                markeredgewidth=0.6)
        ax.annotate(f"L{L}", (lx[-1], ly[-1]), xytext=(6, 0), textcoords="offset points",
                    va="center", fontsize=8.5, color=color, fontweight="bold")
    ax.set_xticks(list(xpos.values()))
    ax.set_xticklabels([str(R) for R in RSW_HAVE], rotation=45, ha="right", fontsize=8)
    ax.set_xlabel("restart budget  -R   (compute ~ linear in R; ~2.3k score_iter/restart)")
    ax.set_ylabel("mean % letters correct")
    ax.set_title("Restart gain at short lengths (german, -J -S i4q10, quad, 10 plugs, 2 seeds)\n"
                 "L45+ reach ~95%; L40 plateaus ~88% by R20k (info floor at L40) -- below 95%, compute cannot close it",
                 fontsize=10, fontweight="bold", pad=10)
    ax.set_ylim(0, 100)
    ax.margins(x=0.08)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "restart_gain_german.png"), bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print("wrote restart_gain_german.png")

print("done ->", OUT)
