#!/usr/bin/env python3
"""Do known-word and X-segmentation features lift the true key's z?

Every number in the discussion that motivated this probe came from ONE message
(FTNBK, 101 letters), which turned out to be unusually X-rich -- 0.119 against a
corpus mean of 0.060 -- so its 6 sigma was a favourable case, not a typical one.
This runs the same measurement over every authentic message with a known key.

For each message: climb the true key and NSAMP wrong keys with the board hidden
on both arms (the attack scenario), then score every resulting decrypt on three
features and ask how far above its own wrong-key null the true key sits.

  quad  pure quadgram log10 P(text) -- the baseline the tool already has
  word  sum over distinct known words present of -log10 P(word by chance),
        so a long word counts and an accidental short one does not
  xrate fraction of letters that are X -- word separators in telegraphic German

The combination is the sum of the three features standardised by the WRONG-KEY
mean/sd, so correlation between them is handled empirically rather than assumed.
A break needs z > sqrt(2 ln K); for the sweep these messages would really need
(K = 160293120) that bar is 6.15.

  ./eval/word_segment_probe.py [nsamp] [restarts]

RESULT (46 messages, 48 wrong keys each, -R 32; full run in
eval/results-word-segment.txt):
NOT worth adding to the score.  The median gain of +1.64 z is flattering; split
by whether the message was already breakable on quadgrams alone, it inverts.

  already above the bar   n=25   median gain +7.75   helped 20 of 25
  BELOW the bar           n=21   median gain -0.33   helped  7 of 21

corr(quad z, gain) = +0.40: the features add the most z where it is already won
and nothing where it is needed.  Net effect on breakability is ONE message,
25 -> 26 of 46, with a flip in each direction (up FTNBK, FDTZP; down RDNAQ).

The mechanism is what kills it, not the size of the effect.  Among the 21
below-bar messages, 18 have a word-feature z of about -0.2 -- the "no words
found at all" floor.  The word bonus only fires once the climb has ALREADY
recovered readable plaintext; where the climb fails at the true key, the
true-key decrypt is as wordless as the wrong-key ones.  So this is not a
weighting problem: no reweighting extracts signal from a feature that is flat
across the null and the truth alike.  The regressions are the mirror -- comb is
an equal-weight sum of standardised features, so adding two near-noise terms to
a quad z of 36 inflates the composite's own sd and LOWERS z (outgoing -5.38,
DEROP -5.26, ABGUX -4.51).

FTNBK, the message that motivated the probe, is real but narrow: 0.90 -> 11.21,
because its climb DOES produce readable plaintext at the true key and the
pathology is specifically that quadgrams will not reward what it recovered.
Across the corpus that shape is one clean case plus FDTZP (already at 5.29).

WHY THE MEDIAN IS THE WRONG SUMMARY HERE, in general: the population is a
mixture of already-won and not-won messages, and a feature that scales with how
much plaintext is already recovered is necessarily strongest in the half where
it cannot change the outcome.  Report the split, not the median, for anything
whose value depends on the search having partly succeeded.

Caveat: 48 wrong keys gives the sd estimate ~10% error, so single-message z
values are +/-10%.  That is nowhere near enough to move the 21-message median
off -0.33, but do not read individual rows closely.
"""
import json
import math
import os
import random
import re
import subprocess
import sys

BIN = "./enigma"
DBS = ("eval/enigma-army-messages-1941.txt", "eval/enigma-messages.txt")
WORDS_FILE = "cribs/german-hgnord.txt"
QUADS = "ngrams/wehrmacht_quadgrams.txt"
RECIPE = ["-c", "-f", "-l", "wehrmacht", "-S", "i4f10", "-J", "--polish"]
NSAMP = int(sys.argv[1]) if len(sys.argv) > 1 else 48
RESTARTS = sys.argv[2] if len(sys.argv) > 2 else "32"
THREADS = os.environ.get("Z_THREADS", "4")
BAR = math.sqrt(2 * math.log(160293120))
ORDERS = [a + b + c for a in "12345" for b in "12345" for c in "12345"
          if len({a, b, c}) == 3]
A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

Q, QT = {}, 0
for line in open(QUADS, encoding="utf-8"):
    p = line.split()
    if len(p) == 2:
        Q[p[0]] = int(p[1])
        QT += int(p[1])
FLOOR = math.log10(1.0 / QT)
L26 = math.log10(26)
WORDS = [l.split()[0].upper() for l in open(WORDS_FILE, encoding="utf-8")
         if l.strip() and not l.startswith("#")
         and l.split()[0].isalpha() and len(l.split()[0]) >= 4]


def quad(t):
    return sum(math.log10(Q[t[i:i + 4]] / QT) if t[i:i + 4] in Q else FLOOR
               for i in range(len(t) - 3))


def word(t):
    n = len(t)
    return sum(max(0.0, len(w) * L26 - math.log10(n - len(w) + 1))
               for w in WORDS if w in t)


def xrate(t):
    return t.count("X") / len(t) if t else 0.0


def records():
    out = []
    for f in DBS:
        for b in re.split(r"\n### ", open(f, encoding="utf-8").read())[1:]:
            def g(k):
                m = re.search(k + r":\s+(.*)", b)
                return m.group(1).strip() if m else None
            ct = "".join(re.search(r"CIPHERTEXT:\s+((?:.|\n)*?)\n[A-Z]",
                                   b).group(1).split())
            if "-" in ct or g("WHEELS") is None or len(ct) < 60:
                continue
            out.append((b.split("\n")[0].split("(")[-1].rstrip(") "),
                        g("WHEELS").split("(-w ")[1].rstrip(")"),
                        g("RING"), g("START"), ct))
    return out


