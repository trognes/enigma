# cribs.md — a plan for crib-driven plugboard deduction

**Status: a plan. Nothing here is built yet.** The numbers quoted are from
measurements made while writing it; where a figure is an estimate rather than a
measurement, it says so.

---

## 1. What this is about, in one page

Today the tool breaks a message like this: try every rotor setting, and for each
one *search* for a plugboard by hill-climbing — try a plug, see if the text
scores more like German, keep it if so. The search is the expensive part. We
measured it at **0.338 milliseconds per rotor setting**, which over the full
60-wheel-order space at 200 letters comes to **24.9 hours**, and even then it
recovers the right answer only about half the time.

A **crib** is a guess at some of the plaintext — say, you believe the message
contains the word `SIEGFRIED` starting at letter 40. If the guess is right, you
no longer have to *search* for the plugboard. You can *calculate* it. That is
what this plan is about.

The idea is not new — it is what the wartime Bombe machines did. What is new
here is that the machinery it needs is already sitting in this tool, unused.

**Rough gain**, from the same measurements: a crib turns the 24.9-hour job into
something on the order of **two minutes**. The estimate is arithmetic on a
measured throughput, not a benchmark, so treat it as "minutes, not hours" rather
than as a precise figure.

**The catch** is that you need a crib, it has to be exactly right, and it has to
be long enough. Most of this plan is about those three problems.

---

## 2. Words this plan uses

Plain definitions, because several of these terms are used loosely elsewhere.

**plugboard** (German *Steckerbrett*) — the cable panel on the front of the
machine. Ten cables swap ten pairs of letters, before and after the rotors. It
is the biggest part of the key.

**plug** — one cable. If `A` is plugged to `B` then `B` is plugged to `A`; a
cable has two ends.

**crib** — a guess at part of the plaintext, *together with where it sits*.
"Letters 40-59 are `XSIEGFRIEDSIEGFRIEDX`" is a crib; "the message mentions
Siegfried somewhere" is not.

**alignment** — which position in the ciphertext the crib is lined up against. A
20-letter crib in a 200-letter message has 181 possible alignments.

**menu** — the crib redrawn as a diagram: a dot for each letter, and a line
joining the plaintext letter to the ciphertext letter at each position.

**loop** (or **closure**) — a path in the menu that comes back to where it
started. Loops are what let a crib *reject* a wrong rotor setting.

**stop** — a rotor setting the crib fails to reject. Every stop must be followed
up by decrypting the whole message and checking whether it reads as German.

**diagonal board** — Gordon Welchman's 1940 addition to the Bombe: wiring that
enforces "if `A` is plugged to `B` then `B` is plugged to `A`". It costs us
nothing (see §6.4).

**rotor core** — the rotor stack plus reflector, with the plugboard taken off.
It is a 26-letter lookup table, different at every character position because
the rotors step. In the source this is the `rows[i]` table; §6.4a prints four
real ones.

---

## 3. Why extend the existing tool, not write a Bombe simulator

**Recommendation: extend `enigma.cc`. Do not start a separate tool.**

Four reasons, in order of weight.

**3.1 The machine equation is already in the hot path.** Decryption of one
character is currently one line:

```c
decode_at(steck, rows, ct, i)  =  steck[rows[i][steck[ct[i]]]]
```

Read it right to left: take the ciphertext letter, push it through the
plugboard, through the rotor core for this position, and back through the
plugboard. Now suppose a crib tells us the plaintext letter at position `i` is
`p`. Rearranged, that same line says:

```
   steck[p]  =  rows[i][ steck[ ct[i] ] ]
```

which reads: *if you know what the ciphertext letter is plugged to, the rotor
core tells you what the plaintext letter is plugged to.* That is the whole
deduction step. It is one table lookup.

This rearrangement works because the rotor core is its own inverse — the same
property that lets an Enigma decrypt by re-typing the ciphertext on the same
settings.

**3.2 The tables it needs are already built.** `setup_mapping()` already
constructs `rows[]` — the rotor core for every character position — once per
rotor setting, because the scorer needs it. The deduction needs exactly the same
table and nothing else.

**3.3 The codebase already does this inversion elsewhere.** The `--polish`
finisher's gain cascade computes `core_j(steck[bx])` — the identical step,
already written, already tested, already thread-safe.

**3.4 Everything around the search gets reused.** The rotor key sweep, the
thread pool (`-T`), the guarantee that results do not depend on thread count,
the n-gram scorers, the settings echo, and the 267-check test suite. A separate
tool would have to re-earn all of it.

