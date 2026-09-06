#!/usr/bin/env python3
"""Converged-board identity: the GPU host against the CPU's --int climb.

    python3 metal/verify_identity.py                       # enigma-ref
    python3 metal/verify_identity.py --host metal/enigma-metal
    python3 metal/verify_identity.py --sweeps              # + key sweeps

metal/DESIGN.md 8.2: for N fixtures per length -- an authentic HG Nord
excerpt, a random rotor key, a random 10-pair board -- run the CPU tool
with --int --dump-all and the GPU host with the identical command line,
and compare EVERY restart's converged (key, score, board) row. Under --int
the requirement is 100%: a differing row is a bug and is printed with both
sides. The stdout decrypt is compared too, and the host's own component
check (8.1) is read back from its stderr.

--sweeps adds three wildcarded-key cases per length, which is where the
host's key enumeration (the collapses, the two-notch wheels, M4) has to
agree with search_worker(): 17 576 starts on a single-notch order, the
same on a two-notch right wheel, and an M4 order.
"""

import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.join(HERE, os.pardir)
ENIGMA = os.path.join(TOP, "enigma")
NGRAMS = os.path.join(TOP, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-S", "k4f10", "-f", "-l", "wehrmacht", "--int",
          "--dump-all"]


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(binary, argv, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    p = subprocess.run([binary] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.returncode, p.stdout.strip(), p.stderr


def dumps(stderr):
    return sorted(ln for ln in stderr.splitlines() if ln.startswith("dumpall "))


def fixture(rng, corpus, L, m4=False):
    pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
    if m4:
        w = "B" + "".join(str(x) for x in rng.sample(range(1, 9), 3))
        r = "A" + "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(4))
        key = ["-4", "-u", "b", "-w", w, "-r", r, "-g", g]
    else:
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        key = ["-u", "B", "-w", w, "-r", r, "-g", g]
    ls = list(LET)
    rng.shuffle(ls)
    pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(10))
    _, ct, _ = run(ENIGMA, key + ["-s", pb], pt)
    return pt, key, ct


def compare(host, key, ct, extra, threads):
    """One case: (rows compared, rows differing, decrypt differs,
    component line, host exit code, first differing pair)."""
    args = key + RECIPE + extra + ["-T", threads]
    _, o0, e0 = run(ENIGMA, args, ct)
    rc, o1, e1 = run(host, args, ct)
    d0, d1 = dumps(e0), dumps(e1)
    bad = sum(1 for a, b in zip(d0, d1) if a != b) + abs(len(d0) - len(d1))
    first = None
    for a, b in zip(d0, d1):
        if a != b:
            first = (a, b)
            break
    m = re.search(r"Components: (\d+) of (\d+)", e1)
    comp = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
    if rc != 0 and not m:
        sys.stderr.write(e1[-2000:])
    return len(d0), bad, o0 != o1, comp, rc, first


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.path.join(HERE, "enigma-ref"))
    ap.add_argument("--fixtures", type=int, default=20)
    ap.add_argument("--restarts", type=int, default=64)
    ap.add_argument("--lengths", type=int, nargs="+", default=[60, 107, 167])
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--sweeps", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    for b in (ENIGMA, args.host):
        if not os.path.exists(b):
            sys.exit(f"build {b} first")

    corpus = "".join(decrypts(os.path.join(TOP, "eval", "enigma-messages.txt"))
                     + decrypts(os.path.join(TOP, "eval",
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)
    print(f"# {os.path.relpath(args.host, TOP)} vs ./enigma, {RECIPE}, "
          f"10 plugs hidden, rotor key given\n")
    print(f"identity: {args.fixtures} fixtures per length, -R {args.restarts}, "
          f"every restart's (key, score, board) row compared")
    total_bad = 0
    for L in args.lengths:
        rows = bad = dec = cbad = ctot = fails = 0
        n = 0
        shown = None
        for _ in range(args.fixtures):
            pt, key, ct = fixture(rng, corpus, L)
            if len(ct) != L:
                continue
            r, b, d, comp, rc, first = compare(args.host, key, ct,
                                               ["-R", args.restarts],
                                               args.threads)
            n += 1
            rows += r
            bad += b
            dec += d
            ctot += comp[1]
            cbad += comp[1] - comp[0]
            fails += (rc != 0)
            if first and not shown:
                shown = first
        print(f"  L={L:>3}: {n} fixtures, {rows} restarts: {bad} differing "
              f"rows, {dec} differing decrypts, {cbad} of {ctot} boards "
              f"with inexact components, {fails} host failures")
        if shown:
            print(f"        first difference:\n        cpu {shown[0]}\n"
                  f"        gpu {shown[1]}")
        total_bad += bad + dec + cbad + fails

    if args.sweeps:
        print("\nsweeps: wildcarded keys, -R 1, the host's enumeration "
              "against search_worker()'s")
        cases = [("-w 123 -r AA. -g A.. (17 576 keys, single-notch)",
                  ["-u", "B", "-w", "123", "-r", "AA.", "-g", "A.."], False),
                 ("-w 126 -r AA. -g A.. (two-notch right wheel)",
                  ["-u", "B", "-w", "126", "-r", "AA.", "-g", "A.."], False),
                 ("-4 -u b -w B317 -r AAA. -g .A.. (M4, Greek wildcarded)",
                  ["-4", "-u", "b", "-w", "B317", "-r", "AAA.", "-g", ".A.."],
                  True)]
        for L in args.lengths:
            for name, sweep, m4 in cases:
                pt, key, ct = fixture(rng, corpus, L, m4)
                if len(ct) != L:
                    continue
                r, b, d, comp, rc, first = compare(args.host, sweep, ct,
                                                   ["-R", 1], args.threads)
                print(f"  L={L:>3} {name}: {r} keys, {b} differing rows, "
                      f"decrypt {'differs' if d else 'same'}, components "
                      f"{comp[0]}/{comp[1]}, host exit {rc}")
                if first:
                    print(f"        cpu {first[0]}\n        gpu {first[1]}")
                total_bad += b + d + (comp[1] - comp[0]) + (rc != 0)

    print("\nRESULT:", "identical" if total_bad == 0 else
          f"{total_bad} discrepancies")
    sys.exit(0 if total_bad == 0 else 1)


if __name__ == "__main__":
    main()
