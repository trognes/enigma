#!/usr/bin/env python3
# Does the --crib-rerank finisher help on top of the telegraphic tables? For each of the 69
# authentic messages: fix the rotor key, hide the plugboard, hill-climb it back under the
# telegraphic tables WITHOUT cribs vs WITH cribs (lambda sweep), mean %-letters-correct.
# The crib finisher can only help where the true board is among the converged restarts but
# out-ranked on n-grams; this measures the net effect (including false-positive losses).
import os, re, subprocess, sys, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, os.pardir)
BIN = os.path.join(ROOT, "enigma")
os.environ["ENIGMA_SEED"] = "0"
R = os.environ.get("R", "150"); T = os.environ.get("T", "4")
CRIB = os.path.join(ROOT, "cribs", "german-hgnord.txt")
FILES = ["enigma-messages.txt", "enigma-army-messages-1941.txt"]
# label -> crib weight (None = no crib)
CONF = [("tele", None)] + [("crib" + w, float(w)) for w in os.environ.get("LAM", "0.5,1.0").split(",")]

CMD = re.compile(r"-u (\S+) -w (\S+) -r (\S+) -g (\S+) -s \"([^\"]*)\"")


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
                return re.sub(r"[^A-Z-]", "", m.group(1)) if m else ""
            body, pt = field("CIPHERTEXT"), field("DECRYPT")
            if body and pt:
                recs.append(dict(id=mid.split("(")[0].split("--")[0].strip(),
                                 u=u, w=w, r=r, g=g, body=body, pt=pt))
    return recs


def recover(rc, lam):
    args = [BIN, "-u", rc["u"], "-w", rc["w"], "-r", rc["r"], "-g", rc["g"], "-c", "-R", R,
            "--random", "10", "-a", "-S", "m4a10", "-J", "--polish",
            "-l", "wehrmacht", "-T", T]
    if lam is not None:
        args += ["--crib-rerank", CRIB, "--crib-weight", str(lam)]
    out = subprocess.run(args, input=rc["body"].replace("-", "A"), stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, universal_newlines=True).stdout.strip().upper()
    out = re.sub(r"[^A-Z]", "", out)
    n = ok = 0
    for c, p in zip(rc["pt"], out):
        if c == "-":
            continue
        n += 1; ok += (c == p)
    return 100.0 * ok / n if n else 0.0


def band(L):
    return "<40" if L < 40 else "40-69" if L < 70 else "70-119" if L < 120 else ">=120"


def main():
    recs = parse()
    labs = [c[0] for c in CONF]
    print("crib finisher eval  R=%s  messages=%d  configs=%s" % (R, len(recs), labs))
    per = {l: [] for l in labs}
    rows = []
    for rc in recs:
        L = len(rc["body"]); res = {}
        for lab, lam in CONF:
            res[lab] = recover(rc, lam); per[lab].append((L, res[lab]))
        rows.append((rc["id"], L, res))
        if abs(res[labs[1]] - res["tele"]) > 1 or abs(res[labs[-1]] - res["tele"]) > 1:
            print("  %-14s L%-3d  %s" % (rc["id"][:14], L,
                  "  ".join("%s %3.0f" % (l, res[l]) for l in labs))); sys.stdout.flush()
    print("\n=== mean %%-correct by band ===")
    for bnd in ("<40", "40-69", "70-119", ">=120"):
        sub = {l: [p for L, p in per[l] if band(L) == bnd] for l in labs}
        n = len(sub[labs[0]])
        if n:
            print("%-8s n=%-3d %s" % (bnd, n, "  ".join("%s %5.1f" % (l, st.mean(sub[l])) for l in labs)))
    print("%-8s n=%-3d %s" % ("ALL", len(recs),
          "  ".join("%s %5.1f" % (l, st.mean([p for _, p in per[l]])) for l in labs)))
    for lab, lam in CONF[1:]:
        d = [res[lab] - res["tele"] for _, _, res in rows]
        w = sum(x > 1 for x in d); ls = sum(x < -1 for x in d)
        print("%-8s vs tele: win %d loss %d  mean d=%+.1fpp" % (lab, w, ls, st.mean(d)))


if __name__ == "__main__":
    main()
