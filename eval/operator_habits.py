#!/usr/bin/env python3
"""Three operator / key-sheet habits checked against the authentic 1941
corpus -- ENHANCEMENTS.md 3c.  Read-only, seconds:

    python3 eval/operator_habits.py eval/enigma-messages.txt \
        eval/enigma-army-messages-1941.txt

  * the Herivel tip: the clear-text Grundstellung sits within a notch of the
    day's Ringstellung on every wheel (operator sets the rings, closes the
    lid, uses what the windows show);
  * the end-of-previous-message cilli: the next message key is where the
    previous message's rotors stopped;
  * key-sheet rules across consecutive days: no wheel in the same slot,
    repeated plug pairs, alphabetically adjacent plugs.

Windows (0..3) and categories were fixed before the corpus was read, and
none of the rules is fitted here, so the counts are held-out estimates.
Consecutive-day pairs that share a whole key (one day filed under two dates)
are skipped."""
import re, sys, itertools
from collections import defaultdict

NOTCH = {'I': 'Q', 'II': 'E', 'III': 'V', 'IV': 'J', 'V': 'Z',
         'VI': 'MZ', 'VII': 'MZ', 'VIII': 'MZ'}
A = ord('A')

def parse(path):
    recs, cur = [], None
    for line in open(path, encoding='utf-8'):
        m = re.match(r'### Message No\. (\d+)\s+--\s+(.*?)\s+\((\w+)\)', line)
        if m:
            cur = {'no': int(m.group(1)), 'date': m.group(2).strip(),
                   'kg': m.group(3), 'file': path}
            recs.append(cur)
            continue
        if cur is None or ':' not in line or line.startswith('#'):
            continue
        k, v = line.split(':', 1)
        k, v = k.strip(), v.strip()
        if k == 'WHEELS':
            cur['wheels'] = v.split('(')[0].split()
        elif k == 'RING':
            cur['ring'] = v.split()[0]
        elif k == 'START':
            cur['start'] = v.split()[0]
        elif k == 'PLUGS':
            cur['plugs'] = v.split()
        elif k == 'INDICATOR':
            parts = v.split('(')[0].split()
            cur['grund'] = parts[0] if parts else None
        elif k == 'CIPHERTEXT':
            cur['len'] = len(v.split()[0]) if v else 0
    return [r for r in recs if all(x in r for x in
            ('wheels', 'ring', 'start', 'plugs', 'len'))]

def step(wheels, ring, start, n):
    """Rotor window positions after n keypresses (double-step included)."""
    g = [ord(c) - A for c in start]
    w = wheels
    for _ in range(n):
        mid_notch = chr(g[1] + A) in NOTCH[w[1]]
        right_notch = chr(g[2] + A) in NOTCH[w[2]]
        if mid_notch:
            g[0] = (g[0] + 1) % 26
            g[1] = (g[1] + 1) % 26
        elif right_notch:
            g[1] = (g[1] + 1) % 26
        g[2] = (g[2] + 1) % 26
    return ''.join(chr(x + A) for x in g)

def cdist(a, b):
    d = abs(ord(a) - ord(b))
    return min(d, 26 - d)

recs = []
for p in sys.argv[1:]:
    recs += parse(p)
print(f"{len(recs)} records with full keys")

# ---- Herivel tip: Grundstellung near the Ringstellung -------------------
print("\n== HERIVEL TIP: clear Grundstellung vs Ringstellung ==")
withg = [r for r in recs if r.get('grund') and len(r['grund']) == 3]
hits = defaultdict(int)
for r in withg:
    d = [cdist(r['grund'][i], r['ring'][i]) for i in range(3)]
    r['hdist'] = d
    for w in (0, 1, 2, 3):
        if all(x <= w for x in d):
            hits[w] += 1
n = len(withg)
for w in (0, 1, 2, 3):
    p = ((2 * w + 1) / 26) ** 3
    print(f"  all three positions within +-{w}: {hits[w]:3d} of {n}  "
          f"(uniform expects {p * n:.2f})")
