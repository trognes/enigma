#!/usr/bin/env python3
#
# Greedy restart climb vs simulated annealing on SHORT WEHRMACHT messages,
# at matched compute.
#
#   STAGE=1  per-arm tuning at one length: sweep each arm's own free
#            parameters, so stage 2 compares each algorithm in its best
#            variant rather than in its prose-tuned default.
#   STAGE=2  head-to-head across a length grid, paired, with a confidence
#            interval on the paired difference.
#
# Why a separate harness from tests/crack_quality.py: that one samples PROSE
# corpora, and `-l wehrmacht` is a writing style, not a separate language -- it only makes
# sense on telegraphic plaintext. Here the plaintext is pooled from the 69
# authentic decrypts in eval/, excerpted at random. The ciphertext is
# re-enciphered under a random 10-plug board (the authentic ciphertext is
# given up deliberately: the writing style lives in the plaintext, and only 25
# authentic messages are shorter than 70 letters -- far too few to resolve a
# few-point difference).
#
# Scope: the plugboard-recovery tier -- the true rotor key is FIXED and given,
# the plugboard is hidden and recovered. This isolates the two plugboard
# search algorithms. It is not the full-crack problem.
#
# Compute is matched on score_iter, not wall time: at these budgets a run is
# ~0.2 s and dominated by n-gram table loading, so wall time measures startup.
# score_iter's documented undercount (the --polish gain scan runs outside the
# counted loop) does not bias this comparison, because every arm calls
# --polish exactly once.
#
# All configs see the SAME trial instances (excerpt, board, seed), so every
# comparison is paired -- essential when per-trial outcomes are near-bimodal.
#
# Usage (from the repo root):
#   python3 eval/eval_sa_vs_greedy.py                 # stage 1
#   STAGE=2 python3 eval/eval_sa_vs_greedy.py         # stage 2
# Env: LENGTH (90, stage 1), LENGTHS ("50 60 70 80 90", stage 2), TRIALS (300),
#      BUDGET (200000), PAIRS (10), JOBS (nproc), SEEDFAM (1), TSV (path).

import os, re, math, random, subprocess, statistics as st
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")

STAGE = int(os.environ.get("STAGE", "1"))
LENGTH = int(os.environ.get("LENGTH", "90"))
LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "50 60 70 80 90").split()]
TRIALS = int(os.environ.get("TRIALS", "300"))
BUDGET = int(os.environ.get("BUDGET", "200000"))
PAIRS = int(os.environ.get("PAIRS", "10"))
JOBS = int(os.environ.get("JOBS", str(os.cpu_count() or 4)))
SEEDFAM = int(os.environ.get("SEEDFAM", "1"))
TSV = os.environ.get("TSV", "")
# SA pre-pass probe (PERFORMANCE.md 3.11): by default -A ignores the leading
# --score stages and always seeds with IC. SA_SCHED/SA_STAGES let stage 2 A/B the
# ENIGMA_SA_STAGES build flag that makes -A honour the whole schedule.
SA_SCHED = os.environ.get("SA_SCHED", "a10")
SA_STAGES = os.environ.get("SA_STAGES", "0") == "1"
# CORPUS selects the plaintext substrate and the scoring language:
#   "wehrmacht" (default) -- the 69 authentic telegraphic decrypts in eval/
#   "english" / "german"  -- the PROSE corpora tests/crack_quality.py samples,
#                            read from that file so the two benchmarks agree.
# Prose is the control the shipped SA pre-pass was originally tuned on.
# STAGE=3 arms: paired greedy-vs-greedy schedule A/B (which pre-pass?).
GA = os.environ.get("GA", "m4a10")     # arm A schedule (shipped recommendation)
GB = os.environ.get("GB", "i4a10")     # arm B schedule
# ENIGMA_IC_BLEND lambda applied per arm in STAGE 3 ("" = probe off). Setting
# GA==GB and only BLEND_B makes stage 3 a clean paired blend-off vs blend-on A/B.
BLEND_A = os.environ.get("BLEND_A", "")
BLEND_B = os.environ.get("BLEND_B", "")
# ENIGMA_IC_BLEND_MODE for arm B: ""/0 = blend both climbs and ranks, 1 = blend the
# climb but rank on the pure model, 2 = climb pure but rank on the blend. Decomposes
# the blend's surface-reshaping effect from its selection effect.
BLEND_MODE_B = os.environ.get("BLEND_MODE_B", "")
# STAGE=5 (cross-key): wildcard part of the start position so the score must rank
# KEYS, not just plugboards. FIXR fixes -R for both arms, giving structural compute
# parity (identical keys x restarts) without calibration.
WILDG = os.environ.get("WILDG", "Q..")
FIXR = os.environ.get("FIXR", "4")
CORPUS = os.environ.get("CORPUS", "wehrmacht")
LANG = "wehrmacht" if CORPUS == "wehrmacht" else CORPUS

