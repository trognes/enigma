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
to the true-plugboard output (`-a` vs `-q`, `-S m4{a,q}10 -J --polish -l german`).

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

**57 further authentic HG Nord messages** with keys + verified plaintext, from the older
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

Together the two files hold **70 authentic messages (~6,950 letters)** of real
telegraphic German — the statistical power the original 13 (bimodal, too small to settle
`-a` vs `-q` on real traffic) lacked. Intended split: the published Appendix-C n-gram
statistics stay the *telegraphic corpus*; these 70 messages are *held-out validation*.

## 5. Standing challenge — unbroken ciphertexts (`eval/enigma-challenge-1941.txt`)

The same collection's 18 ciphertexts that were unbroken when this file was
assembled, kept as a future challenge. There is no key or plaintext to verify
against, so `eval/build_challenge_1941.py` only checks each transcription
against the letter count on the message form (all match bar one known
form-miscount, Nr 53).

### 5a. Status re-checked against the message list, 01 Aug 2026

The BGAC **1941 Message List** (`cryptocellar.org/bgac/1941-msg-list.html`)
carries a live status per message, and it moved a long way after this set was
captured. **Six of the 18 are no longer a challenge** — five broken, and Nr 138
is a second transcription of one of them:

| | date | letters | status |
|---|---|---:|---|
| Nr 214 FTNBK | 16 Jul | 101 | broken by **Enigma@Home**, 15.09.2017 — key now known, see §5d |
| Nr 140 WEUWY | 09 Jul | 48 | broken by Michael Craig, 17.07.2007 |
| Nr 38 GEHRG | 09 Sep | 74 | broken 12.07.2026 |
| Nr 81 ALQFI | 28 Aug | 87 | broken 14.07.2026 |
| Nr 8 ALGXZ | 02 Oct | 67 | broken 31.07.2026 |
| Nr 138 WEUWY | 09 Jul | 48 | same message as Nr 140 |

The recovered keys are not on the list itself, so they are not recorded here.
**Fetching one moves that message from this file to the validation set**, where
it is worth more: an authentic instance with ground truth at a length the repo
has none at (Nr 214 is 101 letters, and the break-rate work stops at L=167).

**Two unbroken Enigma messages are missing from this repo entirely**, both 29
Sep 1941: **QTXMA, 155 letters** — the *second-longest unbroken message in the
collection* — and SZAEJ, 51. Neither is transcribed here, which is the only
reason they are
absent. Transcribing QTXMA from its message form is the single highest-value
addition to the challenge set.

**Two blanket caveats in the older text were wrong, and both mattered.**

- *"July Batch A is largely hand cipher, and the Enigma ones are short."* The
  manual cipher is the **Truppenschlüssel**, and the list marks it per message
  (cipher length printed as `TS`). None of the 18 is marked TS. The "Enigma ones
  are short" half was actively misleading: Nr 214 is the **longest** message in
  that batch and it broke as Enigma.
- *"The Batch C trio may not be Enigma at all."* The suspicion belongs to
  footnote **\*3** — "special format, probably tactical, possibly a manual
  cipher on a 25-letter alphabet, J not used" — and that footnote is attached to
  Batch C Nrs 1–5, 14 and 14a, **not** to BYQMZ, FKQLZ or XFEDT. The list
  classifies all three as unbroken **Enigma**.

### 5b. The J test — footnote \*3 is testable, and it splits the trio

Footnote \*3 names a falsifiable property, so it can be checked against the
ciphertext this repo holds. Under a 26-letter cipher the letter J appears at
rate 1/26; a 25-letter alphabet without J gives zero. Control first: the **70
solved authentic messages carry 277 J in 6 944 letters, 3.99%** against the
3.85% expected — so the test's null is sound.

| | letters | J | expected | reading |
|---|---:|---:|---:|---|
| BYQMZ | 167 | **6** | 6.4 | indistinguishable from Enigma |
| FKQLZ | 107 | **0** | 4.1 | p = 0.015 |
| XFEDT | 97 | **0** | 3.7 | p = 0.022 |
| FKQLZ + XFEDT | 204 | **0** | 7.8 | **p = 3e-4** |

