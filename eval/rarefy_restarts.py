"""Rarefaction of converged boards over the restart budget.

How many of -R N restarts land somewhere ALREADY VISITED?  --dump-all prints
the board of every converged restart, so ONE invocation at -R N yields N
boards instead of N invocations, and they are real board strings rather than
score proxies -- 10 000 restarts cost ~2.7 s (k4) or ~5.8 s (k4f10) per key.

Three arms: the cap-4 SEED under a 4-plug and a 10-plug kick, and the FINAL
board after the f10 continuation.  Seeds are the useful unit for costing,
because a duplicate seed can be detected before the expensive continuation
runs; final boards are the unit CLAUDE.md's restart ladder reports.

Results: eval/results-experiment-f.txt §7.
"""
import os, random, subprocess, sys, statistics, importlib.util
from concurrent.futures import ThreadPoolExecutor
HERE="eval"; sys.path.insert(0,HERE)
spec=importlib.util.spec_from_file_location("pab",HERE+"/prepass_ab.py")
pab=importlib.util.module_from_spec(spec); spec.loader.exec_module(pab)
corpus="".join(pab.decrypts(HERE+"/enigma-messages.txt")
               +pab.decrypts(HERE+"/enigma-army-messages-1941.txt"))
L=int(os.environ.get("L",100)); KEYS=int(os.environ.get("KEYS",100))
R=int(os.environ.get("R",1000)); SEED=int(os.environ.get("SEED",808))
env=dict(os.environ, ENIGMA_SEED="0"); letters=[chr(65+i) for i in range(26)]
rng=random.Random(SEED); keys=[]
for _ in range(KEYS):
    pt=corpus[rng.randrange(0,len(corpus)-L):][:L]
    w="".join(str(x) for x in rng.sample(range(1,6),3))
    r="".join(chr(65+rng.randrange(26)) for _ in range(3))
    g="".join(chr(65+rng.randrange(26)) for _ in range(3))
    ls=rng.sample(letters,20)
    keys.append((pt,w,r,g," ".join(ls[i]+ls[i+1] for i in range(0,20,2))))
ARMS=[("k4 seeds, kick 4",  ["-S","k4","--random","4"]),
      ("k4 seeds, kick 10", ["-S","k4","--random","10"]),
      ("k4f10 final, kick 10",["-S","k4f10","--random","10"])]
def boards(job):
    (pt,w,r,g,truth), args = job
    ct,_=pab.run(["-u","B","-w",w,"-r",r,"-g",g,"-s",truth],pt)
    p=subprocess.run(["./enigma","-u","B","-w",w,"-r",r,"-g",g,"-c","-J",
                      "-l","wehrmacht","-T","1","-R",str(R),"--dump-all"]+args,
                     input=ct,capture_output=True,text=True,env=env)
    return [" ".join(l.split()[5:]) for l in p.stderr.splitlines()
            if l.startswith("dumpall")]
GRID=[8,32,128,512,1000,2000,4000,10000]
print(f"rarefaction, L={L}, {KEYS} keys, -R {R}, boards from --dump-all\n")
for lab,args in ARMS:
    with ThreadPoolExecutor(max_workers=4) as ex:
        allb=[b for b in ex.map(boards,[(k,args) for k in keys]) if len(b)>=R]
    print(f"{lab}   ({len(allb)} keys with a full {R} dumps)")
    print(f"   {'n':>6} {'distinct':>9} {'dup %':>7} {'next is new':>12}")
    prev=None
    for n in GRID:
        d=statistics.mean(len(set(b[:n])) for b in allb)
        nx=statistics.mean(len(set(b[:min(n+1,R)]))-len(set(b[:n])) for b in allb) \
           if n<R else float('nan')
        print(f"   {n:>6} {d:>9.1f} {100*(n-d)/n:>6.1f}% {100*nx:>11.1f}%")
    print()
