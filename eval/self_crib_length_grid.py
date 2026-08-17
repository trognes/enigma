#!/usr/bin/env python3
"""The full --self-crib-length grid: signature vs anywhere, L = 4..9, vs -R.

    python3 eval/self_crib_length_grid.py

Fixed population -- every corpus message carrying a 4+ doubling ANYWHERE -- so
the floor is the only thing moving and lowering it can only add hypotheses.
K = 10, the measured operating point; 676-key sweeps, 10-pair board hidden.

-R 0 is one deterministic climb with NO KICK; -R N for N>=1 is N kicked climbs.
"""
import sys, random, re, subprocess, time, numpy as np
sys.path.insert(0,'eval')
import selfcrib_probe as SC
from crib_menu import corpus
from ring_stride_geometry_probe import crypt, plugboard
RECIPE=["-c","-f","-J","--polish","-S","i4f10"]
KEY=["-u","B","-w","231","-r","AAA","-g","A..","-l","wehrmacht","-T","4"]
def run(a,t):
    t0=time.perf_counter()
    p=subprocess.run(["./enigma"]+a,input=t,capture_output=True,text=True)
    w=time.perf_counter()-t0
    it=0;h=0
    for l in p.stderr.splitlines():
        m=re.search(r"scored (\d+) plugboards",l)
        if m: it=int(m.group(1))
        m=re.search(r"(\d+) hypotheses",l)
        if m: h=int(m.group(1))
    return p.stdout.strip(),it,w,h
def pct(r,t): return 100.0*sum(a==b for a,b in zip(r,t))/len(t) if r else 0.0
ARMS=[("-R 0",["-R","0"]),("-R 1",["-R","1"]),("-R 16",["-R","16"])]
for L in range(4,10):
    ARMS.append(("sig L%d"%L,["-R","0","--self-crib-seeds","10",
                              "--self-crib-signature",
                              "--self-crib-length",str(L)]))
for L in range(4,10):
    ARMS.append(("any L%d"%L,["-R","0","--self-crib-seeds","10",
                              "--self-crib-length",str(L)]))
rng=random.Random(20260822)          # same seed/population as the length sweep
msgs=[t for t in corpus() if SC.doublings(t,minlen=4,maxlen=20)]
REPS=2
res={t:[[],[],[],[]] for t,_ in ARMS}
print("population: %d messages with a 4+ doubling anywhere, %d trials, K=10"
      %(len(msgs),len(msgs)*REPS),flush=True)
n=0
for pt in msgs*REPS:
    g1,g2=rng.randrange(26),rng.randrange(26)
    plug=plugboard(np.random.default_rng(rng.randrange(1<<30)),10)
    ct=crypt(pt,[1,2,0],'B',np.array([0,0,0]),np.array([0,g1,g2]),plug)
    for tag,extra in ARMS:
        rec,it,w,h=run(RECIPE+KEY+extra,ct)
        r=res[tag]
        r[0].append(pct(rec,pt)); r[1].append(it)
        r[2].append(w); r[3].append(h)
    n+=1
    if n%5==0: print("  %d/%d"%(n,len(msgs)*REPS),flush=True)
print("\n%-9s %-9s %-10s %-11s %-10s %s"
      %("arm","mean %","exact","wall/trial","per key","hyps"))
for tag,_ in ARMS:
    m=np.array(res[tag][0]); w=np.array(res[tag][2]); h=np.array(res[tag][3])
    print("%-9s %-9.1f %-10s %-11.2f %-10.0f %.0f"
          %(tag,m.mean(),"%d/%d"%((m>99.999).sum(),m.size),w.mean(),
            1e6*w.mean()/676,h.mean()))