So the trio splits: **BYQMZ behaves exactly like a 26-letter cipher** and the
other two do not. That is the opposite of the note this file used to carry, and
it bears directly on where compute goes — BYQMZ is both the longest unbroken
message and the one of the three that looks like Enigma.

**Do not over-read a single zero-J message.** Nine of the 18 contain no J,
against 1.7 expected if all were Enigma — but most are short, where the test has
no power, and the decisive counter-example is **Nr 38 GEHRG: zero J in 74
letters (p = 0.054), and now broken as Enigma**. Only the messages at n ≳ 90
carry enough power to say anything, which is why the table above holds just
those. Nr 214 FTNBK, confirmed Enigma, sits at 7 J in 101 as it should.

### 5d. Nr 214 FTNBK — solved, and its ciphertext here was wrong

The key is now in the repo, and Nr 214 has **moved to
`enigma-army-messages-1941.txt`** (57 records, up from 56):

    reflector B, wheels III I IV, ring AHV, start FQR
    plugs AH CN DF EI KY MP OZ RU SW VX          (10 pairs)

    STANDORT DER LNK X LNK X IST X KUSOW X KUSOW X SEQS X KM X
    SUEDWESTLIQ X SAGOSKA X SAGOSKA X KEINE AUSFAELLE X MATHIAT X MATHIAT

The published ring is given as two letters, `HV`: the **left wheel's ring is
unidentifiable** from ciphertext, since only start-minus-ring reaches the
machine, so it is written `A` here to pair with the published start `FQR`.

**The transcription in the challenge file was wrong in 13 of its 101 letters**
(positions 14, 29, 30, 33, 34, 42, 50, 52, 58, 59, 78, 89, 94) and does not
decrypt under the true key. It has been replaced by the correct one and moved.
Nothing in the repo could have caught this: the only automatic check on a
challenge ciphertext is its length against the message form, and the length was
right.

**The failure mode is worth understanding, because it produced a confident wrong
answer twice over.** Attacking the corrupted ciphertext with a crib taken from a
correspondingly corrupted plaintext, the search returned two candidates and
*both* were one step from the truth:

- the top-scoring board carried the **true rotor key** with a single wrong plug
  (`BV` for `VX`);
- the crib-seeded board carried the **true plugboard** with a rotor key in the
  same §7.12 equivalence class as the truth — the two keys are byte-identical
  decrypts up to 409 letters, so on a 101-letter message they are the same key.

Reading the second as "solved" was circular: the crib and the ciphertext shared
their corruption, so reproducing the remaining 78 uncribbed letters proved only
that the two corrupt artifacts were consistent with each other, not that either
was right. **A crib and the ciphertext it is matched against are not independent
evidence when both come from the same source.** The check that would have caught
it is the one applied afterwards: re-encrypt the recovered plaintext under the
recovered key and compare against an *independently sourced* ciphertext.

**The erroneous transcription is kept here deliberately**, because it is a
useful artefact in its own right — an *authentic* message on which the true
plaintext is not the highest-scoring one:

    XNQAEQNZLWFMQGTXOQZVXJKBOJKPCLJQZOVFLSVJBSRIYMRYWNUJVWYKXAKK
    FMSQFBBARNKNHBRHQSLIUVNEHMJKAZRXLJLWISNZZ

    wrong at 14, 29, 30, 33, 34, 42, 50, 52, 58, 59, 78, 89, 94 (13 of 101)

Give the search the **true rotor key** and hide the board, and it does not come
back: the climb finds a board scoring **above** the truth whose decrypt is
gibberish. Measured at `-R 64`, `-S i4<model>10 -J --polish`, board hidden:

