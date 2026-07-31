#!/usr/bin/env python3
"""Why --ring-stride's refinement needs the width it does: the ring2 score geometry.

The end-to-end harnesses (eval/ring_stride_probe.py, eval/ring_stride_wehrmacht_probe.py,
eval/ring_stride_window_probe.py) measure what the shipped flag RECOVERS. This one
measures the landscape underneath that, which is what explains the width: how far the
coarse winner's ring2 sits from the truth, and whether its start2 needs re-searching.

The right rotor's substitution depends only on the offset (start - ring); the ring
enters the machine a second time only by moving where the TURNOVER falls
(setup_mapping reads the absolute position for the notch, with no ring term). So a
candidate with the right offset but a ring wrong by d decrypts everything except the
d positions per 26-character cycle where one machine has stepped the middle rotor and
the other has not. The score along the offset-preserving diagonal is therefore a
smooth tent peaking at d = 0 -- and how sharp that tent is decides how far the coarse
argmax can stray.

Method (one trial):
  * take an authentic Wehrmacht plaintext, encipher it under a random key,
  * fix reflector / wheel order / left+middle ring+start at the truth and score all
    26x26 (right ring, right start) candidates -- by default with no plugboard, so
    this is a plain rotor-key scan (--plugs N applies a known N-pair board to both
    the encipher and the scoring),
  * for stride s, take the argmax over the coarse grid {ring = 0, s, 2s, ...} x
    {all 26 starts},
  * report the circular ring distance from that coarse winner to the true ring, and
    whether a refinement limited to rings within k of the winner recovers the true
    (ring, start) -- either re-scanning all 26 starts (k= columns) or holding the
    coarse winner's offset (o= columns).

SCOPE, and why the binary-driven harnesses stay authoritative. This pins the rest of
the key at the truth, so it isolates the right wheel and NOTHING else; the shipped
refinement additionally re-opens ring1/start1, because the coarse winner's middle
wheel can itself have drifted (archived/PERFORMANCE.md 7.11's reproducible case). A
coarse winner that is off for that reason is not a distance this probe can see, so
read its distances as the best case -- the floor on how wide the refinement must be,
not the answer.

The Enigma model mirrors enigma.cc (rotor/notch/reflector tables, the (start - ring)
offset, absolute-position turnover with the double step) and is checked against the
./enigma binary at startup. Requires numpy.

Usage:  python3 eval/ring_stride_geometry_probe.py [--trials N] [--lengths 100,200] \
                                          [--model quad|all] [--plugs N] [--seed S]

Recorded output: eval/results-ring-stride-geometry.tsv.
"""

import argparse
import os
import random
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NGRAMS = os.path.join(ROOT, "ngrams")
BINARY = os.path.join(ROOT, "enigma")

# Rotor indices mirror enigma.cc's, minus the Norway block: 0-7 = I-VIII,
# 8/9 = Beta/Gamma (M4's static Greek wheel, which never steps -- hence the
# empty notch strings).
ROTOR = [
    "EKMFLGDQVZNTOWYHXUSPAIBRCJ",  # I
    "AJDKSIRUXBLHWTMCQGZNPYFVOE",  # II
    "BDFHJLCPRTXVZNYEIWGAKMUSQO",  # III
    "ESOVPZJAYQUIRHXLNFTGKDCMWB",  # IV
    "VZBRGITYUPSDNHLXAWMJQOFECK",  # V
    "JPGVOUMFYQBENHZRDKASXLICTW",  # VI
    "NZJHGRCXMYSWBOUFAIVLPEKQDT",  # VII
    "FKQHTLXOCBJSPDZRAMEWNIUYGV",  # VIII
    "LEYJVCNIXWPBQMDRTAKZGFUHOS",  # Beta
    "FSOKANUERHMBTIYCWLQPZXVGJD",  # Gamma
]
# VI-VIII carry TWO notches, so they turn the middle wheel twice per revolution.
NOTCH = ["Q", "E", "V", "J", "Z", "MZ", "MZ", "MZ", "", ""]
BETA, GAMMA = 8, 9
REFLECTOR = {
    "A": "EJMZALYXVBWFCRQUONTSPIKHGD",
    "B": "YRUHQSLDPXNGOKMIEBFZCWVJAT",
    "C": "FVPJIAOYEDRZXWGCTKUQSBNMHL",
    "b": "ENKQAUYWJICOPBLMDXZVFTHRGS",   # M4 thin
    "c": "RDOBJNTKVEHMLFCWZAXGYIPSUQ",   # M4 thin
}