**And one reason that decides it:** the hybrid in §7 — deduce the plugs the crib
determines, then hill-climb the ones it does not — is only possible *inside* the
existing tool. A standalone Bombe cannot do it at all. Since we expect real
cribs to be short and partly wrong, the hybrid is likely the mode that actually
gets used.

**The argument against**, stated fairly: `enigma.cc` is a single large file, and
this adds a fourth search mode alongside the plain scan, `-F` and `--exhaust`.
That is a real maintenance cost. It is outweighed by the four points above, but
it means the crib code should be self-contained and skippable, in the same way
`--exhaust` is.

---

## 4. What we already know, and how confident we are

This section exists so the plan is not re-derived from scratch later. All
figures are from authentic telegraphic German — the 58 decrypted messages in
`eval/` — with a real 10-cable plugboard.

**4.1 How long a crib has to be.** A crib works by contradiction: guess what one
letter is plugged to, follow the chain of deductions, and see whether it comes
back consistent. Loops are what make that possible. Using the largest connected
part of the menu (see §6.3 for why only the largest):

| crib length | loops | rotor settings rejected |
|--:|--:|--:|
| 12 | 0.32 | 0.01% |
| 14 | 0.57 | 1.8% |
| 16 | 1.00 | 36.8% |
| 18 | 1.56 | 85.0% |
| **20** | **2.13** | **97.5%** |
| 22 | 3.23 | 99.9% |
| 25 | 4.93 | ~100% |

**There is no hard floor.** A setting the crib fails to reject is not a failure,
just a *stop* that has to be checked by decrypting the message and scoring it —
and on a computer a stop costs microseconds. So a weak crib does not stop
working, it only shifts effort from rejecting settings to checking them.

Adding both costs, for **one crib** tried at **every alignment** against **every
rotor setting** — 60 wheel orders × 26³ positions — in a 200-letter message:

| crib length | alignments | rejected | sweep | checking | total |
|--:|--:|--:|--:|--:|--:|
| 12 | 118 | 0.01% | 100 s | 585 s | 685 s |
| 14 | 108 | 1.8% | 106 s | 237 s | 343 s |
| **16** | 99 | **36.8%** | 111 s | 53 s | **165 s** |
| **18** | 90 | **85.0%** | 114 s | 8 s | **122 s** |
| 20 | 83 | 97.5% | 116 s | 1 s | 117 s |
| 22 | 76 | 99.9% | 117 s | 0 s | 117 s |

("Alignments" is after the self-encryption filter of §6.6, which removes about
half of them.)

**The sweep is flat, and that is the important shape.** A longer crib costs more
work per alignment but has fewer alignments to try, and the two almost exactly
cancel — every row sweeps in 100–117 seconds. So crib length does not really
trade against sweep cost at all. It trades **hit rate against checking cost**,
and nothing else.

A 14-letter crib rejects almost nothing and still finishes in under six minutes,
and a 12-letter one — which rejects one setting in ten thousand — in eleven. The
curve is smooth, not a cliff. What 16–18 letters buys is the *cheapest* total,
not the only workable one.

(The "20 letters, three loops" rule of thumb comes from 1941, when every stop
cost a human several minutes at a checking machine. That economics is gone. See
§6.3 for a second place the same assumption misled this plan.)

**4.2 How often a crib is actually present.** Building a crib library from 57 of
the 58 messages and testing it on the 58th:

| crib length | 12 | 14 | 16 | 18 | 20 | 25 |
|---|--:|--:|--:|--:|--:|--:|
| messages hit | 55% | 24% | 19% | 9% | 3% | 0% |

Read §4.1 and §4.2 together and the design follows: **shorter cribs are found
far more often, and cost only a little more to use.** Going from 20 letters to
16 raises the hit rate sixfold, from 3% to 19%, for a 25% increase in total
time. Going down to 14 doubles the hit rate again for about 2× the time, and 12
letters reaches **55%** — more than half of all messages — for about eleven
minutes each. That last row is the striking one: the cribs that are actually
easy to find are affordable, just not cheap.

So the generator should target **16–18 letters, not 20**, and should keep the
12–15 range as fallback tiers rather than discarding it. This is the most
consequential number in the plan, and it rests on only 58 messages — see §11.

**4.3 Doubled words are the best crib source we have.** German operators sent
important words twice for error correction. You can watch it working in message
18, where both copies of a place name are garbled differently and the reader can
still reconstruct it:

```
   ... XWOEINSZWOX KOENIGSBCRG X COENIGSBNRG XOBX ...
                        ↑              ↑
                   two corruptions of KOENIGSBERG
```

