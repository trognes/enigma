#!/usr/bin/env python3
#
# --repair3 on the tangle cases (PERFORMANCE.md section 6.14).
#
# Would try_repair_3 (--repair3) actually SOLVE the tangle cases (ignoring its
# higher per-climb cost)? Run the IDENTICAL problem set (same seed) twice --
# baseline vs +--repair3, best of -R -- and cross-tabulate. Predicted by the
# swap-component finding: --repair3 re-pairs 3 plugs = 6 letters at once, so it
# should crack tangles whose knot spans <=6 letters and miss the 8-10 ones.

import os, sys, random, statistics as st, collections
sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
import eval as E  # noqa

BIN   = "./enigma"
LANGS = os.environ.get("LANGS", "english german").split()
LENS  = [int(x) for x in os.environ.get("LENS", "55 60 65").split()]
PAIRS = int(os.environ.get("PAIRS", "10"))
NPROB = int(os.environ.get("NPROB", "120"))
R     = int(os.environ.get("R", "40"))
BASE  = os.environ.get("BASE", "-J -S i4q10").split()
SEED  = int(os.environ.get("SEED", "12345"))
os.environ["ENIGMA_DATA"] = "ngrams"
os.environ["ENIGMA_SEED"] = "0"


def pairset(pb):
    toks = pb.split() if " " in pb else [pb[i:i+2] for i in range(0, len(pb), 2)]
    return {frozenset(t) for t in toks if len(t) == 2}


def endpoints(ps):
    s = set()
    for p in ps:
        s |= p
    return s


def comp_size(wrong, miss):
    letters = endpoints(set(wrong))
    changed = True
    used = set()
    while changed:
        changed = False
        for m in miss:
            if m not in used and (m & letters):
                used.add(m); letters |= m; changed = True
    return len(letters)


def solve(lang, key, ct, opts):
    u, w, r, g, _ = key
    args = ["-q", "-l", lang, "-c", "-R", str(R), "-u", u, "-w", w,
            "-r", r, "-g", g] + opts
    _, err, _ = E.run(BIN, args, ct)
    _, plugs = E.parse_recovered(err)
    return pairset(plugs), E.score_iter(err)


rows = []
for lang in LANGS:
    corpora = E.load_corpora(lang)
    for L in LENS:
        rng = random.Random(SEED)
        for _ in range(NPROB):
            _, excerpt, key = E.gen_problem(rng, corpora, L, PAIRS)
            ct = E.encrypt(BIN, key, excerpt)
            tset = pairset(key[4])
            b_set, b_si = solve(lang, key, ct, BASE)
            r_set, r_si = solve(lang, key, ct, BASE + ["--repair3"])
            b_wrong = len(b_set - tset)
            r_wrong = len(r_set - tset)
            comp = comp_size(b_set - tset, tset - b_set) if b_wrong else 0
            rows.append(dict(bw=b_wrong, rw=r_wrong, comp=comp,
                             b_si=b_si, r_si=r_si,
                             b_solved=(b_set == tset), r_solved=(r_set == tset)))

n = len(rows)
b_solved = sum(r["b_solved"] for r in rows)
r_solved = sum(r["r_solved"] for r in rows)
print(f"problems: {n}   exact-solved  baseline {b_solved}  --repair3 {r_solved}"
      f"  (Δ {r_solved - b_solved:+d})")

# the few-wrong (tangle) population under BASELINE
few = [r for r in rows if 1 <= r["bw"] <= 3]
fixed = [r for r in few if r["r_solved"]]
improved = [r for r in few if r["rw"] < r["bw"] and not r["r_solved"]]
same = [r for r in few if r["rw"] == r["bw"]]
worse = [r for r in few if r["rw"] > r["bw"]]
print(f"\nbaseline few-wrong (1..3): {len(few)}")
print(f"  --repair3 SOLVED them (0 wrong):   {len(fixed)}")
print(f"  --repair3 improved (fewer wrong):  {len(improved)}")
print(f"  unchanged wrong-count:             {len(same)}")
print(f"  worse:                             {len(worse)}")

# predicted split by swap-component size (<=6 fits a 3-plug re-pair, >6 doesn't)
print("\nfew-wrong outcome by swap-component size:")
print("  comp | n  | solved by --repair3")
for lo, hi, lab in [(2, 6, "<=6 (<=3 plugs)"), (7, 99, ">6 (4-5 plugs)")]:
    b = [r for r in few if lo <= r["comp"] <= hi]
    s = sum(r["r_solved"] for r in b)
    print(f"  {lab:16s} | {len(b):2d} | {s}")

# also: did --repair3 break any baseline solves?
broke = [r for r in rows if r["b_solved"] and not r["r_solved"]]
print(f"\nbaseline solves broken by --repair3: {len(broke)}")

# compute cost
br = st.mean(r["b_si"] for r in rows if r["b_si"])
rr = st.mean(r["r_si"] for r in rows if r["r_si"])
print(f"mean score_iter  baseline {br:.0f}  --repair3 {rr:.0f}  ({rr/br:.2f}x)")
