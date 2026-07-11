# Modern breaking of Enigma — method notes & authentic message set

Notes on **Ostwald & Weierud, "Modern Breaking of Enigma Ciphertexts,"** *Cryptologia*
41(5):395–421 (2017) — the reference method this tool descends from — together with the
13 authentic WWII messages from its appendix, transcribed and verified here as a real-traffic
test set (`eval/enigma-messages.txt`).

## 1. The method, and how this tool maps onto it

**Gillogly (1995), the ciphertext-only skeleton.** Exhaust the 60 wheel orders × 26³ start
positions with an **empty plugboard**, ranked by **Index of Coincidence (IC)**; then optimise
the middle/right ring settings; then recover the plugboard by **hill-climbing**, switching the
score from **IC to trigrams** once a few plugs are set. The empty board is the right start
because it already holds the 6 self-steckered plugs correctly (the Wehrmacht used only 10 of 13
plugs). → This is exactly the tool's staged climb `-S i4q10` (IC pre-pass, then the n-gram
target) starting from the empty board.

**Why IC first, then n-grams (their Fig. 3 / Table 1).** At the correct rotor location but an
empty board, a candidate letter is, on average, **~10 % plaintext / ~19 % monoalphabetic
substitute / ~71 % pseudorandom**. IC measures *monoalphabeticity*, so it "shines through" the
empty board and finds the first few plugs; a plaintext measure (trigram) does not, until the
plaintext part overtakes the monoalphabetic part at **~3–4 correct plugs** — "after four correct
plugs the rest is easy." → This is the theoretical justification for capping the IC pre-pass at
~4 plugs (`i4…`), which the tool does.