Doubling is a gift three times over. It makes phrases recur across messages, it
doubles the length of any crib, and — because a doubled word reuses every one of
its own letters — it makes the menu close. Measured:

| crib | letters | distinct letters | rotor settings rejected |
|---|--:|--:|--:|
| `XFORDXFORDXVIKTORXAQTX` | 22 | 11 | 99.99% |
| `XSIEGFRIEDSIEGFRIEDX` | 20 | 8 | 99.94% |
| `XSCHUSTERXSCHUSTERX` | 19 | 8 | 98.8% |
| `XZANDERSXZANDERSX` | 17 | 8 | ~0% (too short) |
| *average 20-letter crib* | 20 | ~15 | 97.5% |

The rule that falls out: **a doubled name of eight letters or more.** `SCHUSTER`
(8) works, `ZANDERS` (7) does not.

**4.4 The corpus is full of long recurring names.** Tokens appearing in more
than one place, with their length and how many distinct letters they use:

```
   STUERZBECHER  ×4   (12 letters,  9 distinct)
   SCHUSTER      ×4   ( 8 letters,  7 distinct)
   OPOTSCHKA     ×4   ( 9 letters,  8 distinct)
   ZANDERS       ×4   ( 7 letters,  7 distinct)
   SCHNEIDER     ×3   ( 9 letters,  8 distinct)
   HARTJENSTEIN  ×2   (12 letters,  9 distinct)
   TSCHEDINOVA   ×2   (11 letters, 11 distinct)
   BETRIEBSSPRUQ ×2   (13 letters,  9 distinct)
```

Doubled, `STUERZBECHER` gives a 27-letter crib using 10 distinct letters —
comfortably inside the cheap band, with rejection to spare.

**4.5 Two ways cribs fail, both seen in this corpus.**

*Garbling.* Message 44 reads `...LUGWIG X FRIEDRIVH X HRINRIHO...` — that is
`LUDWIG`, `FRIEDRICH`, `HEINRICH`, all corrupted in transmission. An exact-match
crib spanning corrupted text fails outright.

*Punctuation variance.* The same unit appears in two messages as:

```
   message  0:   ...VIERX[ SIEGFRIEDSIEGFRIEDTONI ]XDIVX...
   message 25:   ...MASTX[ SIEGFRIEDXSIEGFRIEDXTONI ]XDIVX...
```

Same words, two extra `X` separators. The longest run they share drops to 10
letters, which is still usable but costs perhaps fifteen minutes instead of two.
A crib built from one form misses the other form completely. **The generator
must produce punctuation variants**; this is not an edge case.

**4.6 A crib does two separate jobs.** Worth stating because they fail at
different lengths:

1. **It calculates the plugboard.** This is the big win — it replaces a search
   over roughly 3,000 candidate boards per rotor setting with a single
   calculation.
2. **It rejects wrong rotor settings.** This needs loops, and fades as the crib
   gets shorter.

At 16 letters you keep job 1 and mostly lose job 2, and that is still worth
having — which is why short cribs stay usable rather than failing. Job 1 is what
replaces the search; job 2 only decides how much checking is left over. The
folklore "20 letters, three loops" is right for a 1941 Bombe, where every stop
cost a human being several minutes at a checking machine; on a computer a stop
costs microseconds.

---

## 5. Step by step: the crib generator

A separate program, run once, producing a file. It does not touch `enigma.cc`.
Suggested name: `eval/build_cribs.py`, output `cribs/<name>.cribs`.

**Step 1 — collect words.** Split every message in the source corpus on the
letter `X` and keep the pieces of four letters or more. From our 58 messages
that gives 331 distinct words.

**Step 2 — add vocabulary that is not in the corpus.** The existing
`cribs/german-hgnord.txt` already lists generic telegraphic vocabulary — spelled
out numbers, the phonetic alphabet, standard military nouns. These are guessable
without having seen the traffic, which matters for messages unlike anything in
the corpus.