KEY = ["-u", "B", "-w", "241", "-r", "AAA", "-g", "QEW"]
SCORED = re.compile(r"scored (\d+) plugboard")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

# Minimum move budget below which an "anneal" is starved into a plain quench --
# i.e. no longer simulated annealing at all. Splits that cannot afford this many
# moves per restart within BUDGET are reported infeasible rather than silently
# degenerating into a greedy climb wearing an -A flag (which is what an earlier
# naive calibrator did: it drove A to 1 and the result then "beat" real SA).
A_MIN = 2000


def prose_corpus(name):
    """Lift CORPORA[name] out of tests/crack_quality.py without importing it
    (module-level env reads there would otherwise leak into this run)."""
    import ast
    src = open(os.path.join(ROOT, "tests", "crack_quality.py")).read()
    for node in ast.walk(ast.parse(src)):
        if (isinstance(node, ast.Assign) and node.targets
                and getattr(node.targets[0], "id", "") == "CORPORA"):
            table = ast.literal_eval(node.value)
            if name not in table:
                raise SystemExit("no prose corpus %r (have %s)"
                                 % (name, ", ".join(sorted(table))))
            return table[name]
    raise SystemExit("CORPORA not found in tests/crack_quality.py")


def corpus():
    """Pool the authentic telegraphic decrypts into one plaintext string."""
    cmd = re.compile(r'-u (\S+) -w (\S+) -r (\S+) -g (\S+) -s "([^"]*)"')
    out = []
    for fn in ("enigma-messages.txt", "enigma-army-messages-1941.txt"):
        txt = open(os.path.join(HERE, fn)).read()
        for blk in re.split(r"^### Message", txt, flags=re.M)[1:]:
            if not cmd.search(blk):
                continue
            m = re.search(r"DECRYPT:\s+((?:.*\n)(?:             .*\n)*)", blk)
            if m:
                out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return "".join(p for p in out if len(p) >= 40)


POOL = corpus() if CORPUS == "wehrmacht" else prose_corpus(CORPUS)


def make_trials(n, length):
    """Fixed instance list, shared by every config so comparisons are paired."""
    rng = random.Random(1000 * SEEDFAM + 7 + length)
    ts = []
    for _ in range(n):
        off = rng.randrange(0, len(POOL) - length)
        pt = POOL[off:off + length]
        letters = list(ALPHA)
        rng.shuffle(letters)
        board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(PAIRS))
        ct = subprocess.run([BIN, "-i"] + KEY + ["-s", board], input=pt,
                            capture_output=True, text=True).stdout.strip()
        ts.append((pt, ct, rng.randrange(1, 2**31)))
    return ts


