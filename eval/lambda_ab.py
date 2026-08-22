"""Paired A/B of the -S k lambda retune: 0.1*L against 1.1*sqrt(L).

Both arms are k4f10 in the SAME binary; the old rule is reproduced exactly by
ENIGMA_MONOIC_BLEND, since at a fixed length both rules are constants:
    g * sqrt(L) = 0.1 * L   =>   g = 0.1 * sqrt(L)
That makes this a genuine paired comparison of the retune itself, which
comparing each arm against i4f10 separately cannot be.
"""
import math, os, random, subprocess, sys, importlib.util

HERE = "eval"
spec = importlib.util.spec_from_file_location("pab", HERE + "/prepass_ab.py")
pab = importlib.util.module_from_spec(spec); spec.loader.exec_module(pab)

L = int(sys.argv[1]); TRIALS = int(sys.argv[2]); SEED = 4242
corpus = "".join(pab.decrypts(HERE + "/enigma-messages.txt")
                 + pab.decrypts(HERE + "/enigma-army-messages-1941.txt"))
old_g = 0.1 * math.sqrt(L)          # reproduces lambda = 0.1*L at this length
rng = random.Random(SEED)
base = dict(os.environ, ENIGMA_SEED="0")

def recover(ct, w, r, g, env):
    p = subprocess.run(["./enigma", "-u", "B", "-w", w, "-r", r, "-g", g, "-c",
                        "-J", "--polish", "-l", "wehrmacht", "-S", "k4f10",
                        "-R", "8", "-T", "4", "-e", "7"],
                       input=ct, capture_output=True, text=True, env=env)
    return p.stdout.strip()

def pct(a, b):
    return 100.0 * sum(x == y for x, y in zip(a, b)) / len(b)

env_new = base
env_old = dict(base, ENIGMA_MONOIC_BLEND=repr(old_g))
dif = []; ex_new = ex_old = 0; only_new = only_old = 0
for _ in range(TRIALS):
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    w = "".join(str(x) for x in rng.sample(range(1, 6), 3))
    r = "".join(chr(65 + rng.randrange(26)) for _ in range(3))
    g = "".join(chr(65 + rng.randrange(26)) for _ in range(3))
    ls = rng.sample([chr(65 + i) for i in range(26)], 20)
    pg = " ".join(ls[i] + ls[i + 1] for i in range(0, 20, 2))
    ct, _ = pab.run(["-u", "B", "-w", w, "-r", r, "-g", g, "-s", pg], pt)
    a = pct(recover(ct, w, r, g, env_new), pt)
    b = pct(recover(ct, w, r, g, env_old), pt)
    dif.append(a - b)
    na, nb = a > 99.999, b > 99.999
    ex_new += na; ex_old += nb
    only_new += na and not nb; only_old += nb and not na

n = len(dif); mu = sum(dif) / n
sd = math.sqrt(sum((d - mu) ** 2 for d in dif) / (n - 1)) if n > 1 else 0.0
se = sd / math.sqrt(n)
print(f"L={L}  n={n}  -R 8  new lambda=1.1*sqrt(L)  old emulated by "
      f"ENIGMA_MONOIC_BLEND={old_g:.4f}")
print(f"  exact: new {ex_new}/{n}   old {ex_old}/{n}   "
      f"(only-new {only_new}, only-old {only_old})")
print(f"  paired diff (new - old): {mu:+.2f} pp  "
      f"95% CI [{mu-1.96*se:+.2f}, {mu+1.96*se:+.2f}]")