**Step 3 — build candidate phrases.** For each word, emit the doubling variants
(this is where §4.5's punctuation problem is handled):

```
   word = SCHUSTER

   XSCHUSTERXSCHUSTERX      19 letters
   XSCHUSTERSCHUSTERX       18
   SCHUSTERXSCHUSTER        17
   SCHUSTERSCHUSTER         16
   XSCHUSTERX               10   (too short on its own — see step 5)
```

Also emit combinations that the corpus shows recurring, such as a name followed
by a phonetic letter (`SIEGFRIEDXSIEGFRIEDXTONI`).

**Step 4 — score each candidate offline.** Two numbers, both computable without
any ciphertext:

- **length** — 16 to 18 is the target band (§4.1). Do not insist on 20: it is
  six times rarer for almost no saving.
- **spare letters** = length − number of distinct letters. This predicts whether
  the menu will close. Measured: at 20 letters, cribs with 8 or more spare
  letters close twice over 84% of the time; cribs with 5 spare only 65%.

These two rank *different* things, and §7 shows how far apart they can be.
Length predicts how many plugs get deduced; spare letters predict how many rotor
settings get rejected. `NULLNULLNULL` and `UNITIONFUERL` are both 12 letters and
both deduce ~6.7 cables, but reject 81% and 0%. Sort by spare letters — the
checking cost varies by a factor of 500 across those two, while coverage barely
moves.

**Step 5 — sort into tiers, do not simply cut.** Since a short crib costs more
time rather than failing, the library should be *tiered* by length and used in
that order:

| tier | length | cost each | hits | when to use it |
|---|--:|--:|--:|---|
| 1 | 16-20 | 2-3 min | 3-19% | always try first |
| 2 | 14-15 | ~6 min | 24% | no tier-1 crib matched |
| 3 | 12-13 | ~11 min | 55% | nothing longer has matched |

Within each tier, sort by spare letters descending. How many to keep is set by
the compute budget — see §9.

**Step 6 — write the file**, one crib per line, with its two scores as comments
so a human can inspect the ranking.

**What this step deliberately does not do:** generate text from the n-gram
tables. A crib must be *exactly* right, and text sampled from a language model
is plausible but almost certainly not the actual plaintext. Generation must come
from templates that really do recur word for word.

---

## 6. Step by step: the solver inside `enigma.cc`

**6.1 Where it goes.** The tool already loops over rotor settings and, for each,
calls `hillclimb()` to find a plugboard. The crib solver is an alternative to
that call. Everything outside — the key sweep, threading, scoring, reporting —
is untouched.

**6.2 Build the menu once per crib and alignment, not per rotor setting.** The
menu depends only on the crib and the ciphertext, both of which are fixed before
the sweep starts. Building it inside the per-key loop would be a large and
pointless cost.

For crib `XSIEGFRIED...` at alignment 3, the menu records: *position 3 joins
letter `X` to whatever the ciphertext has at position 3*, and so on for every
crib letter.

**6.3 Keep only the largest connected part of the menu.** A menu often falls
into several disconnected pieces. Each separate piece needs its own starting
guess, and 26 guesses per piece multiply: two pieces means 676 combinations,
three means 17,576. A small extra piece costs far more than it contributes.

Discarding all but the largest piece means there is **always exactly one
starting guess, so always exactly 26 hypotheses to try.** For a typical
20-letter crib the largest piece holds about 17 of the 20 positions, so little
is lost.

*This was got wrong once while planning.* Costing the solver as "one guess per
piece" made an average 20-letter crib look nearly useless (28% of settings
rejected). Using only the largest piece — which is what a real Bombe menu is —
the same crib rejects **97.5%**. The lesson is that the algorithm choice, not
the crib, was the limiting factor.

**6.4 For each rotor setting, try 26 hypotheses.** Pick one letter in the menu.
Suppose it is plugged to `A`, then to `B`, and so on through all 26
possibilities. For each:

- walk the menu, using `steck[p] = rows[i][steck[ct[i]]]` at each step to derive
  the next plug;
- whenever a plug is derived, immediately record **both** ends — deriving "`B`
  is plugged to `K`" also fixes "`K` is plugged to `B`". *This is Welchman's
  diagonal board, and we get it for free*, because the tool already stores the
  plugboard as a reciprocal table;
- if any derivation contradicts something already recorded, the hypothesis dies.

If all 26 hypotheses die, the rotor setting is rejected outright. Otherwise it
is a *stop*.

**6.4a A worked example.** Crib `XSIEGFRIEDXS` — twelve letters — with the true
rotor key and a real ten-cable board hidden:

```
   crib     X S I E G F R I E D X S
   cipher   O N M T U Q J T G C D X
```

The **core** is the rotor stack plus reflector with the plugboard taken off: one
26-letter table per position, different at each because the rotors step. These
are the real ones for this key — note the right-hand rotor offset counting up:

