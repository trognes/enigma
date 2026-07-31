#!/usr/bin/env python3
"""The three conditions --ring-stride was never measured under.

archived/PERFORMANCE.md 7.11 records the flag's accuracy on short authentic
messages with the plugboard supplied via -s and no -c, wheels I-VIII, K <= 5.
Three conditions sat outside every run and were listed as open:

  * K >= 8, where the coarse pass keeps two or three ring2 values and the
    refinement is most of the work,
  * a HIDDEN plugboard, where the coarse winner is chosen under a score the
    stecker is still corrupting rather than a clean one,
  * messages long enough for the LEFT wheel to step, which is the only
    condition that exercises the refinement's left-wheel derivation at all.

K >= 8 needs no new harness -- eval/ring_stride_wehrmacht_probe.py measures it
with KS="8 10 13 26". This one covers the other two.

METRIC, in the style of 7.11: the stride-specific miss. Every trial is run at
K=1 (the exhaustive ring2 sweep) as well as at each stride, on the same
ciphertext and key; a trial that fails at K=1 is a scoring-floor case that no
ring2 policy can reach, so it is excluded. What is reported is trials the
exhaustive search recovers and the strided one does not. That makes the
exhaustive search the reference, so no second binary is needed.

MODES

  hidden     The left wheel is pinned at its TRUE ring0/start0 -- the same
             isolation eval/ring_stride_refine_shape_probe.py uses -- while
             ring1/start1/ring2/start2 stay open, so the middle-wheel
             derivation is live. HIDE of the 10 plug pairs are withheld and the
             rest given via -s, with -c recovering the remainder. HIDE=10 is a
             fully hidden board; it is ~4x slower per trial than HIDE=5 because
             the climb has every letter free.

             Pinning the left wheel is what makes this affordable, and it is
             legitimate here: 7.10's collapse makes ring0 unidentifiable, so
             the left wheel contributes a single offset that a hidden board
             does not interact with. Note that "-r A.. -g A.." does NOT express
             this -- pinning ring0 is free only when start0 stays wildcarded to
             enumerate the offset. Pinning both nails the offset to 0 while the
             truth is random, and the true key is then absent from the search
             space entirely; that mistake reads as 0% recovery and looks like a
             difficulty result.

  leftwheel  ring0 and start0 both wildcarded (-r ... -g ...) on messages long
             enough that the middle wheel reaches its OWN notch, which is what
             steps the left wheel. The corpus tops out at 214 characters, so
             these are authentic telegraphic blocks CONCATENATED to length --
             real Wehrmacht traffic was capped near 250 characters precisely to
             limit depth, so treat this as a correctness condition for the
             derivation, not an operational one. Each trial reports how many
             times the left wheel actually stepped, and the miss rate is also
             given restricted to the trials where it stepped at least once,
             since a trial with no left step tests nothing new.

  notch      The same question at a length the flag is actually used at. Rather
             than waiting for a random start1 to reach the middle wheel's notch,
             start1 is placed a few steps before it, so the left wheel steps
             inside a short message. This is the targeted version of leftwheel;
             both are reported because a placed start1 is not a random one.

Usage: MODE=hidden python3 eval/ring_stride_scope_probe.py
Env: MODE (hidden|leftwheel|notch), LENGTHS, KS, TRIALS, SEED, THREADS, HIDE
"""
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")

sys.path.insert(0, HERE)
from ring_stride_wehrmacht_probe import CORPUS_BLOCKS, ALPHA   # noqa: E402
from ring_stride_geometry_probe import NOTCH, num              # noqa: E402

MODE = os.environ.get("MODE", "hidden")
DEFAULTS = {
    "hidden":    ("200", "3 8 13", "24", "5"),
    "leftwheel": ("600", "3 8 13 26", "60", "0"),
    "notch":     ("110", "3 8 13 26", "60", "0"),
}
_L, _K, _T, _H = DEFAULTS.get(MODE, DEFAULTS["hidden"])
LENGTHS = [int(x) for x in os.environ.get("LENGTHS", _L).split()]
KS = [int(x) for x in os.environ.get("KS", _K).split()]
TRIALS = int(os.environ.get("TRIALS", _T))
HIDE = int(os.environ.get("HIDE", _H))
SEED = int(os.environ.get("SEED", "0"))
THREADS = os.environ.get("THREADS", "4")


def run(args, inp):
    r = subprocess.run([BIN] + args, input=inp, capture_output=True, text=True,
                       cwd=ROOT)
    return r.stdout.strip()


def cat_text(rng, L):
    """A length-L excerpt of authentic telegraphic German, concatenating whole
    blocks when L exceeds the longest single message (214 characters)."""
    pool = [b for b in CORPUS_BLOCKS if len(b) >= L]
    if pool:
        b = rng.choice(pool)
        off = rng.randrange(0, len(b) - L + 1)
        return b[off:off + L]
    out = ""
    while len(out) < L:
        out += rng.choice(CORPUS_BLOCKS)
    return out[:L]


