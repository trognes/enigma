# The hard-seeded arm has run at -R 8 --random 10 -S i4f10 throughout, i.e. at
# the recipe's defaults, while only the SOFT arm ever got a tuning sweep.  Same
# trial set as the A/B (same seed and the same per-trial RNG draws).
import random, sys, numpy as np
sys.path.insert(0, 'eval')
import selfcrib_probe as SC
from seeder_vs_restarts import seeds_for, run, pct, ALPHA, RECIPE, binom_two
from crib_menu import core_rows, corpus, random_key
from ring_stride_geometry_probe import crypt, plugboard

# (label, extra args).  -R 0 is one unkicked climb; --random 0 would instead
# make every restart an identical copy of it.
CELLS = [("R0  nokick", ["-R","0"])]
for R in (1, 2, 4, 8):
    for k in (1, 3, 10):
        CELLS.append(("R%-3d r%-2d" % (R, k),
                      ["-R", str(R), "--random", str(k)]))
rng = random.Random(20260817)
tab = SC.ngram_tables('wehrmacht')
texts = [t for t in corpus() if SC.doublings(t)]
ends = [t for t in texts if any(len(t)-(s+2*L+1) <= 1
        for s, L in SC.doublings(t, minlen=4, maxlen=20))]
acc = {c[0]: [[], [], []] for c in CELLS}
hits = []
for pt_text in ends * 30:
    wheels, refl, ring, start = random_key(rng)
    plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
    ct = crypt(pt_text, wheels, refl, ring, start, plug)
    rows = core_rows(wheels, refl, ring, start, len(ct))
    ranked, corr = seeds_for(ct, rows, plug, tab)
    if not ranked:
        continue
    base = ["-u", refl, "-w", "".join(str(x+1) for x in wheels),
            "-r", "".join(ALPHA[x] for x in ring),
            "-g", "".join(ALPHA[x] for x in start),
            "-l", "wehrmacht", "-T", "4", "-e", str(rng.randrange(1 << 30))]
    cables, noplug, _ = ranked[0]
    hits.append(corr == 0)
    for label, extra in CELLS:
        args = RECIPE + base + extra
        if cables:
            args += ["-s", cables]
        if noplug:
            args += ["--no-plug", noplug]
        rec, it, wl, _ = run("./enigma", args, ct)
        p = pct(rec, pt_text)
        acc[label][0].append(p); acc[label][1].append(p > 99.999)
        acc[label][2].append(it or 0)
hits = np.array(hits)
print("hard-seeded arm, restarts x kick, n=%d (%d ranked right)\n"
      % (len(hits), hits.sum()))
print("%-11s %-8s %-10s %-13s %s" % ("cell", "mean %", "exact", "right/WRONG",
                                     "score_iter"))
ref = np.array(acc["R8   r10"][1])
for label, _ in CELLS:
    m = np.array(acc[label][0]); e = np.array(acc[label][1])
    i = np.array(acc[label][2])
    print("%-11s %-8.1f %-10s %-13s %.0f"
          % (label, m.mean(), "%d/%d" % (e.sum(), e.size),
             "%.1f / %.1f" % (m[hits].mean(), m[~hits].mean()), i.mean()))
print()
for label, _ in CELLS:
    e = np.array(acc[label][1])
    x = int((e & ~ref).sum()); y = int((ref & ~e).sum())
    if x or y:
        print("%-11s vs R8 r10 exact: only it %2d, only R8r10 %2d, p = %.3f"
              % (label, x, y, binom_two(x, y)))
