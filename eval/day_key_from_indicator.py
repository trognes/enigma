#!/usr/bin/env python3
"""Recover the TRUE day-key ring from an indicator plus any key in its class.

THE PROBLEM.  A key recovered from ciphertext alone does not pin the ring.
The left wheel's ring is never identifiable (CLAUDE.md 7.10: only start-minus-
ring reaches the machine), the middle wheel's often is not either on a short
message (7.12), and a two-notch right wheel adds a shift-of-13 equivalence
(the same section's table).  Every member of that class decrypts the message
BYTE-IDENTICALLY, so the ciphertext cannot choose between them -- which is why
this repo records e.g. `ALZ` for 09.09.1941 and warns that it is a class
representative, not necessarily what was printed on the key sheet.

THE LEVER.  The indicator escapes the equivalence.  `SDG EKN` means the
operator set his rotors to the ABSOLUTE position SDG and enciphered his message
key, getting EKN.  Shifting ring and start together leaves the message decrypt
alone precisely because both move; the Grundstellung does NOT move with them,
so only the true ring reproduces the message key from the indicator.  One
3-letter test, ~1/17576 of matching by chance.

So: enumerate the class, keep the members whose indicator decrypts to their own
start, and intersect over every message sharing the day key.

VALIDATED, and that is what licenses believing it on an unbroken message.  Over
the 55 corpus messages carrying an indicator (eval/results-indicator-ring.txt):

    43  unique ring, equal to the recorded one
     2  unique ring, DIFFERENT from the recorded one
     4  two candidates, the true ring among them
     6  no candidate

and all nine multi-message day keys agree on exactly one ring.  The two
"different" results are the pay-off case: both are days whose ring this repo
recovered from ciphertext and recorded as an explicit class representative,
and the method returns what Frode Weierud's key page independently publishes
-- 28.08.1941 -> CWJ (recorded AVJ) and 09.09.1941 -> KFZ (recorded ALZ).
NEITHER IS A NEW FACT.  Both are checks on the method, and they are the two
checks that matter, because they are the only cases where the recorded ring
was not already the true one.

The six no-candidate messages are misread indicators, not method failures:
four of them sit on PUBLISHED day keys, where the ring is certainly right and
all 26 left-wheel shifts were tested, so nothing but the indicator can be
wrong.  At ~11% of a corpus transcribed from handwritten 1941 forms that is
unremarkable -- and it is why a day key wants more than one message.

--suggest-fix then asks, of each indicator no ring satisfies, whether ONE
misread letter would explain it.  On the corpus it names the letter for two of
the six -- `IPG PHA` -> `IPG BHA` (P->B) for Nr 161 and `QCV MLN` -> `QCV MZN`
(L->Z) for Nr 197 -- and reports nothing usable for the rest.  See suggest_fix()
for why a hit is evidence on a published ring and is not on a representative
one.  IT SUGGESTS ONLY; nothing here edits the corpus, whose indicator fields
record what the forms say.

  ./eval/day_key_from_indicator.py --corpus
  ./eval/day_key_from_indicator.py --corpus --suggest-fix
  ./eval/day_key_from_indicator.py --date 09.09.1941
  ./eval/day_key_from_indicator.py --wheels 342 --ring ALZ \
      --plugs "AZ DV ET FS GQ JP LX MY NR OW" \
      --msg VAT:KFZ:JPO --msg UXT:BOZ:IWD
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import enigma_ref

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "enigma-army-messages-1941.txt")

# Wheels VI, VII and VIII notch at M and Z -- 13 apart, so a shift of 13 is an
# exact decode equivalence for them in the middle or right position.  Only the
# RIGHT position needs handling here: a middle-wheel shift of any size is
# already covered by the ring1 sweep below.
TWO_NOTCH = {"6", "7", "8"}


def shift(letter, k):
    return chr(65 + (ord(letter) - 65 + k) % 26)


def class_members(ring, start, wheels):
    """Every (ring, start) that MIGHT decrypt identically to the given one.

    A superset: the ring0 axis is an exact equivalence, the ring1 axis is only
    one on messages too short to reach the notch, and ring2 only for a
    two-notch right wheel.  Callers holding a ciphertext should filter with
    `same_decrypt` -- the indicator test does not need the filter to be
    correct, but a smaller candidate set is a sharper test.
    """
    out = []
    r2_shifts = (0, 13) if wheels[2] in TWO_NOTCH else (0,)
    for k in range(26):            # left wheel: exact, unconditional
        for j in range(26):        # middle wheel: exact only on short messages
            for h in r2_shifts:    # right wheel: exact for VI/VII/VIII
                out.append(
                    (shift(ring[0], k) + shift(ring[1], j)
                     + shift(ring[2], h),
                     shift(start[0], k) + shift(start[1], j)
                     + shift(start[2], h)))
    return out


def same_decrypt(cands, ct, pt, wheels, plugs, refl="B"):
    """Keep only the members that really do reproduce the known plaintext."""
    keep = []
    for r, s in cands:
        if enigma_ref.decrypt(ct, wheels, r, s, plugs, refl) == pt:
            keep.append((r, s))
    return keep


def indicator_ok(ring, start, grund, enc, wheels, plugs, refl="B"):
    """Does the indicator, deciphered at this ring, give this start?"""
    return enigma_ref.decrypt(enc, wheels, ring, grund, plugs, refl) == start


ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def suggest_fix(ring, start, grund, enc, wheels, plugs, refl="B",
                trusted_ring=True, cands=None):
    """Single-letter corrections to an indicator that no ring satisfies.

    Six letters x 25 substitutions = 150 variants. Whether that is a diagnosis
    or a coin toss depends entirely on WHAT THE VARIANT HAS TO HIT, and the two
    cases are far apart:

    TRUSTED RING (the day key is published, so the recorded ring and start are
    the true ones). A variant must reproduce THAT start at THAT ring -- one
    test each, so 150 tests at 1/17576, an expected 0.0085 false positives.
    A hit here is a diagnosis: it names the misread letter.

    REPRESENTATIVE RING (recorded left ring written A, the true one unknown).
    The variant may satisfy ANY member of the class, so it is 150 x |class|
    tests -- around 75 000, for ~4 expected false positives. A hit there means
    nothing on its own and is reported as such.

    That gap is not hypothetical: run unfiltered over the six corpus failures,
    this returns hits for four of them, and only the two trusted-ring hits
    survive. Both of those land on the published ring AND the independently
    confirmed start at once, from one letter -- two constraints, which chance
    does not satisfy together.

    Stops at one letter deliberately. Two would be 9375 variants, and even in
    the trusted-ring case that is ~0.53 expected false positives -- the same
    order as a real answer, so a hit would no longer be evidence.
    """
    out = []
    ind = grund + enc
    if trusted_ring:
        targets = [(ring, start)]
    else:
        targets = (cands if cands is not None
                   else class_members(ring, start, wheels))
    for pos in range(6):
        for c in ALPHABET:
            if c == ind[pos]:
                continue
            v = ind[:pos] + c + ind[pos + 1:]
            g, e = v[:3], v[3:]
            for r, s in targets:
                if enigma_ref.decrypt(e, wheels, r, g, plugs, refl) == s:
                    out.append(dict(grund=g, enc=e, ring=r, start=s, pos=pos,
                                    was=ind[pos], now=c, trusted=trusted_ring))
    return out


def solve(messages, ring, wheels, plugs, refl="B"):
    """Rings consistent with EVERY message's indicator.

    `messages` is a list of (label, start, grundstellung, enciphered[, ct, pt]).
    Each message contributes its own start, so they need not share one; what
    they share is the day key, hence the ring.
    """
    per_message = {}
    for msg in messages:
        label, start, grund, enc = msg[:4]
        cands = class_members(ring, start, wheels)
        if len(msg) >= 6 and msg[4] and msg[5]:
            cands = same_decrypt(cands, msg[4], msg[5], wheels, plugs, refl)
        per_message[label] = sorted(
            {r for r, s in cands
             if indicator_ok(r, s, grund, enc, wheels, plugs, refl)})
    # Intersect only over the messages that produced a candidate at all. A
    # misread indicator yields the EMPTY set, and letting that into the
    # intersection would veto the whole day -- turning one bad transcription
    # into "no agreement" for eight good messages, which is what it did before
    # this line existed. An empty result is an absent vote, not a dissenting
    # one; a message that genuinely disagrees still shows up, because it
    # contributes a non-empty set that fails to intersect.
    informative = [set(h) for h in per_message.values() if h]
    agreed = set.intersection(*informative) if informative else set()
    return per_message, sorted(agreed)


# ---------------------------------------------------------------- corpus mode

def read_corpus():
    text = open(CORPUS, encoding="utf-8").read()
    records = []
    for block in re.split(r"(?=### Message No\.)", text)[1:]:
        no = re.search(r"### Message No\. (\S+)", block).group(1)
        date = re.search(r"--\s+(.*?)\s+\(", block).group(1).strip()

        def field(key, keep_spaces=False):
            # CIPHERTEXT and DECRYPT wrap across lines and must be rejoined with
            # NO separator; PLUGS is a space-separated pair list and must keep
            # its spaces -- collapsing them turns "AV BG" into one 20-letter
            # "pair" that the reference machine mis-parses into a null board,
            # which then looks like a total class-membership failure rather
            # than a parsing bug.
            m = re.search(key + r":\s+(.*?)(?=\n[A-Z]+:|\Z)", block, re.S)
            if not m:
                return None
            return " ".join(m.group(1).split()) if keep_spaces \
                else "".join(m.group(1).split())

        wheels = re.search(r"\(-w (\d+)\)", block)
        ind = re.search(r"INDICATOR:\s+([A-Za-z]{3})\s+([A-Za-z]{3})", block)
        if not wheels or not ind:
            continue
        ct, pt = field("CIPHERTEXT"), field("DECRYPT")
        rec = dict(no=no, date=date, wheels=wheels.group(1), ring=field("RING"),
                   start=field("START"), plugs=field("PLUGS", keep_spaces=True),
                   refl=field("REFLECTOR") or "B",
                   grund=ind.group(1).upper(), enc=ind.group(2).upper(),
                   ct=ct, pt=pt)
        # A '-' marks a letter that was illegible on the form.  It is a real
        # rotor position, so it cannot be dropped from the ciphertext -- such a
        # record simply cannot be checked against its plaintext, and the
        # indicator test runs on the unfiltered class instead.
        if any(x is None for x in (rec["ring"], rec["start"], rec["plugs"])):
            continue
        if ct and pt and "-" in (ct + pt):
            rec["ct"] = rec["pt"] = None
        records.append(rec)
    return records


MONTHS = {m: "%02d" % (i + 1) for i, m in enumerate(
    "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split())}


def normalise_date(text):
    """'09 Sep 1941' and '09.09.1941' both -> '09.09.1941'; None if neither."""
    m = re.match(r"(\d{1,2})\s+([A-Za-z]{3})[a-z]*\s+(\d{4})", text)
    if m and m.group(2).capitalize() in MONTHS:
        return "%02d.%s.%s" % (int(m.group(1)), MONTHS[m.group(2).capitalize()],
                               m.group(3))
    m = re.match(r"(\d{2})\.(\d{2})\.(\d{4})", text)
    return m.group(0) if m else None


def group_by_day(records):
    days = {}
    for r in records:
        key = (r["date"], r["refl"], r["wheels"], r["plugs"])
        days.setdefault(key, []).append(r)
    return days


def corpus_report(only_date=None, out=sys.stdout, fixes=False):
    records = read_corpus()
    if only_date:
        # The corpus writes "09 Sep 1941"; the day-key tables and this script's
        # own --help write "09.09.1941". Accept either rather than making the
        # caller know which file they are addressing -- the first version took
        # only a substring of the corpus form and so rejected the exact string
        # the help offers as its example.
        want = normalise_date(only_date)
        records = [r for r in records
                   if want and normalise_date(r["date"]) == want
                   or only_date in r["date"]]
        if not records:
            have = sorted({normalise_date(r["date"]) or r["date"]
                           for r in read_corpus()})
            sys.exit("no corpus message dated %r carries an indicator.\n"
                     "dates with indicators: %s" % (only_date, ", ".join(have)))
    days = group_by_day(records)

    print("# Recovering the true day-key ring from the indicator", file=out)
    print("# %d messages carrying an indicator, in %d day-key groups.\n"
          % (len(records), len(days)), file=out)
    print("%-8s %-22s %5s %6s %-8s %-6s %-5s %s"
          % ("msg", "date", "len", "class", "indicator", "ring", "rec",
             "verdict"), file=out)

    stats = dict(unique=0, agree=0, multi=0, none=0)
    day_lines = []
    failures = []
    for (date, refl, wheels, plugs), msgs in sorted(
            days.items(), key=lambda kv: kv[1][0]["no"]):
        ring = msgs[0]["ring"]
        per, agreed = solve(
            [(m["no"], m["start"], m["grund"], m["enc"], m["ct"], m["pt"])
             for m in msgs], ring, wheels, plugs, refl)
        for m in msgs:
            hits = per[m["no"]]
            members = class_members(m["ring"], m["start"], wheels)
            n_class = len(
                same_decrypt(members, m["ct"], m["pt"], wheels, plugs, refl)
                if m["ct"] and m["pt"] else members)
            if not hits:
                verdict, key = "no match (indicator misread?)", "none"
                failures.append((m, n_class))
            elif len(hits) > 1:
                verdict, key = "%d candidates" % len(hits), "multi"
            elif hits[0] == m["ring"]:
                verdict, key = "unique, = recorded ring", "unique"
            else:
                verdict = "unique, RECORDED RING IS A REPRESENTATIVE"
                key = "agree"
            stats[key] += 1
            print("%-8s %-22s %5s %6d %-8s %-6s %-5s %s"
                  % (m["no"], m["date"],
                     len(m["ct"]) if m["ct"] else "?", n_class,
                     m["grund"] + " " + m["enc"], hits[0] if hits else "-",
                     m["ring"], verdict), file=out)
        if len(msgs) > 1:
            voted = sum(1 for m in msgs if per[m["no"]])
            day_lines.append("%-22s %d of %d messages voted -> %s"
                             % (date, voted, len(msgs),
                                agreed or "no agreement"))

    print("\n# %d unique and equal to the recorded ring, %d unique but"
          " DIFFERENT (recorded ring was a\n# class representative),"
          " %d ambiguous, %d no match."
          % (stats["unique"], stats["agree"], stats["multi"],
             stats["none"]), file=out)
    if day_lines:
        print("\n# Joint over each multi-message day key -- this is what"
              "\n# removes the residual ambiguity, since one message can"
              "\n# leave two candidates:", file=out)
        for line in day_lines:
            print("#   " + line, file=out)
    if fixes and failures:
        report_fixes(failures, out)
    return stats


def report_fixes(failures, out=sys.stdout):
    """--suggest-fix: name the misread letter where the evidence supports it."""
    print("\n# --suggest-fix: single-letter corrections to the %d indicators"
          "\n# that no ring satisfies." % len(failures), file=out)
    print("#\n# A ring recorded with a left ring of A is a class"
          "\n# REPRESENTATIVE,"
          "\n# so a variant there may satisfy any of the ~500 class members and"
          "\n# ~4 hits are expected by chance. Where the ring is the published"
          "\n# one, the variant must reproduce that exact start: 150 tests at"
          "\n# 1/17576, so a hit names the letter.", file=out)
    for m, n_class in failures:
        trusted = m["ring"][0] != "A"
        cands = None
        if not trusted:
            members = class_members(m["ring"], m["start"], m["wheels"])
            cands = (same_decrypt(members, m["ct"], m["pt"], m["wheels"],
                                  m["plugs"], m["refl"])
                     if m["ct"] and m["pt"] else members)
        hits = suggest_fix(m["ring"], m["start"], m["grund"], m["enc"],
                           m["wheels"], m["plugs"], m["refl"],
                           trusted_ring=trusted, cands=cands)
        head = "#   Nr %-6s %s %s  ring %s%s" % (
            m["no"], m["grund"], m["enc"], m["ring"],
            "" if trusted
            else " (a representative -- hits below are not evidence)")
        print(head, file=out)
        if not hits:
            print("#       no single-letter correction reaches the"
                  " recorded start", file=out)
            continue
        for h in hits:
            note = "names the letter" if h["trusted"] else "chance-level"
            print("#       %s %s   (%s->%s at position %d),"
                  " ring %s -> start %s   [%s]"
                  % (h["grund"], h["enc"], h["was"], h["now"], h["pos"] + 1,
                     h["ring"], h["start"], note), file=out)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", action="store_true",
                    help="survey every corpus message with an indicator")
    ap.add_argument("--date",
                    help="restrict --corpus to one date, e.g. 09.09.1941")
    ap.add_argument("--suggest-fix", action="store_true",
                    help="for indicators no ring satisfies, look for a single "
                         "misread letter that would explain them")
    ap.add_argument("--wheels", help="wheel order, e.g. 342")
    ap.add_argument("--ring", help="any ring in the class, e.g. ALZ")
    ap.add_argument("--plugs", default="", help='plugboard, e.g. "AZ DV ET"')
    ap.add_argument("--reflector", default="B")
    ap.add_argument("--msg", action="append", default=[],
                    metavar="START:GRUND:ENC",
                    help="one message: its start (in the same class as"
                         " --ring), "
                         "and the indicator's two groups. Repeatable.")
    ap.add_argument("--no-selftest", action="store_true",
                    help="skip checking enigma_ref against ./enigma")
    args = ap.parse_args()

    # The Python reference is only a convenience for scoring 676 candidates
    # in-process; the binary is the authority, so verify they agree before any
    # number below depends on it.
    if not args.no_selftest and os.path.exists("./enigma"):
        enigma_ref.selftest()

    if args.corpus or args.date:
        corpus_report(args.date, fixes=args.suggest_fix)
        return
    if not (args.wheels and args.ring and args.msg):
        ap.error("give --corpus, or --wheels/--ring with at least one --msg")

    messages = []
    for i, spec in enumerate(args.msg):
        parts = spec.split(":")
        if len(parts) != 3 or any(len(p) != 3 for p in parts):
            ap.error("--msg wants START:GRUND:ENC, three letters each: %r"
                     % spec)
        messages.append(("msg%d" % (i + 1),) + tuple(p.upper() for p in parts))

    per, agreed = solve(messages, args.ring.upper(), args.wheels,
                        args.plugs.upper(), args.reflector)
    for label, hits in per.items():
        print("%-8s indicator-consistent rings: %s" % (label, hits or "none"))
    if len(messages) > 1:
        print("\nagreed across all %d messages: %s"
              % (len(messages), agreed or "none"))
    if not agreed:
        print("\nNo ring satisfies every indicator. Either one is misread,"
              "\nor the start given is not in the same class as --ring.",
              file=sys.stderr)
        if args.suggest_fix:
            for label, start, grund, enc in messages:
                if per[label]:
                    continue
                # No ciphertext here, so the class cannot be narrowed and the
                # ring cannot be trusted -- report at chance level and say so.
                hits = suggest_fix(args.ring.upper(), start, grund, enc,
                                   args.wheels, args.plugs.upper(),
                                   args.reflector, trusted_ring=False)
                print("%-8s single-letter variants that some class member "
                      "satisfies (chance-level, ~4 expected):" % label)
                for h in hits or []:
                    print("           %s %s   (%s->%s at position %d), ring %s "
                          "-> start %s" % (h["grund"], h["enc"], h["was"],
                                           h["now"], h["pos"] + 1, h["ring"],
                                           h["start"]))
                if not hits:
                    print("           none")
        sys.exit(1)


if __name__ == "__main__":
    main()
