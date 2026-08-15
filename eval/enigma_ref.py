#!/usr/bin/env python3
"""A reference Enigma decrypt in Python, for probes that need to score many
candidate keys without paying a subprocess per key.

`enigma.cc` is the authority; this exists only so an analysis script can
decrypt tens of thousands of candidates in-process.  `selftest()` checks it
against the binary and every probe that uses it should call that first --
a silently divergent reimplementation would corrupt every number downstream.

Standard 3-rotor Wehrmacht machine only (reflectors A/B/C, wheels I-VIII),
which is all the doubling probes need.
"""
import subprocess
import sys

REFLECTOR = {
    "A": "EJMZALYXVBWFCRQUONTSPIKHGD",
    "B": "YRUHQSLDPXNGOKMIEBFZCWVJAT",
    "C": "FVPJIAOYEDRZXWGCTKUQSBNMHL",
}
ROTOR = ["EKMFLGDQVZNTOWYHXUSPAIBRCJ", "AJDKSIRUXBLHWTMCQGZNPYFVOE",
         "BDFHJLCPRTXVZNYEIWGAKMUSQO", "ESOVPZJAYQUIRHXLNFTGKDCMWB",
         "VZBRGITYUPSDNHLXAWMJQOFECK", "JPGVOUMFYQBENHZRDKASXLICTW",
         "NZJHGRCXMYSWBOUFAIVLPEKQDT", "FKQHTLXOCBJSPDZRAMEWNIUYGV"]
NOTCH = ["Q", "E", "V", "J", "Z", "MZ", "MZ", "MZ"]

_FWD = [[ord(c) - 65 for c in r] for r in ROTOR]
_REV = []
for r in _FWD:
    inv = [0] * 26
    for i, v in enumerate(r):
        inv[v] = i
    _REV.append(inv)


def _plugboard(spec):
    s = list(range(26))
    for pair in spec.split():
        a, b = ord(pair[0]) - 65, ord(pair[1]) - 65
        s[a], s[b] = b, a
    return s


def decrypt(ct, wheels, ring, start, plugs, refl="B"):
    """wheels e.g. '123' (left..right, 1-based), ring/start e.g. 'AAA'."""
    w = [int(c) - 1 for c in wheels]
    r = [ord(c) - 65 for c in ring]
    g = [ord(c) - 65 for c in start]
    st = _plugboard(plugs)
    ukw = [ord(c) - 65 for c in REFLECTOR[refl]]
    out = []
    for ch in ct:
        # stepping, double step included: the middle wheel advances on its own
        # notch as well as on the right wheel's carry.
        if chr(g[1] + 65) in NOTCH[w[1]]:
            g[1] = (g[1] + 1) % 26
            g[0] = (g[0] + 1) % 26
        elif chr(g[2] + 65) in NOTCH[w[2]]:
            g[1] = (g[1] + 1) % 26
        g[2] = (g[2] + 1) % 26
        c = st[ord(ch) - 65]
        for i in (2, 1, 0):
            o = (g[i] - r[i]) % 26
            c = (_FWD[w[i]][(c + o) % 26] - o) % 26
        c = ukw[c]
        for i in (0, 1, 2):
            o = (g[i] - r[i]) % 26
            c = (_REV[w[i]][(c + o) % 26] - o) % 26
        out.append(chr(st[c] + 65))
    return "".join(out)


def selftest(binary="./enigma", n=40, seed=99):
    """Compare against enigma.cc on random keys.  Returns (ok, detail)."""
    import random
    rng = random.Random(seed)
    A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    for t in range(n):
        ct = "".join(rng.choice(A) for _ in range(rng.randint(40, 200)))
        wl = rng.sample("12345678", 3)
        wheels = "".join(wl)
        ring = "".join(rng.choice(A) for _ in range(3))
        start = "".join(rng.choice(A) for _ in range(3))
        ls = rng.sample(A, 20)
        plugs = " ".join(ls[i] + ls[i + 1] for i in range(0, 20, 2))
        refl = rng.choice("ABC")
        got = decrypt(ct, wheels, ring, start, plugs, refl)
        want = subprocess.run(
            [binary, "-u", refl, "-w", wheels, "-r", ring, "-g", start,
             "-s", plugs], input=ct, capture_output=True,
            text=True).stdout.strip()
        if got != want:
            return False, ("trial %d  -u %s -w %s -r %s -g %s -s '%s'\n"
                           "  python %s\n  binary %s"
                           % (t, refl, wheels, ring, start, plugs, got, want))
    return True, "%d/%d identical to %s" % (n, n, binary)


if __name__ == "__main__":
    ok, detail = selftest()
    print(("OK    " if ok else "FAIL  ") + detail)
    sys.exit(0 if ok else 1)
