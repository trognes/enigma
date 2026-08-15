#!/usr/bin/env python3
"""Does the X-doubling pattern survive a partly-wrong plugboard?

`doubling_probe.py` measured the pattern in the RECORDED plaintexts: ~42% of
real messages carry a >=6-letter repeat around an X (one mismatch allowed) and
essentially no shuffled null does.  That says the phenomenon exists.  It says
nothing about the question that decides the idea, which is whether a climb that
has NOT recovered the board still produces both copies intact -- the exact way
the known-word bonus died (ENHANCEMENTS.md item 4: 18 of the 21 below-bar
messages had no words in the true-key decrypt at all).

So: climb the true key and NSAMP wrong keys with the board hidden on both arms,
then ask of every resulting decrypt whether the pattern fires.  Two rates matter
and they answer different questions --

  true-key fire rate    can it CONFIRM a correct key?
  wrong-key fire rate   how often does it confirm a WRONG one?

The wrong-key rate is the one the shuffled null could not reach, and it is the
harder test: a climbed wrong key is not random text, it is text a hill-climb has
actively optimised to look like German, which is exactly the process that might
manufacture spurious repeats.

EVERY DECRYPT IS SAVED to results-doubling-climb-texts.json.  The previous probe
in this series threw its texts away and so had to be re-run from scratch to ask
one new question of the same data; this one should not have to be.  Re-analyse
with analyse() instead of re-climbing.

  ./eval/doubling_climb_probe.py [nsamp] [restarts]
  REUSE=1 ./eval/doubling_climb_probe.py     # re-analyse the saved decrypts

RESULT (46 messages, 128 wrong keys each, -R 32).  Two findings, opposite signs:

PRECISION IS PERFECT and better than the shuffled null predicted -- 0 of 5888
climbed wrong-key decrypts fire, so a hit is worth >=512:1 by the rule of three.
That survives the harder null: these are texts a hill-climb actively optimised
to look like German, and it still manufactures no doublings.

RECALL COLLAPSES exactly where it is needed, and the ceiling control proves it
is the climb rather than the corpus.  The pattern is about equally PRESENT in
both populations (ceiling 11/24 above the bar, 9/18 below), but the climb
reproduces 11 of 11 above and 1 of 9 below.  Nine below-bar messages carry
TSCHEDINOVAXTSCHEDINOVA, WASCHBUSCHXWASCHBUSCH, NIKOLAJEWOXNIKOLAJEWO and the
rest; the climb recovers one of them.

THE REASON IS STARKER THAN "HARDER TO KEEP", and this is the useful form of it.
Letters recovered at the true key over those nine: 100% for FTNBK and 1.4-15.5%
for the other eight, at or near the 1/26 = 3.8% chance floor.  The outcome is
BIMODAL even with the rotor key given, so these are not messages where the
doubling was fragile -- they are eight climb failures plus one true scoring
failure.  FTNBK's climb reproduces the plaintext byte-for-byte and the quadgram
model still ranks it below wrong keys; ALWOK's produces
ARORSQRINRBFLLABHIMLYQKBEEMLOOHIRK... against a truth of
ANXZWOXSANXKOMPXMITTAGESANBRUQ..., and no feature can read structure out of
that because there is none.

So the bound on ANY plaintext-side feature is tighter than a fragility argument
suggests: it can only address SCORING failures, which here is one message of the
22 below the bar.  The rest are search failures, which -R moves and no scorer
does.

So the RESCUE application is dead and the CONFIRMATION application is real but
narrow: a one-sided flag that fires on 26% of messages, mostly ones already won,
and never lies.  Non-firing means nothing.
"""
import json
import math
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from doubling_probe import decrypts, repeats             # noqa: E402
from word_segment_probe import records                   # noqa: E402

BIN = "./enigma"
RECIPE = ["-c", "-f", "-l", "wehrmacht", "-S", "i4f10", "-J", "--polish"]
NSAMP = int(sys.argv[1]) if len(sys.argv) > 1 else 128
RESTARTS = sys.argv[2] if len(sys.argv) > 2 else "32"
THREADS = os.environ.get("Z_THREADS", "4")
TEXTS = "eval/results-doubling-climb-texts.json"
WSEG = "eval/results-word-segment.json"
BAR = math.sqrt(2 * math.log(160293120))
ORDERS = [a + b + c for a in "12345" for b in "12345" for c in "12345"
          if len({a, b, c}) == 3]
A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

# (label, min length, mismatches allowed).  The second is the operating point
# doubling_probe.py found dominant on the recorded plaintexts.
VARIANTS = (("len>=4 exact", 4, 0), ("len>=6 mm<=1", 6, 1))


def climb(ct, w, r, g):
    p = subprocess.run([BIN] + RECIPE + ["-u", "B", "-w", w, "-r", r, "-g", g,
                                         "-R", RESTARTS, "-T", THREADS],
                       input=ct, capture_output=True, text=True,
                       env={**os.environ, "ENIGMA_SEED": "0"})
    return p.stdout.strip()


