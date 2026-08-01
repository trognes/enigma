#!/usr/bin/env python3
"""Build a crib library from the authentic message corpus (cribs.md section 5,
step 1 of its order of work), and report what it would cover.

    python3 eval/build_cribs.py                 # report only, nothing written
    python3 eval/build_cribs.py --out FILE      # also write the library
    python3 eval/build_cribs.py --budget-hours 6

A CRIB is a guess at a fragment of plaintext.  Given one that is exactly right,
part of the plugboard follows by arithmetic instead of search -- see cribs.md.
This program produces candidates; it does not use them.

WHAT IT EMITS

  observed  a phrase that really recurs in the corpus, found by harvesting
            substrings shared by two or more messages and keeping only the
            MAXIMAL ones (cribs.md step 4a: every shorter window inside a
            recurring phrase recurs too, and emitting all of them costs several
            times more for strictly less)
  doubled   a doubling variant of a word -- XSCHUSTERXSCHUSTERX and its four
            cousins.  Operators repeated important words for error correction,
            which is what makes these worth guessing at all: doubling makes a
            phrase long AND reuses every letter
  derived   a window of a phrase already emitted, held back to the last tier.
            Worthless when the parent matches, and the only thing that works
            when the parent is garbled or punctuated differently

THE ONE NUMBER THIS PROGRAM EXISTS TO PRODUCE is held-out coverage: build the
library WITHOUT a message, then ask whether it contains that message.  Coverage
measured on the messages the library was built from is not evidence -- an
observed phrase trivially "covers" the messages it was harvested from.  The
report gives both, and the gap between them is the point.

RANKING is by SPARE LETTERS (length minus distinct letters), not length, and
they are not the same ranking: NULLNULLNULL and UNITIONFUERL are both 12
letters and both deduce about 6.7 cables, but reject 81% and 0% of rotor
settings.  Spare letters predict whether the deduction closes on itself, which
is what makes a wrong rotor setting cheap to reject.
"""
import argparse
import os
import random
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
FILES = ["enigma-messages.txt", "enigma-army-messages-1941.txt"]
VOCAB = os.path.join(ROOT, "cribs", "german-hgnord.txt")

# Measured seconds to sweep a 200-letter message with one crib of each length
# (cribs.md section 4.1: rejection is what makes a long crib cheap, so the cost
# falls steeply with length and then flattens).  Used only to price a library.
COST = {8: 1499, 10: 977, 12: 648, 14: 336, 16: 178, 18: 122, 20: 117, 25: 116}

MIN_WORD = 4        # shorter pieces are not words, they are fragments
MIN_CRIB = 8        # cribs.md tiers stop here: below it a crib rejects nothing
MAX_CRIB = 40       # longer than this is one message's quirk, not a phrase


def crib_cost(n):
    """Seconds to try one crib of n letters against one message."""
    keys = sorted(COST)
    if n <= keys[0]:
        return COST[keys[0]]
    if n >= keys[-1]:
        return COST[keys[-1]]
    for a, b in zip(keys, keys[1:]):
        if a <= n <= b:
            t = (n - a) / (b - a)
            return COST[a] + t * (COST[b] - COST[a])
    return COST[keys[-1]]


def load_messages():
    """[(id, decrypt)] for every message with a recorded plaintext.

    The decrypt is the RAW machine output, not the human-emended reading: a
    crib has to match what the machine actually produces, garbles included.
    """
    out = []
    for fn in FILES:
        path = os.path.join(HERE, fn)
        txt = open(path, encoding="utf-8").read()
        for blk in re.split(r"^### Message", txt, flags=re.M)[1:]:
            mid = blk.split("\n", 1)[0].strip()
            mid = mid.split("(")[0].split("--")[0].strip()
            m = re.search(r"DECRYPT:\s+((?:.*\n)(?:             .*\n)*)", blk)
            if not m:
                continue
            pt = re.sub(r"[^A-Z-]", "", m.group(1))
            if pt:
                out.append((mid, pt))
    return out


def segments(text):
    """Runs of known letters.  A dash is an unrecorded-but-real letter, so a
    crib may not span one -- we do not know what is there."""
    return [s for s in text.split("-") if s]


def occurs(crib, text):
    return any(crib in s for s in segments(text))


def words(messages):
    """Distinct words: split each message on the separator X and keep the
    pieces long enough to be words."""
    seen = set()
    for _, pt in messages:
        for seg in segments(pt):
            for w in seg.split("X"):
                if len(w) >= MIN_WORD:
                    seen.add(w)
    return seen


