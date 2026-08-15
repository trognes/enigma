# Sources

Archived copies of the pages and documents the `eval/` message sets are built
from, kept so the corpus can be re-derived and re-checked offline.

All of this material is the work of **Geoff Sullivan, Olaf Ostwald and Frode
Weierud**, published on *Breaking German Wehrmacht Ciphers*:

> **<https://cryptocellar.org/bgac/index.html>**

It is theirs, published under CC BY-NC-SA, and is included here for research
with attribution — it is not covered by this program's GPL.

| file | what it supplies |
|---|---|
| `enigma-messages--july-1941.html` | July 1941 Batch B ciphertexts (48 messages) |
| `enigma-keys--july-1941.html` | July 1941 day keys, message keys, indicators |
| `enigma-keys--juneoctober-1941.html` | Jun–Oct 1941 day keys, incl. 02.10.1941 |
| `ultimate-enigma-challenge.html` | the five Batch C ciphertexts (QTXMA, SZAEJ, BYQMZ, FKQLZ, XFEDT) |
| `modern-breaking-of-enigma-ciphertexts.html` | the 13 messages of the 2017 Cryptologia article, with keys and emended plaintext |
| `heeresgruppe-nord-july-1941.pdf` | the same July material as a compilation, with a Schlüsseltafel |

The scanned message forms live one directory up, in `eval/forms/`.

**Two things these settle that the repo could not settle alone.** The
June–October key page publishes 09.09.1941 as `B/342/KFZ`, independently
confirming the day key recovered ciphertext-only here (`ALZ` is an equivalent
class representative — see `../MODERN_BREAKING_NOTES.md` §5j). And the
01.07.1941 key note states plainly that the indicator gives no credible
Ringstellung, lists
three candidates, and picks one on a keyboard-diagonal heuristic — which is the
identifiability limit this repo documents, admitted at the source.
