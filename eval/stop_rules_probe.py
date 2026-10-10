#!/usr/bin/env python3
"""Three early-stop rules for the target-stage climb, priced from a trace.

    git apply eval/climb_cut.patch && make && cp enigma /tmp/enigma-cut
    git checkout src && make
    python3 eval/stop_rules_probe.py --binary /tmp/enigma-cut

ENIGMA_TRACE=1 (eval/climb_cut.patch) prints, for every target-stage
first-improvement sweep, its start score (SW), each accepted move as
probe index / IC rank / gain (A), its end (SE), the converged score handed
to try_repair (RQ) and the climb's final score (HE).  This reads the traces
of true-key climbs (-R 100) and of wrong-key climbs (676 keys x -R 2) and
answers the question each rule depends on, BEFORE building any of them:

  rank   Under -K moves are visited in IC order.  If accepted moves almost
         never come from the bottom of that order, the convergence check
         could skip it.  Reported: where accepted moves fall in the order,
         first cycle and later cycles separately.

  gain   Stop a sweep once k consecutive accepted moves each gained less
         than eps.  Reported: probes that would save, and how much of the
         sweep's later gain it would forfeit.

  gate   Apply the 200-probe cap (and skip try_repair) only to climbs that
         are losing.  The gate is a score threshold T at the moment the cap
         would fire, set as a quantile of the WRONG-key climbs' scores
         there.  Reported: the share of wrong-key climbs the gate cheapens,
         against the share of the true key's best-ending climbs it would
         have capped -- a climb that MATTERS being one that finishes above
         every wrong-key climb on the same message, i.e. one that would win
         the sweep.  If the second is near zero, the cap's quality cost
         goes away while most of its wrong-key saving stays.

All offline estimates assume the trajectory up to the stop is unchanged,
which is exact up to that point and says nothing about what a later move
would have become.  So they decide whether a rule is worth an A/B, not
whether it wins.  Recipe -c -S k4f10 -K --polish -l wehrmacht; no hgnord.
"""
import argparse
import os
import random
import subprocess

from climb_cut_ab import LET, RECIPE, decrypts, fixture, run

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = 200


def trace(binary, args, ct):
    env = dict(os.environ, ENIGMA_TRACE="1", ENIGMA_SEED="0")
    p = subprocess.run([binary] + args, input=ct, capture_output=True,
                       text=True, check=False, env=env)
    return p.stderr


def climbs(err):
    """Split a trace into climbs: a list of sweeps plus the final score.
    A sweep is (start, [(probe, rank, gain)], probes, end)."""
    out, cur, sw = [], [], None
    for line in err.splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == "SW":
            sw = [float(f[1]), [], 0, 0.0]
        elif f[0] == "A" and sw is not None:
            sw[1].append((int(f[1]), int(f[2]), float(f[3])))
        elif f[0] == "SE" and sw is not None:
            sw[2], sw[3] = int(f[1]), float(f[2])
            cur.append(tuple(sw))
            sw = None
        elif f[0] == "HE":
            if cur:
                out.append((cur, float(f[1])))
            cur = []
    return out