def vocab_words():
    """Generic telegraphic vocabulary -- spelled-out numbers, the phonetic
    alphabet, standard military nouns.  Guessable without having seen the
    traffic, which is what makes it useful on a message unlike anything in the
    corpus."""
    out = set()
    if not os.path.exists(VOCAB):
        return out
    for line in open(VOCAB, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        w = line.split()[0].upper()
        if len(w) >= MIN_WORD and w.isalpha():
            out.add(w)
    return out


def doubling_variants(w):
    """The five ways a repeated word appears, differing only in where the
    operator put separators.  cribs.md section 4.5: this is exactly the
    variance that breaks exact matching, so all five are emitted."""
    return ["X" + w + "X" + w + "X", "X" + w + w + "X", w + "X" + w,
            w + w, "X" + w + "X"]


def repeated_phrases(messages):
    """Substrings occurring in two or more DISTINCT messages, keeping only the
    maximal ones.

    Maximal means: not contained in a longer kept phrase whose message support
    is a superset.  Such a window adds no coverage its parent does not already
    have -- it is the sub-window case in cribs.md step 4a, and it goes to the
    derived tier rather than being emitted beside its parent.
    """
    support = defaultdict(set)
    for mid, pt in messages:
        for seg in segments(pt):
            for n in range(MIN_CRIB, min(MAX_CRIB, len(seg)) + 1):
                for i in range(len(seg) - n + 1):
                    support[seg[i:i + n]].add(mid)
    shared = {p: s for p, s in support.items() if len(s) >= 2}

    kept, derived = {}, {}
    for p in sorted(shared, key=len, reverse=True):
        parent = None
        for q in kept:
            if len(q) > len(p) and p in q and shared[p] <= shared[q]:
                parent = q
                break
        if parent is None:
            kept[p] = shared[p]
        else:
            derived[p] = shared[p]
    return kept, derived


def spare(crib):
    """Length minus distinct letters: how much the crib reuses itself, which is
    what closes the deduction into a loop and lets a wrong rotor setting be
    rejected."""
    return len(crib) - len(set(crib))


def tier(crib, kind):
    """cribs.md step 5.  Tier is what the crib can DO, so it is set by length
    (16+ rejects rotor settings, 8-11 can only seed a climb) -- except that a
    derived window always goes last, however long it is."""
    if kind == "derived":
        return 5
    n = len(crib)
    if n >= 16:
        return 1
    if n >= 14:
        return 2
    if n >= 12:
        return 3
    return 4


def build_library(messages, use_vocab=True, order="spare"):
    """[(crib, tier, spare, kind, support)], ordered as it should be tried."""
    kept, derived = repeated_phrases(messages)
    entries = {}

    def add(crib, kind, support):
        if not (MIN_CRIB <= len(crib) <= MAX_CRIB):
            return
        # An observed phrase outranks the same string guessed by doubling, and
        # both outrank a derived window -- keep the strongest claim.
        rank = {"observed": 0, "doubled": 1, "derived": 2}
        old = entries.get(crib)
        if old is None or rank[kind] < rank[old[0]]:
            entries[crib] = (kind, support)

    for p, s in kept.items():
        add(p, "observed", s)
    src = words(messages) | (vocab_words() if use_vocab else set())
    for w in sorted(src):
        for v in doubling_variants(w):
            add(v, "doubled", set())
    for p, s in derived.items():
        add(p, "derived", s)

    lib = [(c, tier(c, k), spare(c), k, s) for c, (k, s) in entries.items()]
    # Two orderings, because they answer different questions and the curve in
    # the report is what decides between them:
    #   spare  most self-overlap first -- the crib that rejects the most rotor
    #          settings per second spent, ignoring how likely it is to match
    #   kind   phrases the corpus actually shows recurring before words merely
    #          guessed to have been doubled, THEN by spare
    # A crib that never matches costs its full sweep and returns nothing, so
    # ordering by likelihood of matching can beat ordering by strength.
    rank = {"observed": 0, "doubled": 1, "derived": 2}
    if order == "value":
        # Tier LAST, not first.  A tier says what a crib can do if it matches;
        # it says nothing about whether it will, and a crib that never matches
        # costs its whole sweep for nothing.  So order by evidence that the
        # phrase recurs (how many messages hold it), then by cost.
        lib.sort(key=lambda e: (rank[e[3]], -len(e[4]), crib_cost(len(e[0])),
                                -e[2], e[0]))
    elif order == "kind":
        lib.sort(key=lambda e: (e[1], rank[e[3]], -e[2], -len(e[0]), e[0]))
    else:
        lib.sort(key=lambda e: (e[1], -e[2], -len(e[0]), e[0]))
    return lib


def held_out_coverage(messages, use_vocab=True, order="spare"):
    """Leave-one-out: for each message, build the library from the OTHERS and
    ask whether it contains that message.  This is the honest measure -- a
    phrase harvested from a message covers that message by construction.

    Returns (id, length, hit, hours) where `hours` is the time spent getting to
    the first crib that matches, cribs being tried in file order.  That single
    number gives the whole coverage-vs-budget curve without rebuilding the
    library once per budget: a budget B covers exactly the messages whose hours
    are at most B.
    """
    rows = []
    for i, (mid, pt) in enumerate(messages):
        others = messages[:i] + messages[i + 1:]
        lib = build_library(others, use_vocab, order)
        hit, spent = None, 0.0
        for crib, tr, sp, kind, _ in lib:
            spent += crib_cost(len(crib)) / 3600.0
            if occurs(crib, pt):
                hit = (crib, tr, sp, kind)
                break
        rows.append((mid, len(pt), hit, spent if hit else None))
    return rows


def shuffled_control(lib, seed=0):
    """The same library with every crib's letters permuted.

    A crib must be EXACTLY right, so a short one is worth having only if it
    recurs because the traffic really repeats that phrase -- not because eight
    letters of German collide often.  Shuffling preserves each crib's length
    and letter multiset and destroys nothing else, so whatever coverage
    survives is the part that was never about the phrase.  Read the real
    coverage against this, not against zero.
    """
    rng = random.Random(seed)
    out = []
    for crib, t, sp, kind, sup in lib:
        letters = list(crib)
        rng.shuffle(letters)
        out.append(("".join(letters), t, sp, kind, sup))
    return out


def worst_case_hours(lib):
    """Time to try every crib against one message and find nothing.  A run
    stops at the first crib that works, so this is the bound, not the bill."""
    return sum(crib_cost(len(c)) for c, _, _, _, _ in lib) / 3600.0


def report(messages, lib, args):
    n = len(messages)
    print("Corpus: %d messages, %d letters, %d distinct words"
          % (n, sum(len(p) for _, p in messages), len(words(messages))))
    print("Vocabulary file: %d words" % len(vocab_words()))
    print()

    by_tier = defaultdict(list)
    for e in lib:
        by_tier[e[1]].append(e)
    print("Library: %d cribs" % len(lib))
    print("  %-6s %-7s %6s %8s %10s  %s"
          % ("tier", "length", "cribs", "spare", "worst-case", "example"))
    for t in sorted(by_tier):
        es = by_tier[t]
        lens = [len(e[0]) for e in es]
        sp = [e[2] for e in es]
        hrs = sum(crib_cost(l) for l in lens) / 3600.0
        print("  %-6d %-7s %6d %8.1f %9.1fh  %s"
              % (t, "%d-%d" % (min(lens), max(lens)), len(es),
                 sum(sp) / len(sp), hrs, es[0][0][:28]))
    print("  %-6s %-7s %6d %8s %9.1fh" % ("all", "", len(lib), "",
                                          worst_case_hours(lib)))
    print()

    # Coverage, both ways.  The in-corpus figure is the optimistic bound and is
    # printed only so the held-out figure can be read against it.
    incorpus = sum(1 for _, pt in messages
                   if any(occurs(c, pt) for c, _, _, _, _ in lib))
    rows = held_out_coverage(messages, not args.no_vocab, args.order)
    hits = [r for r in rows if r[2]]
    print("Coverage (crib order: %s)" % args.order)
    print("  in-corpus (optimistic): %d/%d = %.0f%%"
          % (incorpus, n, 100.0 * incorpus / n))
    print("  held out (leave-one-out): %d/%d = %.0f%%"
          % (len(hits), n, 100.0 * len(hits) / n))
    if hits:
        bt, bk = defaultdict(int), defaultdict(int)
        for _, _, h, _ in hits:
            bt[h[1]] += 1
            bk[h[3]] += 1
        print("  best tier reached, held out: "
              + ", ".join("t%d=%d" % (t, bt[t]) for t in sorted(bt)))
        print("  by kind: "
              + ", ".join("%s=%d" % (k, bk[k]) for k in sorted(bk)))
    print()

    # The curve that decides the budget.  A run stops at the first crib that
    # matches, so what a budget buys is not "how many cribs fit" but "how many
    # messages are reached before the budget runs out".  Read against the 24.9h
    # a no-crib run costs: a budget above that is only worth spending on the
    # messages it actually reaches.
    print("Coverage vs budget, held out (no-crib baseline: 24.9h)")
    print("  %8s %10s %8s" % ("budget", "covered", "of corpus"))
    for b in (0.5, 1, 2, 4, 8, 16, 25, 50, 100, 1e9):
        c = sum(1 for r in rows if r[3] is not None and r[3] <= b)
        lab = "all" if b > 1e8 else "%.1fh" % b
        print("  %8s %6d/%-3d %7.0f%%" % (lab, c, n, 100.0 * c / n))
    reached = sorted(r[3] for r in rows if r[3] is not None)
    if reached:
        print("  time to first hit, held out: median %.1fh, worst %.1fh"
              % (reached[len(reached) // 2], reached[-1]))
    print()

    # Falsification control.  Cheap, and the one result that would sink the
    # whole approach: if scrambled cribs cover nearly as much, the coverage
    # above is short strings colliding, not phrases recurring.
    ctrl = 0
    for i, (_, pt) in enumerate(messages):
        others = messages[:i] + messages[i + 1:]
        clib = shuffled_control(build_library(others, not args.no_vocab,
                                             args.order))
        if any(occurs(c, pt) for c, _, _, _, _ in clib):
            ctrl += 1
    print("Control: same cribs with their letters shuffled cover %d/%d = %.0f%%"
          % (ctrl, n, 100.0 * ctrl / n))
    print()

    if args.verbose:
        print("Per message, held out:")
        for mid, ln, h, hrs in rows:
            if h:
                print("  %-10s %4d  tier %d %-8s spare %2d  %6.1fh  %s"
                      % (mid, ln, h[1], h[3], h[2], hrs, h[0][:32]))
            else:
                print("  %-10s %4d  --" % (mid, ln))
        print()


def write_library(path, lib, messages):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Crib library generated by eval/build_cribs.py from %d "
                "authentic messages.\n" % len(messages))
        f.write("# Regenerate with: python3 eval/build_cribs.py --out %s\n"
                % os.path.relpath(path, ROOT))
        f.write("# One crib per line, tried in file order; '#' starts a "
                "comment.\n")
        f.write("# Columns after the crib are informational: tier, spare "
                "letters (length minus\n")
        f.write("# distinct letters -- what closes the deduction), how the "
                "crib was obtained, and\n")
        f.write("# how many corpus messages contain it.\n")
        f.write("#\n")
        f.write("# Tiers 1-3 can reject rotor settings; tier 4 is too short "
                "to reject anything\n")
        f.write("# and can only seed a plugboard climb; tier 5 holds windows "
                "of phrases listed\n")
        f.write("# elsewhere, a hedge against a garbled or differently "
                "punctuated parent.\n")
        f.write("#\n")
        f.write("# The order is NOT by tier: a tier says what a crib can do if "
                "it matches, and\n")
        f.write("# says nothing about whether it will. Cribs are ordered by "
                "evidence that the\n")
        f.write("# phrase recurs, then by cost, so the ones most likely to end "
                "the run come\n")
        f.write("# first (measured: median time to first hit 10h against 82h "
                "ordered by tier).\n\n")
        for crib, t, sp, kind, sup in lib:
            f.write("%-42s # t%d spare %2d %-8s seen %d\n"
                    % (crib, t, sp, kind, len(sup)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="write the library here "
                    "(default: report only)")
    ap.add_argument("--budget-hours", type=float,
                    help="truncate the written library to this worst-case "
                    "cost (the order decides what survives)")
    ap.add_argument("--order", choices=("spare", "kind", "value"),
                    default="value",
                    help="crib order: by tier then strength (spare), by tier "
                    "then kind (kind), or by evidence-of-recurrence and cost "
                    "with tier last (value, the default)")
    ap.add_argument("--no-vocab", action="store_true",
                    help="corpus words only, no generic vocabulary file")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="per-message held-out detail")
    args = ap.parse_args()

    messages = load_messages()
    if not messages:
        sys.exit("no messages found in %s" % ", ".join(FILES))
    lib = build_library(messages, not args.no_vocab, args.order)
    report(messages, lib, args)
    if args.out:
        out = lib
        if args.budget_hours:
            out, spent = [], 0.0
            for e in lib:
                c = crib_cost(len(e[0])) / 3600.0
                if spent + c > args.budget_hours:
                    break
                out.append(e)
                spent += c
        write_library(args.out, out, messages)
        print("wrote %s (%d of %d cribs, %.1fh worst case)"
              % (args.out, len(out), len(lib), worst_case_hours(out)))


if __name__ == "__main__":
    main()