def collect():
    out = []
    for kenn, w, r, st, ct in records():
        rng = random.Random(7)
        rec = {"kenn": kenn, "len": len(ct), "true": climb(ct, w, r, st),
               "wrong": []}
        for _ in range(NSAMP):
            ww = rng.choice(ORDERS)
            while ww == w:
                ww = rng.choice(ORDERS)
            rec["wrong"].append(climb(ct, ww,
                                      "A" + rng.choice(A) + rng.choice(A),
                                      "".join(rng.choice(A) for _ in range(3))))
        rec["wrong"] = [t for t in rec["wrong"] if t]
        if not rec["true"] or len(rec["wrong"]) < 10:
            continue
        out.append(rec)
        fires = ["%s %s" % (lbl, "YES" if repeats(rec["true"], k, mm) else "no")
                 for lbl, k, mm in VARIANTS]
        print("  %-9s %4d  true: %s" % (kenn[:9], rec["len"], ", ".join(fires)),
              flush=True)
        json.dump(out, open(TEXTS, "w"))       # resumable / never lose the run
    return out


def pct(a, b):
    """percent of letters the climb got right -- the control that shows the
    outcome is bimodal (either ~100% or near the 1/26 chance floor)"""
    n = min(len(a), len(b))
    return 100.0 * sum(1 for i in range(n) if a[i] == b[i]) / n if n else 0.0


def analyse(recs):
    zq = {}
    if os.path.exists(WSEG):
        zq = {r["kenn"]: r["z_quad"] for r in json.load(open(WSEG))}
    # The CEILING control.  Without it a low fire rate is unreadable: it could
    # mean the climb destroys the pattern, or simply that these messages never
    # carried one.  Splitting the ceiling by baseline separates those, and that
    # is what decides the idea.
    truth = dict(decrypts())
    n = len(recs)
    print("\n%d messages, %d wrong keys each, -R %s" % (n, NSAMP, RESTARTS))
    for lbl, k, mm in VARIANTS:
        tf = sum(bool(repeats(r["true"], k, mm)) for r in recs)
        wf = [bool(repeats(t, k, mm)) for r in recs for t in r["wrong"]]
        nw = len(wf)
        print("\n  %s" % lbl)
        print("    fires on the TRUE key:  %2d of %d messages (%3.0f%%)"
              % (tf, n, 100.0 * tf / n))
        print("    fires on a WRONG key:   %d of %d climbed decrypts (%.3f%%)"
              % (sum(wf), nw, 100.0 * sum(wf) / nw))
        # Rule of three: 0 hits in nw bounds the rate at 3/nw, so quote the
        # bound rather than the point estimate, which is an artefact of nw.
        if sum(wf):
            print("    likelihood ratio:       %.0f : 1"
                  % ((tf / n) / (sum(wf) / nw)))
        else:
            print("    likelihood ratio:       >=%.0f : 1  (95%% bound)"
                  % ((tf / n) / (3.0 / nw)))
            print("      rule of three: 0 of %d gives FP <= %.4f%%"
                  % (nw, 100.0 * 3.0 / nw))

        if not zq:
            continue
        print("    %-22s %5s %10s %14s"
              % ("", "n", "ceiling", "climb keeps"))
        pops = (("quad above the bar",
                 [r for r in recs if zq.get(r["kenn"], 0.0) > BAR]),
                ("quad BELOW the bar",
                 [r for r in recs
                  if r["kenn"] in zq and zq[r["kenn"]] <= BAR]))
        for name, sel in pops:
            have = [r for r in sel if r["kenn"] in truth]
            avail = [r for r in have if repeats(truth[r["kenn"]], k, mm)]
            got = [r for r in avail if repeats(r["true"], k, mm)]
            print("    %-22s %5d %7d/%-3d %8d/%-3d %s"
                  % (name, len(sel), len(avail), len(have), len(got),
                     len(avail),
                     "(%3.0f%%)" % (100.0 * len(got) / len(avail))
                     if avail else ""))

    # The single most legible piece of evidence: the doublings that are THERE
    # in the plaintext of a message quadgrams cannot break, and what the climb
    # does with them.
    k, mm = VARIANTS[-1][1], VARIANTS[-1][2]
    print("\n  below-bar messages whose plaintext DOES carry a doubling.")
    print("  The %-correct column is the point: the outcome is BIMODAL even")
    print("  with the rotor key given, so these are not messages where the")
    print("  doubling was fragile -- they are climb failures, at which no")
    print("  plaintext-side feature can work, plus one true scoring failure.")
    print("    %-8s %6s %8s  %s" % ("msg", "z_quad", "%correct", "doubling"))
    for r in sorted(recs, key=lambda x: -pct(truth.get(x["kenn"], ""),
                                             x["true"])):
        if r["kenn"] not in zq or zq[r["kenn"]] > BAR or r["kenn"] not in truth:
            continue
        hit = repeats(truth[r["kenn"]], k, mm)
        if not hit:
            continue
        print("    %-8s %6.2f %7.1f%%  %sX%s  reproduced: %s"
              % (r["kenn"], zq[r["kenn"]], pct(truth[r["kenn"]], r["true"]),
                 hit[0][0], hit[0][1],
                 "YES" if repeats(r["true"], k, mm) else "no"))


def main():
    recs = collect() if not os.environ.get("REUSE") \
        else json.load(open(TEXTS))
    analyse(recs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
