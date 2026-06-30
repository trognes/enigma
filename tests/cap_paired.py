#!/usr/bin/env python3
# Paired IC-vs-mono pre-pass comparison on short texts (same trials per model,
# so a paired CI). Run from anywhere: python3 tests/cap_paired.py
import os
os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUREOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDINTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERALYEARSANDSAVEDCOUNTLESSLIVES"
SEED=1; PAIRS=10; N=1000; CAP=5
def run(args, text):
    return subprocess.run(["./enigma"]+args, input=text, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, universal_newlines=True).stdout.strip()
def pct(rec, truth):
    return 100.0*sum(a==b for a,b in zip(rec,truth))/len(truth)
def trials(L):
    rng=random.Random(SEED*1000003+L); out=[]
    for _ in range(N):
        off=rng.randrange(len(CORPUS)-L+1); ex=CORPUS[off:off+L]
        u=rng.choice("ABC"); w="".join(str(d) for d in rng.sample(range(1,9),3))
        r="".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        g="".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        ls=rng.sample(string.ascii_uppercase,2*PAIRS)
        pb=" ".join(ls[2*i]+ls[2*i+1] for i in range(PAIRS))
        out.append((ex,u,w,r,g,pb))
    return out
print("cond        n   mean(mono-IC)   95%CI            paired-t   verdict")
for L in (40,70):
    for R in (1,10):
        ts=trials(L); diffs=[]
        for ex,u,w,r,g,pb in ts:
            ct=run(["-i","-u",u,"-w",w,"-r",r,"-g",g,"-s",pb], ex)
            base=["-q","-l","english","-u",u,"-w",w,"-r",r,"-g",g,"-c","-R",str(R),"-L",str(CAP)]
            ic =pct(run(base+["-S","i"], ct), ex)
            mo =pct(run(base+["-S","m"], ct), ex)
            diffs.append(mo-ic)
        n=len(diffs); m=sum(diffs)/n
        sd=math.sqrt(sum((d-m)**2 for d in diffs)/(n-1)); se=sd/math.sqrt(n)
        t=m/se if se>0 else 0.0; lo,hi=m-1.96*se,m+1.96*se
        verdict="mono>IC" if lo>0 else ("IC>mono" if hi<0 else "tie")
        print(f"L{L},R{R:<2}  {n:5d}   {m:+6.2f}        [{lo:+5.2f},{hi:+5.2f}]    {t:+6.2f}    {verdict}")
