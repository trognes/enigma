# The hard arm saturates at -R 0 (one unkicked climb, 2734 score_iter), so the
# hedge gets ~2x cheaper and testing MORE hypotheses becomes affordable.  Run
# every seed once at -R 0 and read B_k -- best-by-converged-score over the
# top k -- off the same runs, for every k at once.
import random, sys, numpy as np
sys.path.insert(0, 'eval')
import selfcrib_probe as SC
from seeder_vs_restarts import seeds_for, run, pct, ALPHA, RECIPE, binom_two
from crib_menu import core_rows, corpus, random_key
from ring_stride_geometry_probe import crypt, plugboard

KS = [1, 2, 3, 5, 10, 999]
rng = random.Random(20260817)
tab = SC.ngram_tables('wehrmacht')
texts = [t for t in corpus() if SC.doublings(t)]
ends = [t for t in texts if any(len(t)-(s+2*L+1) <= 1
        for s, L in SC.doublings(t, minlen=4, maxlen=20))]
res = {k: [[], [], []] for k in KS}
nseeds, intop = [], []
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
            "-l", "wehrmacht", "-T", "4", "-R", "0",
            "-e", str(rng.randrange(1 << 30))]
    got = []
    for cables, noplug, _ in ranked:
        args = RECIPE + base
        if cables:
            args += ["-s", cables]
        if noplug:
            args += ["--no-plug", noplug]
        got.append(run("./enigma", args, ct))
    nseeds.append(len(got)); intop.append(corr)
    for k in KS:
        sub = got[:k]
        best = max(sub, key=lambda g: (g[3] is not None, g[3]))
        p = pct(best[0], pt_text)
        res[k][0].append(p); res[k][1].append(p > 99.999)
        res[k][2].append(sum(g[1] or 0 for g in sub))
nseeds = np.array(nseeds); intop = np.array([-1 if c is None else c
                                             for c in intop])
print("best-by-score over the top k seeds, each at -R 0, n=%d" % len(nseeds))
print("mean %.1f seeds per message (max %d); correct seed in top k:"
      % (nseeds.mean(), nseeds.max()))
print("   " + "  ".join("k=%s %d" % ("all" if k > 900 else k,
                        int(((intop >= 0) & (intop < k)).sum())) for k in KS))
print()
print("%-6s %-9s %-11s %-13s %s"
      % ("k", "mean %", "exact", "right/WRONG", "score_iter"))
hits = intop == 0
for k in KS:
    m = np.array(res[k][0]); e = np.array(res[k][1]); i = np.array(res[k][2])
    print("%-6s %-9.1f %-11s %-13s %.0f"
          % ("all" if k > 900 else k, m.mean(), "%d/%d" % (e.sum(), e.size),
             "%.1f / %.1f" % (m[hits].mean(), m[~hits].mean()), i.mean()))
ref = np.array(res[3][1])
for k in KS:
    e = np.array(res[k][1])
    x = int((e & ~ref).sum()); y = int((ref & ~e).sum())
    if x or y:
        print("k=%-4s vs k=3 exact: only it %2d, only k3 %2d, p = %.4f"
              % ("all" if k > 900 else k, x, y, binom_two(x, y)))