def run_one(args, ct, seed, sa_stages=False, blend="", blend_mode=""):
    env = dict(os.environ, ENIGMA_SEED=str(seed))
    if sa_stages:
        env["ENIGMA_SA_STAGES"] = "1"
    else:
        env.pop("ENIGMA_SA_STAGES", None)
    if blend:
        env["ENIGMA_IC_BLEND"] = str(blend)
    else:
        env.pop("ENIGMA_IC_BLEND", None)
    if blend_mode:
        env["ENIGMA_IC_BLEND_MODE"] = str(blend_mode)
    else:
        env.pop("ENIGMA_IC_BLEND_MODE", None)
    r = subprocess.run([BIN] + args + ["-a", "-l", LANG, "-T", "1"] + KEY,
                       input=ct, capture_output=True, text=True, env=env)
    m = SCORED.search(r.stderr)
    return r.stdout.strip(), (int(m.group(1)) if m else 0)


def evaluate_trials(args, trials, sa_stages=False, blend="", blend_mode=""):
    """Per-trial (%-correct, score_iter) -- retained so paired differences get
    a real confidence interval rather than a comparison of two bare means."""
    def one(t):
        pt, ct, seed = t
        out, si = run_one(args, ct, seed, sa_stages, blend, blend_mode)
        return 100.0 * sum(a == b for a, b in zip(pt, out)) / len(pt), si
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        return list(ex.map(one, trials))


def summarise(res):
    pcts = [p for p, _ in res]
    return (st.mean(pcts),
            100.0 * sum(1 for p in pcts if p > 99.99) / len(pcts),
            st.mean([s for _, s in res]))


def evaluate(args, trials):
    return summarise(evaluate_trials(args, trials))


GREEDY_BASE = ["-c", "-J", "--polish"]


def greedy(sched, kick):
    return lambda R: (GREEDY_BASE + ["--score", sched, "--random", str(kick),
                                     "-R", str(R)])


def sa(split_r, polish=True, sched=None):
    base = ["-c"] + (["--polish"] if polish else [])
    sc = sched if sched else SA_SCHED
    return lambda A: (base + ["-A", str(A), "--score", sc,
                              "-R", str(split_r)])


def calibrate(build, knob0, trials, pilot=12):
    """Scale the cost knob so mean score_iter ~= BUDGET (cost ~linear in it)."""
    sub = trials[:pilot]
    knob = knob0
    for _ in range(4):
        _, _, si = evaluate(build(knob), sub)
        if si <= 0:
            break
        k = max(1, round(knob * BUDGET / si))
        if k == knob:
            break
        knob = k
    return knob


def sa_depth(split_r, trials, pilot=12):
    """Depth A for this restart count at the target budget; None if infeasible.

    Purely measurement-driven, climbing from A_MIN. An analytic two-point cost
    fit was tried and discarded: SA does not always consume its full move
    budget, which flattens the fitted slope and inflates the intercept, and the
    bad seed then rejected splits (e.g. R=12) that are in fact affordable --
    including the shipped `-A 12000 -R 12` recipe. Feasibility is therefore
    decided by measurement: a split is infeasible only when reaching the budget
    would require starving the anneal below A_MIN."""
    sub = trials[:pilot]
    a = A_MIN
    for _ in range(6):
        _, _, si = summarise(evaluate_trials(sa(split_r)(a), sub, SA_STAGES))
        if si <= 0:
            return None
        if abs(si - BUDGET) < 0.03 * BUDGET:
            return a
        nxt = int(a * BUDGET / si)
        if nxt < A_MIN:            # cannot fit a real anneal in this many restarts
            return None
        if nxt == a:
            return a
        a = nxt
    return a


def report(title, rows):
    print("\n=== %s ===" % title)
    print("%-34s %6s %8s %8s %10s" % ("config", "knob", "mean%", "exact%", "score_it"))
    for name, knob, mean, ex, si in rows:
        if mean != mean:   # NaN -> infeasible split
            print("%-34s %6s %8s %8s %10s" % (name, "-", "-", "-", "-"))
        else:
            print("%-34s %6d %8.1f %8.1f %10.0f" % (name, knob, mean, ex, si))


