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
| `BYQMZ-30091941-008-out.pdf` | 8 | BYQMZ | 172 | unbroken (Nr 8-C) |

The first two are outgoing, dated 9.9.41, *befördert* 0913, message time (0850),
from Nachschub to SS-T.Div. The **length printed on each form** is the reason
they are kept: it is an independent check that a transcription dropped or added
nothing, and all four match.

## BYQMZ — the one that agreed

The newest scan is the only one so far whose transcription needed **no
correction at all**: read again at up to 1400 dpi, it agrees with the stored
ciphertext letter for letter, **172 of 172**, and the form's printed count is
172. That is worth recording precisely because the other four did not — five
of ALVPM's six "garbles" turned out to be transcription errors, and FMNGI and
MVUEH produced twenty disagreements between them.

Three details did the work, and they are properties of the *hand* rather than
of the reader:

- **`z` is barred through the descender and `y` is not**, which settles
  `NYZKY`, `TWMYC` and `ZXYFT` outright — the single most dangerous confusion
  in this ciphertext, since it carries 19 of the two combined.
- **`q` descends straight where `g` loops left**, settling `VGQTW`, `ZUQBL`
  and `URQES`.
- **`u` carries the German bow above it**, separating it from `n` in `PSDUH`,
  `ZVSUA` and `UMOSH`.

**The dash is real.** At row 2 group 3 (`HIF-Y`) the fourth cell is genuinely
blank on the form: the letter was never written down, and the form's count of
172 counts it. So the dash records a gap in the 1941 record rather than a
failure of transcription, and nothing is recoverable there from the image.

**The indicator is new: `MGS TPL`.** The ciphertext block opens `0014 - 172 - `
followed by six letters, the same run-together form as ALVPM's `kfzjpo` and
ALRHG's `ftjmyc`. Read off the form here and independently confirmed by the
Cryptocellar documents — two readers, the same standard that let MVUEH's
position 85 be corrected.

**And it is the one place a glyph comparison actively misled, for a reason
worth keeping.** The preamble and the ciphertext cells are written in
**different scripts**: the cells carry careful *Latin* letters, because
ciphertext has to be unambiguous, while the preamble and the Vermerke are in
the *running German* hand. The two disagree on exactly the letters at issue —

| letter | in the ciphertext cells | in the preamble |
|---|---|---|
| `s` | narrow slanted Latin loop | flat-topped cursive form |
| `t` | short, with a crossbar | tall, open hook, no crossbar |

— so a preamble `t` looks far more like the cells' `r` (a tall *looped* stroke,
as in `QNRAI` and `URQES`) than like their `t`. Reading the indicator at 800
dpi gave `MGS TPL`; cross-checking those two glyphs against ciphertext
exemplars at 2200 dpi then argued them away, wrongly. **Calibrate a preamble
glyph against the Vermerke, never against the cells** — the `l` of the
Vermerke's `yzl` and the `l` of the indicator are the same closed loop, which
is how the two running-hand lines are known to match.

This inverts the lesson the FMNGI/MVUEH section draws below. There,
magnification always confirmed the stored reading and never a disagreement;
here magnification overturned a *correct* first reading, because the reference
alphabet it was checked against was the wrong one. Magnification settles the
shape of a glyph, not which alphabet it belongs to.

Do not confuse it with the **Vermerke** line, which reads `Spruch 2352 yzl -
gjb - Durchgegeben`. That is a relay note about a different message sent at
2352; this message was *befördert* 0025 with time of origin 0014.

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

## Readings that differ, and which one was applied

Reading these two scans letter by letter produced disagreements with the stored
transcriptions — **7 letters in FMNGI and 14 in MVUEH**, of which exactly one
has been applied (below). The rest are listed in full so a later attack can try
them. Positions are 1-based over the whole string, Kenngruppe included.

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

MVUEH, NOT applied (13 letters, 6 groups; faint pencil throughout, so every one
is low or medium confidence):

| pos | group | stored | read here |
|---:|---|---|---|
| 7, 8 | 2 `IDEVS` | `D`, `E` | `C`, `R` |
| 11 | 3 `ARMCC` | `A` | `O` |
| 16 | 4 `NQTAT` | `N` | `W` |
| 23, 25 | 5 `YEVFC` | `V`, `C` | `K`, `X` |
| 29, 30 | 6 `DBZGG` | `G`, `G` | `J`, `F` |
| 48 | 10 `WURRT` | `R` | `L` |
| 52, 54, 55 | 11 `BZCVG` | `Z`, `V`, `G` | `R`, `U`, `F` |

**One further difference has been APPLIED**: position 85, group 17, `F` → `Y`,
so the tail now reads `DNXJY TC`. The repo owner reads the final group as `dnxjy
tc-` off the scan and the reading here agreed independently, which is two
readers rather than one — the only disagreement of the twenty where that is
true. It cannot be confirmed by decryption (the message is unbroken) and the
length check is silent on it, since any letter in that cell gives 87; it rests
on the image alone. It does not weaken the tail argument below, which is about
the *shape* of the last group (`DNXJ?` + `TC` against `DURXJ` + `YTC`) and not
about which letter fills the fifth cell.

Confirmed identical at high magnification: FMNGI `FGROV FDIVQ QVNQW LGBLJ VRLEB
EKFHE MCF`, and MVUEH `MVUEH SMXWL PSYWZ YTCBS ODVJU SLSOO MJQJZ SXSEB ZPEYM TC`
— ten of MVUEH's eighteen groups.

**None of the remaining twenty is applied**, for a reason worth stating rather
than out of caution. Neither message has a working key, so a proposed correction
cannot be arbitrated by decryption — which is exactly what made the ALVPM
corrections safe and makes these unsafe. And **every check that was available
went the stored transcription's way**:

- **The form's own letter count arbitrates MVUEH's tail, and the stored text
  wins.** Read at low magnification the last row looked like `DURXJ` + `YTC`,
  which totals **88** letters against the form's stated 87. Enlarged, it is
  `DNXJ?` + `TC` — exactly **87**, and `TC` as stored. An arithmetic check, not
  a judgement about handwriting. It fixes the *shape* of the group, not the
  letter in its fifth cell, which is why position 85 could still be corrected.
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
