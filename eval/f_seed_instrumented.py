"""Instrumented k4 -> f10 pipeline: 4- vs 10-plug kick, with and without -M.

Per candidate, records the board and the score at BOTH stages, so the
intermediate state is visible rather than collapsed to a boolean:

  k4_plugs / k4_correct   plugs after the cap-4 climb, and how many of them
                          are pairs of the true board
  k4_pct                  %-correct letters decrypting under the k4 board
  k4_score_k              the k4 climb's own score (its target model)
  k4_score_f              the SAME board rescored under -f  (extra invocation)
  f10_*                   the same four after the full f10 continuation

Kick draws: the 4-pair and 10-pair kicks are INDEPENDENT draws.  The same
kicks are reused for -M on and off, so that axis is paired.

FOR THE NEXT RUN: ALSO SAVE THE BOARD STRINGS.  This version records plug
COUNTS and scores but not the boards themselves, which cost two analyses:
seed/basin diversity had to be estimated from the count of distinct SCORE
values (an undercount whenever two boards score alike to 4 dp), and no
board-level question -- which plugs are found first, whether two arms reach
the same optimum, how far apart converged boards are -- can be asked of the
saved data at all.  The strings are already in hand at the point the row is
written; adding k4_board and f10_board columns costs nothing but file size.
"""
import os, random, subprocess, sys, importlib.util
from concurrent.futures import ThreadPoolExecutor

HERE = "eval"; sys.path.insert(0, HERE)
import enigma_ref
spec = importlib.util.spec_from_file_location("pab", HERE + "/prepass_ab.py")
pab = importlib.util.module_from_spec(spec); spec.loader.exec_module(pab)

L     = int(os.environ.get("L", 100))
KEYS  = int(os.environ.get("KEYS", 500))
KICKS = int(os.environ.get("KICKS", 40))
JOBS  = int(os.environ.get("JOBS", 4))
SEED  = int(os.environ.get("SEED", 4242))
OUT   = os.environ.get("OUT", "f_instrumented.csv")
SIZES = (4, 10)

corpus = "".join(pab.decrypts(HERE + "/enigma-messages.txt")
                 + pab.decrypts(HERE + "/enigma-army-messages-1941.txt"))
letters = [chr(65 + i) for i in range(26)]
env = dict(os.environ, ENIGMA_SEED="0")

def core_table(w, r, g, n):
    cols = [enigma_ref.decrypt(chr(65 + x) * n, w, r, g, "") for x in range(26)]
    return [[ord(cols[x][i]) - 65 for x in range(26)] for i in range(n)]

def decrypt_pct(board, ctn, core, ptn):
    s = enigma_ref._plugboard(board)
    return 100.0 * sum(s[core[i][s[c]]] == ptn[i]
                       for i, c in enumerate(ctn)) / len(ptn)

def pairset(board):
    return {frozenset(p) for p in board.split() if len(p) == 2}

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

fh = open(OUT, "w")
fh.write("key,size,M,kick,k4_plugs,k4_correct,k4_pct,k4_score_k,k4_score_f,"
         "f10_plugs,f10_correct,f10_pct,f10_score_f\n")

for kx, (pt, w, r, g, truth) in enumerate(keys):
    ct, _ = pab.run(["-u","B","-w",w,"-r",r,"-g",g,"-s",truth], pt)
    core = core_table(w, r, g, L)
    ctn = [ord(c) - 65 for c in ct]; ptn = [ord(c) - 65 for c in pt]
    tset = pairset(truth)
    base = ["./enigma","-u","B","-w",w,"-r",r,"-g",g,"-l","wehrmacht","-T","1"]

    def dump(args):
        p = subprocess.run(base + args, input=ct, capture_output=True,
                           text=True, env=env)
        d = [l for l in p.stderr.splitlines() if l.startswith("dumpall")]
        if not d:
            return None, "", p.stdout.strip()
        f = d[0].split()
        return float(f[4]), " ".join(f[5:]), p.stdout.strip()

    def one(job):
        size, mflag, ki, kick = job
        m = ["-M"] if mflag else []
        sk, bk, _ = dump(["-c","-J","-S","k4","-R","0","--dump-all",
                          "--soft-plug", kick.replace(" ", "")] + m)
        if bk == "":
            return None
        # the SAME k4 board rescored under -f, no climb
        p = subprocess.run(base + ["-s", bk, "-f"], input=ct,
                           capture_output=True, text=True, env=env)
        ln = [l for l in p.stderr.splitlines() if l.strip().startswith("-")]
        skf = float(ln[0].split()[0]) if ln else float("nan")
        sf, bf, out = dump(["-c","-J","-S","f10","-R","0","--dump-all",
                            "--soft-plug", bk.replace(" ", "")] + m)
        kp, fp = pairset(bk), pairset(bf)
        return (kx, size, int(mflag), ki, len(kp), len(kp & tset),
                round(decrypt_pct(bk, ctn, core, ptn), 1), sk, skf,
                len(fp), len(fp & tset),
                round(100.0*sum(a == b for a, b in zip(out, pt))/L, 1), sf)

    jobs = []
    for size in SIZES:
        kr2 = random.Random(SEED * 100000 + kx * 100 + size)
        for ki in range(KICKS):
            pl = kr2.sample(letters, 2 * size)
            kick = " ".join(pl[i] + pl[i+1] for i in range(0, 2 * size, 2))
            for mflag in (False, True):
                jobs.append((size, mflag, ki, kick))
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        rows = [x for x in ex.map(one, jobs) if x]
    for row in rows:
        fh.write(",".join(str(v) for v in row) + "\n")
    fh.flush()
    print(f"[progress] key {kx+1}/{KEYS}", file=sys.stderr, flush=True)
fh.close()
print(f"[done] wrote {OUT}", file=sys.stderr, flush=True)
