#!/usr/bin/env python3
#
# Directed plug repair via quadgram "gain" -- component probe (PERFORMANCE.md 4.10).
#
# Self-contained prototype of the gain-cascade near-solution finisher, measured on
# REAL tool-converged boards from eval/results*.tsv. It reconstructs each instance
# from true_* + plaintext, takes recovered_plugs as the converged (real) board, and
# reports, on near-solution search-failure boards:
#   - dual gain-vote coverage (a correct/missing plug is in the shortlist)
#   - full-plug hit@1 (top-ranked single candidate is a missing plug)
#   - cascade+reclimb solve rate vs reclimb-alone
#
# The rotor core is extracted EXACTLY from the tool via 26 empty-plugboard decodes
# of x*n (Enigma stepping is content-independent), so decode/gain are machine-exact.
# The climb here is a simplified quad-only greedy climb (prototype): strong enough
# to reclimb a near-solution board, too weak to PRODUCE one -- which is why the
# end-to-end matched-compute verdict needs an in-tool implementation (see 4.10).
#
# Usage:   python3 eval/gain_cascade_probe.py
# Env:     LANGS PER MISSLO MISSHI SEED   (defaults reproduce the section-4.10 numbers)

import os, sys, glob, csv, math, random, collections, statistics as st
sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
import eval as E  # noqa

BIN   = "./enigma"
LANGS = os.environ.get("LANGS", "english german").split()
PER   = int(os.environ.get("PER", "150"))
MISSLO = int(os.environ.get("MISSLO", "2"))
MISSHI = int(os.environ.get("MISSHI", "4"))
N1 = int(os.environ.get("N1", "8")); N2 = int(os.environ.get("N2", "8"))
SEED = int(os.environ.get("SEED", "11"))
os.environ["ENIGMA_DATA"] = "ngrams"
AZ = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
A2I = {c: i for i, c in enumerate(AZ)}

# quad tables as packed-int -> log10 prob (A-Z only; accented grams skipped)
QL = {}; FL = {}
for lg in LANGS:
    d = {}; tot = 0; rows = []
    for line in open(f"ngrams/{lg}_quadgrams.txt"):
        p = line.split()
        if len(p) == 2 and len(p[0]) == 4 and p[0].isalpha():
            rows.append((p[0].upper(), int(p[1]))); tot += int(p[1])
    for q, c in rows:
        if all(ch in A2I for ch in q):
            k = 0
            for ch in q:
                k = k * 26 + A2I[ch]
            d[k] = math.log10(c / tot)
    QL[lg] = d; FL[lg] = math.log10(1.0 / tot)


def core_extract(key, n):
    u, w, r, g, _ = key
    core = [[0] * 26 for _ in range(n)]
    for x in range(26):
        out = E.encrypt(BIN, (u, w, r, g, ""), AZ[x] * n).strip()
        for j in range(n):
            core[j][x] = A2I[out[j]]
    return core


def decode(S, ct, core):
    return [S[core[j][S[ct[j]]]] for j in range(len(ct))]


def qscore(lang, pt):
    d = QL[lang]; fl = FL[lang]; s = 0.0
    for i in range(len(pt) - 3):
        s += d.get(((pt[i] * 26 + pt[i+1]) * 26 + pt[i+2]) * 26 + pt[i+3], fl)
    return s


def gain_candidates(lang, ct, pt, S, core, cap=30):
    d = QL[lang]; fl = FL[lang]
    lp = lambda a, b, c, e: d.get(((a * 26 + b) * 26 + c) * 26 + e, fl)
    n = len(pt); votes = collections.Counter()
    for j in range(n):
        lo = max(0, j - 3); hi = min(n - 4, j)
        if hi < lo:
            continue
        cur = sum(lp(pt[i], pt[i+1], pt[i+2], pt[i+3]) for i in range(lo, hi + 1))
        orig = pt[j]; bx = orig; bs = cur
        for x in range(26):
            if x == orig or x == ct[j]:
                continue                                  # no-self-encryption prune
            s = 0.0
            for i in range(lo, hi + 1):
                q = [pt[i], pt[i+1], pt[i+2], pt[i+3]]; q[j - i] = x
                s += lp(q[0], q[1], q[2], q[3])
            if s > bs:
                bs = s; bx = x
        if bs <= cur or bx == orig or bx == ct[j]:
            continue
        g = bs - cur
        r = S[pt[j]]                                       # exit lever
        if r != bx:
            votes[(min(r, bx), max(r, bx))] += g
        y = core[j][S[bx]]                                 # entry lever (reciprocal)
        if y != ct[j]:
            votes[(min(ct[j], y), max(ct[j], y))] += g
    return [pl for pl, _ in votes.most_common(cap)]


def apply_plug(S, a, b):
    T = S[:]
    if T[a] != a:
        T[T[a]] = T[a]
    if T[b] != b:
        T[T[b]] = T[b]
    T[a] = b; T[b] = a
    return T


