#!/usr/bin/env python3
"""Build a crib library from the authentic message corpus (archived/cribs.md section 5,
step 1 of its order of work), and report what it would cover.

    python3 eval/build_cribs.py                 # report only, nothing written
    python3 eval/build_cribs.py --out FILE      # also write the library
    python3 eval/build_cribs.py --budget-hours 6

A CRIB is a guess at a fragment of plaintext.  Given one that is exactly right,
part of the plugboard follows by arithmetic instead of search -- see archived/cribs.md.
This program produces candidates; it does not use them.

WHAT IT EMITS

  observed  a phrase that really recurs in the corpus, found by harvesting
            substrings shared by two or more messages and keeping only the
            MAXIMAL ones (archived/cribs.md step 4a: every shorter window inside a
            recurring phrase recurs too, and emitting all of them costs several
            times more for strictly less)
  vocab     a word from the generic telegraphic vocabulary, long enough to be
            a crib on its own (8 letters or more).  These are the only cribs
            here that owe nothing to this corpus -- ABENDMELDUNG and
            FELDLAZARETT are guessable from knowing the network, not from
            having read its traffic, so they are what would carry over to
            traffic this corpus does not resemble
  number    a spelled-out number -- EINSNULL, EINSEINSNULLNULL.  Off by
            default (--numbers).  Against a full library they earn nothing: a
            number common enough to guess already RECURS, so the harvester has
            it.  Against a thin one -- a network you have only started breaking
            -- they earn a few messages, which is the case they exist for
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

# Hours to sweep one message with one crib of each length, against the 24.9 h a
# no-crib run costs.  MEASURED (archived/cribs.md 4.2b and 12 step 6), not modelled: each
# length was timed on one message over one key space with -c, the ~0.12 s process
# startup subtracted, and the ratio to the same sweep without a crib taken.  The
# 16-letter figure agrees with an independent measurement on a 5x larger key
# space (0.074x here against 0.085x there), which is what makes the correction
# credible rather than a rescaling.
#
# THE SHAPE IS A CLIFF, NOT A SLOPE, and the previous table had neither the shape
# nor the magnitude: it read {8: 1499 s ... 25: 116 s}, a 13x spread declining
# smoothly, taken from 4.1's cost table.  That table charged CHECKING per
# surviving KEY, and 4.2b showed the unit is the surviving HYPOTHESIS -- under -c
# each one is climbed.  A crib too short to reject therefore multiplies the work
# rather than dividing it, which is why 8 letters costs 52x a no-crib run while
# 25 letters costs 0.02x.  The real spread is ~2600x.
#
# The consequence for a library is the opposite of what the old model implied:
# cribs of 14 letters and up are nearly free, so a budget should admit all of
# them, and it is the 8-11 letter band that has to be rationed.
COST_HOURS = {8: 1293.0, 10: 167.0, 12: 16.7, 14: 1.8,
              16: 1.8, 18: 1.2, 20: 0.92, 25: 0.5}
COST = {n: h * 3600.0 for n, h in COST_HOURS.items()}   # seconds, as before

MIN_WORD = 4        # shorter pieces are not words, they are fragments
MIN_CRIB = 8        # archived/cribs.md tiers stop here: below it a crib rejects nothing
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


# Wehrmacht telegraphic orthography spells digits out, with Q for ch: ZWO not
# ZWEI, AQT not ACHT, SEQS not SECHS.  Measured over the corpus, the Q forms are
# the only ones used (ZWO in 17 messages, ZWEI in 1; ACHT and SECHS in none).
DIGIT = {0: "NULL", 1: "EINS", 2: "ZWO", 3: "DREI", 4: "VIER",
         5: "FUENF", 6: "SEQS", 7: "SIEBEN", 8: "AQT", 9: "NEUN"}


def number_cribs():
    """Spelled-out numbers: every two-digit value, and clock times on the hour
    and half hour.

    Three- and four-digit numbers are deliberately absent.  They are
    individually excellent -- EINSEINSNULLNULL is 16 letters with 10 spare --
    but only 4 of the 10 000 four-digit strings occur anywhere in the corpus,
    so enumerating the family costs hundreds of hours to find one.  The cost
    runs backwards from the intuition: the SHORT numbers are the common ones,
    and 8-12 letters is the most expensive band to sweep.
    """
    out = set()
    for a in range(10):
        for b in range(10):
            out.add(DIGIT[a] + DIGIT[b])
    for h in range(24):
        for m in (0, 30):
            out.add(DIGIT[h // 10] + DIGIT[h % 10]
                    + DIGIT[m // 10] + DIGIT[m % 10])
    return {c for c in out if len(c) >= MIN_CRIB}


def doubling_variants(w):
    """The five ways a repeated word appears, differing only in where the
    operator put separators.  archived/cribs.md section 4.5: this is exactly the
    variance that breaks exact matching, so all five are emitted."""
    return ["X" + w + "X" + w + "X", "X" + w + w + "X", w + "X" + w,
            w + w, "X" + w + "X"]


def repeated_phrases(messages):
    """Substrings occurring in two or more DISTINCT messages, keeping only the
    maximal ones.

    Maximal means: not contained in a longer kept phrase whose message support
    is a superset.  Such a window adds no coverage its parent does not already
    have -- it is the sub-window case in archived/cribs.md step 4a, and it goes to the
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
    """archived/cribs.md step 5.  Tier is what the crib can DO, so it is set by length
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


def build_library(messages, use_vocab=True, order="spare",
                  vocab_cribs=True, numbers=False):
    """[(crib, tier, spare, kind, support)], ordered as it should be tried."""
    kept, derived = repeated_phrases(messages)
    entries = {}

    def add(crib, kind, support):
        if not (MIN_CRIB <= len(crib) <= MAX_CRIB):
            return
        # An observed phrase outranks the same string guessed by doubling, and
        # both outrank a derived window -- keep the strongest claim.
        rank = {"vocab": 0, "observed": 1, "number": 2, "doubled": 3,
                "derived": 4}
        old = entries.get(crib)
        if old is None or rank[kind] < rank[old[0]]:
            entries[crib] = (kind, support)

    for p, s in kept.items():
        add(p, "observed", s)
    vocab = vocab_words() if use_vocab else set()
    if vocab_cribs:
        # A vocabulary word of 8 letters or more is already a crib: no doubling
        # needed, and unlike a doubling variant it is not a guess on top of a
        # guess.  Its support is how many corpus messages hold it -- real
        # evidence, available at build time, and 0 for a word the corpus never
        # uses (which is exactly the word that might carry over elsewhere).
        for w in sorted(vocab):
            if len(w) >= MIN_CRIB:
                sup = {mid for mid, pt in messages if occurs(w, pt)}
                add(w, "vocab", sup)
    if numbers:
        # After the attested phrases and before the speculative doublings: a
        # number is a real form, but which number a message carries is anyone's
        # guess, so it ranks below a phrase the traffic is known to repeat.
        for c in sorted(number_cribs()):
            add(c, "number", {mid for mid, pt in messages if occurs(c, pt)})
    src = words(messages) | vocab
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
    # Vocabulary words go FIRST, ahead of even the best-attested observed
    # phrase.  Not because they are likelier to match -- they are not -- but
    # because there are only 19 of them, about five hours between them, and
    # they are the cribs that owe nothing to this corpus.  Measured: trying
    # them first covers 47 of 69 held-out messages within a 25-hour budget
    # against 42 when they follow the observed phrases, and halves the median
    # time to the first hit (6.0h against 10.1h) for the same total coverage.
    rank = {"vocab": 0, "observed": 1, "number": 2, "doubled": 3,
            "derived": 4}
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


def held_out_coverage(messages, use_vocab=True, order="spare",
                      vocab_cribs=True, numbers=False):
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
        lib = build_library(others, use_vocab, order, vocab_cribs, numbers)
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


def number_families():
    """The candidate number families, as they were compared.

    number_cribs() ships only the two that survived.  These are what it was
    chosen from, kept so --numbers-sweep can regenerate the comparison rather
    than the reader having to take archived/cribs.md section 5b on trust.
    """
    def n(t):
        return "".join(DIGIT[d] for d in t)
    fams = [
        ("clock times HH00", sorted({n((h // 10, h % 10, 0, 0))
                                     for h in range(24)})),
        ("clock times HH00/HH30",
         sorted({n((h // 10, h % 10, m // 10, m % 10))
                 for h in range(24) for m in (0, 30)})),
        ("two digits 00-31", sorted({n(divmod(v, 10)) for v in range(32)})),
        ("two digits 00-99", sorted({n((a, b)) for a in range(10)
                                     for b in range(10)})),
        ("three digits", sorted({n(t) for t in
                                 [(a, b, c) for a in range(10)
                                  for b in range(10) for c in range(10)]})),
        ("four digits", sorted({n(t) for t in
                                [(a, b, c, d) for a in range(10)
                                 for b in range(10) for c in range(10)
                                 for d in range(10)]})),
    ]
    return [(lab, [c for c in f if len(c) >= MIN_CRIB]) for lab, f in fams]


def numbers_sweep(messages, args):
    """Regenerate archived/cribs.md section 5b's number tables.

    Three questions, because they have different answers and only the third
    decides anything:

      in-corpus   does the family occur in this traffic at all
      held out    does it reach a message the library misses -- the honest
                  within-corpus test, and the one that reads zero, because a
                  number common enough to be worth guessing already RECURS,
                  which is exactly what the harvester detects
      transfer    does it reach a message the library misses when the library
                  was built from OTHER traffic -- the case the family exists
                  for, and the only one where it pays
    """
    n = len(messages)
    rows = held_out_coverage(messages, not args.no_vocab, args.order,
                             not args.no_vocab_cribs, False)
    uncovered = [(mid, pt) for (mid, pt), r in zip(messages, rows)
                 if r[2] is None]
    covered = [(mid, pt) for (mid, pt), r in zip(messages, rows)
               if r[2] is not None]
    cur = {mid: r[3] for (mid, _), r in zip(messages, rows)}
    base = sorted(h for h in cur.values() if h is not None)
    base_25 = sum(1 for h in base if h <= 25)

    # Transfer split: the two published collections (see transfer_report).
    half = []
    for fn in FILES:
        txt = open(os.path.join(HERE, fn), encoding="utf-8").read()
        msgs = []
        for blk in re.split(r"^### Message", txt, flags=re.M)[1:]:
            m = re.search(r"DECRYPT:\s+((?:.*\n)(?:             .*\n)*)", blk)
            if m:
                pt = re.sub(r"[^A-Z-]", "", m.group(1))
                if pt:
                    msgs.append((blk.split("\n", 1)[0].strip(), pt))
        half.append(msgs)
    tlib = [e[0] for e in build_library(half[0], not args.no_vocab, args.order,
                                        not args.no_vocab_cribs, False)]
    tmiss = [pt for _, pt in half[1] if not any(occurs(c, pt) for c in tlib)]

    print("Number families (archived/cribs.md §5b). Baseline library: %d of %d messages "
          "held out,\n%d within 25h, median %.1fh.\n"
          % (len(covered), n, base_25, base[len(base) // 2]))
    print("  %-22s %6s %7s %9s %8s %8s %9s"
          % ("family", "cribs", "cost", "in-corpus", "held-out", "<=25h",
             "transfer"))
    print("  %-22s %6s %7s %9s %8s %8s %9s"
          % ("", "", "", "covered", "rescues", "if first", "rescues"))
    for lab, fam in number_families():
        cost = sum(crib_cost(len(c)) for c in fam) / 3600.0
        incorp = sum(1 for _, pt in messages
                     if any(occurs(c, pt) for c in fam))
        resc = sum(1 for _, pt in uncovered
                   if any(occurs(c, pt) for c in fam))
        tres = sum(1 for pt in tmiss if any(occurs(c, pt) for c in fam))
        # Family tried FIRST: a message it holds is reached after the part of
        # the family up to that crib; every other message pays the whole family
        # on top of what it cost before.
        new = []
        for mid, pt in covered:
            t = 0.0
            for c in fam:
                t += crib_cost(len(c)) / 3600.0
                if occurs(c, pt):
                    break
            else:
                t = cur[mid] + cost
            new.append(min(t, cur[mid] + cost))
        print("  %-22s %6d %6.1fh %6d/%-2d %8d %5d/%-2d %9d"
              % (lab, len(fam), cost, incorp, n, resc,
                 sum(1 for h in new if h <= 25), n, tres))
    print()
    print("  in-corpus  the family occurs in this many messages")
    print("  held-out   messages the library misses that the family reaches")
    print("  <=25h      messages reached within a 25h budget with the family")
    print("             tried first (baseline %d/%d)" % (base_25, n))
    print("  transfer   messages the library misses that the family reaches,")
    print("             library built from the OTHER collection (%d missed)"
          % len(tmiss))
    print()


def collections():
    """The corpus as its two published collections, kept apart."""
    out = []
    for fn in FILES:
        txt = open(os.path.join(HERE, fn), encoding="utf-8").read()
        msgs = []
        for blk in re.split(r"^### Message", txt, flags=re.M)[1:]:
            mid = blk.split("\n", 1)[0].strip()
            mid = mid.split("(")[0].split("--")[0].strip()
            m = re.search(r"DECRYPT:\s+((?:.*\n)(?:             .*\n)*)", blk)
            if m:
                pt = re.sub(r"[^A-Z-]", "", m.group(1))
                if pt:
                    msgs.append((mid, pt))
        out.append((fn, msgs))
    return out


def first_hits(lib, test):
    """(messages covered, hits by kind) walking lib in order for each."""
    kinds, cov = defaultdict(int), 0
    for _, pt in test:
        for crib, _t, _sp, kind, _s in lib:
            if occurs(crib, pt):
                cov += 1
                kinds[kind] += 1
                break
    return cov, kinds


def transfer_report(args):
    """Does a library built on one body of traffic work on another?

    The obvious test -- train on one published collection, test on the other --
    ANSWERS THE WRONG QUESTION ON ITS OWN, and that is the point of this
    report.  The two collections differ in size (13 and 56 messages), and a
    harvester that keeps phrases recurring in two or more messages finds far
    fewer of them in 13 than in 56.  Run the cross-collection test alone and
    the training-set size shows up looking exactly like a transfer loss.

    So each cross-collection row is printed beside a SAME-collection control at
    the same training size: leave-one-out within the larger collection (matched
    to the 56-message direction), and a subsample of 13 of its messages tested
    on the rest (matched to the 13-message direction).  Whatever differs
    between a row and its control is transfer; whatever they share is size.
    """
    def build(tr):
        return build_library(tr, not args.no_vocab, args.order,
                             not args.no_vocab_cribs, args.numbers)

    def line(label, cov, n, kinds):
        print("  %-42s %2d/%-2d = %3.0f%%  %s"
              % (label, cov, n, 100.0 * cov / n,
                 ", ".join("%s=%d" % (k, kinds[k]) for k in sorted(kinds))))

    (an, a), (bn, b) = collections()
    if len(a) > len(b):
        (an, a), (bn, b) = (bn, b), (an, a)      # a = small, b = large
    print("Transfer, each row against a same-collection control of the same")
    print("training size (%s = %d msgs, %s = %d msgs)\n"
          % (an, len(a), bn, len(b)))

    print("Training on ~%d messages:" % (len(b) - 1))
    cov, kinds = first_hits(build(b), a)
    line("cross: train large, test small", cov, len(a), kinds)
    cov, kinds, tot = 0, defaultdict(int), 0
    for i, m in enumerate(b):
        c, k = first_hits(build(b[:i] + b[i + 1:]), [m])
        cov += c
        tot += 1
        for kk, v in k.items():
            kinds[kk] += v
    line("control: leave-one-out inside large", cov, tot, kinds)
    print()

    print("Training on %d messages:" % len(a))
    cov, kinds = first_hits(build(a), b)
    line("cross: train small, test large", cov, len(b), kinds)
    for seed in range(args.control_seeds):
        rng = random.Random(seed)
        idx = set(rng.sample(range(len(b)), len(a)))
        tr = [m for i, m in enumerate(b) if i in idx]
        te = [m for i, m in enumerate(b) if i not in idx]
        cov, kinds = first_hits(build(tr), te)
        line("control: %d of large, test the rest (seed %d)"
             % (len(a), seed), cov, len(te), kinds)
    print()


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
    rows = held_out_coverage(messages, not args.no_vocab, args.order,
                             not args.no_vocab_cribs, args.numbers)
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
                                             args.order,
                                             not args.no_vocab_cribs,
                                             args.numbers))
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
    ap.add_argument("--numbers", action="store_true",
                    help="also emit spelled-out numbers (two-digit values and "
                    "clock times); worth it only on traffic this corpus does "
                    "not cover")
    ap.add_argument("--numbers-sweep", action="store_true",
                    help="regenerate archived/cribs.md §5b's comparison of the number "
                    "families, in-corpus, held out and across collections")
    ap.add_argument("--transfer", action="store_true",
                    help="report cross-collection transfer against a "
                    "same-collection control at the same training size")
    ap.add_argument("--control-seeds", type=int, default=5,
                    help="subsample controls to run for --transfer [5]")
    ap.add_argument("--no-vocab-cribs", action="store_true",
                    help="do not emit long vocabulary words as cribs in "
                    "their own right (they still seed doubling variants)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="per-message held-out detail")
    args = ap.parse_args()

    messages = load_messages()
    if not messages:
        sys.exit("no messages found in %s" % ", ".join(FILES))
    if args.transfer:
        transfer_report(args)
        return
    if args.numbers_sweep:
        numbers_sweep(messages, args)
        return
    lib = build_library(messages, not args.no_vocab, args.order,
                        not args.no_vocab_cribs, args.numbers)
    report(messages, lib, args)
    if args.out:
        out = lib
        if args.budget_hours:
            # Fill the budget CHEAPEST FIRST, then restore the library's own
            # order for writing.  Two corrections in one, both forced by the
            # measured cost cliff (see COST_HOURS above).
            #
            # Walking in library order and stopping at the first unaffordable
            # crib -- what this used to do -- truncated the library to nothing,
            # because the library is ordered by evidence of recurrence and so
            # opens with short vocabulary cribs costing hundreds of hours each.
            # Merely skipping those instead still spends most of the budget on
            # the few short cribs that happen to fit, and admits ~29 cribs.
            #
            # Cheapest-first admits every long crib before any short one, which
            # is what the cost curve says to do: a 25-letter crib costs 0.02x a
            # no-crib sweep against 52x for an 8-letter one, so the entire long
            # tail fits in the space of a single short crib.  The corpus supplies
            # ~1150 cribs of 14 letters or more, and they were being thrown away.
            #
            # The FILE order is left alone -- it stays by evidence of recurrence,
            # which is what the tool's --no-crib-reorder preserves.  Selection and
            # ordering are separate questions and only selection is at stake here.
            priced = [(crib_cost(len(e[0])) / 3600.0, i, e)
                      for i, e in enumerate(lib)]
            keep, spent = set(), 0.0
            for c, i, _e in sorted(priced, key=lambda x: (x[0], x[1])):
                if spent + c > args.budget_hours:
                    continue
                keep.add(i)
                spent += c
            out = [e for i, e in enumerate(lib) if i in keep]
        write_library(args.out, out, messages)
        print("wrote %s (%d of %d cribs, %.1fh worst case)"
              % (args.out, len(out), len(lib), worst_case_hours(out)))


if __name__ == "__main__":
    main()
