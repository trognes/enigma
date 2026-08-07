#!/usr/bin/env python3
"""Check `enigma --crib`'s deduction against eval/crib_menu.py's test vectors.

    python3 eval/crib_menu.py --vectors /tmp/v.txt --count 40
    python3 eval/crib_vectors_check.py /tmp/v.txt

Or in one step, which is how the test suite runs it:

    python3 eval/crib_vectors_check.py --generate --count 40

WHAT IS BEING CHECKED, and why it is not circular.  Each vector carries a real
message, its true key and its true plugboard, so the plugs the deduction must
produce are known from the ANSWER KEY -- the Python is only the thing that
writes them down.  Two properties, both from archived/cribs.md §10:

  §10.2  the true rotor setting must not be rejected: at least one hypothesis
         survives, and the one matching the true board is among the survivors
  §10.1  every plug that hypothesis deduces must match the true board exactly.
         Not "mostly" -- a single mismatch is a bug

The binary prints one `cribstop` line per surviving hypothesis under
`--crib-dump`, so the comparison is against the line whose hypothesis is the
true partner of the anchor letter.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")


def read_vectors(path):
    """Split the vector file into records of field -> value."""
    recs, cur = [], {}
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("#"):
            continue
        if not line.strip():
            if cur:
                recs.append(cur)
                cur = {}
            continue
        k, v = line.split(None, 1)
        cur[k] = v.strip()
    if cur:
        recs.append(cur)
    return recs


def run(rec):
    """Run the binary on one vector and return its cribstop lines, parsed."""
    cmd = [BIN, "-i", "-u", rec["reflector"], "-w", rec["wheels"],
           "-r", rec["ring"], "-g", rec["start"],
           "--crib", rec["crib"],
           # --crib-at is 1-based; the vectors carry 0-based offsets
           "--crib-at", str(int(rec["at"]) + 1),
           "--crib-dump"]
    p = subprocess.run(cmd, input=rec["cipher"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, universal_newlines=True)
    out = {}
    for line in p.stderr.splitlines():
        f = line.split()
        if f and f[0] == "cribstop":
            # cribstop <wheels> <ring> <start> <at> <anchor> <hyp> <plugs..>
            # The vectors pin --crib-at, so every line is at that one alignment.
            out[f[6]] = sorted(f[7:])
    return out, p.returncode


def check(path, verbose=False):
    recs = read_vectors(path)
    if not recs:
        sys.exit("no vectors in %s" % path)
    bad = 0
    for i, rec in enumerate(recs, 1):
        # What the true board plugs the anchor letter to -- the hypothesis the
        # deduction must have kept.
        board = {}
        for pair in rec.get("plugs", "").split():
            board[pair[0]] = pair[1]
            board[pair[1]] = pair[0]
        anchor = rec["anchor"]
        truth = board.get(anchor, anchor)      # unplugged letters map to self
        stops, rc = run(rec)
        want = sorted(rec["deduced"].split())
        if rc != 0:
            print("FAIL vector %d: enigma exited %d" % (i, rc))
            bad += 1
        elif truth not in stops:
            print("FAIL vector %d (§10.2): the true hypothesis %s->%s was "
                  "rejected; survivors: %s"
                  % (i, anchor, truth, " ".join(sorted(stops)) or "(none)"))
            bad += 1
        elif stops[truth] != want:
            print("FAIL vector %d (§10.1): deduced plugs differ" % i)
            print("   python: %s" % " ".join(want))
            print("   enigma: %s" % " ".join(stops[truth]))
            bad += 1
        elif verbose:
            print("ok   vector %d: %-26s %2d plugs, %d stop(s)"
                  % (i, rec["crib"], len(want), len(stops)))
    print("%s: %d vector(s), %d failure(s)" % (path, len(recs), bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("vectors", nargs="?", help="vector file from crib_menu.py")
    ap.add_argument("--generate", action="store_true",
                    help="generate the vectors first, into a temporary file")
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    path = args.vectors
    if args.generate or not path:
        path = path or os.path.join(os.environ.get("TMPDIR", "/tmp"),
                                    "crib_vectors.txt")
        subprocess.check_call([sys.executable,
                               os.path.join(HERE, "crib_menu.py"),
                               "--vectors", path, "--count", str(args.count),
                               "--seed", str(args.seed)],
                              stdout=subprocess.DEVNULL)
    return check(path, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
