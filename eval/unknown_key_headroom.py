#!/usr/bin/env python3
"""Would a full unknown-key break succeed at this length? -- without sweeping.

    python3 eval/unknown_key_headroom.py --trials 60 --restarts 8

WHY THIS EXISTS.  Every tuning result in this repo -- the 75% exact recovery at
L=167, the i4f10 pre-pass finding, crack_real.py's validation against the
published breaks -- measures the PLUGBOARD-RECOVERY sub-problem with the rotor
key GIVEN.  The full problem, where the true key must also outscore millions of
competitors, was never measured.  So "the search came back negative" could never
be turned into "the message is probably not breakable", because the break rate
was unknown.

WHY NOT JUST RUN THE SWEEP.  Because it is unaffordable and unnecessary.  A
single trial at the keyspace people actually attack (80M keys) takes ~10 hours;
sixty of them is a month.  But a break needs two INDEPENDENT things:

  1. the climb recovers the board AT THE TRUE KEY   -- independent of K
  2. the true key's score beats the best of K wrong keys

and (2) is arithmetic once you know the true key's z-score, because the best of
K draws from the null sits at mu + sigma*sqrt(2 ln K).  So measure that z once
per message -- one pinned climb, plus the mu/sigma that --confidence already
samples -- and every keyspace follows.  Three seconds a trial, not ten hours.

MEASURED (60 trials, L=167, 10-pair board hidden, -R 8, authentic HG Nord
plaintext held out from the wehrmacht tables):

      keyspace                bar    z>bar   x climb   = break
      start only (17,576)    4.42     75%      73%       55%
      wheels+ring2 (27.4M)   5.85     72%      73%       53%
      full -r A.. (230M)     6.21     72%      73%       53%

KEYSPACE SIZE IS NEARLY IRRELEVANT.  Four orders of magnitude of K cost two
points of break rate, because the bar grows as sqrt(2 ln K) -- 4.42 to 6.21 --
while the true key's z has a median of 11.5.  Discrimination is not the
bottleneck and never was.

The two real limits are both independent of K: climb failure at the true key,
and a scoring floor where the true key's z is simply too low (min observed 1.1).

SEPARATING THEM IS CIRCULAR IF DONE AT ONE -R.  A failed climb also produces a
low z, so "low z" and "climb failed" are the same trials, and reading the split
off a single -R 8 run gives a "~25% scoring floor" that is WRONG.  Judge
breakability at a HIGH -R, where the climb nearly always succeeds: at -R 64,
95% of messages are intrinsically breakable at L=167.  The floor is 5%; the rest
of the -R 8 residual is climb failure, which -R moves.  Measured climb curve
over breakable messages: 50/68/79/87/95/100% at R = 2/4/8/16/32/64.

SPEND ON -R UNTIL THE CURVE FLATTENS, THEN BUY COVERAGE.  At matched wall time
the middle option wins (24 h at ~11k items/s): -r A.. exact affords -R 4 for a
66% break rate, -r AA. affords -R 34 for 65%, and -r A.. --ring-stride 3 affords
-R 12 for 80%.  Exact coverage costs 2.89x for the ~1pp the stride gives up;
-r AA. buys restarts long past the point they help while excluding 28% of keys.

RUNS ARE NOT INDEPENDENT.  The scoring floor is common mode, so repeated
attempts converge on 95%, not 100%.  Update the posterior instead.
"""
import argparse
import math
import os
import random
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ENIGMA = os.path.join(ROOT, "enigma")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

# Keyspaces worth reporting, smallest first. The last two are what a real
# attempt on an unbroken message uses.
SPACES = [("start only", 17576),
          ("ring2+start", 456976),
          ("wheels+start", 1054560),
          ("wheels+ring2", 27418560),
          ("-r A.. --ring-stride 3", 79579776),
          ("-r A.. exact", 229894080)]


def decrypts(path):
    """DECRYPT fields only -- stop at the next FIELD: label, or EMENDED and
    TRANSLATION leak in and the corpus stops being machine output."""
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def run(args, text):
    p = subprocess.run([ENIGMA] + args, input=text, capture_output=True,
                       text=True, check=False)
    return p.stdout.strip(), p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=60)
    ap.add_argument("--restarts", default="8")
    ap.add_argument("--length", type=int, default=167)
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--threads", default="4")
    ap.add_argument("--language", default="wehrmacht")
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")

    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    climb = ["-c", "-S", "i4f10", "-J", "--polish",
             "-l", args.language, "-T", args.threads]
    rng = random.Random(args.seed)
    L = args.length
    zs, recovered = [], []

    for _ in range(args.trials):
        pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
        w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
        r = "".join(rng.choice(LET) for _ in range(3))
        g = "".join(rng.choice(LET) for _ in range(3))
        ls = list(LET)
        rng.shuffle(ls)
        pb = " ".join(ls[2 * i] + ls[2 * i + 1] for i in range(args.plugs))
        ct, _ = run(["-u", "B", "-w", w, "-r", r, "-g", g, "-s", pb], pt)

        # (1) the climb at the TRUE key: does the board come back, at what score
        out, err = run(["-u", "B", "-w", w, "-r", r, "-g", g,
                        "-R", args.restarts] + climb, ct)
        pct = 100.0 * sum(a == b for a, b in zip(out, pt)) / L
        scores = re.findall(r"^\s*(-?\d+\.\d{4})\s+[A-Z]", err, re.M)
        if not scores:
            continue
        # (2) mu/sigma of the CLIMBED null for this ciphertext. A cheap sweep at
        # -R 1 is enough: --confidence samples and climbs its own keys, and the
        # null depends on the ciphertext and model, not on the sweep's size.
        _, err2 = run(["-u", "B", "-w", w, "-r", r, "-g", "...",
                       "-R", "1", "--confidence", "256"] + climb, ct)
        m = re.search(r"null (-?\d+\.\d+) \+/- (\d+\.\d+)", err2)
        if not m:
            continue
        mu, sd = float(m.group(1)), float(m.group(2))
        zs.append((float(scores[-1]) - mu) / sd)
        recovered.append(pct > 99.99)

    if len(zs) < 2:
        sys.exit("no usable trials -- is the binary current? "
                 "(needs --confidence and -S i4f10)")

    n = len(zs)
    climb_rate = sum(recovered) / n
    print(f"n={n}  R={args.restarts}  L={L}  plugs={args.plugs}  "
          f"-l {args.language}")
    print(f"  climb recovers the board at the TRUE key: "
          f"{sum(recovered)}/{n} = {climb_rate:.0%}")
    print(f"  true key's z above the climbed null: median "
          f"{statistics.median(zs):.1f}  "
          f"(min {min(zs):.1f}, max {max(zs):.1f})")
    print()
    print(f"  {'keyspace':>24}{'bar':>7}{'z>bar':>8}"
          f"{'x climb':>9}{'= break':>9}")
    for label, K in SPACES:
        bar = math.sqrt(2 * math.log(K))
        frac = sum(1 for z in zs if z > bar) / n
        print(f"  {label:>24}{bar:>7.2f}{frac:>8.0%}"
              f"{climb_rate:>9.0%}{frac * climb_rate:>9.0%}")
    print("\n  bar = sqrt(2 ln K), where the best of K wrong keys lands by "
          "chance.\n  A break needs the climb to work AND z to clear the bar.")


if __name__ == "__main__":
    main()