**Ostwald–Weierud improvements (the tool's lineage).**
- **Trigrams, deliberately, because of garbles.** They tested IC/bigram/trigram/hexagram and
  chose **trigrams** as the compromise: higher orders discriminate plaintext better but are
  *fragile to garbles* (real messages carry 5–30 % garbles), lower orders are garble-robust but
  weak. Bigrams were rejected outright (accidental EN/ER/RE reward wrong plugs). **Relevance to
  `-a`:** the tool's weighted log-linear model blends quad/tri/bi/mono, so on garbled text the
  garble-robust lower orders automatically carry more weight — `-a` is plausibly the right model
  for real traffic, though this set is too small to prove it (§3).
- **Domain-matched corpus.** Common German prose is *not* the best text base; Wehrmacht traffic
  is telegraphic (X = space, Q for ch/ck, spelled-out numbers, fixed abbreviations). Their n-gram
  statistics came from ~500 real HG Nord messages. (This is CODE_REVIEW §2's open "telegraphic
  corpus" item.)
- **Partial exhaustion of the first plugs** = the tool's `--exhaust E`. They name the depths
  "solo/duet/trio/quartet" (exhaust the first 1/2/3/4 plugs: 325 / 44 850 / 3.45 M / 164 M
  starts). Refinement the tool lacks: only exhaust plugs containing **frequent** letters — the
  "E-/R-/I-Stecker" methods (E-only = 26 cases; E,N,R = 73; E,N,R,X,S,I = 136), a ~2.4× cut over
  all 325 with little loss, since rare letters (Y < 0.25 % in HG Nord plaintext) barely occur.
  Wehrmacht plaintext letter order is **E, N, X, R** most frequent; C, P, J, Y rarest.

**Unicity distance.** With H ≈ 72 bits of key and D ≈ 3.1 bits/char of plaintext redundancy for
HG Nord, the unicity distance is **~23 letters**. Messages below ~20 letters are essentially
unbreakable; the practical turning point is ~24. Short messages fail not only on search but on
*statistics*: e.g. BOTKB (69 letters) has an accidental IC of 4.9 % (≈ random 3.8 %) and is
"nearly unbreakable," while YYBRW (46) and HODSN (48) have IC 7.0 % / 6.9 % and break easily —
breakability tracks the plaintext's accidental statistics, not just its length.

## 2. The authentic message set (`eval/enigma-messages.txt`)

13 messages from the article appendix (German Army / Heer, Enigma I, reflector B, 10 plugs;
HG Nord, Operation Barbarossa, Jul–Oct 1941). Each record carries the full key (reflector,
wheels, ring, start, plugs), the Kenngruppe (discriminant, not enciphered), the ciphertext, the
raw machine decrypt, the emended German, a translation, and garble notes — plus a ready
`./enigma` command. Built and verified by `eval/build_enigma_messages.py`:

- **All 13 decrypt exactly at every known position** (the garbled No. 233 has 3 unrecorded
  letters at positions 117–119, held as placeholders so the rotor stepping stays in sync — a
  dash is a real position and must not be stripped).
- **Correction:** message No. 2 (1 Oct 1941) start is **ZAQ** — as printed in the article PDF
  (Version 5); the cryptocellar.org *web page* mis-prints `DEI` (copied from No. 1). Recovered
  independently here by brute-forcing the start against the known plaintext, then confirmed
  against the PDF.
- No. 233 is special: enciphered 18 Jul with a **left-wheel turnover at position 81**, which
  wrecked the middle of the message; the authors split it in two to break it. With the rotor key
  known, the tool decrypts it directly (the stepping handles the turnover).

## 3. Real-traffic test (`eval/crack_real.py`) — findings

Fix the (known) rotor key, **hide the plugboard**, hill-climb it back, and compare the decrypt
to the true-plugboard output (`-a` vs `-q`, `-S m4{a,q}10 -J --gainfix-best3 -l german`).

- **The tool reproduces the article's breakability pattern exactly** — the long messages (214,
  174) and the "easy" 36-letter PFCXY solve at 100 %; BOTKB and the sub-unicity short ones do
  not — independent external validation of the search.
- **`-a` vs `-q` is inconclusive on this set.** At R=256 `-a` looked +3…+9pp ahead on four
  mid-length messages, but at R=1024 three of those collapsed to ties — the %-correct on
  *unrecovered* messages is restart-draw noise (the set is bimodal: either both models fully
  solve, or neither can). The clean-prose `crackquality` benchmark (large N) remains the real
  evidence that `-a` > `-q`.
- **The residual on short real messages is a *scoring*-failure floor, not search.** More
  restarts (R=1024) don't help; the true plugboard genuinely doesn't score highest under n-gram
  statistics on short garbled text. The article says the same about No. 128: spurious solutions
  outscore the truth on trigrams, fixable only by "a special assessment stage evaluating words
  we know are frequently used by HG Nord (Berta, Eins, Frage, Roem)." That is a **crib /
  known-plaintext objective** — CODE_REVIEW §2's highest-value open item, now doubly motivated
  and with the crib vocabulary sitting in this very database.

## 4. Expanded real-traffic set (`eval/enigma-army-messages-1941.txt`)

**56 further authentic HG Nord messages** with keys + verified plaintext, from the older
Sullivan & Weierud "Breaking German Army Ciphers" collection (Cryptologia 2005;
cryptocellar.org/bgac). These are ciphertexts the authors originally **failed to break**
(2003–04) whose day-keys were **recovered later** (released 04 Aug 2017) — the "messages
we failed to break" and "July Batch A" pages. `eval/build_army_messages_1941.py` decrypts
each at its stated key and reproduces the plaintext exactly; every one is clean
telegraphic German (fuel/ammunition/movement traffic: `BETRIEBSTOFF`, `MUNITION`,
`ABENDMELDUNG`, phonetic `LUDWIG/FRIEDRICH/HEINRICH`, spelled numbers, `X` separators,
`Q`→ch). Two that duplicate the 13-message set (No. 203 CFYZR — the 2005 paper's
"singular unbroken" message, now broken — and No. 233 XNRLR) are dropped by
ciphertext-dedup, so the two files are disjoint.

Together the two files hold **69 authentic messages (~6,850 letters)** of real
telegraphic German — the statistical power the original 13 (bimodal, too small to settle
`-a` vs `-q` on real traffic) lacked. Intended split: the published Appendix-C n-gram
statistics stay the *telegraphic corpus*; these 69 messages are *held-out validation*.

## 5. Standing challenge — unbroken ciphertexts (`eval/enigma-challenge-1941.txt`)

The same collection's **18 still-unbroken ciphertexts** (no rotor key ever recovered),
kept as a future challenge. There is no key or plaintext to verify against, so
`eval/build_challenge_1941.py` only checks each transcription against the letter count on
the message form (all match bar one known form-miscount, Nr 53). Attack with care:
several are short (below the ~23-letter unicity distance), the source flags that many
"July Batch A" messages are **hand cipher** (Doppelkasten), the "Batch C" trio may not be
Enigma at all, and a few carry a known day-key they demonstrably do **not** break on.

## 6. Telegraphic scoring tables — the corpus payoff (`ngrams-telegraphic/`)

The domain-matched corpus idea, realised. `eval/build_telegraphic_ngrams.py` bends the
bundled prose German tables toward the **published telegraphic statistics** in the 2005
paper's Appendix C (Fig 17 single-letter + Fig 18 top-400 trigram frequencies over ~20 000
letters of 1941 decrypts), by marginal-matching the quad table's folded low-order marginals
to telegraphic (strength A=0.5 mono / B=2.0 tri). Because `-a` folds every order from the
quad windows, one corrected table makes the whole scorer telegraphic; use it with
`-d ngrams-telegraphic -l german`.

