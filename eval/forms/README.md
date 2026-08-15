# Message forms

Scans of the original *Fern-Funk-Blink-Spruch* forms. The first two are the
09.09.1941 messages whose day key was recovered from ciphertext here (see
`../MODERN_BREAKING_NOTES.md` §5j); the other two are **unbroken** challenge
messages (`../enigma-challenge-1941.txt`).

| file | Spruch Nr | Kenngruppe | letters | status |
|---|---:|---|---:|---|
| `ALVPM-09091941-038-out-nf.pdf` | 38 | ALVPM | 177 | broken, §5j |
| `ALRHG-09091941-039-out-nf.pdf` | 39 | ALRHG | 116 | broken, §5j |
| `FMNGI-31071941-205-out-nf.pdf` | 205 | FMNGI | 63 | unbroken (Nr 285) |
| `MVUEH-10071941-088-061-out-nf.pdf` | 88/61 | MVUEH | 87 | unbroken (Nr 172) |

The first two are outgoing, dated 9.9.41, *befördert* 0913, message time (0850),
from Nachschub to SS-T.Div. The **length printed on each form** is the reason
they are kept: it is an independent check that a transcription dropped or added
nothing, and all four match.

## The two unbroken forms

They carry something the transcriptions did not: the **indicator**, the
operator's enciphered message key. That is worth more than it looks. A candidate
day key can now be tested in a *single decrypt* — set the machine to the first
group, decipher the second, and that is the message start — instead of sweeping
all 17 576 starts, and the two answers must agree.

Used immediately on MVUEH, which the records describe as not breaking on the
10.07.1941 day key. `GTA KCI` derives start `USU` at that key and `SED` at the
Nr-173 network key (same wheels, ring `MRP`); neither decrypts, and an
exhaustive sweep over all starts on both keys tops out at margin **+0.5 sd**,
i.e. nothing. The different-network reading therefore stands, now on a direct
test rather than an absence of results.

FMNGI has no 31.07.1941 day key at all, so nothing about it can be checked by
decryption.

## Readings that differ, and why they were NOT applied

Reading these two scans letter by letter produced disagreements with the stored
transcriptions — **7 letters in FMNGI and 13 in MVUEH**, listed in full so a
later attack can try them. Positions are 1-based over the whole string,
Kenngruppe included.

FMNGI (7 letters, 5 groups):

| pos | group | stored | read here | confidence |
|---:|---|---|---|---|
| 16 | 4 `MNMNI` | `M` | `R`/`P` | high — bowl + descender, not an `m` |
| 20 | 4 `MNMNI` | `I` | `E` | high — the same ε used in `EKFHE` |
| 23 | 5 `LIFZB` | `F` | `V` | medium — no descender |
| 44 | 9 `XIQEX` | `E` | `I` | medium — dotted |
| 47 | 10 `CSAQP` | `S` | `H` | medium — looped, like `EKFHE`'s `h` |
| 50 | 10 `CSAQP` | `P` | `R` | low |
| 60 | 12 `KFBIK` | `K` | `X` | medium |

MVUEH (13 letters, 7 groups; faint pencil throughout, so every one is low or
medium confidence):

| pos | group | stored | read here |
|---:|---|---|---|
| 7, 8 | 2 `IDEVS` | `D`, `E` | `C`, `R` |
| 11 | 3 `ARMCC` | `A` | `O` |
| 16 | 4 `NQTAT` | `N` | `W` |
| 23, 25 | 5 `YEVFC` | `V`, `C` | `K`, `X` |
| 29, 30 | 6 `DBZGG` | `G`, `G` | `J`, `F` |
| 48 | 10 `WURRT` | `R` | `L` |
| 52, 54, 55 | 11 `BZCVG` | `Z`, `V`, `G` | `R`, `U`, `F` |
| 85 | 17 `DNXJF` | `F` | `Y` |

Confirmed identical at high magnification: FMNGI `FGROV FDIVQ QVNQW LGBLJ VRLEB
EKFHE MCF`, and MVUEH `MVUEH SMXWL PSYWZ YTCBS ODVJU SLSOO MJQJZ SXSEB ZPEYM TC`
— ten of MVUEH's eighteen groups.

**None is applied**, for a reason worth stating rather than out of caution.
Neither message has a working key, so a proposed correction cannot be arbitrated
by decryption — which is exactly what made the ALVPM corrections safe and makes
these unsafe. And **every check that was available went the stored
transcription's way**:

- **The form's own letter count arbitrates MVUEH's tail, and the stored text
  wins.** Read at low magnification the last row looked like `DURXJ` + `YTC`,
  which totals **88** letters against the form's stated 87. Enlarged, it is
  `DNXJ?` + `TC` — exactly **87**, and `TC` as stored. An arithmetic check, not
  a judgement about handwriting.
- **Magnification kept overturning this reading, never the stored one.** Four
  MVUEH groups read wrong at low resolution — `YTCBS`, `ODVJU`, `DNXJ`, `TC` —
  and two FMNGI ones, `FDIVQ` and `LGBLJ`; enlarging confirmed the stored text
  in all six. Not once did enlarging confirm a disagreement.
- On the groups legible beyond doubt the two agree exactly: ten of MVUEH's
  eighteen groups and seven of FMNGI's thirteen.

A transcription that survives every checkable test should not be overwritten on
the strength of the unfalsifiable ones. The remaining disagreements sit almost
entirely in the faintest pencil, which is also where this reading has already
been shown to fail.

They are recorded because they are the cheapest thing to try if either message
is ever attacked and fails — 20 candidate letters, and one of them may be the
one that matters.

## The 09.09.1941 pair

Those two scans settled which "garbles" were 1941's and which were ours — five
of ALVPM's six were transcription errors, corrected against these images, and
one `ENHANCEMENTS.md` claim was withdrawn as a result. A transcription slip and
a transmission garble look identical in the plaintext (Enigma has no diffusion),
and the round-trip and self-encryption checks cannot separate them, so the image
is the only authority. Keep it with the data.

**Note on numbering.** The `Spruch Nr` on the form is the originating station's
serial. It is *not* the number used by the cryptocellar message list, which
calls GEHRG "Message No. 38" — a different 09.09.1941 message. The records in
`../enigma-army-messages-1941.txt` are keyed by Kenngruppe for these two to
avoid the collision.