def left_steps(wheelnums, start, n):
    """How many times the LEFT wheel steps over n characters. Mirrors
    setup_mapping()'s stepping, double-step branch included: the middle wheel
    sitting on its own notch advances both itself and the left wheel."""
    notch = [set(num(NOTCH[w - 1]).tolist()) if NOTCH[w - 1] else set()
             for w in wheelnums]
    g0, g1, g2 = (ord(c) - 65 for c in start)
    steps = 0
    for _ in range(n):
        if g1 in notch[1]:
            steps += 1
            g0 = (g0 + 1) % 26
            g1 = (g1 + 1) % 26
        elif g2 in notch[2]:
            g1 = (g1 + 1) % 26
        g2 = (g2 + 1) % 26
    return steps


def random_key(rng, wheel_hi=8):
    u = rng.choice("ABC")
    w = rng.sample(range(1, wheel_hi + 1), 3)
    r = "".join(rng.choice(ALPHA) for _ in range(3))
    g = "".join(rng.choice(ALPHA) for _ in range(3))
    letters = list(ALPHA)
    rng.shuffle(letters)
    pairs = [letters[2 * j] + letters[2 * j + 1] for j in range(10)]
    return u, "".join(str(x) for x in w), r, g, pairs


def place_start1(rng, wheelnum_mid, back):
    """A start1 sitting `back` steps before the middle wheel's own notch, so the
    left wheel steps once the middle wheel has advanced that far."""
    n = sorted(num(NOTCH[wheelnum_mid - 1]).tolist())
    return chr(65 + (rng.choice(n) - back) % 26)


def trial(L, rng):
    u, w, r, g, pairs = random_key(rng)
    wn = [int(c) for c in w]
    if MODE == "notch":
        # the middle wheel advances about once per 26 characters, so putting
        # start1 a couple of steps short of its notch makes the left wheel step
        # well inside a 110-character message
        g = g[0] + place_start1(rng, wn[1], rng.randrange(1, 3)) + g[2]
    pt = cat_text(rng, L)
    ct = run(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", " ".join(pairs)], pt)

    if MODE == "hidden":
        base = ["-f", "-l", "wehrmacht", "-c", "-u", u, "-w", w,
                "-r", r[0] + "..", "-g", g[0] + "..", "-T", THREADS]
        if HIDE < 10:
            base += ["-s", " ".join(pairs[:10 - HIDE])]
    else:
        base = ["-f", "-l", "wehrmacht", "-u", u, "-w", w,
                "-r", "...", "-g", "...", "-s", " ".join(pairs), "-T", THREADS]

    out = {"left": left_steps(wn, g, L), 1: run(base, ct) == pt}
    for K in KS:
        out[K] = run(base + ["--ring-stride", str(K)], ct) == pt
    return out


def sweep():
    print("# mode=%s trials=%d seed=%d hide=%d threads=%s"
          % (MODE, TRIALS, SEED, HIDE, THREADS))
    print("# lost = the exhaustive K=1 search recovers it and the stride does not")
    cols = ["len", "K", "n", "K=1 base", "strided", "lost", "lost%"]
    if MODE != "hidden":
        cols += ["n left-step", "lost there"]
    print("\t".join(cols))
    for L in LENGTHS:
        rng = random.Random(SEED * 1000 + L)     # paired across every K
        t0 = time.time()
        rows = [trial(L, rng) for _ in range(TRIALS)]
        base = [row for row in rows if row[1]]
        stepped = [row for row in base if row["left"] > 0]
        for K in KS:
            lost = [row for row in base if not row[K]]
            cells = [str(L), str(K), str(TRIALS),
                     "%d%%" % round(100 * len(base) / TRIALS),
                     "%d%%" % round(100 * sum(1 for r in rows if r[K]) / TRIALS),
                     str(len(lost)),
                     "%.1f%%" % (100.0 * len(lost) / max(len(base), 1))]
            if MODE != "hidden":
                cells += [str(len(stepped)),
                          str(sum(1 for r in stepped if not r[K]))]
            print("\t".join(cells))
            sys.stdout.flush()
        if MODE != "hidden":
            tot = sum(r["left"] for r in rows)
            print("# L=%d: left wheel stepped in %d/%d trials, %d steps total, "
                  "%.0fs/trial" % (L, sum(1 for r in rows if r["left"] > 0),
                                   TRIALS, tot, (time.time() - t0) / TRIALS))
        else:
            print("# L=%d: %.0fs/trial" % (L, (time.time() - t0) / TRIALS))
        sys.stdout.flush()


if __name__ == "__main__":
    sweep()
