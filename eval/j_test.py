#!/usr/bin/env python3
"""Reproduce the J test of MODERN_BREAKING_NOTES.md 5b.

An Enigma ciphertext uses all 26 letters, so J appears at rate 1/26.  A
25-letter manual alphabet without J gives zero.  The footnote *3 hypothesis
about the collection's Batch C is therefore falsifiable against the ciphertext
this repo holds.

Three things are printed, in increasing order of strength:

  1. the null control -- the J rate across the solved, verified corpus;
  2. the POPULATION test -- zero-J counts, solved vs unbroken, against what
     Enigma predicts.  This is the strongest form, because it needs no choice
     of which message to test and so cannot be data-dredged;
  3. per-message counts, which are weak at these lengths and are printed with
     P(0 J) beside them so that weakness is visible.

Reads only the emitted corpus files, so it stays true after a rebuild.

    python3 eval/j_test.py
"""
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
VERIFIED = ["enigma-army-messages-1941.txt", "enigma-messages.txt"]
CHALLENGE = "enigma-challenge-1941.txt"


def load(path, strip_designator):
    """Records as (label, ciphertext-letters, unbroken?).

    The challenge file keeps the 5-letter Kenngruppe on the front of
    CIPHERTEXT (it is a discriminant, never enciphered, and its header says to
    strip it before deciphering); the solved files have already removed it.
    Garble dashes are real positions but carry no letter, so they are dropped
    for counting.
    """
    out, cur, key = [], None, None
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^### Message No\. (\S+)\s+--\s+(.+?)\s+\((\w+)\)", line)
        if line.startswith("### "):
            cur = {"label": "%s %s" % (m.group(1), m.group(3)) if m else "?",
                   "ct": "", "key": ""}
            out.append(cur)
            key = None
            continue
        if cur is None:
            continue
        m = re.match(r"^([A-Z][A-Z ]*):\s*(.*)$", line)
        if m:
            key = m.group(1)
            if key == "CIPHERTEXT":
                cur["ct"] += m.group(2).strip()
            elif key == "KEY":
                cur["key"] = m.group(2)
        elif line.startswith(" " * 5) and key == "CIPHERTEXT":
            cur["ct"] += line.strip()
    recs = []
    for r in out:
        body = r["ct"].replace("-", "")
        if strip_designator:
            body = body[5:]
        recs.append((r["label"], body, r["key"].startswith("UNKNOWN")))
    return recs


def zero_j(recs):
    """(observed zero-J count, count Enigma predicts)."""
    obs = sum(1 for _, b, _ in recs if "J" not in b)
    exp = sum((25.0 / 26.0) ** len(b) for _, b, _ in recs)
    return obs, exp


def binom_cdf(k, n, p):
    """P(X <= k) for X ~ Binomial(n, p) -- exact.

    A J count IS binomial (n letters, each J with probability 1/26), so this is
    the exact test rather than the Poisson approximation.  It matters at the
    only place a number is quoted: for a zero-J message it returns exactly
    (25/26)^n, which is what the P(0 J) column shows, so the two agree instead
    of differing in the third decimal.
    """
    return sum(math.comb(n, i) * p ** i * (1 - p) ** (n - i)
               for i in range(k + 1))


def poisson_binomial_tail(k, ps):
    """P(X >= k) where X counts successes with per-trial probabilities ps.

    The zero-J messages have DIFFERENT probabilities (they differ in length),
    so their count is Poisson-binomial, not Poisson.  Computed exactly by
    convolution -- cheap at these sizes, and it avoids having to argue that an
    approximation is good enough for the one p-value the section rests on.
    """
    dist = [1.0]
    for p in ps:
        nxt = [0.0] * (len(dist) + 1)
        for i, d in enumerate(dist):
            nxt[i] += d * (1 - p)
            nxt[i + 1] += d * p
        dist = nxt
    return sum(dist[k:])


def main():
    verified = [r for f in VERIFIED for r in load(os.path.join(HERE, f), False)]
    challenge = load(os.path.join(HERE, CHALLENGE), True)
    unbroken = [r for r in challenge if r[2]]

    n = sum(len(b) for _, b, _ in verified)
    j = sum(b.count("J") for _, b, _ in verified)
    print("1. NULL CONTROL -- solved, verified Enigma")
    print("   %d records, %d letters, J = %d (%.2f%%) against %.1f expected "
          "(3.85%%)\n" % (len(verified), n, j, 100.0 * j / n, n / 26.0))

    print("2. POPULATION TEST -- messages with ZERO J")
    print("   %-26s %7s %9s %9s %7s" % ("", "records", "observed",
                                        "expected", "ratio"))
    for name, recs in (("solved, verified Enigma", verified),
                       ("unbroken challenges", unbroken)):
        obs, exp = zero_j(recs)
        print("   %-26s %7d %9d %9.2f %6.1fx"
              % (name, len(recs), obs, exp, obs / exp))
    obs, exp = zero_j(unbroken)
    ps = [(25.0 / 26.0) ** len(b) for _, b, _ in unbroken]
    print("   unbroken excess: exact Poisson-binomial P(X >= %d) = %.4f\n"
          % (obs, poisson_binomial_tail(obs, ps)))

    print("3. PER-MESSAGE -- unbroken challenges, longest first")
    print("   %-12s %6s %5s %9s %9s %8s" % ("", "letters", "J", "expected",
                                            "P(0 J)", "p vs *3"))
    for label, body, _ in sorted(unbroken, key=lambda r: -len(r[1])):
        n, j, lam = len(body), body.count("J"), len(body) / 26.0
        # ONE-SIDED, and deliberately so: footnote *3 predicts a J DEFICIT, so
        # the evidence against Enigma is P(X <= observed).  A two-sided p would
        # flag a J-RICH message as "significant" when an excess of J is the
        # opposite of what *3 claims -- evidence FOR Enigma, not against.
        p = binom_cdf(j, n, 1.0 / 26.0)
        print("   %-12s %6d %5d %9.1f %8.1f%% %8.3f%s"
              % (label, n, j, lam, 100.0 * (25.0 / 26.0) ** n, p,
                 "   J-rich, so *3 is not in play" if j > lam else ""))

    print("\n   zero-J records in the VERIFIED corpus (the counter-examples):")
    for label, body, _ in sorted(verified, key=lambda r: -len(r[1])):
        if "J" not in body:
            print("   %-12s %6d %5d %9.1f %8.1f%%"
                  % (label, len(body), 0, len(body) / 26.0,
                     100.0 * (25.0 / 26.0) ** len(body)))


if __name__ == "__main__":
    main()
