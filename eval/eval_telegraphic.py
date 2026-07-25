#!/usr/bin/env python3
# Held-out evaluation: does the telegraphic table (eval/build_telegraphic_ngrams.py, from
# Appendix C) recover more of the 69 authentic messages than the prose table? For each
# message we fix the (known) rotor key, HIDE the plugboard, hill-climb it back with the
# recommended recipe, and compare the decrypt to the true plaintext -- prose vs telegraphic.
# Reports mean %-correct by length band and per-message win/loss.
import os, re, subprocess, sys, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
os.environ["ENIGMA_SEED"] = "0"
R = os.environ.get("R", "150"); T = os.environ.get("T", "4")
FILES = ["enigma-messages.txt", "enigma-army-messages-1941.txt"]
# label -> scoring language, optionally "lang@datadir" to A/B a candidate table set
# generated elsewhere (see eval/build_telegraphic_ngrams.py: OUTDIR=/tmp/x LANG_NAME=foo).
TABLES = {}
for spec in os.environ.get("TELE", "prose:german,wehrmacht:wehrmacht").split(","):
    lab, val = spec.split(":", 1); TABLES[lab] = val


def lang_args(val):
    """'lang' or 'lang@dir' -> the -l/-d arguments for it."""
    if "@" in val:
        lang, d = val.split("@", 1)
        return ["-l", lang, "-d", d if os.path.isabs(d) else os.path.join(ROOT, d)]
    return ["-l", val]

CMD = re.compile(r"-u (\S+) -w (\S+) -r (\S+) -g (\S+) -s \"([^\"]*)\"")
SCORE = re.compile(r"^\s*(-?\d+\.\d+)\s")


def parse():
    recs = []
    for fn in FILES:
        txt = open(os.path.join(HERE, fn)).read()
        for blk in re.split(r"^### Message", txt, flags=re.M)[1:]:
            mid = blk.split("\n", 1)[0].strip()
            cm = CMD.search(blk)
            if not cm:
                continue
            u, w, r, g, s = cm.groups()

            def field(name):
                m = re.search(name + r":\s+((?:.*\n)(?:             .*\n)*)", blk)
                if not m:
                    return ""
                return re.sub(r"[^A-Z-]", "", m.group(1))
            body = field("CIPHERTEXT"); pt = field("DECRYPT")
            if body and pt:
                recs.append(dict(id=mid.split("(")[0].split("--")[0].strip(),
                                 u=u, w=w, r=r, g=g, s=s, body=body, pt=pt))
    return recs


def score_of(err):
    b = None
    for ln in err.splitlines():
        m = SCORE.match(ln)
        if m:
            b = float(m.group(1))
    return b


def true_score(rc, val):
    r = subprocess.run([BIN, "-u", rc["u"], "-w", rc["w"], "-r", rc["r"], "-g", rc["g"],
                        "-s", rc["s"], "-a"] + lang_args(val), input=rc["body"].replace("-", "A"),
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, universal_newlines=True)
    return score_of(r.stderr)


def recover(rc, val):
    r = subprocess.run([BIN, "-u", rc["u"], "-w", rc["w"], "-r", rc["r"], "-g", rc["g"],
                        "-c", "-R", R, "--random", "10", "-a", "-S", "m4a10", "-J",
                        "--polish"] + lang_args(val) + ["-T", T],
                       input=rc["body"].replace("-", "A"), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    out = re.sub(r"[^A-Z]", "", r.stdout.strip().upper())
    n = ok = 0
    for c, p in zip(rc["pt"], out):
        if c == "-":
            continue
        n += 1; ok += (c == p)
    pct = 100.0 * ok / n if n else 0.0
    return pct, score_of(r.stderr)


def band(L):
    return "<40" if L < 40 else "40-69" if L < 70 else "70-119" if L < 120 else ">=120"


def main():
    recs = parse()
    print("messages: %d   R=%s   tables: %s" % (len(recs), R, list(TABLES)))
    labs = list(TABLES)
    per = {lab: [] for lab in labs}
    rows = []
    for rc in recs:
        L = len(rc["body"]); res = {}
        for lab, val in TABLES.items():
            ts = true_score(rc, val); pct, rs = recover(rc, val)
            gap = (rs - ts) if (rs is not None and ts is not None) else float("nan")
            res[lab] = (pct, gap); per[lab].append((L, pct))
        rows.append((rc["id"], L, res))
        cells = "  ".join("%s %3.0f%%" % (lab, res[lab][0]) for lab in labs)
        print("  %-16s L%-3d  %s" % (rc["id"][:16], L, cells)); sys.stdout.flush()

    print("\n=== mean %%-correct by length band ===")
    print("%-8s %6s  %s" % ("band", "n", "  ".join("%-9s" % l for l in labs)))
    for bnd in ("<40", "40-69", "70-119", ">=120"):
        sub = {lab: [p for L, p in per[lab] if band(L) == bnd] for lab in labs}
        n = len(sub[labs[0]])
        if not n:
            continue
        print("%-8s %6d  %s" % (bnd, n, "  ".join("%-9.1f" % st.mean(sub[lab]) for lab in labs)))
    print("%-8s %6d  %s" % ("ALL", len(recs),
          "  ".join("%-9.1f" % st.mean([p for _, p in per[lab]]) for lab in labs)))
    # win/loss vs prose on the discriminating band
    for lab in labs:
        if lab == "prose":
            continue
        w = l = tie = 0
        for _id, L, res in rows:
            dp = res[lab][0] - res["prose"][0]
            if dp > 1: w += 1
            elif dp < -1: l += 1
            else: tie += 1
        print("%-10s vs prose:  win %d  loss %d  tie %d  (mean d=%+.1fpp)"
              % (lab, w, l, tie, st.mean([res[lab][0] - res["prose"][0] for _, _, res in rows])))


if __name__ == "__main__":
    main()
