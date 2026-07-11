#!/usr/bin/env python3
# First real-traffic test: for each authentic message, FIX the rotor key, HIDE the plugboard,
# and hill-climb it back. Compare -a (weighted, -S m4a10) vs -q (quad, -S m4q10) at matched
# budget. Metric: %-correct vs the true-plugboard decrypt (at known, non-garbled positions).
import os, re, subprocess
os.environ["ENIGMA_DATA"] = "ngrams"; os.environ["ENIGMA_SEED"] = "0"
BIN = "./enigma"
DB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "enigma-messages.txt")
R = os.environ.get("R", "256")

# --- parse the verified database ---
recs = []
for block in open(DB).read().split("### Message No. ")[1:]:
    lines = block.splitlines()
    no = lines[0].split()[0]
    f, cur = {}, None
    for ln in lines[1:]:
        m = re.match(r"^([A-Z]+):\s+(.*)$", ln)
        if m:
            cur = m.group(1); f[cur] = m.group(2)
        elif cur in ("CIPHERTEXT", "DECRYPT") and ln.startswith(" " * 13):
            f[cur] += ln.strip()
    recs.append(dict(no=no, refl=f["REFLECTOR"].strip(),
                     wheels=re.search(r"-w (\d+)", f["WHEELS"]).group(1),
                     ring=f["RING"].strip(), start=f["START"].strip(),
                     cipher=f["CIPHERTEXT"].strip(), raw=f["DECRYPT"].strip()))


def crack(rc, model):
    ct = rc["cipher"].replace("-", "A")   # placeholder keeps stepping in sync
    args = [BIN, "-u", rc["refl"], "-w", rc["wheels"], "-r", rc["ring"], "-g", rc["start"],
            "-c", "-R", R, "-S", "m4%s10" % model, "-J", "--gainfix-best3",
            "-" + model, "-l", "german", "-T", "8", "-e", "0"]
    out = subprocess.run(args, input=ct, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         universal_newlines=True).stdout.strip().upper()
    raw = rc["raw"]
    known = [(o, r) for o, r in zip(out, raw) if r != "-"]
    correct = sum(1 for o, r in known if o == r)
    return 100.0 * correct / len(known) if known else 0.0


print("Real-traffic plugboard recovery (rotor key fixed, board hidden), R=%s, -l german\n" % R)
print("%-5s %5s %8s %8s %8s" % ("No.", "len", "-q %", "-a %", "a-q"))
sq = sa = 0.0
for rc in recs:
    n = len([c for c in rc["raw"] if c != "-"])
    pq, pa = crack(rc, "q"), crack(rc, "a")
    sq += pq; sa += pa
    print("%-5s %5d %8.1f %8.1f %+8.1f" % (rc["no"], n, pq, pa, pa - pq))
print("-" * 40)
print("%-5s %5s %8.1f %8.1f %+8.1f" % ("mean", "", sq/len(recs), sa/len(recs), (sa-sq)/len(recs)))