A = ord("A")


def num(s):
    return np.frombuffer(s.encode(), dtype=np.uint8).astype(np.int64) - A


def txt(v):
    return "".join(chr(int(c) + A) for c in v)


# ---------------------------------------------------------------- machine ---

def rotor_tables(w):
    """fwd[o][x] / rev[o][x]: rotor w applied at offset o = start - ring."""
    fwd0 = num(ROTOR[w])
    rev0 = np.argsort(fwd0)
    o = np.arange(26)[:, None]
    x = np.arange(26)[None, :]
    fwd = (fwd0[(x + o) % 26] - o) % 26
    rev = (rev0[(x + o) % 26] - o) % 26
    return fwd, rev


def effective_reflector(refl, greek=None, greek_offset=0):
    """The reflector the rotor stack sees. For M4 the Greek wheel is STATIC, so
    it folds into the reflector as greek . thin . greek^-1 at its fixed offset
    (start - ring) mod 26 -- mirrors enigma.cc's set_effective_reflector(),
    which is why the engine stays a 3-stepping-rotor machine in M4 mode."""
    u = num(REFLECTOR[refl])
    if greek is None:
        return u
    gf, gr = rotor_tables(greek)
    o = greek_offset % 26
    return gr[o][u[gf[o][np.arange(26)]]]


def subst_array(wheels, refl, greek=None, greek_offset=0):
    """S[o0][o1][o2][x] -- the rotor-stack + reflector substitution, as in
    enigma.cc's precompute()."""
    f = [rotor_tables(w) for w in wheels]
    u = effective_reflector(refl, greek, greek_offset)
    x = np.arange(26)
    # core[o0][o1][x] = rev1(rev0(U(fwd0(fwd1(x))))) -- the left+middle stack
    core = np.empty((26, 26, 26), dtype=np.int64)
    for o0 in range(26):
        for o1 in range(26):
            y = f[1][0][o1][x]
            y = f[0][0][o0][y]
            y = u[y]
            y = f[0][1][o0][y]
            y = f[1][1][o1][y]
            core[o0, o1] = y
    S = np.empty((26, 26, 26, 26), dtype=np.uint8)
    for o2 in range(26):
        y = f[2][0][o2][x]                # fwd right
        S[:, :, o2, :] = f[2][1][o2][core[:, :, y]]
    return S


def positions(g, wheels, n):
    """The stepped rotor positions for each of n characters (absolute, no ring
    term in the turnover -- mirrors setup_mapping)."""
    notch = [set(num(NOTCH[w]).tolist()) for w in wheels]
    g0, g1, g2 = int(g[0]), int(g[1]), int(g[2])
    out = np.empty((n, 3), dtype=np.int64)
    for i in range(n):
        if g1 in notch[1]:
            g0 = (g0 + 1) % 26
            g1 = (g1 + 1) % 26
        elif g2 in notch[2]:
            g1 = (g1 + 1) % 26
        g2 = (g2 + 1) % 26
        out[i] = (g0, g1, g2)
    return out


def plugboard(rng, npairs):
    """A random involution with npairs plugs (identity when npairs == 0)."""
    p = np.arange(26)
    free = list(range(26))
    rng.shuffle(free)
    for i in range(npairs):
        a, b = free[2 * i], free[2 * i + 1]
        p[a], p[b] = b, a
    return p