```
              ABCDEFGHIJKLMNOPQRSTUVWXYZ
   core[ 0]   JLYQUTOPSANBVKGHDXIFEMZRCW    offsets  4  4  4
   core[ 4]   FRJOHATEZCWYSPDNVBMGXQKULI    offsets  4  4  8
   core[ 6]   HOETCWVAQSYRPUBMILJDNGFZKX    offsets  4  4 10
   core[10]   JQEZCMOTYAPXFSGKBUNHRWVLID    offsets  4  4 14
```

Read `core[0]` as A→J, B→L, … X→R. Two properties, both from the reflector, and
both verified across all twelve tables:

- **each core is its own inverse** — `core[0]` sends `X` to `R` *and* `R` to
  `X`, so the deduction runs in whichever direction you have knowledge;
- **no core maps a letter to itself** — the same fact that lets §6.6 rule out
  alignments.

Now guess *X is unplugged* and let it run:

```
   pos  0   X->X known, so core[ 0][X] = R    gives  O-R
   pos  6   R->O known, so core[ 6][O] = B    gives  J-B
   pos 10   X->X known, so core[10][X] = L    gives  D-L
   pos 11   X->X known,                       gives  S-U
   pos  1   S->U known,                       gives  N-P
   pos  4   U->S known, so core[ 4][S] = M    gives  G-M
   pos  8   G->M known,                       gives  E-F
   pos  9   D->L known,                       gives  C-H
   pos  2   M->G known,                       gives  I-K
   pos  3   E->F known,                       gives  T-T
   pos  5   F->E known,                       gives  Q-A

   result:  AQ BJ CH DL EF GM IK NP OR SU
```

That is the complete true plugboard — all ten cables — from twelve crib letters,
and for this crib **only 1 of the 26 hypotheses survives** at the true rotor
setting. Wrong guesses die instead:

```
   X->B :  ... pos 4  G->F known, gives U-A  ->  contradiction, dead
   X->M :  ... pos 1  S->A known, gives N-T  ->  contradiction, dead
```

Three things this makes concrete:

**No loops were used.** Every line derives something new; none comes back to
re-check a letter already assigned. That is why §4.1's short cribs still work.

**The chain does not run left to right** — it goes 0, 6, 10, 11, 1, 4, 8, 9, 2,
3, 5, jumping to whichever position has just become solvable, like filling in a
crossword.

**Reciprocity does the heavy lifting.** Twelve crib letters produced *twenty*
letters of plugboard, because deriving "O connects to R" at once gives "R
connects to O", which unlocks position 6, which unlocks more. That is the
diagonal board of §6.4, and it is free here.

One caveat: this trace is at the **true** rotor setting. At a wrong setting
around nine hypotheses survive at this crib length, each yielding a
plausible-looking board that still has to be decrypted and scored — which is the
585 seconds of checking in §4.1's table.

**6.5 Follow up each stop.** A surviving hypothesis gives a partial plugboard —
the letters the crib touched. Fill in the rest (see §7), decrypt the whole
message with the existing decoder, and score it with the existing n-gram scorer.
Keep the best.

**6.6 Sweep alignments.** Repeat for every position the crib could sit at. One
cheap filter first: an Enigma never encrypts a letter to itself, so any
alignment where the crib letter matches the ciphertext letter is impossible.
That removes about 54% of alignments for a 20-letter crib — useful, but do not
expect more from it. Pinning the position uniquely would need a crib of about
130 letters.

---

## 7. The hybrid: deduce, then climb

This is the part that justifies extending the tool rather than writing a Bombe.

A crib rarely determines the whole plugboard. **Measured** — deducing from the
true hypothesis at the true rotor setting, averaged over 300 random keys and
boards:

| crib | letters settled | true cables found | shown unplugged |
|---|--:|--:|--:|
| `NULLNULLNULL` (12) | 16.3 / 26 | **6.9 / 10** | 2.5 |
| `UNITIONFUERL` (12) | 15.8 | 6.6 | 2.5 |
| `XSIEGFRIEDXS` (12) | 15.8 | 6.6 | 2.6 |
| `XSIEGFRIEDSIEGFRIED` (19) | 21.5 | 8.8 | 3.9 |
| `XFORDXFORDXVIKTORXAQTX` (22) | 23.2 | **9.3** | 4.5 |

So a 12-letter crib typically hands over **seven of the ten cables** and
confirms two or three letters as unplugged, leaving about ten free letters and
three cables for the climb. A 22-letter crib leaves barely anything.

