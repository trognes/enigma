#!/usr/bin/env python3
"""Calibrate the pre-flight statistics: is this ciphertext even Enigma?

    python3 eval/preflight_null.py              # the full calibration
    python3 eval/preflight_null.py --trials 500 # a quick look

A 28-hour sweep of QTXMA (75.2M keys) returned nothing, and the reason turned
out to be visible in the ciphertext before the search started: its index of
coincidence is far too high for Enigma output, and four letters of the
alphabet never occur at all.  Enigma is a permutation cipher, so its output is
near-flat; anything with residual language structure is not Enigma.

This script establishes the null those two statistics are judged against, and
fixes the thresholds the tool ships with.  THE NULL MUST BE LENGTH-DEPENDENT:
IC variance grows as 1/C(n,2), so a short message reaches a high IC by chance
routinely -- two of the four BROKEN (i.e. genuinely Enigma) challenge messages
sit above +4 sd, at 47 and 74 letters.  A fixed IC threshold would flag them.

WHY NO TABLES ARE NEEDED.  Both statistics have closed forms under a uniform
multinomial, and this script checks that against simulated Enigma encryptions:

  IC      = P / C(n,2), where P counts same-letter position pairs.  With
            uniform p the pair indicators are pairwise UNCORRELATED -- the
            shared-index covariance is sum p^3 - (sum p^2)^2 = 1/A^2 - 1/A^2
            = 0 -- so E[IC] = 1/A exactly and Var[IC] = q(1-q)/C(n,2),
            q = 1/A.  No dependence on the plaintext at all.

  absent  = number of letters of the alphabet that never occur.
            E[X] = A(1-1/A)^n and
            Var[X] = A(1-1/A)^n + A(A-1)(1-2/A)^n - A^2(1-1/A)^(2n).
            X is tiny and skewed, so a z-score misleads; the tail is reported
            as the first Bonferroni term P(X>=k) ~ C(A,k)(1-k/A)^n, which is
            an upper bound and an excellent approximation when small.

WHAT THE THRESHOLDS MUST SATISFY.  A false positive is expensive in trust and
a false negative merely costs what it costs today, so the bar is set from the
measured tail of GENUINE Enigma, not from a nominal p-value -- the same
discipline `--confidence` documents for its own Gaussian tail.
"""
import argparse
import collections
import math
import os
import random
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import corpus, random_key                       # noqa: E402
from ring_stride_geometry_probe import crypt, plugboard         # noqa: E402

A = 26
Q = 1.0 / A


def ic(s):
    c = collections.Counter(s)
    n = len(s)
    return sum(v * (v - 1) for v in c.values()) / (n * (n - 1)) if n > 1 else 0.0


def ic_sd(n):
    """Analytic sd of IC under a uniform multinomial."""
    return math.sqrt(Q * (1 - Q) / (n * (n - 1) / 2.0))


def absent_mean_sd(n):
    m = A * (1 - 1.0 / A) ** n
    v = (A * (1 - 1.0 / A) ** n
         + A * (A - 1) * (1 - 2.0 / A) ** n
         - A * A * (1 - 1.0 / A) ** (2 * n))
    return m, math.sqrt(max(v, 0.0))


def absent_tail(k, n):
    """P(X >= k), first Bonferroni term: an upper bound, tight when small."""
    if k <= 0:
        return 1.0
    if k > A:
        return 0.0
    return min(1.0, math.comb(A, k) * (1 - float(k) / A) ** n)


# The shipped rule.  Thresholds are set from the MEASURED tail of genuine
# Enigma below, not from a nominal p-value.
Z_IC = 6.0
P_ABSENT = 1e-4


def flags(ct):
    n = len(ct)
    zi = (ic(ct) - Q) / ic_sd(n)
    pa = absent_tail(A - len(set(ct)), n)
    return zi > Z_IC, pa < P_ABSENT


def sample_enigma(rng, texts, n, trials):
    """Enigma encryptions of authentic German at length n.

    The machine model is the real one; the key, the 10-pair board and the
    excerpt are random, so these are SIMULATED ciphertexts rather than real
    traffic -- the same distinction the unknown-key section of CLAUDE.md draws.
    """
    out_ic, out_ab = [], []
    for _ in range(trials):
        pt = rng.choice(texts)
        o = rng.randrange(len(pt) - n + 1)
        w, r, ring, st = random_key(rng)
        pl = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
        c = crypt(pt[o:o + n], w, r, ring, st, pl)
        out_ic.append(ic(c))
        out_ab.append(A - len(set(c)))
    return np.array(out_ic), np.array(out_ab)


