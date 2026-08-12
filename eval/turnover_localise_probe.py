#!/usr/bin/env python3
"""Can the right wheel's PHASE be read straight off one decrypt?

    python3 eval/turnover_localise_probe.py 167 12 5      # L, trials, seed

THE IDEA.  Shift the right wheel's phase -- ring2 and start2 together by delta
-- and offset2 is unchanged, so that wheel's own contribution to the
substitution is identical at every position.  The ONLY thing that moves is when
its notch fires, so the middle wheel is displaced by exactly one step on a
contiguous cyclic block of columns, repeating with period 26.  The same columns
are corrupt in every 26-letter window and the rest are clean plaintext.  So
instead of SEARCHING the phase (--tune-phase scans 26x26 with the board frozen),
fold the per-position score by i mod 26, find the low-scoring block, and its
boundary IS the notch position -- the phase read off in O(1) decrypts rather
than O(676) scorings.

WHAT THIS MEASURES.  With the board KNOWN and the offsets correct -- the best
case, and a precondition the method needs anyway -- shift the phase by delta,
fold, fit the best contiguous block, and compare it with the block that is
actually corrupt.  No search: this asks only whether the structure is LEGIBLE,
which is the premise everything else rests on.

RESULT: the physics is exactly right, the signal is real, and it still does not
localise at operational length.

  * min(delta, 26-delta) confirmed -- delta=1 and delta=25 both give a 1-column
    block, delta=13 gives 13.  So x is in [1,13].
  * The signal exists: corrupt columns score 0.6-0.8 log10/char below clean ones
    (the sep@truth column).
  * But THE FIT OVERFITS.  Over 26 starts x 13 lengths against 26 noisy columns
    the best-separating block beats the true one -- at L=167, 1.02 against 0.68
    at the truth.  Exact block recovery is 0% at L=167, 8% at L=400, 25% at
    L=900 (delta=13, the easiest case).  ~6 periods is not enough data for a
    two-parameter fit.

A CONFOUND THE IDEA DOES NOT ACCOUNT FOR: the left wheel.  Displacing the middle
wheel also changes when the MIDDLE wheel passes its own notch, so the left wheel
steps elsewhere and everything after it is corrupt -- breaking the clean mod-26
periodicity the method assumes.  Measured: delta=2 should give a 2-column block
but averages 3.8, and delta=5 gives 6.3-9.0 rather than 5.

UNTESTED REFINEMENT THAT MIGHT RESCUE IT.  Scoring a block only asks "is this
region bad".  A sharper test costs almost nothing: in the corrupt block the
middle wheel is off by EXACTLY ONE, so re-decrypting that stretch with the
middle wheel stepped +/-1 should turn it into clean German.  That is a
verification rather than a ranking, and it would not overfit the way a
two-parameter score fit does.  Not implemented here.

Bigrams, not quadgrams, on purpose: a quadgram ending at i spans i-3..i and so
smears the block edge by three columns -- most of the block when delta is small.
"""
import math, random, re, subprocess, sys, collections
LET="ABCDEFGHIJKLMNOPQRSTUVWXYZ"

def load_quads(path):
    q={}; tot=0
    for line in open(path, encoding='utf-8'):
        p=line.split()
        if len(p)==2 and len(p[0])==4:
            q[p[0]]=int(p[1]); tot+=int(p[1])
    floor=math.log10(1.0/tot)
    return {k: math.log10(v/tot) for k,v in q.items()}, floor
QUAD, FLOOR = load_quads('ngrams/wehrmacht_quadgrams.txt')

def load_bi(path):
    q={}; tot=0
    for line in open(path, encoding='utf-8'):
        p=line.split()
        if len(p)==2 and len(p[0])==2:
            q[p[0]]=int(p[1]); tot+=int(p[1])
    return {k: math.log10(v/tot) for k,v in q.items()}, math.log10(1.0/tot)
BI, BFLOOR = load_bi('ngrams/wehrmacht_bigrams.txt')

def posscore(txt):
    """log10 P of the bigram ENDING at each position -- smears the block edge by
    ONE position instead of the quadgram's three, which matters when the block
    is only a few columns wide."""
    return [None] + [BI.get(txt[i-1:i+1], BFLOOR) for i in range(1, len(txt))]

def decrypts(p):
    o=[]
    for b in open(p,encoding='utf-8').read().split('### Message ')[1:]:
        m=re.search(r'^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)',b,re.S|re.M)
        if m: o.append(re.sub(r'[^A-Z]','',m.group(1)))
    return o