**The two jobs are driven by different properties of the crib**, which the top
four rows show plainly. All four 12-letter cribs deduce the same ~6.7 cables,
yet `NULLNULLNULL` rejects 81% of rotor settings and `UNITIONFUERL` rejects 0%
(§4.1). Coverage tracks crib **length** — the number of edges, hence the number
of chances to derive something. Rejection tracks letter **repetition**, which is
what closes loops. Ranking a library by spare letters (§5 step 4) optimises
rejection and does nothing for coverage; ranking by length does the reverse.

**A note on why coverage is as high as it is.** `NULLNULLNULL` has only three
distinct plaintext letters, so its menu is three separate stars. Left alone
those stay disconnected and the deduction would settle perhaps seven letters.
What merges them is reciprocity: deriving that `L` connects to some letter that
also appears on `N`'s star joins the two, and the cascade continues. The
diagonal board (§6.4) therefore does more than add rejection power — it stitches
the menu's components together, roughly doubling coverage in this case.

The plan:

1. **Deduce** the plugs the crib determines. These are known with certainty, not
   guessed.
2. **Freeze** them. The tool already supports exactly this: the `-s` option
   marks plugs as fixed so the climb never rewires them.
3. **Climb** the rest, using the existing hill-climb on the remaining free
   letters only.

This is strictly better than either method alone. Compared with the pure climb,
the search space drops from all 1.5 x 10^14 boards to the few cables left over
about ten free letters. Compared with a pure Bombe, a crib that determines only
part of the board still produces a full answer.

**How much this is worth, measured.** Giving the true rotor key and *N* of the
ten cables via `-s`, then climbing the rest with `-R 16 -J --polish`, 40 trials
per cell on authentic telegraphic German:

| message length | 0 given | 5 given | **7 given** | 8 given |
|--:|--:|--:|--:|--:|
| 60 | 2% | 52% | **62%** | 68% |
| 100 | 15% | 80% | **82%** | 85% |
| 150 | 50% | 95% | **95%** | 100% |

At 60 letters seven known cables take exact recovery from **2% to 62%** — a
factor of 31 — and the gain is largest exactly where the tool is weakest today.

**The curve saturates early, and that is the useful part.** Going from 0 to 5
cables buys +50 points at L=60; 5 to 7 buys only +10; 7 to 8 buys +6. Almost all
the value is in the first five. Two consequences:

- **The crib does not have to be good.** A deduction recovering only 5 of 10
  cables captures roughly 80% of the available benefit, and even a 12-letter
  crib averages 6.9. Essentially any usable crib is past the knee.
- **A partly wrong crib may still help.** Some deduced plugs being wrong costs
  the climb the work of undoing them, but if enough are right the hybrid still
  starts well up the curve.

