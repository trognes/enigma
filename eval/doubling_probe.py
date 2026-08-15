#!/usr/bin/env python3
"""How often is a word DOUBLED around an X, and how often does that happen by
chance?  The precondition for a vocabulary-free confirmation signal.

Telegraphic German repeats important words for error correction, separated by
the X word separator -- ZANDERS X ZANDERS, FORD X FORD, PKW X PKW.  Unlike the
known-word bonus (ENHANCEMENTS.md item 4, measured down), spotting one needs no
vocabulary at all: the test is that two runs are identical to each other, not
that either is a word anyone listed in advance.

This measures only the TEXT-LEVEL precondition -- it does no cracking.  Whether
the pattern survives a partly-wrong plugboard is the separate and harder
question, and the prior from item 4 is unfavourable (see that entry).

  ./eval/doubling_probe.py
"""
import random
import re
import sys

DB = "eval/enigma-army-messages-1941.txt"
NULL_TRIALS = 20000
MAXLEN = 12


def decrypts():
    """The recorded plaintexts, as raw letter streams.  Records carrying '-' or
    '[' are skipped: those mark unreceived letters and editorial repairs, and
    either would fabricate or destroy a repeat."""
    out = []
    for b in re.split(r"\n### ", open(DB, encoding="utf-8").read())[1:]:
        m = re.search(r"DECRYPT:\s+((?:.|\n)*?)\n[A-Z]", b)
        if not m:
            continue
        d = "".join(m.group(1).split())
        if "-" in d or "[" in d:
            continue
        out.append((b.split("\n")[0].split("(")[-1].rstrip(") "), d))
    return out


def repeats(t, k, maxmm=0):
    """Every  W X W'  in t with len(W) >= k and at most maxmm mismatches.
    Overlapping and nested hits are all returned; the caller dedups if it
    cares, since for a rate over MESSAGES only the first one matters."""
    out = []
    for i in range(len(t)):
        for L in range(k, MAXLEN + 1):
            if i + 2 * L + 1 > len(t):
                break
            w, v = t[i:i + L], t[i + L + 1:i + 2 * L + 1]
            if t[i + L] != "X" or "X" in w or "X" in v:
                continue
            mm = sum(1 for a, b in zip(w, v) if a != b)
            if mm <= maxmm:
                out.append((w, v))
    return out


def main():
    recs = decrypts()
    n = len(recs)
    print("%d messages with a clean recorded plaintext\n" % n)

    print("  exact repeats  W X W")
    print("  %-9s %-22s %s" % ("min len", "messages carrying one", "instances"))
    for k in (3, 4, 5, 6):
        hits = [(kenn, repeats(d, k)) for kenn, d in recs]
        nz = [x for x in hits if x[1]]
        print("  %-9d %2d of %d (%3.0f%%)%9s %d"
              % (k, len(nz), n, 100.0 * len(nz) / n, "",
                 sum(len(x[1]) for x in nz)))

    near = [(kenn, [p for p in repeats(d, 4, maxmm=1)
                    if p[0] != p[1]]) for kenn, d in recs]
    nznear = [x for x in near if x[1]]
    print("\n  with ONE mismatch allowed (len>=4), excluding exact matches:")
    print("  %2d of %d (%3.0f%%) -- transmission garbles and spelling variants"
          % (len(nznear), n, 100.0 * len(nznear) / n))
    for kenn, ps in nznear[:5]:
        print("     %s" % ", ".join("%s/%s" % p for p in ps[:3]))

    print("\n  what the exact hits ARE (the vocabulary-free argument):")
    seen = []
    for kenn, d in recs:
        for w, _ in repeats(d, 5):
            if w not in seen:
                seen.append(w)
    line = "    "
    for w in seen[:12]:
        if len(line) + len(w) + 2 > 78:
            print(line.rstrip(","))
            line = "    "
        line += " %s," % w
    print(line.rstrip(","))

    # The null.  Shuffling a real decrypt is the harder of the two, because it
    # preserves the letter frequencies -- including the high X rate, which is
    # what a repeat-with-X test could otherwise be fooled by.
    rng = random.Random(1)
    A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    print("\n  by chance (%d trials each):" % NULL_TRIALS)
    for k in (3, 4, 5):
        u = sum(bool(repeats("".join(rng.choice(A) for _ in range(120)), k))
                for _ in range(NULL_TRIALS))
        s = 0
        for _ in range(NULL_TRIALS):
            d = list(rng.choice(recs)[1])
            rng.shuffle(d)
            s += bool(repeats("".join(d), k))
        print("     len>=%d  uniform random %6.3f%%  shuffled real %6.3f%%"
              % (k, 100.0 * u / NULL_TRIALS, 100.0 * s / NULL_TRIALS))
    # Real traffic is garbled, so the exact test throws away real hits.
    # Allowing mismatches recovers them but must cost null rate -- sweep both
    # axes together rather than assuming the trade, because it turns out NOT to
    # be a trade at len>=6.
    print("\n  recall vs null, mismatch tolerance against minimum length:")
    print("  %-7s %-6s %-22s %s" % ("min len", "mm<=", "real messages", "null"))
    for k in (4, 5, 6, 7):
        for mm in (0, 1):
            s = 0
            for _ in range(NULL_TRIALS):
                d = list(rng.choice(recs)[1])
                rng.shuffle(d)
                s += bool(repeats("".join(d), k, maxmm=mm))
            r = [x for x in recs if repeats(x[1], k, maxmm=mm)]
            print("  %-7d %-6d %2d of %d (%3.0f%%)%9s %6.3f%%"
                  % (k, mm, len(r), n, 100.0 * len(r) / n, "",
                     100.0 * s / NULL_TRIALS))

    print("\n  A hit is near-conclusive: carried by ~a third to ~half of")
    print("  real messages and by ~1 in 20000 of the nulls.  len>=6 with ONE")
    print("  mismatch allowed DOMINATES len>=4 exact -- MORE real messages")
    print("  (42% vs 34%) at a null rate no higher -- because real traffic is")
    print("  garbled, so an exact test discards genuine hits.")
    print("  What is NOT measured here is whether any of it survives a")
    print("  partly-wrong plugboard, which is the question that decides it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
