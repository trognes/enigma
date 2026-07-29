#!/usr/bin/env python3
"""Measures --ring-stride's REAL exact-recovery rate (the shipped feature, via the real
binary) on short authentic Wehrmacht (telegraphic German) excerpts, K=2 vs K=3 vs the
K=1 (no-stride) baseline.

Why this harness exists, distinct from eval/ring_stride_probe.py: that script measured
the ring2-selection PROPERTY (does the winning strided candidate land close to the
truth?) via an external re-implementation of the stride idea, run BEFORE --ring-stride
was implemented, on synthetic English prose. This script instead drives the actual
shipped `--ring-stride K` flag end to end, on real excerpts from the authentic 1941
message database (eval/enigma-messages.txt, eval/enigma-army-messages-1941.txt), to
answer the practical question: does `--ring-stride 3`'s cost saving over K=2 (12 vs 16
total ring2 candidates, PERFORMANCE.md 7.11) come at a real exact-recovery cost on the
short, information-poor messages the wehrmacht register exists for?

Like the original 7.11 measurement (and for the same reason), the true plugboard is
given via -s rather than hill-climbed: --ring-stride is a ROTOR-KEY search lever, and
isolating it from plugboard-search noise is what makes the comparison clean and fast (a
bare rotor-key scan, not a keys x restarts hillclimb over an 11M-key space). Scoring
uses -f (fused weighted-all + IC, the recommended model when the language is known) so
the measurement reflects the sharpest discrimination the tool actually offers, not a
weaker proxy.

Method per trial:
  - A real excerpt of length L, cut from the concatenated clean (no unrecorded-letter
    placeholders) DECRYPT blocks of the authentic message database.
  - A random key: reflector, 3 distinct wheels from I-V, ring1/start0/start1/start2
    wildcarded (ring0 auto-collapses per 7.10), a random 10-pair plugboard (matching
    the standard Wehrmacht board size).
  - Encrypt with the true key (the true board given via -s for both encrypt and
    recover). Recover with --ring-stride K (K in {1,2,3}), -f -l wehrmacht, the same
    -s board (plugboard hidden from the SEARCH would need -c and confound the rotor-key
    question this harness targets -- see above).
  - "exact" = recovered plaintext byte-identical to the true excerpt.
  - K=1 is the no-stride baseline: any trial that ALSO fails at K=1 is a pre-existing
    scoring/search-floor case unrelated to ring-stride (PERFORMANCE.md's
    scoring-failure-vs-search-failure distinction) and is reported separately so a
    ring-stride-specific miss rate isn't inflated by floor noise neither K fixes.

Usage: python3 eval/ring_stride_wehrmacht_probe.py
Env: LENGTHS ("40 60"), KS ("2 3"), TRIALS (150), SEED (0), THREADS (4)
"""
import os
import random
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "40 60").split()]
KS = [int(x) for x in os.environ.get("KS", "2 3").split()]
TRIALS = int(os.environ.get("TRIALS", "150"))
SEED = int(os.environ.get("SEED", "0"))
THREADS = os.environ.get("THREADS", "4")


def load_corpus():
    text = ""
    for fname in ("enigma-messages.txt", "enigma-army-messages-1941.txt"):
        text += open(os.path.join(HERE, fname)).read()
    blocks = re.findall(r"^DECRYPT:\s*(.*(?:\n {13}\S.*)*)", text, re.M)
    clean = [re.sub(r"\s+", "", b) for b in blocks]
    clean = [b for b in clean if "-" not in b and len(b) >= 40]
    return clean


CORPUS_BLOCKS = load_corpus()


def excerpt(L, rng):
    """A real contiguous excerpt of length L, cut from one authentic message."""
    candidates = [b for b in CORPUS_BLOCKS if len(b) >= L]
    block = rng.choice(candidates)
    off = rng.randrange(0, len(block) - L + 1)
    return block[off:off + L]


def random_key(rng):
    u = rng.choice("ABC")
    w = rng.sample(range(1, 6), 3)
    r = [rng.choice(ALPHA) for _ in range(3)]
    g = [rng.choice(ALPHA) for _ in range(3)]
    letters = list(ALPHA)
    rng.shuffle(letters)
    board = " ".join(letters[2 * j] + letters[2 * j + 1] for j in range(10))
    return u, "".join(str(x) for x in w), "".join(r), "".join(g), board


def encrypt(args, pt):
    r = subprocess.run([BIN] + args, input=pt, capture_output=True, text=True, cwd=ROOT)
    return r.stdout.strip()


def recover(ct, u, w, board, K):
    args = ["-f", "-l", "wehrmacht", "-u", u, "-w", w, "-r", "...", "-g", "...",
            "-s", board, "-T", THREADS]
    if K > 1:
        args += ["--ring-stride", str(K)]
    r = subprocess.run([BIN] + args, input=ct, capture_output=True, text=True, cwd=ROOT)
    return r.stdout.strip()


def trial(L, rng):
    u, w, r, g, board = random_key(rng)
    pt = excerpt(L, rng)
    ct = encrypt(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", board], pt)
    results = {}
    for K in [1] + KS:
        out = recover(ct, u, w, board, K)
        results[K] = (out == pt)
    return results


def sweep(Ls, Ks, n_trials, seed):
    print("%6s %4s %8s %10s %14s %14s" % ("L", "K", "n", "exact%", "vs-K1-fail", "K1-floor-fail"))
    for L in Ls:
        rng = random.Random(seed * 1000 + L)   # same seed per L -> paired across K
        rows = [trial(L, rng) for _ in range(n_trials)]
        k1_fail = sum(1 for row in rows if not row[1])
        for K in Ks:
            exact = sum(1 for row in rows if row[K])
            # ring-stride-specific miss: K failed but K=1 (no-stride) succeeded
            specific_fail = sum(1 for row in rows if (not row[K]) and row[1])
            print("%6d %4d %8d %9.1f%% %13d%% %13d%%"
                  % (L, K, n_trials, 100 * exact / n_trials,
                     round(100 * specific_fail / n_trials), round(100 * k1_fail / n_trials)))


if __name__ == "__main__":
    print("corpus: %d clean authentic-message blocks, %d letters total"
          % (len(CORPUS_BLOCKS), sum(len(b) for b in CORPUS_BLOCKS)))
    sweep(LENGTHS, KS, TRIALS, SEED)
