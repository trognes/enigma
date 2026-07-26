#!/usr/bin/env python3
# Plot the restart-perturbation sweep: mean %-correct vs restart count R, one curve
# per perturbation strength k, two panels (L40, L70). Merges the main sweep CSV
# (R 1..32, all k) with the high-R extension CSV (R 64..256, k in {leg,4,8}).
# Usage: python3 tests/restart_sweep_plot.py [main.csv hiR.csv out.png]

import csv
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

main_csv = sys.argv[1] if len(sys.argv) > 1 else "tests/restart_sweep.csv"
hir_csv = sys.argv[2] if len(sys.argv) > 2 else "tests/restart_sweep_hiR.csv"
out_png = sys.argv[3] if len(sys.argv) > 3 else "tests/restart_sweep.png"


def load(path, into):
    try:
        with open(path) as f:
            for row in csv.DictReader(f):
                L, k, R = int(row["len"]), int(row["k"]), int(row["R"])
                into.setdefault((L, k), {})[R] = (
                    float(row["mean"]), float(row["lo95"]), float(row["hi95"]))
    except FileNotFoundError:
        pass


data = {}
load(main_csv, data)
load(hir_csv, data)

lengths = sorted({L for (L, _k) in data})
ks = sorted({k for (_L, k) in data})


def klabel(k):
    return "legacy (full random)" if k < 0 else ("k=%d" % k)


# colour the numeric k by rank along a sequential map (so small->large k reads as a
# gradient); legacy (full random) is a thick dashed black reference.
pos_ks = [k for k in ks if k >= 0]
cmap = matplotlib.colormaps["viridis"]
kcolor = {k: cmap(i / max(1, len(pos_ks) - 1)) for i, k in enumerate(pos_ks)}

fig, axes = plt.subplots(1, len(lengths), figsize=(6.4 * len(lengths), 5.0), squeeze=False)
for ax, L in zip(axes[0], lengths):
    for k in ks:
        series = data.get((L, k))
        if not series:
            continue
        Rs = sorted(series)
        ys = [series[R][0] for R in Rs]
        if k < 0:
            ax.plot(Rs, ys, "--", marker="o", markersize=4, linewidth=2.6,
                    color="black", label=klabel(k), zorder=5)
        else:
            ax.plot(Rs, ys, "-", marker="o", markersize=3.5, linewidth=1.8,
                    color=kcolor[k], label=klabel(k))
    ax.set_xscale("log", base=2)
    ax.set_xlabel("restarts  R  (log2)")
    ax.set_ylabel("mean %-correct")
    ax.set_title("length %d" % L)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8, title="perturbation")

fig.suptitle("Restart sweep: mean %-correct vs R, by random-perturbation strength k\n"
             "schedule -S {iq | r<k>iq}, IC->quad; english/quad, 10-pair board",
             fontsize=11)
fig.tight_layout(rect=(0, 0, 1, 0.93))
fig.savefig(out_png, dpi=130)
print("wrote", out_png)