**One thing the tool cannot currently express.** The deduction settles about 2.5
letters as *definitely carrying no cable* (§7's table above counts them), and
`-s` cannot say that — it fixes pairs only. Those letters are then left free and
the climb wastes moves trying to plug them. Marking them would shrink the free
set from roughly 12 letters to 9 at no cost. See §8.

It also degrades gracefully, which matters given §4.5. If the crib is slightly
wrong, the deduction produces a bad partial board — but the follow-up score will
be poor and that alignment simply loses to another.

---

## 7a. Crib as a seed: very short cribs, 8-12 letters

§7's hybrid assumes the crib is long enough to pick out one hypothesis. Below
about 14 letters it cannot, and a different use opens up: stop trying to *solve*
the plugboard and use the crib only to *start* the hill-climb somewhere better
than random.

**What a very short crib deduces**, from the true hypothesis at the true rotor
setting, 300 random keys and boards:

| crib length | letters settled | true cables found |
|--:|--:|--:|
| 6 | 7.0 / 26 | 3.0 / 10 |
| **8** | 9.7 | **4.1** |
| **10** | 12.5 | **5.3** |
| 12 | 15.5 | 6.5 |
| 16 | 20.7 | 8.5 |

A 10-letter crib lands on five cables — exactly the knee of §7's value curve,
where most of the benefit already sits.

**The cost, and why it is not obviously worth paying.** A crib this short
rejects nothing (§4.1), so all 26 hypotheses survive at every rotor setting and
each one gives a *different* seed. You must climb from all of them, so the sweep
costs **26 climbs per rotor setting** instead of one. That is the entire
objection, and it has to be met head-on: is 26 seeded climbs better than 26
ordinary restarts?

**Measured, at L=60 with the true rotor key, 40 trials:**

| | recovery |
|---|--:|
| 0 plugs given, `-R 16` | 0% |
| 0 plugs given, `-R 416` — the 26x compute spent on restarts | **12%** |
| 5 plugs given, `-R 16` — one correct seed among 26 | **55%** |
| 7 plugs given, `-R 16` | 68% |

Only one of the 26 hypotheses carries the correct seed, so the crib-seeded sweep
succeeds at about the 55% rate against 12% for the same compute spent on random
restarts. **Four to five times better**, at the short lengths where the tool is
currently weakest.

**Why this is a different kind of lever.** `IMPROVEMENTS.md` records that
restarts stall because the truth is a rare deep basin and no truth-free signal
exists to steer toward it — per-plug consensus across converged boards is only
~1.1 correct plugs in 10. A crib is exactly such a signal, and it comes from
*outside* the score landscape rather than being mined out of it. That is why it
can beat compute rather than merely adding to it.

**Three cautions, none of them yet measured.**

1. The table above seeds the climb with *correct* plugs. In a real sweep 25 of
   the 26 hypotheses seed garbage, and a garbage seed at the true rotor setting
   could in principle converge to something scoring higher than the correct seed
   does. The ~1% scoring-failure floor makes that unlikely, but unlikely is not
   measured.
2. At *wrong* rotor settings you now run 26 climbs instead of one. That is where
   the whole 26x goes, and without loops none of it can be skipped.
3. Coverage assumes the crib is exactly right. An 8-letter crib is far more
   likely to be present (§4.2) *and* far more likely to be a coincidental match
   that deduces confident nonsense. Short cribs make both errors more common at
   once.

**Where this sits in the plan.** It shares all the machinery of §6 — the same
menu, the same deduction, the same `--no-plug` output — and differs only in what
it does with the result: seed rather than solve. So it costs almost nothing
extra to build once §6 exists, and it should be a flag rather than a separate
path.

---

## 8. Suggested command line

Consistent with existing options; names open to discussion.

```
  --crib TEXT          the crib, as plaintext letters
  --crib-list FILE     a file of cribs, one per line (from §5)
  --crib-at N          pin the crib to position N (default: sweep all)
  --crib-min-loops N   skip alignments whose menu has fewer than N loops
  --no-plug LETTERS    these letters are known to carry no cable
  --crib-seed          seed the climb from the deduction instead of
                       requiring it to solve the board (§7a)
```

Notes:

- `--crib` is a *different feature* from the existing `--crib-file`, which only
  re-ranks finished boards and was measured to be worth about −0.1 percentage
  points. Two similarly named options doing unrelated things is a trap; renaming
  the old one to `--crib-rerank` is worth considering.
- The crib mode should imply the hybrid of §7 by default, since the pure
  deduction rarely yields a complete board.
- **`--no-plug` is nearly free to build.** The climb already consults a
  per-letter table, `plug_fixed[]`, and skips any letter marked in it; `-s` sets
  it for both ends of each given pair. All `--no-plug` needs is to set the same
  flag for a letter while leaving the board unpaired there. It is useful on its
  own — a user who knows a letter was never steckered can say so today only by
  inventing a fake pair — but its real job is carrying the deduction's
  "definitely unplugged" findings into the climb (§7).
- `--no-plug` needs no special handling for `--exhaust`, whose per-worker
  `PLUG_FIXED_EX` is a copy of `plug_fixed`, nor for the `--score` plug caps,
  which count pairs and are unaffected by marking a letter unpaired.

---

## 9. What it should cost

From measurements on a 4-core machine, for one 200-letter message with the
plugboard unknown:

| approach | time | outcome |
|---|--:|---|
| today: climb every rotor setting | 24.9 h *(measured)* | ~50% success |
| with one crib | ~2 min *(estimated)* | certain, if the crib is right |
| for comparison: plugboard given | 137 s *(measured)* | certain |

The third row is the useful one. **A good crib is worth roughly as much as being
handed the plugboard** — both leave you with just the rotor sweep.

The estimate assumes the deduction costs about as much per step as decoding one
character. The deduction is branchier than the tool's tight scoring loop, so
2–3× worse would not be surprising. Even at six minutes the conclusion holds.

**The crib budget.** Since a wrong crib is cheap to reject, you can afford to
try many. At roughly two minutes each against 24.9 hours, the budget is on the
order of **several hundred cribs** per message — which sets *N* in §5 step 5.
Trying hundreds does not risk a false answer: at 200 letters the message is
about eight times longer than the point at which the true decryption becomes
unique, so wrong candidates cannot masquerade as German.

---

## 10. How to verify it works

The repo's standing rule is that a claim of exactness is tested by
*equivalence*, not by success rate. Applied here:

**10.1 Deduction correctness.** Take a known message, key and plugboard. Give
the solver the true crib at the true alignment. Every plug it deduces must match
the true board. Any mismatch is a bug, not a near miss.

**10.2 No false rejection.** With the true crib and the true rotor setting, the
true hypothesis must survive all 26 tests. Run this across many random keys,
message lengths, and all three machine types (standard, Norway, M4). Zero
failures is the only acceptable result.

**10.3 Stop rate.** Measure the fraction of wrong rotor settings that survive,
and check it against §4.1. A large disagreement means the menu logic is wrong.

**10.4 End-to-end recovery.** Plant a known crib in an authentic message,
encrypt under a random key and board, and check the tool recovers the plaintext.
Compare against today's hill-climb at matched wall time.

**10.5 The hybrid beats both parts.** Compare deduce-only, climb-only, and the
§7 hybrid on identical problems.

**10.6 Nothing else changes.** With no crib option given, output must be
byte-identical to today, and `make bench` must show no hot-path movement. This
is the same standard `--ring-stride` and `--polish` were held to.

A new test tier will be needed: `make crackquality` measures plugboard recovery
with the rotor key given, so it cannot see this feature at all. Planting cribs
in known plaintext is a new harness.

---

## 11. Risks, honestly stated

**The crib supply is the real constraint, not the algorithm.** The solver is
maybe 300 lines. Whether it ever fires depends on the library, and the library
is the part we cannot measure well.

**The 58-message corpus is too small to size the library.** At 20 letters it
contains exactly *one* phrase shared between two messages. The 3%/9%/19% hit
rates in §4.2 come from that sample and should be treated as "we found a few",
not as estimates. A real network's traffic would differ, probably favourably —
more messages, more consistent formats — but this is not evidence, it is
expectation.

**Garbling breaks exact matching.** Two of the five `SIEGFRIED` messages are
badly corrupted. No amount of crib generation fixes that; only a
mismatch-tolerant solver would, and that is a much bigger project.

**Punctuation variance may be worse than measured.** We found one clear case
(§4.5) in a small corpus. If separator placement is largely at the operator's
discretion, the variant explosion could eat the crib budget.

**This helps one situation and not the general one.** Without a crib the tool
still has to climb. Nothing here improves the no-crib case, which is what `make
crackquality` measures and what most of the tool's tuning targets.

---

## 12. Suggested order of work

Each step should be worth doing even if the next one never happens.

**Step 1 — the crib generator** (§5), plus a coverage report. Needs no changes
to the tool at all, and tells us whether there is a supply problem before any
C++ is written.

**Step 2 — a menu builder and offline analysis**, in Python. Confirms §4.1's
numbers independently and produces the test vectors step 3 needs.

**Step 3 — deduction inside the tool**, one crib at one alignment (`--crib`,
`--crib-at`). The smallest thing that can be checked against §10.1 and §10.2.

**Step 4 — the alignment sweep**, with the self-encryption filter. This is what
makes it usable on a real message.

**Step 5 — the hybrid** (§7). The mode we expect to be used in practice.

**Step 5a — crib-as-seed** (§7a). Nearly free once step 5 exists, and the mode
that covers the short cribs the corpus actually supplies. Needs its own
measurement of caution 1 above before being recommended.

**Step 6 — crib lists** (`--crib-list`) and the budget logic. Only worth
building once single cribs work.

**Step 1 is the decision point.** If a generated library covers a useful
fraction of held-out messages, the rest follows. If it covers almost nothing,
the honest conclusion is that this approach needs a larger or more uniform
corpus than we have, and the effort belongs elsewhere.

---

## 13. Open questions

1. **How many cribs can we usefully hold?** §9 says several hundred by compute.
   Whether the generator can produce several hundred *good* ones is untested.
2. **Should the crib mode reject or rank?** Rejecting a rotor setting outright
   is faster; keeping a score for every setting is more robust to a slightly
   wrong crib. Perhaps both, under a flag.
3. **Should `--crib-file` be renamed?** Two options with nearly the same name
   doing unrelated things is a documented trap in this repo's own history.
4. **Is the X-separator variant worth building?** Knowing only *where the
   separators are* — not what the words are — is a valid crib and an unusually
   efficient one: 14 known separator positions reject as strongly as a 22-letter
   phrase, because every deduction starts from the same letter and so needs only
   one guess. But the positions have to come from somewhere, and knowing 14 word
   boundaries may be as hard as knowing a phrase. Worth a measurement before any
   code.
5. **Can the menu be reused across alignments?** Shifting a crib by one position
   changes every edge, so probably not — but it is worth checking before
   assuming the alignment sweep costs full price each time.
