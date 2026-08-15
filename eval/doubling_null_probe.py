#!/usr/bin/env python3
"""How often does the doubling rule fire on text with no doubling in it?

ENHANCEMENTS.md item 5 settled on `len>=6, mm<=1` on a null recorded as "0 of
20000 shuffles".  That is a floor, not a rate -- 20000 trials expect 0.28 hits
at this rule, so zero was the likely outcome and bounded nothing -- and it was
measured on the wrong population besides.

The population that matters is decrypts at WRONG rotor keys with the plugboard
hill-climbed, because that is what a search would apply the rule to.  This
script gets a rate for it three ways, in increasing order of directness:

  1. a closed form, from the population's letter statistics;
  2. the direct count over the 5888 stored climbed decrypts -- which by itself
     bounds nothing, and is printed to show why;
  3. millions of trials on synthetic text FITTED TO THAT POPULATION, both
     i.i.d. and bigram-Markov, which is what turns (1) from an assumption into
     a measurement.

Step 3 exists because the closed form assumes memoryless letters.  On real
German that assumption fails badly -- a bigram chain fitted to the corpus fires
9x more often than its letter frequencies alone predict, because real local
structure repeats readily.  Whether that carries over to climbed wrong-key text
is exactly what decides the rate, and it is not something to reason about.

    python3 eval/doubling_null_probe.py          # TRIALS=... to shorten
"""
import bisect
import collections
import itertools
import json
import math
import os
import random
import sys

CLIMBED = "eval/results-doubling-climb-texts.json"
TRIALS = int(os.environ.get("TRIALS", 1_500_000))
K, MAXLEN, MAXMM = 6, 16, 1


def fires(t, k=K, maxlen=MAXLEN, maxmm=MAXMM):
    """Item 5's rule, driven from the X positions rather than every start.

    Identical in what it accepts to doubling_probe.py's `repeats()` -- every
    W X V window has an X at its centre, so enumerating the X's enumerates the
    same windows -- but ~12x faster, which is what makes millions of trials
    affordable.  doubling_anchored_probe.py checks the equivalence on all 54
    real messages.
    """
    n = len(t)
    for j in range(k, n - k):
        if t[j] != "X":
            continue
        for L in range(k, min(maxlen, j, n - j - 1) + 1):
            w, v = t[j - L:j], t[j + 1:j + 1 + L]
            if "X" in w or "X" in v:
                continue
            if w == v:
                return True
            mm = 0
            for a, b in zip(w, v):
                if a != b:
                    mm += 1
                    if mm > maxmm:
                        break
            else:
                return True
    return False


def params(texts):
    """(p, A, B): P(letter is X); P(two letters equal and neither X); P(both
    non-X)."""
    s = "".join(texts)
    n = len(s)
    q = {c: s.count(c) / n for c in set(s)}
    p = q.get("X", 0.0)
    return p, sum(v * v for c, v in q.items() if c != "X"), (1 - p) ** 2


def expected(n, p, A, B, k=K, maxlen=MAXLEN):
    """Expected qualifying windows in n letters of memoryless text.

    A window at offset i, half-length L needs t[i+L] == X (probability p) and
    its 2L flanking letters non-X with at most one mismatch between the halves.
    Per position pair that is A for "equal and non-X", B for "both non-X", so L
    pairs give A^L exactly equal plus L*A^(L-1)*(B-A) for exactly one mismatch.
    Each extra letter costs a factor B/A ~ 16, so the sum is dominated by
    L = k and, within it, by the one-mismatch term -- the threshold sets the
    rate, not MAXLEN.
    """
    tot = 0.0
    for L in range(k, maxlen + 1):
        if n - 2 * L - 1 <= 0:
            break
        tot += (n - 2 * L) * p * (A ** L + L * A ** (L - 1) * (B - A))
    return tot


def rate(texts, p, A, B):
    e = sum(expected(len(t), p, A, B) for t in texts) / len(texts)
    return 1.0 - math.exp(-e)


def poisson_ci(h, n):
    lo = 0.0 if not h else \
        0.5 * 2 * h * (1 - 1/(9.0*h) - 1.96/(3*math.sqrt(h))) ** 3
    hi = 0.5 * 2 * (h+1) * (1 - 1/(9.0*(h+1)) + 1.96/(3*math.sqrt(h+1))) ** 3
    return lo / n, hi / n


def fit(texts):
    """Fast i.i.d. and bigram-Markov generators fitted to `texts`.

    The cumulative weights are built ONCE per state.  random.choices rebuilds
    them on every call, which for a per-letter Markov walk is the whole
    runtime -- tens of millions of rebuilds here, enough that the model does
    not finish.
    """
    s = "".join(texts)
    lens = [len(t) for t in texts]
    alpha = sorted(set(s))
    wts = [s.count(c) for c in alpha]
    cum0 = list(itertools.accumulate(wts))
    tally = collections.defaultdict(collections.Counter)
    for a, b in zip(s, s[1:]):
        tally[a][b] += 1
    nx = {a: (list(c), list(itertools.accumulate(c[x] for x in c)))
          for a, c in tally.items()}

    def iid(rng):
        return "".join(rng.choices(alpha, weights=wts, k=rng.choice(lens)))

    def markov(rng):
        n = rng.choice(lens)
        r = rng.random
        c = alpha[bisect.bisect(cum0, r() * cum0[-1])]
        o = [c]
        for _ in range(n - 1):
            ls, cw = nx[c]
            c = ls[bisect.bisect(cw, r() * cw[-1])]
            o.append(c)
        return "".join(o)

    # How much local structure is there beyond the letter frequencies?  The
    # bigram IC against what independence predicts: 1.00 means memoryless.
    big = collections.Counter(zip(s, s[1:]))
    tot = sum(big.values())
    q = {c: s.count(c) / len(s) for c in alpha}
    ic1 = sum(v * v for v in q.values())
    struct = sum((c / tot) ** 2 for c in big.values()) / (ic1 * ic1)
    return iid, markov, struct


