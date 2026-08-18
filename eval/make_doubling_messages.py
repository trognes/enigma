#!/usr/bin/env python3
"""Synthesise telegraphic German messages carrying a doubling of a CHOSEN length.

    python3 eval/make_doubling_messages.py --length 7 --n 3

WHY THIS EXISTS.  The self-crib measurements on record vary the SEARCH
parameter `--self-crib-length` over a fixed, mixed population -- all 20 corpus
messages carrying a 4+ doubling, whatever their own length (48-214 letters).
That cannot answer "my message is 167 letters and its doubled word is 7 long;
is the seeder worth more than -R 32?", because the message property and the
message length both move with the sample.

Only ~20 authentic messages carry a doubling at all, unevenly spread by length,
so stratifying the real corpus leaves buckets too thin to resolve.  These are
synthetic instead: an authentic carrier with an authentic doubled word spliced
in, so the letter statistics stay telegraphic German while the doubling length
and the message length are both controlled exactly.

HOW ONE IS BUILT.
  * The carrier is a run of concatenated authentic HG Nord decrypts, drawn so
    that it carries NO doubling of 4+ letters of its own -- otherwise the
    seeder could latch onto an unintended one and the bucket label would lie.
  * The doubled word is a real X-delimited token of the requested length taken
    from the same corpus, so it is telegraphic vocabulary and not invented.
  * It is spliced as `...X WORD X WORD X...` at an existing X boundary, the
    same shape the authentic doublings have (96% carry a left X flank).
  * The carrier is then trimmed to land on exactly `--total` letters, so length
    is held fixed across buckets and only the doubling varies.

WHAT THIS CANNOT CONTROL, and it matters when reading a bucket-to-bucket
comparison: a longer doubling is more repeated text in the same 167 letters, so
it raises the index of coincidence and makes the message intrinsically easier
for EVERY method, not just the seeder.  A harness using these must therefore
report the restart arm's own break rate per bucket as well, or it will read a
bucket getting easier as the seeder getting better.
"""
import argparse
import collections
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crib_menu import corpus                                    # noqa: E402


def word_bank(texts, lo=4, hi=20):
    """Authentic X-delimited tokens, grouped by length."""
    by = collections.defaultdict(set)
    for t in texts:
        for w in t.split("X"):
            if w.isalpha() and lo <= len(w) <= hi:
                by[len(w)].add(w)
    return {k: sorted(v) for k, v in by.items()}


def longest_doubling(t, minlen=4, gaps=(0, 1)):
    """Longest L such that t holds W X W (gap 1) or W W (gap 0), W free of X."""
    best = 0
    for gap in gaps:
        for i in range(len(t)):
            for n in range(minlen, (len(t) - i) // 2 + 1):
                j = i + n + gap
                if j + n > len(t):
                    break
                if "X" in t[i:i + n]:
                    continue
                if gap == 1 and t[i + n] != "X":
                    continue
                if t[i:i + n] == t[j:j + n]:
                    best = max(best, n)
    return best


def make(rng, texts, bank, wlen, total, tries=200):
    """One message of exactly `total` letters whose longest doubling is `wlen`."""
    pool = "X".join(texts)
    words = bank.get(wlen)
    if not words:
        raise SystemExit("no authentic word of length %d in the corpus" % wlen)
    insert_len = 2 * wlen + 1            # WORD X WORD, spliced between two X
    if insert_len + 20 > total:
        raise SystemExit("length %d does not fit in %d letters" % (wlen, total))
    carrier_len = total - insert_len - 2     # the two X delimiters
    for _ in range(tries):
        start = rng.randrange(0, len(pool) - carrier_len)
        carrier = pool[start:start + carrier_len]
        # A carrier with its own doubling would mislabel the bucket.
        if longest_doubling(carrier, 4) > 0:
            continue
        word = rng.choice(words)
        cut = rng.randrange(len(carrier) // 4, 3 * len(carrier) // 4)
        msg = carrier[:cut] + "X" + word + "X" + word + "X" + carrier[cut:]
        if len(msg) != total:
            continue
        # The splice must be the ONLY doubling, and exactly the length asked.
        if longest_doubling(msg, 4) != wlen:
            continue
        return msg, word
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--length", type=int, required=True,
                    help="length of the doubled word")
    ap.add_argument("--total", type=int, default=167,
                    help="total message length [167]")
    ap.add_argument("--n", type=int, default=5)
    ap.add_argument("--seed", type=int, default=20260821)
    a = ap.parse_args()

    rng = random.Random(a.seed)
    texts = corpus()
    bank = word_bank(texts)
    for i in range(a.n):
        msg, word = make(rng, texts, bank, a.length, a.total)
        if msg is None:
            print("could not build one at length %d" % a.length)
            return 1
        print("%2d  len=%d  doubling=%s (%d)" % (i + 1, len(msg), word, len(word)))
        print("    %s" % msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
