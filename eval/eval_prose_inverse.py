#!/usr/bin/env python3
# Inverse control for eval_telegraphic.py: on PROSE German plaintext, do the telegraphic
# tables (ngrams-telegraphic/, built for real telegraphic traffic) UNDERPERFORM the prose
# tables (ngrams/)? They should -- telegraphic orthography (X=space, Q=ch, spelled numbers)
# is a domain mismatch for prose. This makes the "real traffic only" scope claim measured.
#
# Method mirrors eval_telegraphic.py's recovery: random prose-German excerpt + random rotor
# key + random 10-plug board, encipher, then fix the rotor key, hide the plugboard, and
# hill-climb it back with the recommended recipe -- prose vs telegraphic, mean %-correct.
import os, re, subprocess, random, string, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
os.environ["ENIGMA_SEED"] = "0"
R = os.environ.get("R", "150"); T = os.environ.get("T", "4")
TRIALS = int(os.environ.get("TRIALS", "25"))
LENGTHS = [int(x) for x in os.environ.get("LENGTHS", "40 70 100 140 190").split()]
SEED = int(os.environ.get("SEED", "1"))
TABLES = {"prose": "ngrams", "telegraphic": "ngrams-telegraphic"}

# Prose German passage (same text the crackquality harness uses for -l german).
GERMAN = ("DIEENIGMAMASCHINEWURDEIMZWEITENWELTKRIEGVONDERDEUTSCHENWEHRMACHTVERWENDETUMGEHEIME"
          "NACHRICHTENZUVERSCHLUESSELNABERDIEALLIIERTENKONNTENDENGEHEIMENCODETROTZDEMBRECHENWEIL"
          "DIEDEUTSCHENOFTDIEGLEICHENFLOSKELNVERWENDETENUNDWEILVIELEBEDIENERIMMERWIEDERDIESELBEN"
          "FEHLERMACHTENDIEPOLNISCHENUNDBRITISCHENMATHEMATIKERBAUTENMASCHINENUMDIETAEGLICHENSCHL"
          "UESSELZUFINDENUNDLASENSODIEGEHEIMENFUNKSPRUECHEDESFEINDESMITUNDVERKUERZTENDADURCHDENK"
          "RIEGUMMEHREREJAHREUNDRETTETENVIELETAUSENDMENSCHENLEBEN")

rng = random.Random(SEED)
LET = string.ascii_uppercase


def rand_key():
    ws = rng.sample("12345", 3)
    r = "".join(rng.choice(LET) for _ in range(3))
    g = "".join(rng.choice(LET) for _ in range(3))
    letters = list(LET); rng.shuffle(letters)
    pb = " ".join(letters[2 * i] + letters[2 * i + 1] for i in range(10))
    return "B", "".join(ws), r, g, pb


def enc(u, w, r, g, pb, plain):
    out = subprocess.run([BIN, "-u", u, "-w", w, "-r", r, "-g", g, "-s", pb],
                         input=plain, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         universal_newlines=True).stdout.strip().upper()
    return re.sub(r"[^A-Z]", "", out)


def recover(u, w, r, g, ct, d):
    out = subprocess.run([BIN, "-u", u, "-w", w, "-r", r, "-g", g, "-c", "-R", R,
                          "--random", "10", "-a", "-S", "m4a10", "-J", "--gainfix-best3",
                          "-l", "german", "-d", os.path.join(ROOT, d), "-T", T],
                         input=ct, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         universal_newlines=True).stdout.strip().upper()
    return re.sub(r"[^A-Z]", "", out)


def pct(a, b):
    n = min(len(a), len(b))
    return 100.0 * sum(a[i] == b[i] for i in range(n)) / n if n else 0.0


def main():
    print("PROSE-German inverse control  R=%s  TRIALS=%d  SEED=%d" % (R, TRIALS, SEED))
    print("%-5s %6s  %s" % ("len", "n", "  ".join("%-11s" % l for l in TABLES)))
    agg = {lab: [] for lab in TABLES}
    for L in LENGTHS:
        per = {lab: [] for lab in TABLES}
        for _ in range(TRIALS):
            off = rng.randint(0, len(GERMAN) - L)
            plain = GERMAN[off:off + L]
            u, w, r, g, pb = rand_key()
            ct = enc(u, w, r, g, pb, plain)
            for lab, d in TABLES.items():
                p = pct(plain, recover(u, w, r, g, ct, d))
                per[lab].append(p); agg[lab].append(p)
        print("%-5d %6d  %s" % (L, TRIALS, "  ".join("%-11.1f" % st.mean(per[lab]) for lab in TABLES)))
        sys.stdout.flush()
    print("%-5s %6d  %s" % ("ALL", sum(len(agg[list(TABLES)[0]]) for _ in [0]) // 1,
          "  ".join("%-11.1f" % st.mean(agg[lab]) for lab in TABLES)))
    dp = st.mean(agg["telegraphic"]) - st.mean(agg["prose"])
    print("\ntelegraphic - prose on PROSE German: %+.1f pp  (expected negative -> domain mismatch)" % dp)


if __name__ == "__main__":
    main()
