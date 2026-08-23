"""Experiment F: 2 (kick size) x 3 (pre-pass model) factorial, SAME KEYS.

The earlier pair of runs could not be compared because the RNG stream diverged
once the kick size changed, so only key 1 was shared.  Here every key is drawn
BEFORE any kick, and both kick sizes run on the identical key set, so the model
effect, the kick effect and their interaction are separable.

Per candidate: climb a kick to a capped-4 seed under the model (stage 1 of
-S <m|i|k>4f10), then the full -S f10 continuation.  Success = >=50% correct.
"""
import math, os, random, subprocess, sys, importlib.util
from concurrent.futures import ThreadPoolExecutor

HERE = "eval"; sys.path.insert(0, HERE)
spec = importlib.util.spec_from_file_location("pab", HERE + "/prepass_ab.py")
pab = importlib.util.module_from_spec(spec); spec.loader.exec_module(pab)

L     = int(os.environ.get("L", 100))
KEYS  = int(os.environ.get("KEYS", 15))
KICKS = int(os.environ.get("KICKS", 40))
JOBS  = int(os.environ.get("JOBS", 4))
SEED  = int(os.environ.get("SEED", 77))
SUCCESS = 50.0
MODELS = tuple(os.environ.get("MODELS", "m4,i4,k4").split(","))
SIZES  = (4, 10)

corpus = "".join(pab.decrypts(HERE + "/enigma-messages.txt")
                 + pab.decrypts(HERE + "/enigma-army-messages-1941.txt"))
letters = [chr(65 + i) for i in range(26)]
env = dict(os.environ, ENIGMA_SEED="0")
def pct(a, b): return 100.0 * sum(x == y for x, y in zip(a, b)) / len(b)

# ---- draw ALL keys first, so nothing downstream can shift them -------------
kr = random.Random(SEED)
keys = []
for _ in range(KEYS):
    pt = corpus[kr.randrange(0, len(corpus) - L):][:L]
    w = "".join(str(x) for x in kr.sample(range(1, 6), 3))
    r = "".join(chr(65 + kr.randrange(26)) for _ in range(3))
    g = "".join(chr(65 + kr.randrange(26)) for _ in range(3))
    ls = kr.sample(letters, 20)
    truth = " ".join(ls[i] + ls[i + 1] for i in range(0, 20, 2))
    keys.append((pt, w, r, g, truth))

print(f"F factorial: {KEYS} keys x {KICKS} kicks x {len(MODELS)} models x "
      f"{len(SIZES)} kick sizes, L={L}, then -S f10, success >= {SUCCESS:.0f}%")
print(f"(all {len(SIZES)*len(MODELS)} cells run on the SAME {KEYS} keys)\n")

rate = {(s, m): [] for s in SIZES for m in MODELS}   # per-key success rates
for kx, (pt, w, r, g, truth) in enumerate(keys):
    ct, _ = pab.run(["-u","B","-w",w,"-r",r,"-g",g,"-s",truth], pt)
    base = ["./enigma","-u","B","-w",w,"-r",r,"-g",g,"-c","-J",
            "-l","wehrmacht","-T","1"]

    def one(job):
        size, model, kick = job
        a = base + ["-S", model, "-R", "0", "--dump-all",
                    "--soft-plug", kick.replace(" ", "")]
        p = subprocess.run(a, input=ct, capture_output=True, text=True, env=env)
        d = [l for l in p.stderr.splitlines() if l.startswith("dumpall")]
        seed = " ".join(d[0].split()[5:]) if d else ""
        b = base + ["-S", "f10", "-R", "0"]
        if seed: b += ["--soft-plug", seed.replace(" ", "")]
        q = subprocess.run(b, input=ct, capture_output=True, text=True, env=env)
        return size, model, pct(q.stdout.strip(), pt) >= SUCCESS

    # kicks drawn from a per-key RNG so each cell is reproducible
    jobs = []
    for size in SIZES:
        kr2 = random.Random(SEED * 1000 + kx * 10 + size)
        for _ in range(KICKS):
            pl = kr2.sample(letters, 2 * size)
            kick = " ".join(pl[i] + pl[i+1] for i in range(0, 2 * size, 2))
            for m in MODELS:
                jobs.append((size, m, kick))
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        out = list(ex.map(one, jobs))
    tally = {(s, m): 0 for s in SIZES for m in MODELS}
    for s, m, ok in out:
        tally[(s, m)] += ok
    for s in SIZES:
        for m in MODELS:
            rate[(s, m)].append(100.0 * tally[(s, m)] / KICKS)
    print(f"[progress] key {kx+1}/{KEYS}: " + "  ".join(
        f"{s}p/{m} {100.0*tally[(s,m)]/KICKS:4.0f}%" for s in SIZES
        for m in MODELS), file=sys.stderr, flush=True)