def climb(ct, w, r, gr):
    p = subprocess.run([BIN] + RECIPE + ["-u", "B", "-w", w, "-r", r, "-g", gr,
                                         "-R", RESTARTS, "-T", THREADS],
                       input=ct, capture_output=True, text=True,
                       env={**os.environ, "ENIGMA_SEED": "0"})
    return p.stdout.strip()


def zs(vals):
    """standardise by the WRONG-key mean/sd; vals[0] is the truth"""
    mu = sum(vals[1:]) / (len(vals) - 1)
    sd = (sum((x - mu) ** 2 for x in vals[1:]) / (len(vals) - 2)) ** 0.5
    return [(v - mu) / sd if sd > 0 else 0.0 for v in vals]


def main():
    rows = []
    for kenn, w, r, st, ct in records():
        rng = random.Random(7)
        texts = [climb(ct, w, r, st)]
        for _ in range(NSAMP):
            ww = rng.choice(ORDERS)
            while ww == w:
                ww = rng.choice(ORDERS)
            texts.append(climb(ct, ww, "A" + rng.choice(A) + rng.choice(A),
                               "".join(rng.choice(A) for _ in range(3))))
        texts = [t for t in texts if t]
        if len(texts) < 10:
            continue
        zq = zs([quad(t) for t in texts])
        zw = zs([word(t) for t in texts])
        zx = zs([xrate(t) for t in texts])
        comb = [zq[i] + zw[i] + zx[i] for i in range(len(texts))]
        zc = zs(comb)
        rows.append({"kenn": kenn, "len": len(ct), "z_quad": zq[0],
                     "z_word": zw[0], "z_xrate": zx[0], "z_comb": zc[0]})
        print("  %-6s %4d  quad %6.2f  word %6.2f  xrate %6.2f  comb %6.2f"
              % (kenn, len(ct), zq[0], zw[0], zx[0], zc[0]), flush=True)
    json.dump(rows, open("eval/results-word-segment.json", "w"), indent=1)
    summarise(rows)


def summarise(rows):
    """Print the summary from rows; re-runnable from the saved JSON, so the
    reading can be revisited without repeating hours of climbs:

      python3 -c "import json,sys; sys.path.insert(0,'eval'); \
        import word_segment_probe as w; \
        w.summarise(json.load(open('eval/results-word-segment.json')))"
    """
    n = len(rows)
    print("\n%d messages, %d wrong keys each, -R %s; bar for a 160M sweep %.2f"
          % (n, NSAMP, RESTARTS, BAR))
    print("  feature      median z   messages clearing %.2f" % BAR)
    for k in ("z_quad", "z_comb"):
        v = sorted(x[k] for x in rows)
        print("  %-11s %8.2f   %d of %d"
              % (k[2:], v[n // 2], sum(1 for x in v if x > BAR), n))
    gain = sorted(x["z_comb"] - x["z_quad"] for x in rows)
    print("  z gained by adding word+xrate: median %+.2f, quartiles %+.2f/%+.2f"
          % (gain[n // 2], gain[n // 4], gain[3 * n // 4]))
    flip = [x for x in rows if x["z_quad"] <= BAR < x["z_comb"]]
    print("  messages flipped from below to above the bar: %d (%s)"
          % (len(flip), ", ".join(x["kenn"] for x in flip) or "none"))
    drop = [x for x in rows if x["z_comb"] <= BAR < x["z_quad"]]
    print("  messages flipped from above to below the bar: %d (%s)"
          % (len(drop), ", ".join(x["kenn"] for x in drop) or "none"))

    # The median above is the WRONG summary and is why this is printed.  The
    # features scale with how much plaintext the climb already recovered, so
    # they are strongest exactly where they cannot change the outcome.  Split
    # by whether the message was already breakable on quadgrams alone.
    print("\n  gain split by whether quad alone already cleared the bar")
    print("  %-22s %4s %13s %9s" % ("", "n", "median gain", "helped"))
    groups = (("already above the bar",
               [x for x in rows if x["z_quad"] > BAR]),
              ("BELOW the bar",
               [x for x in rows if x["z_quad"] <= BAR]))
    for name, grp in groups:
        if not grp:
            continue
        v = sorted(x["z_comb"] - x["z_quad"] for x in grp)
        print("  %-22s %4d %+13.2f %6d of %d"
              % (name, len(v), v[len(v) // 2],
                 sum(1 for x in v if x > 0), len(v)))
    mx = sum(x["z_quad"] for x in rows) / n
    my = sum(x["z_comb"] - x["z_quad"] for x in rows) / n
    cov = sum((x["z_quad"] - mx) * (x["z_comb"] - x["z_quad"] - my)
              for x in rows)
    den = (sum((x["z_quad"] - mx) ** 2 for x in rows)
           * sum((x["z_comb"] - x["z_quad"] - my) ** 2 for x in rows)) ** 0.5
    print("  corr(quad z, gain) = %+.2f  (positive = adds z where already won)"
          % (cov / den if den > 0 else 0.0))
    dead = sum(1 for x in rows if x["z_quad"] <= BAR and x["z_word"] < 1.0)
    print("  below-bar messages whose word feature found nothing: %d of %d"
          % (dead, len([x for x in rows if x["z_quad"] <= BAR])))


if __name__ == "__main__":
    main()