# per-position too: the tip is often strongest on the slow (left) wheel
for i, name in enumerate(('left', 'middle', 'right')):
    c = sum(1 for r in withg if r['hdist'][i] <= 1)
    print(f"  {name:6s} within +-1: {c:3d} of {n} "
          f"(uniform expects {3 / 26 * n:.1f})")
close = sorted(withg, key=lambda r: sum(r['hdist']))[:8]
print("  closest:",
      ", ".join(f"{r['date']} No.{r['no']} G={r['grund']} R={r['ring']}"
                for r in close))

# ---- end-of-previous-message cilli -------------------------------------
print("\n== CILLI: next message's key vs END position of the previous one ==")
bydate = defaultdict(list)
for r in recs:
    bydate[(r['date'], tuple(r['wheels']), r['ring'])].append(r)
pairs = exact = near = gnear = 0
examples = []
for key, rs in bydate.items():
    rs.sort(key=lambda r: r['no'])
    for a, b in zip(rs, rs[1:]):
        pairs += 1
        end = step(a['wheels'], a['ring'], a['start'], a['len'])
        d = [cdist(end[i], b['start'][i]) for i in range(3)]
        if end == b['start']:
            exact += 1
        if all(x <= 1 for x in d):
            near += 1
            examples.append(f"{a['date']} No.{a['no']}->{b['no']} "
                            f"end={end} next={b['start']}")
        if b.get('grund') and all(cdist(end[i], b['grund'][i]) <= 1
                                  for i in range(3)):
            gnear += 1
print(f"  consecutive same-day pairs: {pairs}")
print(f"  next START == end of previous: {exact}   "
      f"(uniform expects {pairs / 17576:.4f})")
print(f"  next START within +-1 on all wheels: {near}   "
      f"(uniform expects {pairs * (3/26)**3:.3f})")
print(f"  next GRUNDSTELLUNG within +-1 of end: {gnear}")
for e in examples[:6]:
    print("   ", e)

# ---- key-sheet rules across consecutive days ----------------------------
print("\n== KEY-SHEET RULES ==")
days = {}
for r in recs:
    days.setdefault(r['date'],
                    (r['wheels'], r['ring'], tuple(sorted(r['plugs']))))
import datetime
def dparse(s):
    m = re.match(r'(\d+)\s+([A-Za-z]{3})[A-Za-z]*\s+(\d{4})', s)
    return datetime.datetime.strptime(' '.join(m.groups()), '%d %b %Y')
dl = sorted(days, key=dparse)
print(f"  distinct day keys: {len(dl)}")
consec = same_slot = plug_rep = 0
for d1, d2 in zip(dl, dl[1:]):
    if (dparse(d2) - dparse(d1)).days != 1:
        continue
    w1, _, p1 = days[d1]; w2, _, p2 = days[d2]
    if w1 == w2 and p1 == p2:
        print(f"  {d1} -> {d2}: identical key, one day under two dates "
              f"-- skipped")
        continue
    consec += 1
    ss = sum(1 for i in range(3) if w1[i] == w2[i])
    same_slot += ss
    pr = len(set(p1) & set(p2))
    plug_rep += pr
    print(f"  {d1} -> {d2}: wheels {' '.join(w1)} -> {' '.join(w2)}  "
          f"same-slot={ss}  shared plugs={pr}")
print(f"  consecutive-day pairs: {consec}; wheel same-slot total {same_slot} "
      f"(uniform over 60 orders expects {consec * 3 / 5:.1f}); "
      f"repeated plug pairs {plug_rep} "
      f"(uniform expects {consec * 10 * 10 / 325:.1f})")
adj = 0; tot = 0
for d in dl:
    for p in days[d][2]:
        tot += 1
        if abs(ord(p[0]) - ord(p[1])) == 1:
            adj += 1
print(f"  alphabetically ADJACENT plug pairs: {adj} of {tot} "
      f"(uniform expects {tot * 25 / 325:.1f})")