def crypt(text, wheels, refl, ring, start, plug=None, greek=None,
          greek_offset=0):
    S = subst_array(wheels, refl, greek, greek_offset)
    p = positions(start, wheels, len(text))
    o = (p - np.asarray(ring)[None, :]) % 26
    c = num(text)
    if plug is None:
        return txt(S[o[:, 0], o[:, 1], o[:, 2], c])
    return txt(plug[S[o[:, 0], o[:, 1], o[:, 2], plug[c]]])


# ---------------------------------------------------------------- scoring ---

# Mirror load_counts()'s explicit UINT32_MAX clamp. The shipped tables no longer
# reach it (the wehrmacht generator blow-up was fixed -- archived/PERFORMANCE.md 6.17);
# kept so an external table is loaded the way the binary would load it.
SATURATE = True


def load_counts(n, lang):
    t = np.zeros(26 ** n, dtype=np.int64 if SATURATE else np.float64)
    path = os.path.join(NGRAMS, "%s_%s.txt" % (lang, {1: "monograms", 2: "bigrams",
                                                      3: "trigrams", 4: "quadgrams"}[n]))
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            f = line.split()
            if len(f) != 2:
                continue
            g = f[0].upper()
            if len(g) != n or not all("A" <= ch <= "Z" for ch in g):
                continue          # accented grams are folded by the tool; skipped here
            idx = 0
            for ch in g:
                idx = idx * 26 + (ord(ch) - A)
            c = int(f[1])
            if SATURATE and c > 0xFFFFFFFF:
                c = 0xFFFFFFFF   # what sscanf("%u") stores for an out-of-range count
            t[idx] += c
    return t


def jlog(t):
    tot = float(t.sum())
    return np.log10(np.maximum(t, 1).astype(np.float64) / tot)


def score_table(model, lang):
    """quad: log10 joint quadgram probability (the -q model).
       all:  the -a log-linear symmetric fold, weights (1, .6, .3, .15)."""
    q = jlog(load_counts(4, lang)).reshape(26, 26, 26, 26)
    if model == "quad":
        return q.astype(np.float32)
    t = jlog(load_counts(3, lang)).reshape(26, 26, 26)
    b = jlog(load_counts(2, lang)).reshape(26, 26)
    m = jlog(load_counts(1, lang))
    v = q.copy()
    v += 0.6 * (t[:, :, :, None] + t[None, :, :, :]) / 2.0
    v += 0.3 * (b[:, :, None, None] + b[None, :, :, None] + b[None, None, :, :]) / 3.0
    v += 0.15 * (m[:, None, None, None] + m[None, :, None, None]
                 + m[None, None, :, None] + m[None, None, None, :]) / 4.0
    return v.astype(np.float32)


def score(pt, tab):
    """pt: (k, n) decoded texts -> (k,) per-symbol score, as the tool reports."""
    idx = pt[:, :-3].astype(np.int64)
    idx = idx * 26 + pt[:, 1:-2]
    idx = idx * 26 + pt[:, 2:-1]
    idx = idx * 26 + pt[:, 3:]
    return tab.reshape(-1)[idx].sum(axis=1) / pt.shape[1]


# ------------------------------------------------------------------ trial ---

def grid_scores(cipher, wheels, refl, ring, start, tab, plug=None):
    """Score every (right ring, right start) with the rest of the key at truth.
    Returns a (26, 26) array indexed [r2][g2]."""
    S = subst_array(wheels, refl)
    c = num(cipher)
    n = len(c)
    out = np.empty((26, 26), dtype=np.float64)
    for g2 in range(26):
        p = positions([start[0], start[1], g2], wheels, n)
        o01 = (p[:, :2] - np.asarray(ring[:2])[None, :]) % 26
        r2 = np.arange(26)[:, None]
        o2 = (p[None, :, 2] - r2) % 26                       # (26, n)
        if plug is None:
            pt = S[o01[None, :, 0], o01[None, :, 1], o2, c[None, :]]
        else:
            pt = plug[S[o01[None, :, 0], o01[None, :, 1], o2, plug[c][None, :]]]
        out[:, g2] = score(pt, tab)
    return out


def circdist(a, b):
    d = abs(int(a) - int(b)) % 26
    return min(d, 26 - d)


