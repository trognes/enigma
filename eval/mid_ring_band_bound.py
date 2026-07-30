#!/usr/bin/env python3
"""Establishes the bound behind enigma.cc's `mid_ring_window`.

Shifting ring2 and start2 together leaves the RIGHT wheel's substitution alone
-- only (start2 - ring2) enters it -- so the shift moves nothing but the notch
TIMING, and the middle wheel's step schedule is time-shifted rather than
lengthened. `--ring-stride`'s refinement relies on the resulting divergence
being small: it bands the middle wheel's offset to +/- mid_ring_window around
the coarse winner's, and a bound that is too small loses keys SILENTLY.

This enumerates the real schedule (double stepping included) over every rotor
pair x 26 start1 x 26 start2 x every shift 1..25 and reports the largest
divergence found. The answer is 2: one step from the ordinary time shift, plus
one when the two schedules straddle the middle wheel's own notch and only one
of them double-steps. Two-notch right rotors (VI-VIII) change how OFTEN that
happens, not how far it reaches.

archived/PERFORMANCE.md 7.11 ran this over shifts 1..13, which was the whole
legal range while --ring-stride was capped at 13. Raising the cap to 26 admits
shifts up to 25, so the range was extended here; the bound is unchanged.

Usage: python3 eval/mid_ring_band_bound.py
"""
import numpy as np
NOTCH = {'I':'Q','II':'E','III':'V','IV':'J','V':'Z','VI':'MZ','VII':'MZ','VIII':'MZ'}
N = {k: set(ord(c)-65 for c in v) for k, v in NOTCH.items()}
L = 600

def run(w1, w2, g1, g2):
    out = np.empty(L, dtype=np.int8)
    for i in range(L):
        if g1 in N[w1]:
            g1 = (g1+1) % 26
        elif g2 in N[w2]:
            g1 = (g1+1) % 26
        g2 = (g2+1) % 26
        out[i] = g1
    return out

worst, where = 0, None
for w1 in NOTCH:
    for w2 in NOTCH:
        for g1 in range(26):
            A = np.stack([run(w1, w2, g1, g2) for g2 in range(26)]).astype(np.int16)
            for d in range(1, 26):
                s = (A - np.roll(A, -d, axis=0)) % 26
                dv = np.minimum(s, 26-s).max()
                if dv > worst:
                    worst, where = int(dv), (w1, w2, g1, d)
print("max middle-wheel divergence over shifts 1..25, L=%d: %d %s" % (L, worst, where))
