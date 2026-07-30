#!/usr/bin/env python3
#
# Tangle-vs-solve separability (archived/PERFORMANCE.md section 6.14).
#
# Is a CHEAP tangle detector even theoretically possible? For each message, run
# R restarts with --dump-all, take the best-of-R board, and classify it via
# the oracle as SOLVE (0 wrong/0 miss) / TANGLE (1-3 wrong) / JUNK (4+ wrong).
# Then ask: can any IN-THE-WILD score feature (no oracle) separate a message whose
# best board is a SOLVE from one whose best is a TANGLE? If yes -> a detector that
# says "keep spending compute" is on the table. If SOLVE and TANGLE overlap on
# every feature -> the tangle is unrecognizable without already knowing the answer.
#
# Wild features (per message, from the R converged scores/boards only):
#   best         best-of-R quad score            (message-specific -> AUC per-cell)
#   d_med        best - median(all R scores)      (how far the top stands above the pack)
#   d_2nd        best - 2nd distinct score        (gap to runner-up)
#   n_top        # restarts hitting the best board exactly   (basin consensus)
#   n_dom        # restarts within 0.02 dits of best
# Oracle-only (for labelling / truth): gap_true = true_score - best_score.

import os, sys, random, statistics as st, collections
sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
import eval as E  # noqa

BIN   = "./enigma"
LANGS = os.environ.get("LANGS", "english german").split()
LENS  = [int(x) for x in os.environ.get("LENS", "55 60 65").split()]
PAIRS = int(os.environ.get("PAIRS", "10"))
NPROB = int(os.environ.get("NPROB", "200"))
R     = int(os.environ.get("R", "40"))
OPTS  = os.environ.get("OPTS", "-J -S i4q10").split()
SEED  = int(os.environ.get("SEED", "777"))
os.environ["ENIGMA_DATA"] = "ngrams"
os.environ["ENIGMA_SEED"] = "0"


def pairset(pb):
    toks = pb.split() if " " in pb else [pb[i:i+2] for i in range(0, len(pb), 2)]
    return {frozenset(t) for t in toks if len(t) == 2}


def dump_solve(lang, key, ct):
    """Return list of (score, board_str) for the R restarts."""
    u, w, r, g, _ = key
    args = ["-q", "-l", lang, "-c", "-R", str(R), "--dump-all",
            "-u", u, "-w", w, "-r", r, "-g", g] + OPTS
    _, err, _ = E.run(BIN, args, ct)
    out = []
    for line in err.splitlines():
        f = line.split(maxsplit=5)
        if len(f) >= 5 and f[0] == "dumpall":
            out.append((float(f[4]), f[5] if len(f) == 6 else ""))
    return out


def auc(pos, neg):
    """P(pos > neg); 0.5 = no separation. pos=SOLVE feature vals, neg=TANGLE."""
    if not pos or not neg:
        return float('nan')
    win = 0.0
    for a in pos:
        for b in neg:
            win += 1.0 if a > b else (0.5 if a == b else 0.0)
    return win / (len(pos) * len(neg))


rows = []
for lang in LANGS:
    corpora = E.load_corpora(lang)
    for L in LENS:
        rng = random.Random(SEED)
        for _ in range(NPROB):
            _, excerpt, key = E.gen_problem(rng, corpora, L, PAIRS)
            ct = E.encrypt(BIN, key, excerpt)
            rs = dump_solve(lang, key, ct)
            if not rs:
                continue
            scores = [s for s, _ in rs]
            bi = max(range(len(rs)), key=lambda i: rs[i][0])
            bscore, bboard = rs[bi]
            tset, cset = pairset(key[4]), pairset(bboard)
            wrong = len(cset - tset)
            miss = len(tset - cset)
            cls = ("SOLVE" if wrong == 0 and miss == 0
                   else "TANGLE" if 1 <= wrong <= 3 else "JUNK")
            tscore = E.oracle_score(BIN, key, ct, lang, "q")
            srt = sorted(set(scores), reverse=True)
            d_2nd = (bscore - srt[1]) if len(srt) > 1 else 0.0
            rows.append(dict(lang=lang, L=L, cls=cls, best=bscore,
                             gap_true=(tscore - bscore) if tscore is not None else None,
                             d_med=bscore - st.median(scores), d_2nd=d_2nd,
                             n_top=sum(1 for _, b in rs if b == bboard),
                             n_dom=sum(1 for s in scores if bscore - s <= 0.02)))

cnt = collections.Counter(r["cls"] for r in rows)
print(f"messages: {len(rows)}   SOLVE {cnt['SOLVE']}  TANGLE {cnt['TANGLE']}  JUNK {cnt['JUNK']}")

sol = [r for r in rows if r["cls"] == "SOLVE"]
tan = [r for r in rows if r["cls"] == "TANGLE"]
print(f"\nSOLVE vs TANGLE separability (AUC; 0.5=indistinguishable, want >>0.5):")
for feat in ["d_med", "d_2nd", "n_top", "n_dom"]:
    a = auc([r[feat] for r in sol], [r[feat] for r in tan])
    ms = st.mean(r[feat] for r in sol) if sol else float('nan')
    mt = st.mean(r[feat] for r in tan) if tan else float('nan')
    print(f"  {feat:6s}  AUC {a:.2f}   mean SOLVE {ms:7.3f}  TANGLE {mt:7.3f}")

# raw best-score: only fair WITHIN a (lang,L) cell (message-specific), pool the AUCs
cell_aucs = []
for lang in LANGS:
    for L in LENS:
        s = [r["best"] for r in sol if r["lang"] == lang and r["L"] == L]
        t = [r["best"] for r in tan if r["lang"] == lang and r["L"] == L]
        if s and t:
            cell_aucs.append(auc(s, t))
if cell_aucs:
    print(f"  best    AUC {st.mean(cell_aucs):.2f}   (raw score, averaged within (lang,L) cells)")

# the oracle truth, for reference: how far below optimum is each class's best?
print("\noracle gap_true (true_score - best_score), by class:")
for c in ["SOLVE", "TANGLE", "JUNK"]:
    g = [r["gap_true"] for r in rows if r["cls"] == c and r["gap_true"] is not None]
    if g:
        print(f"  {c:6s}  n {len(g):3d}  mean gap {st.mean(g):+.3f}  median {st.median(g):+.3f}")
