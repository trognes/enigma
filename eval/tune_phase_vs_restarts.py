#!/usr/bin/env python3
"""Matched-compute A/B: --tune-phase vs spending the same wall time on -R.

The question --tune-phase leaves open is not whether it works (it does) but
whether the compute it saves is better spent on it than on restarts against the
full ring keyspace.  Both arms get the same wall time on the same instance:

  arm B (restarts)   full ring1/ring2 enumeration, 26^4 keys (169676 at L=200
                     after the §7.12 middle-wheel collapse), -R 0
  arm A (tune-phase) offsets only, --tune-phase 2 -> 2704 keys, -R 24

Calibrated on this box at L=200 to 28.3 s vs ~28.6 s (4 threads).  The budget is
length-dependent -- §7.12 collapses more of arm B's keyspace as L falls -- so
AB_LEN and AB_R must be re-calibrated together.  Everything else is identical:
same climb recipe (-J -S m4f10 --polish -f -l english), same reflector and
wheel order, ring0/start0 pinned to A for both.

Paired: each trial's ciphertext goes to both arms.  Metrics are the graded
%-of-letters-correct (primary, per CLAUDE.md) and exact recovery (secondary).
Results append to a JSONL file so the run is resumable.
"""
import hashlib
import json
import os
import random
import subprocess
import sys
import time

BIN = "./enigma"
LEN = int(os.environ.get("AB_LEN", "200"))
PLUGS = 10
THREADS = "4"
CLIMB = ["-f", "-l", "english", "-J", "-S", "m4f10", "-c"]
# AB_POLISH=0 ablates the finisher from BOTH arms.  It is on by default because
# it is the recommended recipe, but it is not neutral between the arms: arm B's
# characteristic miss is a near-solution board (right key, a few plugs wrong),
# which is exactly what the finisher repairs, while arm A's is a wrong offset,
# which nothing downstream can.
if os.environ.get("AB_POLISH", "1") != "0":
    CLIMB.append("--polish")
WHEELS = ["-u", "B", "-w", "231"]

ARM_B = ["-r", "A..", "-g", "A..", "-R", "0"]
ARM_A = ["-r", "A..", "-g", "A..", "--tune-phase", "2",
         "-R", os.environ.get("AB_R", "24")]

OUT = sys.argv[1] if len(sys.argv) > 1 else \
    "eval/results-tune-phase-vs-restarts.jsonl"
NTRIALS = int(sys.argv[2]) if len(sys.argv) > 2 else 20
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 12345


# The corpus is PINNED to a file, not read from the repo's own documentation.
# The first version of this harness built it from README/CLAUDE/IMPROVEMENTS,
# which are mutable: writing up the result edited those files, the corpus grew
# by 654 letters, and every excerpt offset shifted.  38 of 40 trials then drew a
# DIFFERENT plaintext while ring/start/board stayed identical -- because those
# draws do not depend on the text -- so an "are these the same instances?" check
# on the key and board passed while the actual problems had changed underneath.
# A follow-up run silently stopped being paired with the first one.  The file
# below is the exact corpus the shipped §7.15 numbers were measured on.
CORPUS = "eval/corpus-tune-phase-ab.txt"


def corpus():
    t = open(CORPUS, encoding="utf-8").read()
    return "".join(c for c in t.upper() if "A" <= c <= "Z")


def run(args, stdin):
    p = subprocess.run([BIN] + args, input=stdin, capture_output=True,
                       text=True, env={**os.environ, "ENIGMA_SEED": "0"})
    return p.stdout.strip()


def pct_correct(got, want):
    if not got:
        return 0.0
    n = min(len(got), len(want))
    return 100.0 * sum(a == b for a, b in zip(got[:n], want[:n])) / len(want)


def main():
    text = corpus()
    rng = random.Random(SEED)
    done = 0
    if os.path.exists(OUT):
        done = sum(1 for _ in open(OUT))
    fh = open(OUT, "a", buffering=1)

    for i in range(NTRIALS):
        # draw the instance FIRST so the sequence is identical on resume
        off = rng.randrange(len(text) - LEN)
        plain = text[off:off + LEN]
        ring = "A" + rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") \
                   + rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
        start = "A" + rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") \
                    + rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
        letters = list("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
        rng.shuffle(letters)
        board = "".join(letters[:2 * PLUGS])
        if i < done:
            continue

        ct = run(["-i"] + WHEELS + ["-r", ring, "-g", start, "-s", board],
                 plain)

        rec = {"trial": i, "ring": ring, "start": start, "board": board,
               "pt": hashlib.sha1(plain.encode()).hexdigest()[:12]}
        for name, arm in (("B_restarts", ARM_B), ("A_tunephase", ARM_A)):
            t0 = time.time()
            got = run(CLIMB + WHEELS + arm + ["-T", THREADS], ct)
            rec[name] = {"pct": round(pct_correct(got, plain), 2),
                         "exact": got == plain,
                         "sec": round(time.time() - t0, 1)}
        fh.write(json.dumps(rec) + "\n")
        star = lambda x: "*" if x["exact"] else " "
        print("trial %2d  B %5.1f%%%s   A %5.1f%%%s   (%.0fs / %.0fs)" % (
            i, rec["B_restarts"]["pct"], star(rec["B_restarts"]),
            rec["A_tunephase"]["pct"], star(rec["A_tunephase"]),
            rec["B_restarts"]["sec"], rec["A_tunephase"]["sec"]),
            flush=True)


if __name__ == "__main__":
    main()