def cap_point(sweep):
    """Score when a CAP-probe stale run first occurs, else None."""
    start, acc, n, _ = sweep
    s, last = start, 0
    for p, _, g in acc:
        if p - 1 - last >= CAP:
            return s
        s += g
        last = p
    return s if n - last >= CAP else None


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def report(L, true_c, wrong_c, good):
    print(f"=== L={L}: {len(true_c)} true-key climbs, "
          f"{len(wrong_c)} wrong-key climbs")
    for name, cl in (("true", true_c), ("wrong", wrong_c)):
        sweeps = [s for c, _ in cl for s in c]
        acc = [a for s in sweeps for a in s[1]]
        probes = sum(s[2] for s in sweeps)
        first = [r for p, r, _ in acc if p <= 325]
        later = [r for p, r, _ in acc if p > 325]
        print(f"  {name}: {len(sweeps)} sweeps, {len(acc)} accepted, "
              f"{probes} probes, {len(later)} accepted after the first "
              "cycle")
        for cut in (100, 150, 200, 250, 300):
            print(f"    IC rank >= {cut}: first cycle "
                  f"{pct(sum(r >= cut for r in first), len(first)):5.1f}%"
                  f", later {pct(sum(r >= cut for r in later), len(later)):5.1f}%"
                  f"  (share of the order {pct(325 - cut, 325):4.1f}%)")
    print("  gain rule: stop after k accepted moves in a row gaining < eps")
    gains = sorted(a[2] for c, _ in true_c for s in c for a in s[1])
    for q in (0.1, 0.25, 0.5):
        eps = gains[int(q * len(gains))]
        for k in (1, 2, 3):
            row = []
            for name, cl in (("true", true_c), ("wrong", wrong_c)):
                saved = tot = lost = later = 0
                for c, _ in cl:
                    for _, acc, n, _ in c:
                        tot += n
                        run_ = 0
                        for i, (p, _, g) in enumerate(acc):
                            run_ = run_ + 1 if g < eps else 0
                            if run_ >= k:
                                saved += n - p
                                lost += sum(x[2] for x in acc[i + 1:])
                                later += 1
                                break
                row.append(f"{name} saves {pct(saved, tot):4.1f}% of probes,"
                           f" fires in {later} sweeps, forfeits {lost:.3f}")
            print(f"    eps=q{q:.2f} ({eps:.4f}) k={k}: " + "; ".join(row))
    print(f"  gate: cap at {CAP} only below T, T a quantile of wrong-key "
          "scores at the cap point")
    wpts = sorted(x for c, _ in wrong_c
                  if (x := cap_point(c[0])) is not None)
    print(f"    true key: {len(good)} of {len(true_c)} climbs finish above "
          "every wrong-key climb on their message")
    for q in (0.5, 0.9, 0.99, 0.999):
        T = wpts[min(len(wpts) - 1, int(q * len(wpts)))]
        hit = sum(1 for s in good if s is not None and s < T)
        print(f"    T = wrong q{q} ({T:.4f}): wrong climbs gated "
              f"{pct(sum(x < T for x in wpts), len(wpts)):5.1f}%, "
              f"winning true climbs capped {hit} of {len(good)}")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--lengths", type=int, nargs="+", default=[80, 107, 167])
    ap.add_argument("--fixtures", type=int, default=6)
    ap.add_argument("--restarts", type=int, default=100)
    ap.add_argument("--seed", type=int, default=83)
    args = ap.parse_args()
    os.environ.setdefault("ENIGMA_DATA",
                          os.path.join(os.path.dirname(HERE), "ngrams"))
    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(
                         HERE, "enigma-army-messages-1941.txt")))
    print(f"recipe {' '.join(RECIPE)}; {args.fixtures} fixtures per length;"
          f" true key -R {args.restarts}, wrong keys 676 x -R 2\n")
    for L in args.lengths:
        true_c, wrong_c, good = [], [], []
        for f in range(args.fixtures):
            pt, key, pb = fixture(corpus, L, args.seed * 1013 + L * 37 + f)
            ct = run(args.binary, {}, key + ["-s", pb], pt)
            tc = climbs(trace(args.binary, key + RECIPE
                              + ["-R", str(args.restarts)], ct))
            rng = random.Random(f)
            while True:
                w2 = "".join(map(str, rng.sample([1, 2, 3, 4, 5], 3)))
                if w2 != key[3]:
                    break
            wrong = ["-u", "B", "-w", w2, "-r", key[5],
                     "-g", rng.choice(LET) + ".."]
            wc = climbs(trace(args.binary, wrong + RECIPE
                              + ["-R", "2"], ct))
            top = max(h for _, h in wc)
            good += [cap_point(c[0]) for c, h in tc if h > top]
            true_c += tc
            wrong_c += wc
        report(L, true_c, wrong_c, good)


if __name__ == "__main__":
    main()
