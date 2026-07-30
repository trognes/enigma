#!/usr/bin/env python3
#
# What ARE the residual "few wrong plugs" failures? (archived/PERFORMANCE.md section 6.14)
#
# Takes every converged plugboard-recovery board that lands 1..3 plugs WRONG (the
# near-solution residual), with the TRUE rotor key fixed and only the plugboard
# hill-climbed (best of -R restarts, the standard -J -S i4q10 recipe). For each
# such board it asks two questions:
#
#   1. SEARCH or SCORING failure?  gap = true_score - converged_score (both quad).
#      gap > 0  => the true board scores HIGHER; the climb stuck below it (search).
#      gap <= 0 => the score prefers the wrong board (scoring / information floor).
#
#   2. What are the wrong plugs?  Classify each wrong plug by whether its letters
#      are steckered in truth:  TANGLE (both endpoints steckered, wrong partner --
#      a re-pairing knot among known-steckered letters), HALF (one endpoint), or
#      SPURIOUS (neither -- an invented plug). And measure the "swap-component"
#      size: the set of letters that must move SIMULTANEOUSLY to go converged->truth.
#
# Finding: the near-solution residual is a SEARCH problem, not the information
# floor -- ~97% of few-wrong cases have the true board scoring strictly higher, by
# a large margin (+0.15..+0.95 dits even at 1 wrong plug). And ~90% of wrong-plug
# endpoints are genuinely steckered letters cross-wired to the wrong partner: a
# 3-to-5-plug re-pairing tangle (swap-component median 6 letters, up to 10) that a
# single 2-letter toggle cannot cross because every partial step scores lower
# (the plugboard-applied-twice convexity, section 6.11). This is exactly what
# try_repair / try_repair_3 target -- and why they are still dominated at matched
# compute (section 4.7): the tangle cases are only ~5% of problems, too rare to
# amortize the per-climb cost against the restarts it would replace.
#
# Usage:   python3 eval/few_wrong_tangle.py     # solves, prints tables, writes PNG
# Env:     LANGS LENS PAIRS NPROB R OPTS SEED (defaults reproduce the shipped plot)
# Slow (runs the solver); writes eval/plots/few_wrong_tangle.png.

import os, sys, random, statistics as st, collections
sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
import eval as E  # noqa
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BIN   = "./enigma"
LANGS = os.environ.get("LANGS", "english german").split()
LENS  = [int(x) for x in os.environ.get("LENS", "55 60 65").split()]
PAIRS = int(os.environ.get("PAIRS", "10"))
NPROB = int(os.environ.get("NPROB", "120"))
R     = int(os.environ.get("R", "40"))
OPTS  = os.environ.get("OPTS", "-J -S i4q10").split()
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


def solve(lang, key, ct):
    u, w, r, g, _ = key
    args = ["-q", "-l", lang, "-c", "-R", str(R), "-u", u, "-w", w,
            "-r", r, "-g", g] + OPTS
    out, err, _ = E.run(BIN, args, ct)
    score, plugs = E.parse_recovered(err)
    return out.strip(), score, plugs


def comp_size(wrong, miss):
    """Letters spanned by the wrong plugs + any true(missing) plug transitively
    sharing a letter -- the simultaneous swap truth must perform."""
    letters = endpoints(set(wrong))
    changed = True
    used = set()
    while changed:
        changed = False
        for m in miss:
            if m not in used and (m & letters):
                used.add(m)
                letters |= m
                changed = True
    return len(letters)


def collect():
    search = scoring = 0
    tang = half = spur = 0
    comps = []
    nfew = nprob = 0
    for lang in LANGS:
        corpora = E.load_corpora(lang)
        for L in LENS:
            rng = random.Random(SEED)
            for _ in range(NPROB):
                nprob += 1
                _, excerpt, key = E.gen_problem(rng, corpora, L, PAIRS)
                ct = E.encrypt(BIN, key, excerpt)
                _, cscore, cplugs = solve(lang, key, ct)
                tscore = E.oracle_score(BIN, key, ct, lang, "q")
                tset, cset = pairset(key[4]), pairset(cplugs)
                wrong = cset - tset
                if not (1 <= len(wrong) <= 3):
                    continue
                if cscore is None or tscore is None:
                    continue
                nfew += 1
                miss = tset - cset
                if tscore - cscore > 1e-6:
                    search += 1
                else:
                    scoring += 1
                tletters = endpoints(tset)
                for wp in wrong:
                    a, b = tuple(wp)
                    na, nb = a in tletters, b in tletters
                    tang += na and nb
                    half += (na or nb) and not (na and nb)
                    spur += not (na or nb)
                comps.append(comp_size(wrong, miss))
    return dict(search=search, scoring=scoring, tang=tang, half=half, spur=spur,
                comps=comps, nfew=nfew, nprob=nprob)


