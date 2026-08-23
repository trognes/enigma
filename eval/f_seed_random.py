"""ENHANCEMENTS 2a experiment F, gate F1: does a 4-plug SEED's score predict
whether the climb from it reaches the solution?

For each key: generate KICKS random 4-pair boards, score each under mono, IC
and the k blend (all three from the same 26-bin histogram, computed in Python
via the rotor-core table -- exact, and no binary invocation), then climb each
to 10 plugs under -f and record whether the result clears 50% of letters.

The correlation that matters is WITHIN a key: experiment F ranks seeds for one
key and promotes the best.  Comparing across keys would be confounded by
message difficulty.
"""
import math, os, random, subprocess, sys, importlib.util
from concurrent.futures import ThreadPoolExecutor

HERE = "eval"
sys.path.insert(0, HERE)
import enigma_ref
spec = importlib.util.spec_from_file_location("pab", HERE + "/prepass_ab.py")
pab = importlib.util.module_from_spec(spec); spec.loader.exec_module(pab)

L      = int(os.environ.get("L", 100))
KEYS   = int(os.environ.get("KEYS", 3))
KICKS  = int(os.environ.get("KICKS", 100))
JOBS   = int(os.environ.get("JOBS", 4))
PLUGS  = 4
SEED   = int(os.environ.get("SEED", 20))
SUCCESS = 50.0

# wehrmacht monogram log10 probabilities, as the binary loads them
cnt = {}
for line in open("ngrams/wehrmacht_monograms.txt", encoding="utf-8"):
    f = line.split()
    if len(f) == 2 and len(f[0]) == 1:
        cnt[f[0]] = cnt.get(f[0], 0) + int(f[1])
tot = sum(cnt.values())
logp = [math.log10(cnt.get(chr(65 + i), 1) / tot) for i in range(26)]

def core_table(wheels, ring, start, length):
    """Rotor-stack permutation per position, plugboard removed: under an EMPTY
    board S is the identity, so decrypt(chr(x)*L)[i] is exactly core_i[x]."""
    cols = [enigma_ref.decrypt(chr(65 + x) * length, wheels, ring, start, "")
            for x in range(26)]
    return [[ord(cols[x][i]) - 65 for x in range(26)] for i in range(length)]

def stats(board, ct_nums, core):
    s = enigma_ref._plugboard(board)
    h = [0] * 26
    for i, c in enumerate(ct_nums):
        h[s[core[i][s[c]]]] += 1
    n = len(ct_nums)
    ic = sum(f * (f - 1) for f in h) / (n * (n - 1))
    mono = sum(f * logp[j] for j, f in enumerate(h)) / n
    return mono, ic, mono + 0.1 * n * ic          # the shipped k blend

def pct(a, b):
    return 100.0 * sum(x == y for x, y in zip(a, b)) / len(b)

def auc(pos, neg):
    if not pos or not neg:
        return None
    w = sum((p > q) + 0.5 * (p == q) for p in pos for q in neg)
    return w / (len(pos) * len(neg))

corpus = "".join(pab.decrypts(HERE + "/enigma-messages.txt")
                 + pab.decrypts(HERE + "/enigma-army-messages-1941.txt"))
rng = random.Random(SEED)
env = dict(os.environ, ENIGMA_SEED="0")
letters = [chr(65 + i) for i in range(26)]

print(f"F1: seed score vs climb outcome.  L={L}, {KEYS} keys x {KICKS} "
      f"{PLUGS}-plug kicks, climb -S f10 -R 0, success = >={SUCCESS:.0f}% correct\n")
