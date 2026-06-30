#!/usr/bin/env python3
# Measure the key pre-filter (-F) end-to-end. A rotor-key search (wildcard 2 of the
# 3 start positions = 676 keys, so the FULL crack is affordable) is recovered with
# the full climb vs the -F pre-filter, across plug counts. Reports mean %-correct for
# each and the wall-clock speedup. The pre-filter should match the full crack's
# recovery while running several times faster.
#
#   env knobs: TRIALS (20), L (190), F (50), PAIRS ("3 5 8"), RECIPE ("-R 10 -S iq")
import os, random, string, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
E = os.path.join(ROOT, "enigma")
CORPUS = ("THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEX"
 "OFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMO"
 "REOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINC"
 "OMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUR"
 "EOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDI"
 "NTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERAL"
 "YEARSANDSAVEDCOUNTLESSLIVES")
TRIALS = int(os.environ.get("TRIALS", "20"))
L = int(os.environ.get("L", "190"))
F = int(os.environ.get("F", "50"))
PAIRS = [int(x) for x in os.environ.get("PAIRS", "3 5 8").split()]
RECIPE = os.environ.get("RECIPE", "-R 10 -S iq").split()


def run(args, text):
    return subprocess.run([E] + args, input=text, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, universal_newlines=True).stdout.strip()


def pct(a, b):
    return 100.0 * sum(1 for x, y in zip(a, b) if x == y) / len(b)


if not os.access(E, os.X_OK):
    sys.exit("error: %s not built; run 'make' first" % E)
print("pre-filter end-to-end: wildcard 2 start positions (676 keys), L=%d F=%d recipe=%s"
      % (L, F, " ".join(RECIPE)))
print("%5s  %10s  %10s  %8s" % ("plugs", "full %", "filter %", "speedup"))
for P in PAIRS:
    rng = random.Random(1)
    ff = pp = 0.0
    tf = tp = 0.0
    for _ in range(TRIALS):
        off = rng.randrange(len(CORPUS) - L + 1)
        pt = CORPUS[off:off + L]
        u = rng.choice("ABC")
        w = "".join(str(d) for d in rng.sample(range(1, 9), 3))
        r = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        g = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        lets = rng.sample(string.ascii_uppercase, 2 * P)
        pb = " ".join(lets[2 * i] + lets[2 * i + 1] for i in range(P))
        ct = run(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", pb], pt)
        gw = g[0] + ".."   # wildcard start[1:], includes the true key
        base = ["-c", "-l", "english", "-u", u, "-w", w, "-r", r, "-g", gw] + RECIPE
        t0 = time.time(); full = run(base, ct); tf += time.time() - t0
        t0 = time.time(); filt = run(base + ["-F", str(F)], ct); tp += time.time() - t0
        ff += pct(full, pt); pp += pct(filt, pt)
    print("%5d  %10.1f  %10.1f  %7.1fx" % (P, ff / TRIALS, pp / TRIALS, tf / tp if tp else 0))