def load_messages():
    """Every ciphertext in the two 1941 data files, with broken/unbroken."""
    rows = []
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "enigma-challenge-1941.txt")
    txt = open(p, encoding="utf-8").read()
    for b in re.split(r"^### ", txt, flags=re.M)[1:]:
        head = b.split("\n")[0]
        m = re.search(r"CIPHERTEXT:\s*(.*?)\nNOTES:", b, re.S)
        if not m:
            continue
        ct = re.sub(r"[^A-Z]", "", m.group(1))
        kn = re.search(r"\(([A-Z]{5})\)", head)
        if kn and ct.startswith(kn.group(1)):
            ct = ct[5:]
        key = re.search(r"KEY:\s*(.*)", b)
        unb = bool(key) and key.group(1).strip().upper().startswith("UNKNOWN")
        rows.append((kn.group(1) if kn else head[:12], ct, unb))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=20260817)
    ap.add_argument("--out", default="eval/results-preflight-null.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    base = [t for t in corpus()]
    # The corpus tops out at 214 letters; concatenate for the longer cells.
    joined = "".join(base)
    log = []

    def say(s=""):
        print(s, flush=True)
        log.append(s)

    lengths = [40, 60, 100, 155, 300, 600]

    # ONE sample set, reused by every section.  Sampling separately per section
    # was worse than wasteful: it made "18000 ciphertexts" mean a different
    # 18000 in each, so the tail figure and the false-positive figure could not
    # honestly be quoted in the same sentence.
    say(__doc__.split("\n")[0])
    say("\n%d simulated Enigma encryptions per length (%d total), authentic"
        % (a.trials, a.trials * len(lengths)))
    say("1941 German plaintext, random key and random 10-pair board each.")
    say("NOT real traffic: the corpus tops out at %d letters, so n >= 300 draws"
        % max(len(t) for t in base))
    say("from the concatenated corpus rather than from one message.\n")
    sample = {}
    for n in lengths:
        texts = [t for t in base if len(t) >= n] or [joined]
        sample[n] = sample_enigma(rng, texts, n, a.trials)

    say("== 1. the analytic null matches simulated Enigma ==\n")
    say("%6s  %-24s %-24s %-10s"
        % ("n", "analytic IC mean/sd", "measured IC mean/sd", "sd ratio"))
    for n in lengths:
        i_, _ = sample[n]
        sa = ic_sd(n)
        say("%6d  %.6f / %.6f    %.6f / %.6f    %.3f"
            % (n, Q, sa, i_.mean(), i_.std(), i_.std() / sa))

    say("\n%6s  %-24s %-24s" % ("n", "analytic absent mean/sd",
                                "measured absent mean/sd"))
    for n in lengths:
        _, ab = sample[n]
        m, s = absent_mean_sd(n)
        say("%6d  %.4f / %.4f          %.4f / %.4f" % (n, m, s, ab.mean(), ab.std()))

    say("\n== 2. how far simulated Enigma reaches: the tail of z(IC) ==\n")
    say("A threshold must clear this, not a nominal p-value.\n")
    say("%6s  %8s %8s %8s %8s %10s"
        % ("n", "median", "99%", "99.9%", "max", "P(z>6)"))
    allz = []
    for n in lengths:
        i_, _ = sample[n]
        z = (i_ - Q) / ic_sd(n)
        allz.append(z)
        say("%6d  %8.2f %8.2f %8.2f %8.2f %10.4f"
            % (n, np.median(z), np.percentile(z, 99),
               np.percentile(z, 99.9), z.max(), (z > 6).mean()))
    z = np.concatenate(allz)
    say("\npooled over %d samples: max z = %.2f, P(z>6) = %.5f, P(z>8) = %.5f"
        % (z.size, z.max(), (z > 6).mean(), (z > 8).mean()))

    say("\n== 3. false-positive rate of the SHIPPED rule ==\n")
    say("Rule: warn if z(IC) > %.1f OR P(absent) < %.0e.  A false positive is"
        % (Z_IC, P_ABSENT))
    say("expensive in trust; a false negative only costs what it costs now.")
    say("SAME sample set as section 2, so the two figures describe one\n"
        "population and can be quoted together.\n")
    say("%6s %10s %12s %10s" % ("n", "IC fires", "absent fires", "either"))
    tot_e = tot_n = 0
    for n in lengths:
        i_, ab = sample[n]
        fi = (i_ - Q) / ic_sd(n) > Z_IC
        fa = np.array([absent_tail(int(k), n) < P_ABSENT for k in ab])
        say("%6d %10.5f %12.5f %10.5f"
            % (n, fi.mean(), fa.mean(), (fi | fa).mean()))
        tot_e += (fi | fa).sum()
        tot_n += fi.size
    say("\npooled: %d of %d simulated Enigma ciphertexts would be flagged (%.5f)"
        % (tot_e, tot_n, tot_e / tot_n))

    say("\n== 4. the 1941 messages, broken ones as controls ==\n")
    say("%-7s %5s %8s %7s %7s %10s %-8s %s"
        % ("msg", "n", "IC", "z(IC)", "absent", "P(absent)", "VERDICT",
           "status"))
    say("-" * 82)
    for name, ct, unb in sorted(load_messages(), key=lambda r: -len(r[1])):
        n = len(ct)
        if n < 40:
            continue
        k = A - len(set(ct))
        zi = (ic(ct) - Q) / ic_sd(n)
        f_i, f_a = flags(ct)
        say("%-7s %5d %8.4f %+7.1f %7d %10.2e %-8s %s"
            % (name, n, ic(ct), zi, k, absent_tail(k, n),
               "FLAG" if (f_i or f_a) else "ok",
               "unbroken" if unb else "BROKEN (control)"))
    say("\nThe four BROKEN messages are genuine Enigma and must all read ok.")

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