| model | true board | climb winner | truth is behind by |
|---|---:|---:|---:|
| `-t` trigram | −5.0163 | −4.9645 | **0.052** |
| `-q` quad | −7.2238 | −6.9817 | 0.242 |
| `-a` weighted | −11.3156 | −10.9858 | 0.330 |
| `-f` fused | −9.4800 | −9.1858 | 0.294 |

So the failure is **not specific to `i4f10`** — every model prefers a spurious
board — but the margin **shrinks monotonically as the model order falls**, and
trigram is nearly at break-even. That is exactly the trade §1 records from
Ostwald & Weierud, who chose trigrams over higher orders *because* real traffic
carries 5–30% garbles, and §3 notes the 13-message set was too small to show.
Here it is on one authentic message: at 12.9% corruption the higher-order models
lose the truth by 0.24–0.33 while trigram loses it by 0.05. One instance proves
nothing on its own, but it is a real-traffic datapoint where the repo previously
had only synthetic short-message evidence for a scoring floor.

Worth keeping in mind when reading a negative sweep on any of the remaining
challenge messages: **a mis-transcribed ciphertext is not merely harder, it can
be unrecoverable in principle** — no amount of `-R` finds a board the scorer
ranks below a wrong one. The corrected ciphertext, by contrast, recovers exactly
(§5d), so the whole difference here is 13 letters.

With the ciphertext corrected, the tool recovers the whole board from the true
rotor key in **0.13 s** at `-R 64` — no crib, 151 999 boards scored — so Nr 214
is now a clean validation instance at 101 letters, a length the repo previously
had no ground truth at.

### 5e. Are the "does not break on the day key" messages just mis-transcribed?

§5d makes this worth asking: Nrs 100, 138 and 172 carry a *recovered* day key
that they demonstrably do not decrypt under, which the source reads as a
different key or network — but a handful of copying errors would look the same,
and FTNBK proves copying errors happen. Enigma has no diffusion, so a
mis-transcribed message under its true key still decrypts to readable text with
one wrong letter per wrong letter, and `--confidence` can see that.

Tested by fixing each day key (already in the repo) and sweeping all 17 576
start positions, `-f -l wehrmacht --confidence 256`:

| | letters | best margin | verdict |
|---|---:|---:|---|
| Nr 100 LXACA, 5 Jul | 20 | +0.8 sd | noise |
| Nr 138 WEUWY, 9 Jul | 48 | +0.6 sd | noise |
| Nr 172 MVUEH, 10 Jul | 82 | +0.6 sd | noise |

All three sit under the +2 sd line that "is not a find", and all three decrypt
to gibberish. **The source's reading stands: these are a different key, network
or system, not a transcription problem.**

**The negative is only worth as much as the test's power, so that is measured
too** — using FTNBK's own 13-error ciphertext under its true key, truncated to
each message's length:

| letters | errors | margin |
|---:|---:|---:|
| 101 | 13 | **+5.1 sd** |
| 82 | 11 | **+4.0 sd** |
| 48 | 6 | **+2.4 sd** |
| 20 | 1 | +1.0 sd |

So at Nr 172's length a mis-transcribed message would have read +4.0 against the
+0.6 observed, and at Nr 138's length +2.4 against +0.6 — those two negatives
are real. **Nr 100 is not a negative at all**: at 20 letters even a correct,
clean message reads only +1.0, which is below the noise line. It sits under the
~23-letter unicity distance, so no amount of compute will settle it either way.

The general point is reusable: **when a day key is known, this test separates
"wrong key" from "right key, bad transcription" in seconds**, and it needs
nothing but `--confidence` and a start sweep.

### 5c. Attacking

Attack with care: several are short (below the ~23-letter unicity distance), and
a few carry a known day-key they demonstrably do **not** break on (Nrs 100, 138,
172).

