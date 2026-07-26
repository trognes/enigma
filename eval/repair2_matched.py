#!/usr/bin/env python3
#
# 2-plug try_repair value at SHORT lengths, matched compute (PERFORMANCE.md section 4.9).
#
# try_repair (the gated 2-plug re-pair barrier cross) was originally validated
# only at long lengths (L140-250), where its convergence scan is negligible. At
# short lengths a climb converges fast, so that fixed scan is a larger fraction of
# the work -- so its value must be re-checked at matched compute here. Uses the
# --no-repair flag to A/B one binary: default (repair on) vs --no-repair (off),
# sweeping -R to get recovery-vs-score_iter curves. try_repair costs more per
# climb, so the honest test is whether the with-repair curve sits above the
# without-repair curve at matched score_iter (interpolated).
#
# Result: it wins even at L40-70 -- mean %-correct +0.2..+2.8pp at matched compute,
# growing with restart budget. So the 2-plug re-pair earns its cost at short
# lengths too (the 3-plug variant, by contrast, was dominated -- section 4.7).
#
# Usage: python3 eval/repair2_matched.py   # solves, prints table, writes PNG
# Env:   LANGS LENS PAIRS NPROB RGRID OPTS SEED

import os, sys, random, statistics as st
sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
import eval as E  # noqa
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BIN   = "./enigma"
LANGS = os.environ.get("LANGS", "english german").split()
LENS  = [int(x) for x in os.environ.get("LENS", "40 55 70").split()]
PAIRS = int(os.environ.get("PAIRS", "10"))
NPROB = int(os.environ.get("NPROB", "100"))
RGRID = [int(x) for x in os.environ.get("RGRID", "1 2 4 8 16").split()]
OPTS  = os.environ.get("OPTS", "-J -S i4q10").split()
SEED  = int(os.environ.get("SEED", "9001"))
os.environ["ENIGMA_DATA"] = "ngrams"
os.environ["ENIGMA_SEED"] = "0"


def pct(a, b):
    n = min(len(a), len(b))
    return 100.0 * sum(1 for i in range(n) if a[i] == b[i]) / n if n else 0.0


def solve(lang, key, ct, R, extra):
    u, w, r, g, _ = key
    args = ["-q", "-l", lang, "-c", "-R", str(R), "-u", u, "-w", w,
            "-r", r, "-g", g] + OPTS + extra
    out, err, _ = E.run(BIN, args, ct)
    return out.strip(), E.score_iter(err) or 0


data = {(tag, R): {"pct": [], "ex": [], "si": []}
        for tag in ("repair", "norepair") for R in RGRID}
for lang in LANGS:
    corpora = E.load_corpora(lang)
    for L in LENS:
        rng = random.Random(SEED)
        for _ in range(NPROB):
            _, excerpt, key = E.gen_problem(rng, corpora, L, PAIRS)
            ct = E.encrypt(BIN, key, excerpt)
            for R in RGRID:
                for tag, extra in (("repair", []), ("norepair", ["--no-repair"])):
                    pt, si = solve(lang, key, ct, R, extra)
                    d = data[(tag, R)]
                    d["pct"].append(pct(pt, excerpt))
                    d["ex"].append(1 if pt == excerpt else 0)
                    d["si"].append(si)

curve = {"repair": [], "norepair": []}
print(f"L{LENS} {LANGS} {PAIRS}plugs NPROB={NPROB}/cell OPTS={' '.join(OPTS)}\n")
print(f"{'R':>3} | {'repair %':>9} {'siter':>8} | {'no-rep %':>9} {'siter':>8} "
      f"| {'rep exact':>9} {'no exact':>9}")
for R in RGRID:
    r, n = data[("repair", R)], data[("norepair", R)]
    rp, npc = st.mean(r["pct"]), st.mean(n["pct"])
    rs, ns = st.mean(r["si"]), st.mean(n["si"])
    curve["repair"].append((rs, rp))
    curve["norepair"].append((ns, npc))
    print(f"{R:>3} | {rp:9.2f} {rs:8.0f} | {npc:9.2f} {ns:8.0f} "
          f"| {100*st.mean(r['ex']):9.1f} {100*st.mean(n['ex']):9.1f}")


def interp(pts, x):
    pts = sorted(pts)
    if x <= pts[0][0]:
        return pts[0][1]
    if x >= pts[-1][0]:
        return pts[-1][1]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x0 <= x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return pts[-1][1]


print("\nmatched compute (norepair interpolated to repair's score_iter):")
deltas = []
for rs, rp in curve["repair"]:
    ni = interp(curve["norepair"], rs)
    deltas.append((rs, rp, ni, rp - ni))
    print(f"  siter {rs:8.0f}  repair {rp:6.2f}  norepair {ni:6.2f}  Δ {rp-ni:+.2f}")

BLUE, GRAY = '#1f5fa8', '#9a9a9a'
fig, ax = plt.subplots(figsize=(8.4, 5.2))
rx = [c[0] for c in curve["repair"]]; ry = [c[1] for c in curve["repair"]]
nx = [c[0] for c in curve["norepair"]]; ny = [c[1] for c in curve["norepair"]]
ax.plot(rx, ry, '-o', color=BLUE, lw=2.2, ms=7, label='with try_repair (default)')
ax.plot(nx, ny, '--s', color=GRAY, lw=2.0, ms=6, label='--no-repair (2-plug re-pair off)')
for rs, rp, ni, dd in deltas:
    ax.annotate(f"+{dd:.1f}", (rs, rp), textcoords="offset points",
                xytext=(4, 7), fontsize=8.5, color=BLUE)
ax.set_xscale('log')
ax.set_xlabel("score_iter (compute)")
ax.set_ylabel("mean % letters correct")
ax.grid(alpha=.3, which='both')
ax.legend(frameon=False, fontsize=10, loc='upper left')
ax.set_title("2-plug try_repair earns its cost at short lengths, matched compute\n"
             f"en+de L{LENS[0]}-{LENS[-1]}, {PAIRS} plugs, {NPROB}/cell — Δ grows with budget "
             "(+0.2 → +2.8 pp)", fontsize=10.5)
fig.tight_layout()
fig.savefig("eval/plots/repair2_matched.png", dpi=110)
print("\nwrote eval/plots/repair2_matched.png")
