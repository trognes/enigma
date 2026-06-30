#!/usr/bin/env python3
# Premise check for the key pre-filter (-F): is a PLUGBOARD-FREE index-of-coincidence
# scan enough to rank the true rotor key? Encrypt with a known rotor key + 10-pair
# plugboard, then run a plugboard-free IC scan that wildcards the start position and
# ask whether the IC-best start is the TRUE start. Set SCAN_C=1 to instead measure a
# cheap IC *climb* (which partially recovers the stecker). Result: the raw scan has
# ~0% top-1 recall at 10 plugs, the climb ~77.5% -- which is why -F uses an IC climb.
#
#   env knobs: TRIALS (100), LENGTHS ("70 100 140 190 250"), PAIRS (10), SCAN_C
import os, random, re, string, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENIGMA = os.path.join(ROOT, "enigma")
CORPUS = ("THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEX"
          "OFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMO"
          "REOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINC"
          "OMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUR"
          "EOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDI"
          "NTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERAL"
          "YEARSANDSAVEDCOUNTLESSLIVES")
TRIALS = int(os.environ.get("TRIALS", "100"))
LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "70 100 140 190 250").split()]
PAIRS = int(os.environ.get("PAIRS", "10"))
rng = random.Random(1)


def run(args, text):
    p = subprocess.run([ENIGMA] + args, input=text, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    return p.stdout, p.stderr


def best_start(stderr):
    """Last 'G: XYZ' in the progress/config lines = the winning start position."""
    g = None
    for m in re.finditer(r'G:\s*([A-Z]{3})', stderr):
        g = m.group(1)
    return g


if not os.access(ENIGMA, os.X_OK):
    sys.exit("error: %s not built; run 'make' first" % ENIGMA)
mode = "IC climb (-c)" if os.environ.get("SCAN_C") else "plugboard-free IC scan"
print("prefilter probe: %s over wildcarded start (26^3), true board active" % mode)
print("trials=%d  pairs=%d\n" % (TRIALS, PAIRS))
print("%4s  %8s  %8s" % ("len", "top1%", "n"))
for L in LENGTHS:
    hit = 0
    for _ in range(TRIALS):
        off = rng.randrange(len(CORPUS) - L + 1)
        pt = CORPUS[off:off + L]
        u = rng.choice("ABC")
        w = "".join(str(d) for d in rng.sample(range(1, 9), 3))
        r = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        g = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        letters = rng.sample(string.ascii_uppercase, 2 * PAIRS)
        pb = " ".join(letters[2 * i] + letters[2 * i + 1] for i in range(PAIRS))
        ct, _ = run(["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", pb], pt)
        ct = ct.strip()
        scan = ["-i", "-u", u, "-w", w, "-r", r, "-g", "..."]
        if os.environ.get("SCAN_C"):
            scan.append("-c")
        _, err = run(scan, ct)
        if best_start(err) == g:
            hit += 1
    print("%4d  %8.1f  %8d" % (L, 100.0 * hit / TRIALS, TRIALS))