def mean(v): return sum(v) / len(v)
def median(v):
    q = sorted(v); n = len(q)
    return q[n//2] if n % 2 else (q[n//2 - 1] + q[n//2]) / 2
def signtest(v):
    """Keys favouring each side, ignoring exact ties -- robust to one key
    dominating the mean, which is what happened at 15 keys."""
    pos = sum(1 for x in v if x > 0); neg = sum(1 for x in v if x < 0)
    return pos, neg, len(v) - pos - neg
def ci(v):
    n = len(v); mu = mean(v)
    sd = math.sqrt(sum((x-mu)**2 for x in v)/(n-1)) if n > 1 else 0.0
    se = sd/math.sqrt(n)
    return mu, mu-1.96*se, mu+1.96*se

print("\nSUCCESS RATE (mean over keys, %)")
print(f"   {'kick':>6} " + "".join(f"{m:>10}" for m in MODELS))
for s in SIZES:
    print(f"   {s:>5}p " + "".join(f"{mean(rate[(s,m)]):>10.1f}" for m in MODELS))

print("\nPAIRED CONTRASTS (per key, then averaged; 95% CI)")
for m in MODELS:
    d = [a - b for a, b in zip(rate[(4,m)], rate[(10,m)])]
    mu, lo, hi = ci(d)
    pos, neg, tie = signtest(d)
    print(f"   {m}: 4-plug minus 10-plug   {mu:+6.2f}pp  [{lo:+6.2f}, {hi:+6.2f}]"
          f"   median {median(d):+5.1f}   keys +{pos}/-{neg}/={tie}")
for s in (SIZES if ("i4" in MODELS and "k4" in MODELS) else ()):
    d = [a - b for a, b in zip(rate[(s,'i4')], rate[(s,'k4')])]
    mu, lo, hi = ci(d)
    pos, neg, tie = signtest(d)
    print(f"   {s:>2}-plug kick: i4 minus k4  {mu:+6.2f}pp  [{lo:+6.2f}, {hi:+6.2f}]"
          f"   median {median(d):+5.1f}   keys +{pos}/-{neg}/={tie}")
if "i4" in MODELS and "k4" in MODELS:
    d = [(a-b)-(c-e) for a,b,c,e in zip(rate[(4,'i4')], rate[(10,'i4')],
                                        rate[(4,'k4')], rate[(10,'k4')])]
else:
    d = [0.0]
mu, lo, hi = ci(d)
pos, neg, tie = signtest(d)
if "i4" in MODELS and "k4" in MODELS:
  print(f"\nINTERACTION (i4 gain from small kick) - (k4 gain from small kick)")
  print(f"   {mu:+6.2f}pp  [{lo:+6.2f}, {hi:+6.2f}]   median {median(d):+5.1f}"
        f"   keys +{pos}/-{neg}/={tie}"
        + ("   <- CI excludes 0" if lo > 0 or hi < 0 else "   <- CI spans 0"))
live = sum(1 for i in range(len(rate[(SIZES[0], MODELS[0])]))
           if any(rate[(s2,m2)][i] > 0 for s2 in SIZES for m2 in MODELS))
print(f"\nKEYS WITH ANY SUCCESS: {live}/{len(rate[(SIZES[0], MODELS[0])])}  "
      f"(keys at 0 everywhere contribute an exact 0 to every contrast)")
mx = {(s2,m2): max(rate[(s2,m2)]) for s2 in SIZES for m2 in MODELS}
print("MAX per-key rate, to expose single-key dominance: "
      + "  ".join(f"{s2}p/{m2} {mx[(s2,m2)]:.0f}%" for s2 in SIZES for m2 in MODELS))