**Pin `-u B`, do not wildcard the reflector.** On the standard account UKW-B
replaced UKW-A across Wehrmacht service in 1937, and UKW-C appears only rarely
and later; for 1941 Heer traffic — which is what this whole collection is —
UKW-B is the overwhelmingly likely setting. `-u .` costs a **3× larger
keyspace** to cover two reflectors that almost certainly are not there, and that
3× is far better spent on `-R`, which is the measured bottleneck
(`CLAUDE.md`, "The unknown-key break rate"). Treat A and C as a fallback to try
only after a thorough `-u B` sweep comes back negative, not as a hedge to carry
through the first one. Note this is a *historical* prior, not something measured
here — it is the one assumption in a challenge attempt that no amount of compute
will detect if it is wrong.

## 6. The `wehrmacht` scoring language — the corpus payoff

The domain-matched corpus idea, realised. `eval/build_telegraphic_ngrams.py` bends the
bundled prose German tables toward the **published telegraphic statistics** in the 2005
paper's Appendix C (Fig 17 single-letter + Fig 18 top-400 trigram frequencies over ~20 000
letters of 1941 decrypts), by marginal-matching the quad table's folded low-order marginals
to telegraphic (strength A=0.5 mono / B=2.0 tri). Because `-a` folds every order from the
quad windows, one corrected table makes the whole scorer telegraphic. It ships as a
scoring **language** of its own — `ngrams/wehrmacht_*.txt`, selected with `-l wehrmacht` —
rather than a parallel data directory, so it needs no `-d` and composes with a custom one.

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

Scope: `wehrmacht` encodes telegraphic orthography by construction — it is a *writing style*,
not a separate language — so it is for **real Wehrmacht traffic only**; on prose German (and the
`make crackquality` benchmark) `-l german` remains correct. This is **measured, not asserted**
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

## 7. Crib (known-word) finisher — measured-down (`--crib-rerank`, `eval/eval_crib.py`)

The article's other pointer for the short-message floor is a "special assessment stage
evaluating words we know are frequently used by HG Nord (Berta, Eins, Frage, Roem)" — a
crib / known-word objective. Implemented as `--crib-rerank` (Option B, a re-ranking finisher):
after each restart climb converges, the board is ranked by `n-gram score + weight·crib_score`,
where `crib_score` sums the weights of known words (`cribs/german-hgnord.txt` — generic
operational vocabulary: spelled numbers, phonetic alphabet, standard military nouns, chosen
from telegraphy conventions, *not* fitted to the test messages) present in the decrypt. The
climb still optimises n-grams (hot path untouched); only the cross-restart winner sees cribs.

**Measured-down** (`eval/eval_crib.py`, the 69 held-out messages, on top of the telegraphic
tables): net **−0.1 pp** at weight 0.5, **−1.7 pp** at 1.0.

| band | n | tele | crib 0.5 | crib 1.0 |
|---|---|---|---|---|
| 40–69 | 14 | 24.3 | 24.1 | 22.9 |
| 70–119 | 20 | 67.5 | 67.5 | 62.4 |
| **all** | 69 | **60.2** | 60.0 | 58.5 |

It scores the odd genuine scoring-failure win (No. 203: 79→100 %) but that is offset by
**false-positive re-ranking** breaking already-recovered boards (No. 108: 100→17, No. 200:
100→87 at higher weight). The reason is the escalation boundary: once the telegraphic tables
*surface* the true board, the residual is dominated by **wrong-basin** failures — the true
board isn't among the converged restarts — so a re-ranker has nothing true to promote. It's
the board-selection echo of the project's "no truth-free selection signal" finding: the
article's crib stage assumes the truth is already *found*; on our residual it usually isn't.
Kept as an off-by-default, not-recommended opt-in (the negative answer is the artifact), like
`--score-tt`/`--repair3`. A tie-breaker variant (crib only among near-equal-n-gram boards) or
a crib-*directed repair* (Option A) is the untried next step if the line is revisited.

## Reproduce

```sh
python3 eval/build_telegraphic_ngrams.py    # regenerate ngrams/wehrmacht_*.txt from Appendix C
R=150 python3 eval/eval_crib.py             # crib finisher: telegraphic vs +crib over 69 msgs
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
