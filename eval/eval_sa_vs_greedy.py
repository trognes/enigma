#!/usr/bin/env python3
#
# Greedy restart climb vs simulated annealing on SHORT WEHRMACHT messages,
# at matched compute. Stage 1 of the two-stage design: tune each arm's own
# free parameters separately, so the Stage-2 head-to-head compares each
# algorithm in its best variant rather than in its prose-tuned default.
#
# Why a separate harness from tests/crack_quality.py: that one samples PROSE
# corpora, and `-l wehrmacht` is a register, not a language -- it only makes
# sense on telegraphic plaintext. Here the plaintext is pooled from the 69
# authentic decrypts in eval/, excerpted at random. The ciphertext is
# re-enciphered under a random 10-plug board (the authentic ciphertext is
# given up deliberately: the register lives in the plaintext, and one trial
# per message is far too few to resolve a few-point difference).
#
# Scope: the plugboard-recovery tier -- the true rotor key is FIXED and given,
# the plugboard is hidden and recovered. This isolates the two plugboard
# search algorithms. It is not the full-crack problem.
#
# Compute is matched on score_iter, not wall time: at these budgets a run is
# ~0.2 s and dominated by n-gram table loading, so wall time measures startup.
# score_iter's documented undercount (the --polish gain scan runs outside the
# counted loop) does not bias this comparison, because every arm calls
# --polish exactly once. Each config's knob is auto-calibrated to hit the
# target budget, so variants that converge cheaply earn more restarts.
#
# All configs see the SAME trial instances (excerpt, board, seed), so every
# comparison is paired -- essential when per-trial outcomes are near-bimodal.
#
# Usage (from the repo root):
#   python3 eval/eval_sa_vs_greedy.py
# Env: LENGTH (90), TRIALS (300), BUDGET (200000), PAIRS (10), JOBS (nproc),
#      SEEDFAM (1) -- change for an independent seed family.

import os, re, random, subprocess, statistics as st
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")

LENGTH = int(os.environ.get("LENGTH", "90"))
TRIALS = int(os.environ.get("TRIALS", "300"))
BUDGET = int(os.environ.get("BUDGET", "200000"))
PAIRS = int(os.environ.get("PAIRS", "10"))
JOBS = int(os.environ.get("JOBS", str(os.cpu_count() or 4)))
SEEDFAM = int(os.environ.get("SEEDFAM", "1"))

KEY = ["-u", "B", "-w", "241", "-r", "AAA", "-g", "QEW"]
SCORED = re.compile(r"scored (\d+) plugboard")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


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


POOL = corpus()


def make_trials(n):
    """Fixed instance list, shared by every config so comparisons are paired."""
    rng = random.Random(1000 * SEEDFAM + 7)
    ts = []
    for i in range(n):
        off = rng.randrange(0, len(POOL) - LENGTH)
        pt = POOL[off:off + LENGTH]
        letters = list(ALPHA)
        rng.shuffle(letters)
        board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(PAIRS))
        ct = subprocess.run([BIN, "-i"] + KEY + ["-s", board], input=pt,
                            capture_output=True, text=True).stdout.strip()
        ts.append((pt, ct, rng.randrange(1, 2**31)))
    return ts


TRIALSET = make_trials(TRIALS)


def run_one(args, ct, seed):
    env = dict(os.environ, ENIGMA_SEED=str(seed))
    r = subprocess.run([BIN] + args + ["-a", "-l", "wehrmacht", "-T", "1"] + KEY,
                       input=ct, capture_output=True, text=True, env=env)
    m = SCORED.search(r.stderr)
    return r.stdout.strip(), (int(m.group(1)) if m else 0)


def evaluate(args, trials):
    """Return (mean %-correct, exact%, mean score_iter) over the trial set."""
    def one(t):
        pt, ct, seed = t
        out, si = run_one(args, ct, seed)
        pct = 100.0 * sum(a == b for a, b in zip(pt, out)) / len(pt)
        return pct, si
    with ThreadPoolExecutor(max_workers=JOBS) as ex:
        res = list(ex.map(one, trials))
    pcts = [p for p, _ in res]
    return (st.mean(pcts),
            100.0 * sum(1 for p in pcts if p > 99.99) / len(pcts),
            st.mean([s for _, s in res]))


def calibrate(build, knob0, pilot=12):
    """Scale the cost knob so mean score_iter ~= BUDGET (cost is ~linear in it)."""
    sub = TRIALSET[:pilot]
    knob = knob0
    for _ in range(3):
        _, _, si = evaluate(build(knob), sub)
        if si <= 0:
            break
        k = max(1, round(knob * BUDGET / si))
        if k == knob:
            break
        knob = k
    return knob