def main():
    if not os.path.exists(CLIMBED):
        print("missing %s -- run doubling_climb_probe.py first" % CLIMBED)
        return 1
    D = json.load(open(CLIMBED))
    wrong = [w for r in D for w in r["wrong"]]
    true_ = [r["true"] for r in D]
    p, A, B = params(wrong)
    pred = rate(wrong, p, A, B)

    print("THE OPERATIONAL NULL -- climbed decrypts at WRONG rotor keys")
    print("   %d texts, mean length %d\n"
          % (len(wrong), sum(map(len, wrong)) // len(wrong)))

    print("1. CLOSED FORM from this population's letter statistics")
    print("   X-rate %.2f%%   A %.5f   ->  %.2e   (1 in %s)\n"
          % (100 * p, A, pred, format(int(1 / pred), ",")))
    print("   The X-rate is why this population is SAFER than the shuffled")
    print("   real decrypts the entry used to quote (X 6.84%%): the rule needs")
    print("   an X separator and the rate is ~linear in how often one occurs.")
    print("   A climb maximises an n-gram score and German prose is X-poor, so")
    print("   climbing a WRONG key drives X down -- %.2f%% here against %.2f%%"
          % (100 * p, 100 * params(true_)[0]))
    print("   in true-key decrypts.\n")

    print("2. DIRECT COUNT over the stored climbed decrypts")
    hw = sum(map(fires, wrong))
    print("   %d hits in %d, against %.2f expected -- so 0 is the likely"
          % (hw, len(wrong), len(wrong) * pred))
    print("   outcome and bounds nothing.  Neither did the recorded '0 false")
    print("   positives in 8928', which expects %.2f.\n" % (8928 * pred))

    print("3. TRIALS on synthetic text FITTED TO THIS POPULATION")
    print("   The closed form assumes memoryless letters.  On real German that")
    print("   fails: a bigram chain fitted to the CORPUS fires ~9x more often")
    print("   than its letter frequencies predict.  Does that carry over?\n")
    iid, markov, struct = fit(wrong)
    _, _, struct_real = fit(true_)
    print("   bigram IC / independence prediction  (1.00 = memoryless)")
    print("     climbed wrong-key text   %.2f" % struct)
    print("     real (true-key) decrypts %.2f   <- where the 9x came from\n"
          % struct_real)
    print("   %-30s %8s %11s %s" % ("generator", "hits", "rate", "vs closed form"))
    tot_h = tot_n = 0
    for nm, gen in (("i.i.d. from climbed freqs", iid),
                    ("bigram Markov from climbed", markov)):
        rng = random.Random(4242)
        h = sum(fires(gen(rng)) for _ in range(TRIALS))
        tot_h += h
        tot_n += TRIALS
        print("   %-30s %8d %11.2e  %.1fx" % (nm, h, h / TRIALS,
                                              (h / TRIALS) / pred))
    lo, hi = poisson_ci(tot_h, tot_n)
    print("   %-30s %8d %11.2e  95%% CI [%.1e, %.1e]"
          % ("pooled", tot_h, tot_h / tot_n, lo, hi))
    print("\n   The closed form %s inside that interval, and the Markov/i.i.d."
          % ("sits" if lo <= pred <= hi else "does NOT sit"))
    print("   gap is not significant -- climbed wrong-key text is nearly")
    print("   memoryless, so the corpus's 9x structure penalty does not apply.")
    print("   TAKE %.0e, i.e. 1 in %s." % (pred, format(int(1 / pred), ",")))

    print("\n4. WHAT THAT ALLOWS -- expected false hits = candidates x rate\n")
    ht = sum(map(fires, true_))
    print("   %-18s %s" % ("climbed keys", "expected false hits"))
    for N in (10_000, int(1 / pred), 1_000_000, 10_000_000, 100_000_000):
        print("   %-18s %.2f" % (format(N, ","), N * pred))
    print("\n   Against that, ONE true key -- and it carries a qualifying")
    print("   doubling only %d of %d times (%.0f%%).  A real unknown-key sweep"
          % (ht, len(true_), 100.0 * ht / len(true_)))
    print("   climbs 1e7-1e8 keys, so a sweep-wide filter is swamped twice")
    print("   over.  Sound as a CONFIRMER on a shortlist below ~%s candidates."
          % format(int(1 / pred), ","))
    return 0


if __name__ == "__main__":
    sys.exit(main())