def main():
    d = collect()
    nfew, nprob = d["nfew"], d["nprob"]
    tot = d["tang"] + d["half"] + d["spur"]
    print(f"problems: {nprob}   few-wrong (1..3 wrong plugs): {nfew}")
    print(f"  SEARCH fail (truth scores higher): {d['search']}"
          f"   SCORING fail (score prefers wrong): {d['scoring']}")
    print(f"  wrong plugs: TANGLE {d['tang']} ({100*d['tang']/tot:.0f}%)"
          f"  HALF {d['half']} ({100*d['half']/tot:.0f}%)"
          f"  SPURIOUS {d['spur']} ({100*d['spur']/tot:.0f}%)")
    print(f"  swap-component: mean {st.mean(d['comps']):.1f} median {st.median(d['comps']):.0f}"
          f" max {max(d['comps'])}")

    BLUE, GREEN, GRAY, RED = '#1f5fa8', '#2e8b57', '#b0b0b0', '#c44e52'
    fig, (a1, a2, a3) = plt.subplots(1, 3, figsize=(13.2, 4.4))

    # Panel 1: search vs scoring split
    vals = [d['search'], d['scoring']]
    a1.bar(['search\nfailure', 'scoring\nfailure'], vals, color=[BLUE, RED], width=.6)
    for i, v in enumerate(vals):
        a1.annotate(f"{v}\n({100*v/nfew:.0f}%)", (i, v), textcoords="offset points",
                    xytext=(0, 4), ha='center', fontsize=10)
    a1.set_ylabel("few-wrong boards")
    a1.set_ylim(0, max(vals) * 1.25)
    a1.set_title("Is the true board scoring higher?\n"
                 "near-misses are a SEARCH problem, not the info floor", fontsize=10)

    # Panel 2: wrong-plug type
    tv = [d['tang'], d['half'], d['spur']]
    a2.bar(['TANGLE', 'HALF', 'SPURIOUS'], tv, color=[BLUE, GREEN, GRAY], width=.6)
    for i, v in enumerate(tv):
        a2.annotate(f"{100*v/tot:.0f}%", (i, v), textcoords="offset points",
                    xytext=(0, 4), ha='center', fontsize=10)
    a2.set_ylabel("wrong plugs")
    a2.set_ylim(0, max(tv) * 1.2)
    a2.set_title("What are the wrong plugs?\n~90% are steckered letters, wrong partner",
                 fontsize=10)

    # Panel 3: swap-component-size histogram
    c = collections.Counter(d['comps'])
    xs = list(range(2, max(d['comps']) + 1))
    ys = [c.get(x, 0) for x in xs]
    a3.bar(xs, ys, color=BLUE, width=.8)
    med = st.median(d['comps'])
    a3.axvline(med, color=RED, ls='--', lw=1.6, label=f"median {med:.1f}")
    a3.set_xlabel("letters that must swap at once (converged -> truth)")
    a3.set_ylabel("boards")
    a3.set_xticks(xs)
    a3.legend(frameon=False, fontsize=9)
    a3.set_title("How big is the knot?\nmedian ~6 letters = a 3-plug re-pairing", fontsize=10)

    fig.suptitle(
        f"The residual 'few wrong plugs' failures ({nfew} near-solution boards from "
        f"{nprob} problems, en+de L55-65, 10 plugs, R={R})\n"
        "search failures, not scoring: a 3-to-5-plug re-pairing tangle the single-toggle "
        "climb can't cross -- recoverable in principle, too rare to target cheaply",
        fontsize=11)
    fig.tight_layout(rect=[0, 0, 1, 0.9])
    out = "eval/plots/few_wrong_tangle.png"
    fig.savefig(out, dpi=110)
    print("wrote", out)


if __name__ == "__main__":
    main()
