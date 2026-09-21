#!/usr/bin/env python3
"""Generate the "hgnord" scoring language -- ngrams/hgnord_*.txt -- from the
stock wehrmacht tables and the n-gram COUNTS of the authentic HG Nord 1941
decrypts in eval/enigma-army-messages-1941.txt.  ENHANCEMENTS.md 2b.

    mixed[g] = stock[g] + W * stock_total * count[g] / count_total

per order, W = 1000: the stock table is 0.1% of the mass and the ~5 100
letters of decrypted traffic are the model, while every gram the corpus never
saw keeps its prose ORDER against the unseen-gram floor (worth +25 breaks of
400 over dropping the stock table, eval/results-corpus-only-table.txt).  The
weight curve is monotone to 1000 and flat to 10^5, so there is nothing to
tune (eval/results-counted-table.txt, eval/results-counted-table-knobs.txt).

Measured held out by message, five folds so no message scored against a
table it contributed to: +63 breaks of 400 at L=80 and -R 100 over the stock
table (156 -> 219, 55% of messages against 39%), and ahead at L = 60/100/167
too.  The shipped table is counted from EVERY clean decrypt, so it has seen
the whole corpus; that is why it is a separate language and not the
wehrmacht tables:

  * It is an IN-NETWORK prior.  What it encodes is HG Nord's vocabulary and
    habits -- XSIGX, XLKWX, the spelled numbers, the unit designations --
    and it says nothing about another network's traffic.  Use it on HG Nord
    1941 messages; use wehrmacht for other telegraphic German.
  * EVERY EVAL STAYS ON wehrmacht.  The harnesses under eval/ draw their
    trials from the same 62 messages, so a number measured with -l hgnord on
    them is contaminated by construction.  The only honest use on the corpus
    is the messages that were never read (they are not in it).

Which messages: every decrypt whose NOTES do not flag a garble, a partial or
a corrupt form, and whose DECRYPT carries no '-' (an unrecovered letter),
at any length -- the 48 of 62 that are clean, printed by name when the
script runs so the message set is stated.  Garbles included measured about
the same (223 against 219) but add wrong n-grams for nothing.

The loader clamps a count at 2^32 and log10(count / total) is scale
invariant for a seen gram, so each table is scaled to fit under 4e9 -- and
that scaling is what sets the floor: an unseen gram is priced at ONE count
against a total of ~1e10, eight decades under a hapax.  A table scaled to a
small total would put the floor one decade under and read no better than
the stock table (147 against 156).  A stock gram whose scaled count rounds
to zero is dropped and lands on that floor with the unseen ones -- about a
third of the quad table's rarest grams, exactly as in the tables the
measurements were made on (eval/counted_table_ab.py writes them the same
way), so what ships is what was measured.

    python3 eval/build_hgnord_ngrams.py            # writes ngrams/hgnord_*
    OUTDIR=/tmp/x python3 eval/build_hgnord_ngrams.py

The -f IC weight for this language is wehrmacht's 0.25*L rule
(src/scoring.cc); halving it measured a small gain at L=80 only and nothing
at 60/100/167, so the rule is kept.
"""
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
OUT = os.environ.get("OUTDIR", NGRAMS)
CORPUS = os.path.join(HERE, "enigma-army-messages-1941.txt")
W = float(os.environ.get("W", "1000"))
CAP = 4.0e9
ORD = {1: "monograms", 2: "bigrams", 3: "trigrams", 4: "quadgrams"}
FLAG = re.compile(r"garbl|corrupt|partial|poor|does not decrypt|heavily",
                  re.I)


def corpus():
    """(name, decrypt) for every clean message, in file order."""
    text = open(CORPUS, encoding="utf-8").read()
    out = []
    for block in re.split(r"(?=### Message No\.)", text)[1:]:
        m = re.search(r"DECRYPT:\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
        if not m:
            continue
        name = block.split("\n", 1)[0].lstrip("# ").strip()
        n = re.search(r"NOTES:\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
        notes = n.group(1) if n else ""
        if FLAG.search(notes) or "-" in m.group(1):
            continue
        s = "".join(m.group(1).split())
        if s and all("A" <= c <= "Z" for c in s):
            out.append((name, s))
    return out


def load(path):
    d = {}
    for line in open(path):
        p = line.split()
        if len(p) == 2 and p[1].isdigit():
            d[p[0]] = int(p[1])
    return d


def main():
    msgs = corpus()
    letters = sum(len(s) for _, s in msgs)
    print(f"{len(msgs)} clean messages, {letters} letters, W = {W:g}:")
    for name, s in msgs:
        print(f"  {name}  ({len(s)} letters)")
    os.makedirs(OUT, exist_ok=True)
    for n, name in ORD.items():
        stock = load(os.path.join(NGRAMS, f"wehrmacht_{name}.txt"))
        btot = sum(stock.values())
        c = Counter()
        for _, s in msgs:
            for i in range(len(s) - n + 1):
                c[s[i:i + n]] += 1
        ctot = sum(c.values())
        mixed = dict(stock)
        for g, k in c.items():
            mixed[g] = mixed.get(g, 0) + W * btot * k / ctot
        scale = min(1.0, CAP / max(mixed.values()))
        path = os.path.join(OUT, f"hgnord_{name}.txt")
        kept = 0
        with open(path, "w") as f:
            for g, k in sorted(mixed.items(), key=lambda kv: (-kv[1], kv[0])):
                k = int(round(k * scale))
                if k > 0:
                    f.write(f"{g} {k}\n")
                    kept += 1
        print(f"{path}: {kept} grams ({len(c)} seen in the corpus, "
              f"{ctot} counted), scale {scale:.3g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