C="".join(decrypts('eval/enigma-messages.txt')
          + decrypts('eval/enigma-army-messages-1941.txt'))

def run(a,t):
    return subprocess.run(["./enigma"]+a, input=t, capture_output=True,
                          text=True).stdout.strip()

def shift(ch,d): return LET[(LET.index(ch)+d)%26]

def fit_block(cols):
    """best contiguous cyclic block of length 1..13: the one whose mean score is
    lowest relative to the rest. Returns (start, length, separation)."""
    best=None
    m=len(cols)
    for start in range(m):
        for ln in range(1,14):
            idx=[(start+k)%m for k in range(ln)]
            inb=[cols[i] for i in idx if cols[i] is not None]
            out=[cols[i] for i in range(m)
                 if i not in idx and cols[i] is not None]
            if not inb or not out: continue
            sep=sum(out)/len(out)-sum(inb)/len(inb)      # want IN low, OUT high
            if best is None or sep>best[2]: best=(start,ln,sep)
    return best

def trial(rng,L,delta):
    pt=C[rng.randrange(0,len(C)-L):][:L]
    w="".join(str(x) for x in rng.sample([1,2,3,4,5],3))
    r="".join(rng.choice(LET) for _ in range(3))
    g="".join(rng.choice(LET) for _ in range(3))
    ls=list(LET); rng.shuffle(ls)
    pb=" ".join(ls[2*i]+ls[2*i+1] for i in range(10))
    ct=run(["-u","B","-w",w,"-r",r,"-g",g,"-s",pb],pt)
    # decrypt with the RIGHT WHEEL'S PHASE shifted by delta: ring2 and start2
    # both move, so offset2 -- and the whole substitution -- is unchanged.
    r2=r[:2]+shift(r[2],delta); g2=g[:2]+shift(g[2],delta)
    bad_dec=run(["-u","B","-w",w,"-r",r2,"-g",g2,"-s",pb],ct)
    if len(bad_dec)!=L: return None
    wrong=[a!=b for a,b in zip(bad_dec,pt)]
    truex=sum(wrong)/ (L/26.0)          # wrong per 26 -> the block length
    # fold the per-position quadgram score by i mod 26
    sc=posscore(bad_dec)
    cols=[None]*26; acc=collections.defaultdict(list)
    for i,s in enumerate(sc):
        if s is not None: acc[i%26].append(s)
    for k,v in acc.items(): cols[k]=sum(v)/len(v)
    st,ln,sep=fit_block(cols)
    # ground truth block: which columns are actually corrupted
    tcol=collections.defaultdict(list)
    for i,bad in enumerate(wrong): tcol[i%26].append(bad)
    truth={k for k,v in tcol.items() if sum(v)/len(v)>0.5}
    pred={(st+k)%26 for k in range(ln)}
    # separation AT THE TRUE block: is the signal there at all?
    ti=[c for k,c in enumerate(cols) if k in truth and c is not None]
    to=[c for k,c in enumerate(cols) if k not in truth and c is not None]
    tsep=(sum(to)/len(to)-sum(ti)/len(ti)) if ti and to else float('nan')
    return dict(truex=truex, pred_len=ln, true_len=len(truth),
                exact=(pred==truth), jacc=len(pred&truth)/len(pred|truth),
                sep=sep, tsep=tsep)

L=int(sys.argv[1]); n=int(sys.argv[2]); seed=int(sys.argv[3])
rng=random.Random(seed)
print(f"L={L}  n={n} per delta   (board KNOWN, offsets correct, "
      f"right-wheel phase off by delta)")
print(f"  {'delta':>6}{'x (true block)':>16}{'exact block':>13}"
      f"{'jaccard':>9}{'best sep':>12}{'sep@truth':>12}")
for delta in (2,5,13):
    rs=[]
    for _ in range(n):
        t=trial(rng,L,delta)
        if t: rs.append(t)
    if not rs: continue
    ex=sum(1 for t in rs if t['exact'])/len(rs)
    import statistics as _st
    ts=[t['tsep'] for t in rs if t['tsep']==t['tsep']]
    print(f"  {delta:>6}{sum(t['true_len'] for t in rs)/len(rs):>16.1f}"
          f"{ex:>12.0%}{sum(t['jacc'] for t in rs)/len(rs):>9.2f}"
          f"{sum(t['sep'] for t in rs)/len(rs):>12.2f}"
          f"{(_st.mean(ts) if ts else float('nan')):>12.2f}")