def toggle(S, a, b):
    T = S[:]
    if T[a] == b:
        T[a] = a; T[b] = b
    else:
        if T[a] != a:
            T[T[a]] = T[a]
        if T[b] != b:
            T[T[b]] = T[b]
        T[a] = b; T[b] = a
    return T


def climb(lang, S, ct, core):
    cur = qscore(lang, decode(S, ct, core))
    improved = True
    while improved:
        improved = False
        best = cur; bestS = None
        for a in range(26):
            for b in range(a + 1, 26):
                sc = qscore(lang, decode(toggle(S, a, b), ct, core))
                if sc > best:
                    best = sc; bestS = toggle(S, a, b)
        if bestS:
            S = bestS; cur = best; improved = True
    return S, cur


def ps(s):
    return {(min(A2I[t[0]], A2I[t[1]]), max(A2I[t[0]], A2I[t[1]]))
            for t in s.split() if len(t) == 2 and t[0] in A2I and t[1] in A2I}


def correct(S, Tset):
    return len({(min(a, S[a]), max(a, S[a])) for a in range(26) if S[a] > a} & Tset)


def rank(lang, S, ct, core, cl, k):
    return sorted(((qscore(lang, decode(apply_plug(S, a, b), ct, core)), (a, b)) for a, b in cl),
                  reverse=True)[:k]


def main():
    rng = random.Random(SEED)
    pool = []
    for f in glob.glob("eval/results*.tsv"):
        try: rdr = csv.DictReader(open(f), delimiter='\t')
        except OSError: continue
        for d in rdr:
            try:
                Ln = int(d['length']); ex = int(d['exact_match']); pc = float(d['letters_matched_pct'])
                np_ = int(d['num_plugs']); rs = float(d['recovered_score']); ts = float(d['true_score'])
            except (ValueError, KeyError, TypeError): continue
            if ex or np_ != 10 or not (40 <= Ln <= 70) or d['language'] not in LANGS:
                continue
            if d['true_reflector'] not in ('A', 'B', 'C') or len(d['true_rotors']) != 3:
                continue
            if not (50 <= pc <= 88) or rs >= ts:                      # near-solution, search failure
                continue
            if not (MISSLO <= len(ps(d['true_plugs']) - ps(d['recovered_plugs'])) <= MISSHI):
                continue
            pool.append(d)
    rng.shuffle(pool)
    print(f"real near-solution search-failure boards ({MISSLO}-{MISSHI} missing): "
          f"{len(pool)} (using {min(PER, len(pool))})")

    cov = h1 = base_solved = casc_solved = n = 0
    cp0 = []; cp_casc = []
    for d in pool[:PER]:
        lang = d['language']
        key = (d['true_reflector'], d['true_rotors'], d['true_ring'], d['true_grund'], d['true_plugs'])
        ct = E.encrypt(BIN, key, d['plaintext'])
        if len(ct) != len(d['plaintext']):
            continue
        cti = [A2I[c] for c in ct]; core = core_extract(key, len(ct))
        S = list(range(26))
        for a, b in ps(d['recovered_plugs']):
            S[a] = b; S[b] = a
        Tset = ps(d['true_plugs']); missing = Tset - ps(d['recovered_plugs'])
        base = qscore(lang, decode(S, cti, core))
        n += 1; cp0.append(correct(S, Tset))
        cl = gain_candidates(lang, cti, decode(S, cti, core), S, core)
        if missing & set(cl):
            cov += 1
        sing = rank(lang, S, cti, core, cl, 1)
        if sing and sing[0][1] in missing:
            h1 += 1
        # reclimb-only baseline
        Sb, _ = climb(lang, S[:], cti, core)
        if correct(Sb, Tset) == 10:
            base_solved += 1
        # cascade (2-ply net-positive) then reclimb
        best_net = 0.0; best_S = None
        for _, (a1, b1) in rank(lang, S, cti, core, cl, N1):
            S1 = apply_plug(S, a1, b1)
            cl2 = gain_candidates(lang, cti, decode(S1, cti, core), S1, core)
            for sc2, (a2, b2) in rank(lang, S1, cti, core, cl2, N2):
                if sc2 - base > best_net:
                    best_net = sc2 - base; best_S = apply_plug(S1, a2, b2)
        Sc = climb(lang, best_S, cti, core)[0] if best_S is not None else S
        cp_casc.append(correct(Sc, Tset))
        if correct(Sc, Tset) == 10:
            casc_solved += 1

    print(f"\nn={n}   mean correct-plugs (converged): {st.mean(cp0):.1f}")
    print(f"  dual gain-vote coverage (missing plug in shortlist): {100*cov/n:3.0f}%")
    print(f"  full-plug single hit@1:                              {100*h1/n:3.0f}%")
    print(f"  reclimb from converged (baseline):   solved {100*base_solved/n:3.0f}%")
    print(f"  cascade-fix + reclimb:               solved {100*casc_solved/n:3.0f}%   "
          f"mean cp {st.mean(cp_casc):.1f}")


if __name__ == "__main__":
    main()