Validated on the full **69-message held-out set** (`eval/eval_telegraphic.py`; rotor key
fixed, plugboard hidden and hill-climbed, mean %-letters-correct):

| band | n | prose | telegraphic |
|---|---|---|---|
| <40 | 11 | 9.8 | 16.4 |
| 40–69 | 14 | 15.3 | 24.3 |
| 70–119 | 20 | 28.8 | **67.5** |
| ≥120 | 24 | 75.5 | 95.4 |
| **all** | 69 | **39.3** | **60.2** |

**+20.9 pp mean, wins 36 / loses 12** — the biggest gains in the 70–119-letter band (above
unicity, but short enough that prose can't rank the telegraphic truth). This is the corpus
result §1 anticipated and §3 could not settle on the bimodal 13: a domain-matched corpus is
the second real lever for short real-message breaking, alongside the `-a` scoring model.

Scope: these tables encode telegraphic orthography by construction, so they are for **real
Wehrmacht traffic only** — on prose German (and the `make crackquality` benchmark) the
bundled `ngrams/` tables remain correct. This is **measured, not asserted**
(`eval/eval_prose_inverse.py`, the mirror control — random prose-German excerpts recovered
under prose vs telegraphic):

| length | prose | telegraphic |
|---|---|---|
| 40 | 47.2 | 16.7 |
| 70 | 92.7 | 72.4 |
| ≥100 | 100.0 | 100.0 |
| **all** | **88.0** | **77.8** |

The telegraphic tables lose **−10.2 pp** on prose (−30.5 at L40) — the exact mirror of the
+20.9 pp real-traffic win, same tables, opposite sign, biggest where the win was biggest.
And since Appendix C is aggregate over the same HG Nord traffic family, the win itself is a
domain-matched table measured on in-domain real traffic, not a claim about arbitrary German
(no message's plaintext went into the tables).

## Reproduce

```sh
python3 eval/build_telegraphic_ngrams.py    # regenerate ngrams-telegraphic/ from Appendix C
R=150 python3 eval/eval_telegraphic.py      # held-out eval: prose vs telegraphic over 69 msgs
R=150 python3 eval/eval_prose_inverse.py    # inverse control: prose vs telegraphic on PROSE German
python3 eval/build_enigma_messages.py       # regenerate + verify the 13 (Ostwald & Weierud 2017)
python3 eval/build_army_messages_1941.py    # regenerate + verify the 56 (Sullivan & Weierud 2005)
python3 eval/build_challenge_1941.py        # regenerate the 18 unbroken challenge ciphertexts
R=256 python3 eval/crack_real.py            # real-traffic plugboard recovery, -a vs -q
```

## Source

Olaf Ostwald & Frode Weierud, "Modern Breaking of Enigma Ciphertexts," *Cryptologia*
41(5):395–421, 2017 (the 13-message set). Geoff Sullivan & Frode Weierud, "Breaking
German Army Ciphers," *Cryptologia* 29(3):193–232, 2005 (the 56-message set; keys
released 2017). Messages © the authors, CC BY-NC-SA; see cryptocellar.org.