def corpus_texts(minlen):
    """The DECRYPT blocks of the authentic message files: real telegraphic
    Wehrmacht German."""
    texts = []
    for name in ("enigma-messages.txt", "enigma-army-messages-1941.txt"):
        path = os.path.join(HERE, name)
        if not os.path.exists(path):
            continue
        cur = None
        for line in open(path, encoding="utf-8"):
            if line.startswith("DECRYPT:"):
                cur = [line.split(":", 1)[1]]
            elif cur is not None and line.startswith((" ", "\t")) and line.strip():
                cur.append(line)
            elif cur is not None:
                s = re.sub("[^A-Z]", "", "".join(cur).upper())
                if len(s) >= minlen:
                    texts.append(s)
                cur = None
    return texts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--lengths", default="150,250")
    ap.add_argument("--model", default="all", choices=("quad", "all"))
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--strides", default="2,3,5")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--plugs", type=int, default=0,
                    help="apply an N-pair plugboard to both encipher and score "
                         "(the 'plugboard already known/recovered' condition)")
    ap.add_argument("--no-saturate", action="store_true",
                    help="keep counts above UINT32_MAX instead of clamping them "
                         "the way load_counts() does")
    args = ap.parse_args()
    global SATURATE
    SATURATE = not args.no_saturate

    lengths = [int(x) for x in args.lengths.split(",")]
    strides = [int(x) for x in args.strides.split(",")]
    texts = corpus_texts(max(lengths))
    if not texts:
        sys.exit("no corpus texts long enough")
    selftest()
    tab = score_table(args.model, args.lang)
    rng = random.Random(args.seed)

    print("# model=%s lang=%s trials=%d texts=%d saturate=%d plugs=%d" %
          (args.model, args.lang, args.trials, len(texts), SATURATE, args.plugs))
    print("# solvable  = the true (ring,start) is the argmax of the full 26x26 grid")
    print("# dist      = circular ring distance from the coarse winner to the truth")
    print("# k=N       = the refinement recovers the truth when it re-scans the rings")
    print("#             within N of the coarse winner x all 26 starts (k=12 == all)")
    print("# o=N       = same, but keeping the coarse winner's offset (2N+1 candidates)")
    ks = list(range(1, 13))
    hdr = (["len", "stride", "n", "solvable", "d=0", "d<=s/2", "dmax"]
           + ["k=%d" % k for k in ks] + ["o=%d" % k for k in ks])
    print("\t".join(hdr))

    for L in lengths:
        pool = [t for t in texts if len(t) >= L]
        for s in strides:
            rng = random.Random(args.seed)      # same trials for every stride
            dists = []
            ok = {k: 0 for k in ks}
            oko = {k: 0 for k in ks}
            glob = 0
            for tr in range(args.trials):
                pt = pool[tr % len(pool)]
                off = rng.randrange(0, max(1, len(pt) - L + 1))
                pt = pt[off:off + L]
                wheels = rng.sample(range(5), 3)
                refl = "B"
                ring = [rng.randrange(26) for _ in range(3)]
                start = [rng.randrange(26) for _ in range(3)]
                plug = plugboard(rng, args.plugs) if args.plugs else None
                ct = crypt(pt, wheels, refl, ring, start, plug)
                g = grid_scores(ct, wheels, refl, ring, start, tab, plug)
                truth = (ring[2], start[2])
                # if the truth is not even the full-grid argmax, no refinement
                # scope can win; such trials are excluded from the k/o columns
                solvable = np.unravel_index(int(np.argmax(g)), g.shape) == truth
                glob += solvable
                grid = [r for r in range(26) if r % s == 0]
                sub = g[grid, :]
                ci = np.unravel_index(int(np.argmax(sub)), sub.shape)
                cr, cg = grid[ci[0]], int(ci[1])
                dists.append(circdist(cr, ring[2]))
                if not solvable:
                    continue
                for k in ks:
                    rings = [r for r in range(26) if circdist(r, cr) <= k]
                    sg = g[rings, :]
                    bi = np.unravel_index(int(np.argmax(sg)), sg.shape)
                    if (rings[bi[0]], int(bi[1])) == truth:
                        ok[k] += 1
                    starts = [(r + cg - cr) % 26 for r in rings]    # offset kept
                    v = [g[r, gg] for r, gg in zip(rings, starts)]
                    j = int(np.argmax(v))
                    if (rings[j], starts[j]) == truth:
                        oko[k] += 1
            d = np.array(dists)
            n = max(glob, 1)
            row = [L, s, args.trials, "%d%%" % round(100 * glob / args.trials),
                   "%d%%" % round(100 * (d == 0).mean()),
                   "%d%%" % round(100 * (d <= (s // 2)).mean()), int(d.max())]
            row += ["%d%%" % round(100 * ok[k] / n) for k in ks]
            row += ["%d%%" % round(100 * oko[k] / n) for k in ks]
            print("\t".join(str(x) for x in row))
            sys.stdout.flush()


PLAIN = ("ANXPANZXGRUPPEXVIERXSIEGFRIEDSIEGFRIEDTONIXDIVXSTEHTSEITXEIN"
         "SZWOXSIEBENXEINSEINSNULLNULLXUHRMITANFAENGENAMUNTERKUNFTSRAU")


def run_binary(argv, ct):
    r = subprocess.run([BINARY] + argv, input=ct, capture_output=True,
                       text=True,
                       env=dict(os.environ, ENIGMA_DATA=NGRAMS))
    return re.sub("[^A-Z]", "", r.stdout.upper())


def selftest():
    """Check the model against the ./enigma binary -- once per machine variant
    the probes can now generate, since each exercises a different part of the
    model: wheels I-V, a two-notch right wheel (VI-VIII step the middle wheel
    twice a revolution), and M4's folded Greek wheel."""
    if not os.path.exists(BINARY):
        sys.exit("build ./enigma first (make)")
    cases = [
        # label, wheels, refl, ring, start, greek, greek ring/start, argv
        ("I-V", [3, 1, 2], "B", "GTO", "SDV", None, None,
         ["-u", "B", "-w", "423", "-r", "GTO", "-g", "SDV"]),
        # VIII on the right: two notches (M and Z), so the middle wheel turns
        # over twice per revolution -- the case the derivation must not assume
        # away. VI in the middle adds a second two-notch wheel and its own
        # double step.
        ("two-notch", [0, 5, 7], "C", "KQZ", "BMX", None, None,
         ["-u", "C", "-w", "168", "-r", "KQZ", "-g", "BMX"]),
        # M4: thin reflector b + Gamma at a non-trivial offset, folded into the
        # effective reflector. Only (start - ring) of the Greek wheel is
        # identifiable, so the binary is given ring A and start = the offset.
        ("m4", [2, 0, 6], "b", "AXF", "RTU", GAMMA, 9,
         ["-4", "-u", "b", "-w", "G317", "-r", "AAXF", "-g", "JRTU"]),
    ]
    for label, wheels, refl, ring, start, greek, goff, argv in cases:
        ct = crypt(PLAIN, wheels, refl,
                   [ord(x) - A for x in ring], [ord(x) - A for x in start],
                   greek=greek, greek_offset=(goff or 0))
        got = run_binary(argv, ct)
        if got != PLAIN:
            sys.exit("selftest FAILED (%s)\n  want %s\n  got  %s"
                     % (label, PLAIN, got))
    # M4's documented backward compatibility: thin b + Beta at ring/pos A is the
    # standard reflector B, so the fold must reproduce it exactly.
    if txt(effective_reflector("b", BETA, 0)) != REFLECTOR["B"]:
        sys.exit("selftest FAILED: b + Beta@A is not reflector B")
    if txt(effective_reflector("c", GAMMA, 0)) != REFLECTOR["C"]:
        sys.exit("selftest FAILED: c + Gamma@A is not reflector C")
    sys.stderr.write("selftest ok (model agrees with ./enigma: %s)\n"
                     % ", ".join(c[0] for c in cases))


if __name__ == "__main__":
    main()