# Minimum move budget below which an "anneal" is starved into a plain quench --
# i.e. no longer simulated annealing at all. Splits that cannot afford this many
# moves per restart within BUDGET are reported infeasible rather than silently
# degenerating into a greedy climb wearing an -A flag (which is what an earlier
# naive calibrator did: it drove A to 1 and the result then "beat" real SA).
A_MIN = 2000


def sa_depth(split_r, pilot=12):
    """Depth A for this restart count at the target budget; None if infeasible.

    Purely measurement-driven, climbing from A_MIN. An analytic two-point cost
    fit was tried and discarded: SA does not always consume its full move
    budget, which flattens the fitted slope and inflates the intercept, and the
    bad seed then rejected splits (e.g. R=12) that are in fact affordable --
    including the shipped `-A 12000 -R 12` recipe. Feasibility is therefore
    decided by measurement: a split is infeasible only when reaching the budget
    would require starving the anneal below A_MIN."""
    sub = TRIALSET[:pilot]
    a = A_MIN
    for _ in range(6):
        _, _, si = evaluate(sa(split_r)(a), sub)
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


GREEDY_BASE = ["-c", "-J", "--polish"]


def greedy(sched, kick):
    return lambda R: (GREEDY_BASE + ["--score", sched, "--random", str(kick),
                                     "-R", str(R)])


def sa(split_r, polish=True):
    base = ["-c"] + (["--polish"] if polish else [])
    return lambda A: (base + ["-A", str(A), "--score", "a10",
                              "-R", str(split_r)])


def report(title, rows):  # noqa
    print("\n=== %s ===" % title)
    print("%-34s %6s %8s %8s %10s" % ("config", "knob", "mean%", "exact%", "score_it"))
    for name, knob, mean, ex, si in rows:
        if mean != mean:   # NaN -> infeasible split
            print("%-34s %6s %8s %8s %10s" % (name, "-", "-", "-", "-"))
        else:
            print("%-34s %6d %8.1f %8.1f %10.0f" % (name, knob, mean, ex, si))


def main():
    print("wehrmacht SA-vs-greedy, STAGE 1 (per-arm tuning)")
    print("corpus %d letters | L=%d | pairs=%d | trials=%d | budget=%dk score_iter"
          " | seedfam=%d | jobs=%d"
          % (len(POOL), LENGTH, PAIRS, TRIALS, BUDGET // 1000, SEEDFAM, JOBS))

    # --- arm A: greedy. Free knobs are the schedule and the kick; -R is the budget.
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
        R = calibrate(b, 150)
        mean, ex, si = evaluate(b(R), TRIALSET)
        rows.append((name, R, mean, ex, si))
        print("  done: %-34s R=%-5d mean %.1f" % (name, R, mean))
    report("ARM A -- greedy (-c -J --polish), knob = -R", rows)
    best_greedy = max(rows, key=lambda r: r[2])

    # --- arm B: SA. The depth/restart split is a real free parameter at fixed
    # budget, and it is bounded: each restart costs a fixed pre-pass + quench
    # regardless of depth, so beyond some R the budget cannot buy a real anneal.
    rows2 = []
    for r in (1, 2, 3, 6, 12, 24, 48):
        A = sa_depth(r)
        if A is None:
            rows2.append(("R=%-3d  INFEASIBLE (<%d moves/restart)" % (r, A_MIN),
                          0, float("nan"), float("nan"), float("nan")))
            print("  skip: R=%-3d infeasible at this budget" % r)
            continue
        mean, ex, si = evaluate(sa(r)(A), TRIALSET)
        rows2.append(("A=%-6d R=%-3d --polish" % (A, r), A, mean, ex, si))
        print("  done: A=%-6d R=%-3d mean %.1f" % (A, r, mean))
    report("ARM B -- simulated annealing (-A), knob = -A depth", rows2)
    feasible = [r for r in rows2 if r[2] == r[2]]   # drop NaN rows
    best_sa = max(feasible, key=lambda r: r[2])

    # --- control: does SA need the finisher? (isolates finisher from algorithm)
    r_best = int(re.search(r"R=(\d+)", best_sa[0]).group(1))
    A = best_sa[1]
    mean, ex, si = evaluate(sa(r_best, polish=False)(A), TRIALSET)
    report("CONTROL -- best SA split without --polish",
           [("A=%-6d R=%-3d no polish" % (A, r_best), A, mean, ex, si)])

    print("\n--- stage 1 winners (carry into stage 2) ---")
    print("  greedy: %s  (R=%d)  mean %.1f  exact %.1f"
          % (best_greedy[0].split("(")[0].strip(), best_greedy[1],
             best_greedy[2], best_greedy[3]))
    print("  SA:     %s  mean %.1f  exact %.1f"
          % (best_sa[0].strip(), best_sa[2], best_sa[3]))
    print("  head-to-head at this budget: SA - greedy = %+.1f pp mean, %+.1f pp exact"
          % (best_sa[2] - best_greedy[2], best_sa[3] - best_greedy[3]))


if __name__ == "__main__":
    main()
