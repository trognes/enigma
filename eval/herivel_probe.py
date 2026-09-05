#!/usr/bin/env python3
"""Is the Herivel tip alive in the 1941 corpus?

    python3 eval/herivel_probe.py

Herivel's 1940 insight: an operator sets the day's ring settings, closes the
lid, and the Grundstellung of his first message sits at or near the ring --
the rings are what he last saw in the windows.  Bletchley read the day's ring
off the cluster of early Grundstellungen.  No modern ciphertext-only tool uses
it, because it is not a ciphertext statistic at all: it is an operator habit,
read from the message HEADERS.

The corpus carries both halves: the indicator (Grundstellung, sent in clear)
for 54 messages, and the published day-key ring for their dates.  So the tip
is testable directly.  Two views:

  1. every message with an indicator and a known ring: cyclic distance
     |Grundstellung - ring| per wheel.  Uniform gives mean 6.5 and
     P(d <= 2) = 19.2%; all three wheels within +-1 is (3/26)^3 = 0.15%.
  2. by SEND TIME (from the message-header page): is the effect confined to
     the first message of the day, as Herivel described?

Ring 0 is a class representative on three recovered days (16.07, 28.08,
09.09), so wheel 0 is scored only on published rings.  Wheels 1 and 2 are the
ones a constrained sweep would pin, so the headline is their joint distance.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REP = {"16.07.1941", "28.08.1941", "09.09.1941"}


def cd(a, b):
    d = abs(ord(a) - ord(b))
    return min(d, 26 - d)


def main():
    src = open(os.path.join(HERE, "build_army_messages_1941.py"),
               encoding="utf-8").read()
    days = dict(re.findall(
        r'"(\d\d\.\d\d\.\d{4}\w?)":\s*\("B",\s*"\d{3}",\s*"([A-Z]{3})"', src))
    ind = {no: g for no, g, _ in
           re.findall(r'"([\w-]+)":\s*\(?\s*"([A-Z]{3}) ([A-Z]{3})"', src)}
    msgs = re.findall(
        r'\("([\w-]+)",\s*"([\d.]+\w?)",\s*"([^"]*)",\s*"([A-Z]{3})",'
        r'\s*"([^"]*)",\s*"([A-Z]{5})"', src)
    kenn2day = {k: d for _, d, _, _, _, k in msgs}

    # ---- view 1: every message with an indicator and a known ring --------
    rows = []
    for no, date, _, _, _, kenn in msgs:
        if no in ind and date in days:
            g, r = ind[no], days[date]
            rows.append((date, no, kenn, g, r, [cd(g[i], r[i])
                                                for i in range(3)]))
    print(f"view 1: {len(rows)} messages with indicator + day ring\n")
    for w in range(3):
        ds = [d[w] for date, *_, d in rows if not (w == 0 and date in REP)]
        near = sum(x <= 2 for x in ds)
        print(f"  wheel {w}: n={len(ds):>2}  mean d = {sum(ds)/len(ds):4.1f} "
              f"(uniform 6.5)   d<=2: {near:>2}/{len(ds)} = "
              f"{100*near/len(ds):4.1f}% (uniform 19.2%)")
    all3 = [r for r in rows if max(r[5]) <= 1 and r[0] not in REP]
    exp = 0.0
    for r in rows:
        exp += (3 / 26) ** 3 if r[0] not in REP else (3 / 26) ** 2
    print(f"\n  all three wheels within +-1: {len(all3)} messages, "
          f"{exp:.2f} expected by chance")
    for date, no, kenn, g, r, d in all3:
        print(f"    {date} Nr{no:>5} {kenn}  Grund {g}  ring {r}  {d}")
    w12 = [r for r in rows if max(r[5][1:]) <= 1]
    print(f"  wheels 1,2 within +-1: {len(w12)}/{len(rows)} = "
          f"{100*len(w12)/len(rows):.0f}%  (chance (3/26)^2 = 1.3%)")

    # ---- view 2: by send time, from the message-header page -------------
    html = open(os.path.join(HERE, "sources", "german-army-messages.html"),
                encoding="utf-8", errors="replace").read()
    timed = []
    for blk in html.split("Enigma message of ")[1:]:
        h = re.match(r'([\d.]+), Nr\. (?:<font[^>]*>)?([\w-]+)(?:</font>)?,'
                     r' \(([A-Z]{5})\)', blk)
        m = re.search(r'\((\d{4})\)\s*-\s*\d+\s*-\s*([A-Z]{3}) ([A-Z]{3})\s*-',
                      blk)
        if not (h and m):
            continue
        _, nr, kenn = h.groups()
        t, g, _ = m.groups()
        dk = kenn2day.get(kenn)
        if dk not in days:
            continue
        r = days[dk]
        timed.append((dk, int(t), nr, kenn, g, r,
                      [cd(g[i], r[i]) for i in range(3)]))
    byday = {}
    for row in sorted(timed, key=lambda x: (x[0], x[1])):
        byday.setdefault(row[0], []).append(row)
    print(f"\nview 2: {len(timed)} messages with a header send time; "
          f"{len(byday)} days\n")
    print("  first message of each day by send time "
          "(HIT = wheels 1,2 both within +-1):")
    hits = 0
    for dk, lst in sorted(byday.items()):
        _, t, nr, kenn, g, r, d = lst[0]
        hit = max(d[1:]) <= 1
        hits += hit
        print(f"    {dk:<12} {t:04d} Nr{nr:>5} {kenn} {g} {r} {d}"
              f"{'  HIT' if hit else ''}")
    n = len(byday)
    print(f"\n  first-of-day: {hits}/{n} hits   (chance 1.3%)")
    for rank in (1, 2):
        sel = [lst[rank] for lst in byday.values() if len(lst) > rank]
        if sel:
            h = sum(max(x[6][1:]) <= 1 for x in sel)
            print(f"  rank {rank} within its day: {h}/{len(sel)} hits")


if __name__ == "__main__":
    main()