rows = []
allsc = {"mono": [], "ic": [], "k": []}
allok = []
allpct = []
for k in range(KEYS):
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    w = "".join(str(x) for x in rng.sample(range(1, 6), 3))
    r = "".join(chr(65 + rng.randrange(26)) for _ in range(3))
    g = "".join(chr(65 + rng.randrange(26)) for _ in range(3))
    ls = rng.sample(letters, 20)
    truth = " ".join(ls[i] + ls[i + 1] for i in range(0, 20, 2))
    ct, _ = pab.run(["-u", "B", "-w", w, "-r", r, "-g", g, "-s", truth], pt)
    core = core_table(w, r, g, L)
    ctn = [ord(c) - 65 for c in ct]

    sc = {"mono": [], "ic": [], "k": []}
    boards = []
    for _ in range(KICKS):
        pl = rng.sample(letters, 2 * PLUGS)
        board = " ".join(pl[i] + pl[i + 1] for i in range(0, 2 * PLUGS, 2))
        m, i_, b = stats(board, ctn, core)
        sc["mono"].append(m); sc["ic"].append(i_); sc["k"].append(b)
        boards.append(board)

    def climb(board):
        p = subprocess.run(["./enigma", "-u", "B", "-w", w, "-r", r, "-g", g,
                            "-c", "-J", "-l", "wehrmacht", "-S", "f10",
                            "-R", "0", "--soft-plug", board.replace(" ", ""),
                            "-T", "1"],
                           input=ct, capture_output=True, text=True, env=env)
        return pct(p.stdout.strip(), pt)

    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        got = list(ex.map(climb, boards))
    ok = [x >= SUCCESS for x in got]
    allpct.append(got)
    for model in ("mono", "ic", "k"):
        allsc[model].append(sc[model])
    allok.append(ok)
    nok = sum(ok)
    line = {"n": nok}
    for model in ("mono", "ic", "k"):
        pos = [s for s, o in zip(sc[model], ok) if o]
        neg = [s for s, o in zip(sc[model], ok) if not o]
        line[model] = auc(pos, neg)
    rows.append(line)
    a = lambda v: "  n/a" if v is None else f"{v:.3f}"
    print(f"  key {k+1}: {nok:>3}/{KICKS} kicks succeed   "
          f"AUC mono {a(line['mono'])}  ic {a(line['ic'])}  k {a(line['k'])}")


def rank_pct(v):
    order = sorted(range(len(v)), key=lambda i: v[i])
    r = [0.0] * len(v)
    for pos, i in enumerate(order):
        r[i] = pos / (len(v) - 1)
    return r

def spearman(a, b):
    ra, rb = rank_pct(a), rank_pct(b)
    n = len(a); ma = sum(ra)/n; mb = sum(rb)/n
    num = sum((x-ma)*(y-mb) for x, y in zip(ra, rb))
    da = math.sqrt(sum((x-ma)**2 for x in ra))
    db = math.sqrt(sum((y-mb)**2 for y in rb))
    return num/(da*db) if da and db else 0.0

print()
tot_ok = sum(sum(o) for o in allok)
print(f"  successes pooled: {tot_ok} of {len(allok)*KICKS} climbs")
print()
print("  POOLED stratified AUC (scores -> within-key percentile, then pooled)")
for model in ("mono", "ic", "k"):
    pos, neg = [], []
    for sck, okk in zip(allsc[model], allok):
        rp = rank_pct(sck)
        for x, o in zip(rp, okk):
            (pos if o else neg).append(x)
    a = auc(pos, neg)
    se = (1/(12*len(pos)))**0.5 if pos else None
    print(f"   {model:>5}: AUC {a:.3f}" + (f"   ~95% CI [{a-1.96*se:.3f}, "
          f"{a+1.96*se:.3f}]" if a is not None and se else "   n/a"))
print()
print("  Spearman rho(seed score, final %correct), mean over keys")
for model in ("mono", "ic", "k"):
    v = [spearman(sck, gk) for sck, gk in zip(allsc[model], allpct)]
    print(f"   {model:>5}: {sum(v)/len(v):+.3f}   per key "
          + " ".join(f"{x:+.2f}" for x in v))
print("\n  (AUC 0.5 / rho 0 = the seed's score says nothing about its climb)")
