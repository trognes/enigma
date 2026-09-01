#!/usr/bin/env python3
"""Does the climb succeed ON THE RIGHT ROTORS? -- the precondition every
plaintext-side rescue (doubling report, word/name report, LM re-ranker) needs.

    python3 eval/climb_at_truekey.py --trials 150 --lengths 40 50 60 \
            --garble 0 0.05 0.10 0.15 --length-garble 60

A rank-free report or an LM re-ranker can only act on plaintext the search
actually produced. So the question that bounds all of them is: with the TRUE
ROTOR KEY given and only the board hidden, does the climb recover the board?
If not, the true-key decrypt is a wrong-board garble, the doubling never
appears, and there is nothing for any plaintext-side method to read -- which
is exactly what happens to EGERF (recovers 7% at its own true key) while
FTNBK/HBNVE/ALWOK recover 100%.

Scoring failures are rare at operational length, so this GENERATES them two
ways the real corpus shows are the causes: SHORT messages (L=40-60, where the
scoring floor is high) and GARBLES (ciphertext mutated at rate g; Enigma has
no diffusion, so one ciphertext error damages exactly one plaintext letter --
the real transmission-corruption model).

Classification, true rotor key, board hidden:
  SUCCESS       recovered >= 90%  -- true board found; plaintext available,
                                     so a report/LM CAN act
  SCORING FAIL  else and best-board score >= true-board score -- a wrong board
                                     outscores the truth even here; NO
                                     plaintext, hopeless for a plaintext method
  SEARCH FAIL   else -- truth outscores but the climb stuck; more -R helps

The SUCCESS fraction is the ceiling on what a report or an LM can ever reach.
"""

import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(argv, text, dump=False):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    p = subprocess.run([ENIGMA] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return (p.stdout.strip(), p.stderr) if dump else p.stdout.strip()


def best_score(stderr):
    """max dumpall score over the converged boards."""
    best = None
    for ln in stderr.splitlines():
        m = re.match(r"dumpall \S+ \S+ \S+ ([-\d.]+)", ln)
        if m:
            v = float(m.group(1))
            best = v if best is None else max(best, v)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=150)
    ap.add_argument("--lengths", type=int, nargs="+", default=[40, 50, 60])
    ap.add_argument("--garble", type=float, nargs="+", default=[0.0])
    ap.add_argument("--length-garble", type=int, default=60,
                    help="length at which the garble sweep is run")
    ap.add_argument("--restarts", type=int, default=64)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--recover-thresh", type=float, default=90.0)
    ap.add_argument("--seed", type=int, default=13)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")
    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)

    # cells: (length, garble); clean at each length, garble sweep at one
    cells = [(L, 0.0) for L in args.lengths]
    cells += [(args.length_garble, g) for g in args.garble if g > 0]

    def score_true_board(key, ct, pb):
        # true-board score at the true key: dumpall of an -R 0 climb pinned to
        # the true board via -s + --no-plug on the rest? simpler: -s pins it,
        # -R 0 one climb from that seed; but the climb could move it. Instead
        # score the fixed board with a no-move run: -s + everything else fixed
        # is not exposed, so approximate with a very short pinned climb.
        out, err = run(key + ["-c", "-R", "0", "--dump-all", "-s", pb,
                              "-f", "-l", "wehrmacht"], ct, dump=True)
        return best_score(err)

    print(f"# climb at the TRUE ROTOR KEY, board hidden, -c -K --polish "
          f"-S k4f10 -f -l wehrmacht -R {args.restarts}, {args.plugs} plugs")
    print(f"# SUCCESS = recovered >= {args.recover_thresh:.0f}% "
          f"(plaintext available for any report/LM)\n")
    print(f"{'L':>4} {'garble':>7} {'n':>4}  {'SUCCESS':>8} "
          f"{'SCORE-fail':>11} {'SEARCH-fail':>11}")
    for L, g in cells:
        pool_ok = 0
        succ = scorefail = searchfail = 0
        n = 0
        for _ in range(args.trials):
            pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
            w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
            r = "".join(rng.choice(LET) for _ in range(3))
            gg = "".join(rng.choice(LET) for _ in range(3))
            ls = list(LET)
            rng.shuffle(ls)
            pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(args.plugs))
            key = ["-u", "B", "-w", w, "-r", r, "-g", gg]
            ct = run(key + ["-s", pb], pt)
            if len(ct) != L:
                continue
            if g > 0:                       # mutate ciphertext at rate g
                ct = "".join(rng.choice(LET) if rng.random() < g else c
                             for c in ct)
            # the true plaintext THIS ciphertext decrypts to (garbles included)
            true_pt = run(key + ["-s", pb], ct)
            s_true = score_true_board(key, ct, pb)
            out, err = run(key + ["-c", "-K", "--polish", "-S", "k4f10",
                                  "-f", "-l", "wehrmacht", "-R", args.restarts,
                                  "--dump-all", "-T", 1], ct, dump=True)
            if not out or s_true is None:
                continue
            rec = 100.0 * sum(a == b for a, b in zip(out, true_pt)) / L
            s_best = best_score(err)
            n += 1
            if rec >= args.recover_thresh:
                succ += 1
            elif s_best is not None and s_best >= s_true:
                scorefail += 1
            else:
                searchfail += 1
        if n:
            print(f"{L:>4} {g:>7.2f} {n:>4}  {100*succ/n:>7.1f}% "
                  f"{100*scorefail/n:>10.1f}% {100*searchfail/n:>10.1f}%")


if __name__ == "__main__":
    main()
