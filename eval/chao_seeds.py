"""How many distinct seeds exist per key?  Richness estimation, not rarefaction.

eval/rarefy_restarts.py answers "how many distinct seeds have I SEEN by -R n",
which leaves the asymptote to a curve fit -- and the fits disagree by 30x on the
default kick, because that curve has not begun to saturate (its local exponent
is still 0.63 at n = 10000, so one more doubling of R would pass both fitted
asymptotes).

The multiplicity distribution answers it better.  Chao1 uses the number of seeds
seen EXACTLY ONCE and EXACTLY TWICE: rare seeds are what an incomplete sample
tells you about the unseen ones, and the estimator is a genuine lower bound on
richness rather than a shape assumption.  Good-Turing coverage (1 - f1/n) says
what fraction of the probability mass has been sampled at all.

Both are computed PER KEY and then averaged, because the pool is per key -- that
is the unit --seed-dedup filters over, and pooling keys would count a seed found
under two different rotor keys as one.

THE kick-4 ARM IS THE CONTROL.  It has genuinely saturated (0% new at n = 4000),
so its Chao1 must come back near its observed 152.7; an estimator that
overshoots there cannot be trusted on the arm that has not saturated.

Same design as rarefy_restarts.py: --dump-all gives every converged restart's
board, so one invocation at -R N yields N boards.
"""
import os, random, subprocess, sys, statistics, importlib.util
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
HERE="eval"; sys.path.insert(0,HERE)
spec=importlib.util.spec_from_file_location("pab",HERE+"/prepass_ab.py")
pab=importlib.util.module_from_spec(spec); spec.loader.exec_module(pab)
corpus="".join(pab.decrypts(HERE+"/enigma-messages.txt")
               +pab.decrypts(HERE+"/enigma-army-messages-1941.txt"))
L=int(os.environ.get("L",100)); KEYS=int(os.environ.get("KEYS",100))
R=int(os.environ.get("R",10000)); SEED=int(os.environ.get("SEED",808))
env=dict(os.environ, ENIGMA_SEED="0"); letters=[chr(65+i) for i in range(26)]
rng=random.Random(SEED); keys=[]
for _ in range(KEYS):
    pt=corpus[rng.randrange(0,len(corpus)-L):][:L]
    w="".join(str(x) for x in rng.sample(range(1,6),3))
    r="".join(chr(65+rng.randrange(26)) for _ in range(3))
    g="".join(chr(65+rng.randrange(26)) for _ in range(3))
    ls=rng.sample(letters,20)
    keys.append((pt,w,r,g," ".join(ls[i]+ls[i+1] for i in range(0,20,2))))
ALL_ARMS={"kick4":  ("k4 seeds, kick 4  (CONTROL: saturated)",
                     ["-S","k4","--random","4"]),
          "kick10": ("k4 seeds, kick 10 (the default)",
                     ["-S","k4","--random","10"]),
          "final":  ("k4f10 final board, kick 10",
                     ["-S","k4f10","--random","10"])}
ARMS=[ALL_ARMS[a] for a in
      os.environ.get("ARMS","kick4,kick10,final").split(",")]
GRID=[n for n in [8,32,128,512,1000,2000,4000,10000,20000,50000,
                  100000,200000,500000] if n<=R]

def stats(job):
    """Run one key and reduce it IN THE WORKER to the summary statistics.

    The boards are not returned.  Holding them costs ~90 bytes per restart per
    key in CPython -- 450 MB at 100 keys x -R 50000, and it grows linearly with
    a budget this measurement exists to push -- while the summary is a dozen
    numbers.  Reducing here is what lets R rise without the harness becoming
    the limit."""
    (pt,w,r,g,truth), args = job
    ct,_=pab.run(["-u","B","-w",w,"-r",r,"-g",g,"-s",truth],pt)
    p=subprocess.run(["./enigma","-u","B","-w",w,"-r",r,"-g",g,"-c","-J",
                      "-l","wehrmacht","-T","1","-R",str(R),"--dump-all"]+args,
                     input=ct,capture_output=True,text=True,env=env)
    bl=[" ".join(l.split()[5:]) for l in p.stderr.splitlines()
        if l.startswith("dumpall")]
    if len(bl)<R:
        return None
    grid=[len(set(bl[:n])) for n in GRID]      # rarefaction, same key
    c=Counter(bl); s_obs=len(c)
    f=Counter(c.values()); f1=f[1]; f2=f[2]; n=len(bl)
    # Chao1, bias-corrected form -- defined even when f2 == 0, which the
    # classic f1^2/(2 f2) is not.
    chao=s_obs + f1*(f1-1)/(2.0*(f2+1))
    cov=1.0-f1/float(n)                       # Good-Turing sample coverage
    return grid,s_obs,f1,f2,chao,cov
print(f"richness of the seed pool, L={L}, {KEYS} keys, -R {R}, "
      f"boards from --dump-all\n")
for lab,args in ARMS:
    with ThreadPoolExecutor(max_workers=4) as ex:
        st=[x for x in ex.map(stats,[(k,args) for k in keys]) if x is not None]
    m=lambda i: statistics.mean(x[i] for x in st)
    print(f"{lab}   ({len(st)} keys with a full {R} dumps)")
    print("   rarefaction:  " + "  ".join(
        f"{n}:{statistics.mean(x[0][j] for x in st):.1f}"
        for j,n in enumerate(GRID)))
    print(f"   observed distinct      {m(1):>10.1f}")
    print(f"   singletons f1          {m(2):>10.1f}")
    print(f"   doubletons f2          {m(3):>10.1f}")
    print(f"   Chao1 (lower bound)    {m(4):>10.1f}   "
          f"= {m(4)/m(1):.2f}x observed")
    print(f"   Good-Turing coverage   {100*m(5):>10.1f}%   "
          f"({100*(1-m(5)):.1f}% of the mass never sampled)")
    print()