def stage1():
    trials = make_trials(TRIALS, LENGTH)
    print("wehrmacht SA-vs-greedy, STAGE 1 (per-arm tuning)")
    print("corpus %d letters | L=%d | pairs=%d | trials=%d | budget=%dk score_iter"
          " | seedfam=%d | jobs=%d"
          % (len(POOL), LENGTH, PAIRS, TRIALS, BUDGET // 1000, SEEDFAM, JOBS))

    greedy_cfgs = [
        ("m4a10  kick10  (recommended)", "m4a10", 10),
        ("m4a8   kick10", "m4a8", 10),
        ("m4a13  kick10", "m4a13", 10),
        ("i4a10  kick10", "i4a10", 10),
        ("a10    kick10  (no pre-pass)", "a10", 10),
        ("m4a10  kick6", "m4a10", 6),
        ("m4a10  kick13", "m4a10", 13),
    ]
    rows = []
    for name, sched, kick in greedy_cfgs:
        b = greedy(sched, kick)
        R = calibrate(b, 150, trials)
        mean, ex, si = evaluate(b(R), trials)
        rows.append((name, R, mean, ex, si))
        print("  done: %-34s R=%-5d mean %.1f" % (name, R, mean))
    report("ARM A -- greedy (-c -J --polish), knob = -R", rows)
    best_greedy = max(rows, key=lambda r: r[2])

    rows2 = []
    for r in (1, 2, 3, 6, 12, 24, 48):
        A = sa_depth(r, trials)
        if A is None:
            rows2.append(("R=%-3d  INFEASIBLE (<%d moves/restart)" % (r, A_MIN),
                          0, float("nan"), float("nan"), float("nan")))
            print("  skip: R=%-3d infeasible at this budget" % r)
            continue
        mean, ex, si = evaluate(sa(r)(A), trials)
        rows2.append(("A=%-6d R=%-3d --polish" % (A, r), A, mean, ex, si))
        print("  done: A=%-6d R=%-3d mean %.1f" % (A, r, mean))
    report("ARM B -- simulated annealing (-A), knob = -A depth", rows2)
    feasible = [r for r in rows2 if r[2] == r[2]]
    best_sa = max(feasible, key=lambda r: r[2])

    r_best = int(re.search(r"R=(\d+)", best_sa[0]).group(1))
    mean, ex, si = evaluate(sa(r_best, polish=False)(best_sa[1]), trials)
    report("CONTROL -- best SA split without --polish",
           [("A=%-6d R=%-3d no polish" % (best_sa[1], r_best),
             best_sa[1], mean, ex, si)])

    print("\n--- stage 1 winners (carry into stage 2) ---")
    print("  greedy: %s  (R=%d)  mean %.1f  exact %.1f"
          % (best_greedy[0].split("(")[0].strip(), best_greedy[1],
             best_greedy[2], best_greedy[3]))
    print("  SA:     %s  mean %.1f  exact %.1f"
          % (best_sa[0].strip(), best_sa[2], best_sa[3]))
    print("  head-to-head: SA - greedy = %+.1f pp mean, %+.1f pp exact"
          % (best_sa[2] - best_greedy[2], best_sa[3] - best_greedy[3]))


# Stage-2 arms. Both are the SHIPPED defaults, not the per-family stage-1
# winners: stage 1 showed greedy's top three reorder between seed families and
# SA's split is flat across R=2..24, so selecting a "winner" from one family
# would be fitting noise. SA's best split reproduced the shipped -A .. -R 12
# anyway. Both arms get --polish (it is not blocked with -A).
G_SCHED, G_KICK, SA_R = "m4a10", 10, 12


def stage2():
    print("wehrmacht SA-vs-greedy, STAGE 2 (head-to-head across lengths)")
    print("corpus %s, %d letters (-l %s) | pairs=%d | trials=%d | budget=%dk"
          " score_iter | seedfam=%d | jobs=%d" % (CORPUS, len(POOL), LANG, PAIRS,
                                                  TRIALS, BUDGET // 1000, SEEDFAM, JOBS))
    print("arms: greedy -c -J --polish --score %s --random %d -R <cal>"
          "  |  SA -c --polish -A <cal> --score %s -R %d%s"
          % (G_SCHED, G_KICK, SA_SCHED, SA_R,
             "  [ENIGMA_SA_STAGES=1]" if SA_STAGES else ""))
    print("\n%4s %8s %7s %7s | %7s %7s | %-24s %s"
          % ("L", "knobs", "g.mean", "g.exct", "sa.mean", "sa.exct",
             "paired SA-greedy (95% CI)", "verdict"))

    tsv = open(TSV, "w") if TSV else None
    if tsv:
        tsv.write("length\ttrial\tgreedy_pct\tsa_pct\tdiff\n")

    for L in LENGTHS:
        trials = make_trials(TRIALS, L)
        gb = greedy(G_SCHED, G_KICK)
        R = calibrate(gb, 150, trials)
        A = sa_depth(SA_R, trials)
        if A is None:
            print("%4d  SA infeasible at this budget" % L)
            continue
        g = evaluate_trials(gb(R), trials)
        s = evaluate_trials(sa(SA_R)(A), trials, SA_STAGES)

        d = [si[0] - gi[0] for gi, si in zip(g, s)]
        md = st.mean(d)
        se = st.stdev(d) / math.sqrt(len(d)) if len(d) > 1 else 0.0
        lo, hi = md - 1.96 * se, md + 1.96 * se
        gm, gx, gsi = summarise(g)
        sm, sx, ssi = summarise(s)
        verdict = ("SA wins" if lo > 0 else
                   "greedy wins" if hi < 0 else "tie")
        print("%4d %8s %7.1f %7.1f | %7.1f %7.1f | %+6.1f pp [%+6.1f,%+6.1f]  %s"
              % (L, "R%d/A%dk" % (R, A // 1000), gm, gx, sm, sx, md, lo, hi, verdict))
        print("%4s %8s  (compute check: greedy %.0fk vs SA %.0fk score_iter)"
              % ("", "", gsi / 1000.0, ssi / 1000.0))
        if tsv:
            for i, (gi, si) in enumerate(zip(g, s)):
                tsv.write("%d\t%d\t%.4f\t%.4f\t%.4f\n"
                          % (L, i, gi[0], si[0], si[0] - gi[0]))
            tsv.flush()
    if tsv:
        tsv.close()
        print("\nper-trial data: %s" % TSV)


def stage5():
    """Cross-key: does the blended score still rank ROTOR KEYS correctly?

    Every other measurement fixes the rotor key, so the blend has only ever been
    judged on plugboard selection. Here -g is partly wildcarded, so the score also
    decides which key wins -- the riskiest place to change a scoring model."""
    key = list(KEY)
    key[key.index("-g") + 1] = WILDG
    nkeys = 26 ** WILDG.count(".")
    print("cross-key blend A/B, STAGE 5   -g %s (%d keys), -R %s fixed"
          % (WILDG, nkeys, FIXR))
    print("corpus %s (-l %s) | pairs=%d | trials=%d | seedfam=%d"
          % (CORPUS, LANG, PAIRS, TRIALS, SEEDFAM))
    print("arms: A = blend off | B = blend %s%s\n"
          % (BLEND_B or "(unset)", " mode " + BLEND_MODE_B if BLEND_MODE_B else ""))
    print("%4s %8s %8s | %-24s %s"
          % ("L", "off", "blend", "paired B-A (95% CI)", "verdict"))
    tsv = open(TSV, "w") if TSV else None
    if tsv:
        tsv.write("length\ttrial\ta_pct\tb_pct\tdiff\n")
    for L in LENGTHS:
        trials = make_trials(TRIALS, L)
        args = (GREEDY_BASE + ["--score", GA, "--random", str(G_KICK), "-R", FIXR])
        with ThreadPoolExecutor(max_workers=JOBS) as ex:
            a = list(ex.map(lambda t: _one(args, t, ""), trials))
            b = list(ex.map(lambda t: _one(args, t, BLEND_B), trials))
        d = [bi[0] - ai[0] for ai, bi in zip(a, b)]
        m = st.mean(d); se = st.stdev(d) / math.sqrt(len(d))
        lo, hi = m - 1.96 * se, m + 1.96 * se
        am, _, asi = summarise(a); bm, _, bsi = summarise(b)
        v = ("blend better" if lo > 0 else
             "BLEND HURTS KEYS" if hi < 0 else "tie")
        print("%4d %8.1f %8.1f | %+6.1f pp [%+6.1f,%+6.1f]  %s" % (L, am, bm, m, lo, hi, v))
        print("%4s %8s  (compute: %.0fk vs %.0fk score_iter)" % ("", "", asi/1000.0, bsi/1000.0))
        if tsv:
            for i, (ai, bi) in enumerate(zip(a, b)):
                tsv.write("%d\t%d\t%.4f\t%.4f\t%.4f\n" % (L, i, ai[0], bi[0], bi[0]-ai[0]))
            tsv.flush()
    if tsv:
        tsv.close()


def _one(args, t, blend):
    pt, ct, seed = t
    out, si = run_one(args, ct, seed, False, blend,
                      BLEND_MODE_B if blend else "")
    return 100.0 * sum(x == y for x, y in zip(pt, out)) / len(pt), si


def stage3():
    """Paired greedy-vs-greedy: which --score pre-pass is right for THIS substrate?

    Section 6.10 answered this for a QUAD target at R=2560; the shipped recipe is a
    WEIGHTED target at R~40-90, which is a different regime. Both arms are calibrated
    to the same score_iter budget, so a cheaper schedule earns more restarts."""
    print("greedy A/B, STAGE 3   A=%s%s  vs  B=%s%s"
          % (GA, " blend=" + BLEND_A if BLEND_A else "",
             GB, " blend=" + BLEND_B if BLEND_B else ""))
    print("corpus %s, %d letters (-l %s) | pairs=%d | trials=%d | budget=%dk"
          " score_iter | seedfam=%d" % (CORPUS, len(POOL), LANG, PAIRS, TRIALS,
                                        BUDGET // 1000, SEEDFAM))
    print("\n%4s %9s %8s %8s | %-24s %s"
          % ("L", "R(A/B)", GA[:8], GB[:8], "paired B-A (95% CI)", "verdict"))
    tsv = open(TSV, "w") if TSV else None
    if tsv:
        tsv.write("length\ttrial\ta_pct\tb_pct\tdiff\n")
    for L in LENGTHS:
        trials = make_trials(TRIALS, L)
        ba, bb = greedy(GA, G_KICK), greedy(GB, G_KICK)
        Ra, Rb = calibrate(ba, 150, trials), calibrate(bb, 150, trials)
        a = evaluate_trials(ba(Ra), trials, blend=BLEND_A)
        b = evaluate_trials(bb(Rb), trials, blend=BLEND_B, blend_mode=BLEND_MODE_B)
        d = [bi[0] - ai[0] for ai, bi in zip(a, b)]
        m = st.mean(d); se = st.stdev(d) / math.sqrt(len(d))
        lo, hi = m - 1.96 * se, m + 1.96 * se
        am, _, asi = summarise(a); bm, _, bsi = summarise(b)
        v = ("%s better" % GB if lo > 0 else
             "%s better" % GA if hi < 0 else "tie")
        print("%4d %9s %8.1f %8.1f | %+6.1f pp [%+6.1f,%+6.1f]  %s"
              % (L, "%d/%d" % (Ra, Rb), am, bm, m, lo, hi, v))
        print("%4s %9s  (compute: %.0fk vs %.0fk score_iter)"
              % ("", "", asi / 1000.0, bsi / 1000.0))
        if tsv:
            for i, (ai, bi) in enumerate(zip(a, b)):
                tsv.write("%d\t%d\t%.4f\t%.4f\t%.4f\n"
                          % (L, i, ai[0], bi[0], bi[0] - ai[0]))
            tsv.flush()
    if tsv:
        tsv.close()


if __name__ == "__main__":
    {1: stage1, 2: stage2, 3: stage3, 5: stage5}[STAGE]()
