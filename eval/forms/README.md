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
group, decipher the second, and that is the message start — instead of a
17 576-start sweep, and the two answers must agree.

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
transcriptions — one in MVUEH's faint pencil, five in FMNGI:

| message | group | stored | read here |
|---|---|---|---|
| FMNGI | 5 (`LIFZB`) | `F` | `V` |
| FMNGI | 4 (`MNMNI`) | `M…I` | `R/P…E` |
| FMNGI | 9 (`XIQEX`) | `E` | `I` |
| FMNGI | 10 (`CSAQP`) | `S`, `P` | `H`, `R` |
| FMNGI | 12 (`KFBIK`) | final `K` | `X` |

**None is applied**, for a reason worth stating rather than out of caution.
Neither message has a working key, so a proposed correction cannot be arbitrated
by decryption — which is exactly what made the ALVPM corrections safe and makes
these unsafe. And where a check *was* available, the stored transcription won:
on the groups legible beyond doubt (MVUEH `SMXWL PSYWZ`, FMNGI `QVNQW LGBLJ
VRLEB EKFHE`) it matches this reading exactly, and two letters read differently
at low magnification — FMNGI `FDIVQ` and `LGBLJ` — turned out to confirm the
stored text once enlarged. A transcription that survives every checkable test
should not be overwritten on the strength of the unfalsifiable ones.

They are recorded because they are the cheapest thing to try if either message
is ever attacked and fails: the candidates are few, and one of them may be the
letter that matters.

They also settled which "garbles" were 1941's and which were ours — five of
ALVPM's six were transcription errors, corrected against these images, and one
`ENHANCEMENTS.md` claim was withdrawn as a result. A transcription slip and a
transmission garble look identical in the plaintext (Enigma has no diffusion),
and the round-trip and self-encryption checks cannot separate them, so the image
is the only authority. Keep it with the data.

**Note on numbering.** The `Spruch Nr` on the form is the originating station's
serial. It is *not* the number used by the cryptocellar message list, which
calls GEHRG "Message No. 38" — a different 09.09.1941 message. The records in
`../enigma-army-messages-1941.txt` are keyed by Kenngruppe for these two to
avoid the collision.
