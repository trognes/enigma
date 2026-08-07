# cribs.md — crib-driven plugboard deduction: the plan, and what was built

**Status: BUILT. All seven steps of §12 (0 through 6) are done**, and the
document is now a record as much as a plan. Shipped in the tool: `--crib`,
`--crib-at` (optional — omit it to sweep every alignment), `--crib-dump`,
`--crib-list`, `--no-crib-reorder`, `--no-plug`, `--full-text`, and the rename
of the old `--crib-file` to `--crib-rerank`. Shipped in `eval/`:
`build_cribs.py` (§5a), `crib_menu.py` (§4.1), `crib_vectors_check.py` (§10.1,
§10.2) and six measurement probes. `tests/run_tests.sh` carries the crib checks
of §10.

**Read §12 for the step-by-step record and §13 for what is still open.** Four of
§13's five questions remain (crib supply at scale, reject-vs-rank, the
X-separator variant, menu reuse across alignments); the fifth is answered in
§7c. Three items were deliberately **closed or dropped** rather than left
pending, each because a measurement said so: the §6.7 early-exit score
threshold, held-out crib ordering, and a `--crib-max-hyps` pruning flag that was
built, measured and removed.

**Sections written before the build have not been rewritten**, because the
reasoning that led somewhere is worth keeping even where the destination
differs. Where the build corrected the plan, the correction is recorded at the
point it applies — §4.1 (the diagonal board, not menu loops, does the work),
§8 (proposal against shipped surface), §12 (every step). The numbers quoted are
from measurements; where a figure is an estimate rather than a measurement, it
says so.

---

## 1. What this is about, in one page

Today the tool breaks a message like this: try every rotor setting, and for each
one *search* for a plugboard by hill-climbing — try a plug, see if the text
scores more like German, keep it if so. The search is the expensive part. We
measured it at **0.338 milliseconds per rotor setting**, which over the full
60-wheel-order space at 200 letters comes to **24.9 hours**, and even then it
recovers the right answer only about half the time.

A **crib** is a guess at some of the plaintext — say, you believe the message
contains the word `SIEGFRIED` starting at letter 40. If the guess is right, some
of the plugboard follows by arithmetic instead of search. **Not all of it**: a
12-letter crib settles about 7 of the 10 cables, a 10-letter one about 5. What
the crib gives you is a *starting point* — a partly-built board that the
existing hill-climb finishes, instead of the empty board it starts from today.
That is what this plan is about.

How much that is worth: at 60 letters, climbing from an empty board recovers 3%
of messages; climbing from five known cables recovers 77%.

The idea is not new — it is what the wartime Bombe machines did. What is new
here is that the machinery it needs is already sitting in this tool, unused.

**Rough gain**, from the same measurements: with a crib long enough to narrow
the rotor settings too (16 letters or more), the 24.9-hour job comes down to the
order of **two minutes**. Shorter cribs do not narrow the rotor sweep at all and
cost more, but still start the climb from a good board rather than an empty one.
The two-minute figure is arithmetic on a measured throughput, not a benchmark —
read it as "minutes, not hours".

**The catch** is that you need a crib and it has to be exactly right. Length
matters less than it might seem: there is no hard minimum, only a rising cost
(§4.1, §7a). So the plan spends most of its space on *supply* — where cribs come
from, and what to do with the short ones the traffic actually provides.

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
existing tool. A standalone Bombe cannot do it at all — and the measurements
since have made that decisive rather than merely likely: the corpus supplies
mostly *short* cribs (§7a: 79% of messages carry a 10-letter one, 3% a 20-letter
one), and a 10-letter crib rejects nothing at all. It can only seed a climb.
Without the climb to hand it to, most of the available cribs are worthless.

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
back consistent. Measured with `eval/crib_menu.py`, using the largest connected
part of the menu (see §6.3 for why only the largest):

| crib length | loops | menus with a loop | rotor settings rejected |
|--:|--:|--:|--:|
| 8 | 0.10 | 10% | 16.4% |
| 10 | 0.05 | 5% | 55.1% |
| **12** | 0.28 | 20% | **90.6%** |
| 14 | 0.42 | 38% | 97.9% |
| 16 | 0.68 | 52% | 99.9% |
| 18 | 1.73 | 90% | 100% |
| 20 | 2.08 | 92% | 100% |
| 25 | 5.08 | 100% | 100% |

**Loops are not the mechanism, and that is the single most important fact in
this section.** The rejection above is almost entirely the work of Welchman's
diagonal board — the constraint that a plug has two ends, so `steck[x] = y`
forces `steck[y] = x` and no two letters may share a partner. Split the same
measurement by whether the menu has a loop at all:

| crib length | rejected, no loop | rejected, ≥1 loop |
|--:|--:|--:|
| 8 | 7.8% | 94.0% |
| 10 | 53.0% | 95.5% |
| 12 | 88.3% | 99.7% |
| 16 | 99.7% | 100% |

A **loop-free** 12-letter menu still rejects 88% of rotor settings. Turn the
diagonal board off and run the identical trials and that column collapses to
**0.00% at every length** — rejection then comes only from loops, exactly as the
textbook account says. That control is what identifies the mechanism, and it is
reproducible: `eval/crib_menu.py --no-diagonal`.

This is Welchman's 1940 result rediscovered, and it is why the diagonal board
mattered historically: it is what makes short, loop-free menus usable.

**Where this leaves the plan, so far.** The useful floor moves from about 16–20
letters down to about **12**, and that is the length the corpus actually
supplies: **55% of messages carry a 12-letter crib**, against 3% for a 20-letter
one (§7a). The two halves of the problem — a crib strong enough to be worth
trying, and a crib you are likely to have — now overlap, which on the old
numbers they barely did. Two consequences the rest of this document has not yet
absorbed: the 8–11 letter band is **not** seed-only (8 letters rejects 16%, 10
rejects 55%), and §5's tier boundaries were drawn on the old figures.


> ⚠️ **That floor of 12 holds only when you know WHERE the crib sits.** Every
> rejection figure in this section is *per alignment*. A real run does not know
> the alignment and has to sweep, and rejection then compounds multiplicatively
> — see §4.2a, which puts the swept floor back at **16**.

Whether this converts into wall time is **now measured, in §4.2b**, and it
converts in a way the cost table below does not express: a deduction step costs
about what decoding one character costs, the checking unit is a surviving
*hypothesis* rather than a surviving *key*, and the conversion has the opposite
sign against a scan and against a climb. The table below stands as history —
read §4.2b for what it should say.

> ⚠️ **Earlier versions of this table were measured without the diagonal board
> and are wrong by orders of magnitude at the short end** — they read 0.00% at 8
> and 10 letters and 0.02% at 12, against 16%, 55% and 91% here. The
> `--no-diagonal` control reproduces those old figures closely (28.98% at 16
> letters against the old 28.8%), which is what identifies the omission.
> **Everything downstream that was computed from the old rejection rates is
> therefore suspect**: the cost table below, the tier boundaries in §5 step 5,
> the claim that 8–11 letter cribs "can only seed", and the `COST` model in
> `eval/build_cribs.py` that prices the library. Those are corrected where the
> new measurement settles them and flagged where recomputing needs a timing run
> that has not been done.

**There is no hard floor.** A setting the crib fails to reject is not a failure,
just a *stop* that has to be checked by decrypting the message and scoring it —
and on a computer a stop costs microseconds. So a weak crib does not stop
working, it only shifts effort from rejecting settings to checking them.

Adding both costs, for **one crib** tried at **every alignment** against **every
rotor setting** — 60 wheel orders × 26³ positions — in a 200-letter message.
**The rejection column here is the pre-diagonal-board one and is wrong**; the
table is kept because its *sweep* column and the shape argument below do not
depend on it, and because it is what `eval/build_cribs.py`'s cost model was
built from. **The checking column is wrong in its unit as well as its
arithmetic** — it charges one check per surviving key, and §4.2b measures up to
235 per key — so read it as the shape argument only:

| crib length | alignments | rejected | sweep | checking | total |
|--:|--:|--:|--:|--:|--:|
| 8 | 141 | 0.00% | 79 s | 1420 s | 1499 s |
| 10 | 129 | 0.00% | 91 s | 886 s | 977 s |
| 12 | 118 | 0.02% | 100 s | 548 s | 648 s |
| 14 | 108 | 2.0% | 106 s | 229 s | 336 s |
| **16** | 99 | **28.8%** | 111 s | 67 s | **178 s** |
| **18** | 90 | **85.3%** | 114 s | 8 s | **122 s** |
| 20 | 83 | 98.3% | 116 s | 1 s | 117 s |
| 25 | 66 | ~100% | 116 s | 0 s | 116 s |

("Alignments" is after the self-encryption filter of §6.6, which removes about
half of them.)

**The sweep is flat, and that is the important shape.** A longer crib costs more
work per alignment but has fewer alignments to try, and the two almost exactly
cancel — every row sweeps in 100–117 seconds. So crib length does not really
trade against sweep cost at all. It trades **hit rate against checking cost**,
and nothing else.

A 14-letter crib rejects almost nothing and still finishes in under six minutes;
a 10-letter one, which rejects nothing whatever, in sixteen. The curve is
smooth, not a cliff. What 16–18 letters buys is the *cheapest* total, not the
only workable one.

The "20 letters, three loops" rule of thumb comes from 1941, when every stop
cost a human several minutes at a checking machine. That economics is gone, and
the length guidance moves with it.

**4.2a Sweeping alignments multiplies the survival, and that sets the real
floor.** A run that does not know where the crib sits must try every alignment
the self-encryption filter leaves, and a rotor setting is only rejected if it is
rejected at *all* of them. Rejections therefore multiply: with per-alignment
rejection `p` over `A` alignments the setting survives unless every one of them
kills it, so what matters is `∏ p_i`, not `p`.

Measured end to end on a 125-letter message (`--crib` with and without
`--crib-at`, same crib, 17 576 keys):

| crib length | viable alignments | rejected, pinned | rejected, sweeping |
|--:|--:|--:|--:|
| 8 | 86 | 44.4% | **0.0%** |
| 10 | 78 | 96.1% | **0.0%** |
| 12 | 71 | 99.9% | **5.3%** |
| **16** | 56 | 100.0% | **99.9%** |
| 20 | 45 | 100.0% | 100.0% |
| 25 | 36 | 100.0% | 100.0% |

**16 letters is the floor for a swept run**, and the transition is abrupt: one
step down, at 12 letters, the filter throws away 5% of the keyspace instead of
99.9%.

The arithmetic is ordinary compounding, not something subtler. Measuring each of
the 70 alignments separately for the 12-letter crib gives per-alignment
rejection from **63.4%** to 100% (median 99.2%), and the product of those
seventy numbers is **5.2%** against the 5.3% measured. Two things follow. The
weakest alignments dominate — a single 63% one costs more than thirty 99% ones —
and a crib that looks strong on average can still be useless swept.

**This restores §5's 16-letter tier boundary, for a different reason than the
plan gave.** The old rationale was that loops appear around there; §4.1 showed
loops are not the mechanism. The real reason is that 16 letters is where
per-alignment rejection gets close enough to 1 to survive being raised to the
70th power.

It also sharpens where each mode applies. **Pinned, 12 letters is plenty**;
swept, it is not. And since only about 19% of messages carry a 16-letter crib
against 55% for a 12-letter one (§4.2), the common case is a crib that cannot
filter a swept search at all — which is exactly the case §7a's crib-as-seed mode
exists for, and is now the measured argument for it rather than an expectation.

**4.2b What the deduction itself costs — and the unit the cost table got
wrong.** Every figure above prices the *climb* and assumes the deduction is
negligible: "one table lookup". It is not one lookup, it is 26 chained
hypotheses at every viable alignment, so a swept short crib runs up to ~2 300 of
them per rotor setting before any climb starts. Measured by
`eval/crib_deduce_cost.py` on a 125-letter message over 17 576 keys, as a
**plain scan** so no climb is involved and the only per-key work is
`setup_mapping`, the deduction, and one score:

| crib | mode | alignments | rejected | wall | net | per hypothesis |
|--:|---|--:|--:|--:|--:|--:|
| 8 | pinned | 1 | 44.4% | 0.13 s | +0.01 s | 22 ns |
| 8 | swept | 86 | 0.0% | 0.12 s | +0.00 s | — |
| 12 | pinned | 1 | 99.9% | 0.13 s | +0.01 s | 31 ns |
| 12 | swept | 71 | 5.3% | 0.68 s | +0.57 s | 17 ns |
| 16 | pinned | 1 | 99.9% | 0.13 s | +0.01 s | 28 ns |
| **16** | **swept** | 56 | **99.9%** | 1.38 s | **+1.26 s** | **49 ns** |
| 20 | pinned | 1 | 100% | 0.13 s | +0.01 s | 32 ns |
| 20 | swept | 45 | 100% | 1.15 s | +1.03 s | 50 ns |

The 16-letter swept row is the one that prices a hypothesis exactly, because
near-total rejection means almost every hypothesis really is tried: 25.6 M of
them in 1.26 s is **49 ns each**, and at 16 propagations per chain about **3 ns
per propagation**. That is roughly what decoding one character costs — which is
precisely the assumption §9's estimate rests on, now measured instead of
guessed. **Pinned, the deduction is free at every length** (+0.01 s on a 0.12 s
run).

**Against a plain scan a crib cannot pay for itself, and that inverts the
intuition.** Scoring one key costs ~7 µs; a swept deduction costs ~70 µs. So
rejecting 99.9% of a scan saves less than the sweep costs — the 16-letter row is
*ten times slower* than not using the crib at all. Rejection is only worth
buying when what it skips is expensive.

**Against a climb it is, by two orders of magnitude — where it rejects.** Same
key space, with `-c`:

| | wall | vs no crib | rejected |
|---|--:|--:|--:|
| no crib | 16.05 s | — | — |
| 12 letters pinned | 0.13 s | **126×** | 99.9% |
| 16 letters pinned | 0.13 s | **127×** | 100% |
| 16 letters swept | 1.36 s | 11.8× | 99.9% |
| 20 letters pinned | 0.13 s | 126× | 100% |
| 20 letters swept | 1.15 s | 14.0× | 100% |

**But "rejected" is the wrong unit, and this is the correction that matters.**
§4.1's cost table charges *checking* per surviving **key**. Under `-c` a
surviving key is not checked once — it is climbed once per surviving
**hypothesis**, and there are up to 26 of those per alignment. Counting them
directly with `--crib-dump`:

| crib | mode | keys rejected | surviving hypotheses | per surviving key |
|--:|---|--:|--:|--:|
| 8 | pinned | 44.4% | 13 786 | 1.4 |
| **8** | **swept** | **0.0%** | **4 139 010** | **235** |
| 12 | pinned | 99.9% | 14 | 1.0 |
| 12 | swept | 5.3% | 51 563 | 3.1 |
| 16 | pinned | 99.9% | 2 | 1.0 |
| 16 | swept | 99.9% | 9 | 1.0 |

**Where a crib rejects at all, the survivor is essentially unique.** At 12
pinned, 16 pinned and 16 swept every surviving key carries exactly **one**
surviving hypothesis, so the crib does not merely skip keys — it replaces the
whole plugboard search on the keys it keeps with a single seeded climb, which is
itself 3–12× cheaper than the unseeded one (§7a). That is where the 126× comes
from, and it is a bigger effect than rejection alone would give.

The explosion is confined to cribs too weak to reject anything. At 8 letters
swept, 235 hypotheses survive per key and the run costs **66× more** than no
crib at all (26-key space, boards scored — exact and startup-free):

| | boards scored | vs no crib |
|---|--:|--:|
| no crib | 79 922 | — |
| 8 letters pinned | 6 666 | **0.08×** |
| 8 letters swept | 5 319 938 | **66.6×** |
| 12 letters swept | 48 300 | 0.6× |

**So the seed mode of §7a needs a pinned or near-pinned alignment.** §7a priced
it at "26 climbs per rotor setting"; that is the *pinned* price, and pinned it
is a 12× *saving* even at 8 letters, because most of the 26 hypotheses die on
the diagonal board before any climb runs. Swept it is `alignments × 26`, and at
8 letters that is worse than doing nothing. §12 step 5's "seeding works swept as
well as pinned" stands as measured — but it was measured with the rotor key
*given*, one key, where the multiplier has nothing to multiply.

**4.2 How often a crib is actually present.** Building a crib library from 57 of
the 58 messages and testing it on the 58th:

| crib length | 8 | 10 | 12 | 14 | 16 | 18 | 20 | 25 |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| messages hit | 93% | 79% | 55% | 24% | 19% | 9% | 3% | 0% |

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
| *average 20-letter crib* | 20 | ~15 | 98.3% |

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

**Built: `eval/build_cribs.py`, output `cribs/wehrmacht.cribs`.** A separate
program, run once, producing a file; it does not touch `enigma.cc`. §5a gives
the results, which answer the go/no-go question this step exists to settle.

**Step 1 — collect words.** Split every message in the source corpus on the
letter `X` and keep the pieces of four letters or more. From our 58 messages
that gives 331 distinct words.

**Step 2 — add vocabulary that is not in the corpus.** The existing
`cribs/german-hgnord.txt` already lists generic telegraphic vocabulary — spelled
out numbers, the phonetic alphabet, standard military nouns. These are guessable
without having seen the traffic, which matters for messages unlike anything in
the corpus.

**The 19 entries of 8 letters or more are cribs in their own right**, not just
raw material for step 3's doubling — `ABENDMELDUNG`, `FELDLAZARETT`,
`VERPFLEGUNG`. Emitting them adds **+5pp of held-out coverage** (78% → 83%) for
19 cribs, and they belong at the **front of the library** even though they are
less likely to match than the best observed phrase: there are only 19, about
five hours between them, and they are the only cribs that owe nothing to this
particular corpus. Measured, trying them first covers **49 of 69** held-out
messages within a 25-hour budget against 42 when they follow the observed
phrases, and cuts the median time to the first hit from 10.1 to 6.7 hours for
the same total coverage.

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

**Step 4a — keep maximal phrases, and mark their sub-windows as derived.** When
a phrase recurs, every shorter window inside it recurs too. Harvesting 10-letter
windows from the corpus finds 150 shared by two or more messages — but they
merge into only **53 distinct phrases**; the other 97 are slices of those.

Emitting all 150 is worse than emitting none:

| | worst case, nothing matches | vs no crib at all |
|---|--:|--:|
| all 150 windows, 10 letters each | **40.7 h** | 1.63× |
| the 53 maximal phrases | **10.3 h** | 0.41× |
| no crib at all | 24.9 h | 1.00× |

**A sub-window is strictly dominated by its parent.** `XSIEGFRIEDSIEGFRIED` is
19 letters, in two messages; all ten of its 10-letter windows are in the same
two messages:

| | cost | rejects | cables |
|---|--:|--:|--:|
| the 19-letter parent | 122 s | 85% | 8.9 |
| any 10-letter window of it | 977 s | 0% | 5.3 |

Eight times the cost, for strictly less, over exactly the same messages.

**But do not discard them — demote them.** A window beats its parent in one
case: when the parent does not match. `SIEGFRIEDSIEGFRIED` and
`SIEGFRIEDXSIEGFRIED` differ by two separators, and the long crib misses the
second form entirely while `XSIEGFRIED` hits both; garbling does the same
(`COENIGSBNRG`). Sub-windows are a **hedge against §4.5's two failure modes** —
worth having last, worthless first.

So the generator must record which cribs are *derived* from a longer one it is
already emitting, and step 5 must hold those back behind every independent crib.
Length alone cannot express this: a derived 10-letter window and an independent
10-letter phrase look identical.

**Step 5 — order by how likely a crib is to match, not by what it could do if it
did.**

> **SUPERSEDED by the shipped tool, in both halves** (§12 step 6). The premise
> below — "a run stops at the first crib that matches" — is false: there is no
> early exit (§6.7 is closed), so the run sweeps the whole list and ranks. And
> the ordering rule below is *modelled* cost, charged by length on the
> assumption that sweep cost is roughly flat; measured against a real ciphertext
> it is a cliff spanning ~2 600× where the model spanned 13×. The tool
> therefore orders by **measured** cost, re-measured against the actual
> ciphertext every run (`--no-crib-reorder` restores file order). The generator
> still emits this order and it is still the right *file* order — it encodes
> evidence of recurrence, which nothing at run time can recover. What changed is
> that the tool no longer trusts it as a cost estimate.

A tier still says which *mode* a crib is for:

| tier | length | mode | when it applies |
|---|--:|---|---|
| 1 | 16+ | solve (§6, §7) | the deduction settles most of the board |
| 2 | 14-15 | solve, more checking | |
| 3 | 12-13 | solve, much more checking | |
| 4 | 8-11 | **seed only** (§7a) | too short to reject anything |
| 5 | any | derived windows (step 4a) | parent garbled or punctuated otherwise |

**But the tier must not set the order, and this is measured.** A run stops at
the first crib that matches, so what the order decides is how long the run takes
— and a crib that never matches costs its whole sweep for nothing. The generated
library holds 884 tier-1 cribs, 30 hours between them, of which almost none
match; trying them first buys nothing and delays the tier-4 crib that ends the
run. Median time to the first matching crib, held out over the 69-message
corpus:

| order | median | ≤25h | note |
|---|--:|--:|---|
| by tier, then spare letters | 141h | 6% | strongest crib first |
| by tier, then observed-before-guessed | 82h | 6% | |
| by evidence of recurrence, then cost | 10.1h | 61% | tier last |
| **the same, generic vocabulary first** | **6.7h** | **71%** | step 2 |

So order by how many corpus messages hold the phrase, then by cost, and let the
tier follow — with step 2's generic vocabulary ahead of all of it. Against a
no-crib run's 24.9 hours that is a real saving; ordered by tier it is a loss.
Within equal evidence, prefer the cheaper (longer) crib, then more spare
letters. Derived windows stay last however strong they look.

How many to keep is set by the compute budget — see §9. `--budget-hours`
truncates the library in this order, so the cut keeps what is most likely to end
the run early.

**Step 6 — write the file**, one crib per line, with its scores as comments so a
human can inspect the ranking.

**What this step deliberately does not do:** generate text from the n-gram
tables. A crib must be *exactly* right, and text sampled from a language model
is plausible but almost certainly not the actual plaintext. Generation must come
from templates that really do recur word for word.

---

## 5a. What the generator found — the decision point

§12 makes step 1 the go/no-go: if a generated library covers a useful fraction
of **held-out** messages, the rest of the plan follows. It does.

The measure is leave-one-out. Build the library from 68 of the 69 authentic
messages, then ask whether it contains a phrase the 69th really holds. Coverage
measured on the messages the library was harvested from proves nothing — an
observed phrase covers its own source by construction — so only the held-out
number counts. Run it with `python3 eval/build_cribs.py`.

| | coverage |
|---|--:|
| in-corpus (optimistic, not evidence) | 97% |
| **held out** | **83%** |
| same cribs with their letters shuffled | **0%** |

**The control is what makes the 78% believable.** Most hits are 8-to-11-letter
phrases, and §11 warns that short strings may recur because they are short
rather than because the traffic repeats them. Shuffling each crib's letters
preserves its length and letter multiset and destroys only the phrase; coverage
falls to nothing. So the recurrence is real, every bit of it.

**Coverage is supply, not recovery.** It says the library holds a phrase the
message contains. What that phrase then buys is §7a's business: 47 of the 57
held-out hits are tier-4, too short to reject a single rotor setting, so they
seed a climb rather than solve the key. The plan already expected this — it is
why §3 argues for extending the tool rather than writing a Bombe.

Within a **25-hour budget** — the same compute a no-crib run spends — the
library reaches **49 of 69 messages, 71%**, at a median of 6.7 hours each.

**What the corpus supplies:** 69 messages, 6843 letters, 352 distinct words. The
full library is 2528 cribs; a 25-hour budget keeps the first 96.

**Half the held-out hits come from the generic vocabulary** (25 of 57), which is
the part that would carry over to a network this corpus does not resemble.

### 5b. How much of this is the corpus, and how much would carry over

**Coverage is set by how much traffic the library was built from**, and the
curve is steep at the low end (`build_cribs.py --transfer`):

| training messages | coverage |
|--:|--:|
| 13 | ~57% |
| 55 | ~80% |
| 68 (§5a's leave-one-out) | 83% |

**Whether it also depends on *which* traffic — the question that matters — this
corpus cannot answer.** The obvious test is to train on one published collection
and test on the other, and run alone it is misleading: the collections hold 13
and 56 messages, and a harvester that keeps phrases recurring in two or more
messages finds far fewer in 13 than in 56, so the training-set size shows up
looking exactly like a transfer loss. Each cross-collection run therefore has a
same-collection control at the same training size:

| training | cross-collection | same-collection control |
|--:|--:|--:|
| ~55 messages | 77% | 80% |
| 13 messages | 57% | 51–70% (5 subsamples, mean 58%) |

**No transfer loss is detectable.** The two collections are interchangeable —
which is unsurprising, since both are HG Nord traffic from 1941, published
separately but not otherwise different. So the corpus measures the *size* curve
and says nothing about a genuinely unfamiliar network. That question stays open,
and §11's warning about a 58-message corpus stands undiminished.

**What a thin library leans on is measurable, and it is not the harvested
phrases.** At 13 training messages the generic vocabulary supplies 20 of the 32
hits and harvested phrases 2; at 55 it is 20 of 45 against 19. The vocabulary is
a fixed file, so it neither grows nor decays with the corpus — it is simply what
remains when the harvester has little to work with. Whatever a new network does
to the harvested phrases, the vocabulary is the part that keeps working, and it
is the cheapest thing to extend: one line in `cribs/german-hgnord.txt`.

**Enumerated numbers behave the same way.** Times and dates spell out to long,
repetitive strings — `EINSEINSNULLNULL` is 16 letters with 10 spare — so
enumerating them looks promising. Regenerate this with `build_cribs.py
--numbers-sweep`:

| family | cribs | cost | in-corpus | held-out | ≤25h | thin library |
|---|--:|--:|--:|--:|--:|--:|
| *(baseline, no numbers)* | | | | | **49/69** | |
| clock times `HH00` | 24 | 1.3h | 3/69 | 0 | 50/69 | 0 |
| clock times `HH00`/`HH30` | 48 | 2.7h | 4/69 | 0 | 49/69 | 1 |
| two digits, 00–31 | 20 | 7.8h | 15/69 | 0 | 48/69 | 2 |
| two digits, 00–99 | 72 | 26.3h | 20/69 | 0 | 19/69 | **3** |
| three digits | 1000 | 170h | 8/69 | 0 | 1/69 | 1 |
| four digits | 10000 | 543h | 4/69 | 0 | 0/69 | 1 |

*in-corpus* is how many messages hold a member of the family; *held-out* and
*thin library* are how many messages the library **misses** that the family
reaches, built from the other 68 messages and from 13 messages respectively.

Against the full library the marginal is **zero for every family**, and the
reason is circular: a number common enough to be worth guessing already
*recurs*, so the harvester has it (`NULLNULL` in 4 messages, `EINSNULL` in 5),
inside the first thirty entries. Against a **thin** library they earn +2 to +3
messages — the same margin whether the 13 training messages come from the other
collection or from the same one, which is what identifies this as a
library-thinness effect and not a transfer one. Hence `--numbers`, off by
default, worth turning on early in work on a network and not later.

What holds in every setting is the *shape* of the family. The cost runs
backwards from the intuition — a two-digit number is 8–12 letters, the **most
expensive** band (§4.1: 977–1499 s each against 122 s for a 16-letter crib),
while the four-digit times that are individually cheap occur 4 times in 10 000
candidates — so `--numbers` emits the two-digit values and the round clock times
and stops there. A context marker does not help either: `XUHR` appears in **1 of
69** messages.

**The general rule:** an enumerated family pays when it is small relative to its
hit rate *against the library you actually have*. Nineteen vocabulary words pay
in every condition; a hundred numbers pay only while the library is thin; ten
thousand four-digit times never pay.

**Three things the build settled that the plan had guessed at:**

- **The ordering had to change** — see step 5. Ordering by tier costs more than
  it saves; ordering by evidence of recurrence takes the median time-to-hit from
  141 hours to 10.
- **Tier 1 is a trap on this corpus.** 884 cribs of 16+ letters, 30 hours
  between them, and essentially none of them match a held-out message. They are
  the doubling variants of words seen once; the doubling that really happened is
  already in the observed phrases.
- **17% of messages are not covered at all**, and no ordering helps them. For
  those the tool falls back to the plain climb, which is exactly what it does
  today — so the feature never makes a message *harder*, it only fails to help.
  With less traffic to build from the uncovered share is larger — 13 messages
  give ~57% rather than 83% (§5b).

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

**Pitfall.** Do not cost the solver as one starting guess *per* piece. That
gives 26^C hypotheses, and an average 20-letter crib then appears to reject only
28% of rotor settings rather than 98.3% — making cribs look far weaker than they
are, and the shorter tiers look impossible.

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

**What "26 hypotheses" means, exactly.** They are 26 guesses about **one
letter** — not 26 cribs, and not 26 alignments. Three points that are easy to
get wrong:

- **Only one letter ever needs guessing.** That is what §6.3's "largest
  component only" buys: inside a connected menu, fixing one letter's plug forces
  every other letter in it by the chain rule. A menu in three pieces would need
  three guesses and 26³ combinations, which is why the small pieces are thrown
  away rather than used.
- **"Plugged to itself" means unplugged, and is a real answer.** Ten cables
  cover only 20 letters, so 6 of the 26 carry none. Dropping that case would
  sometimes discard the truth — it *is* the truth in §6.4a's example.
- **The Bombe did all 26 at once, in hardware.** Twenty-six wires into the
  letter's terminal, one per hypothesis, with current spreading through the
  menu's wiring; a contradiction lit everything up and the machine stopped when
  it did *not*. Our version walks them in a loop — slower per hypothesis,
  enormously cheaper per second.

For §6.4a's crib at the true rotor setting, all 26 look like this:

```
   X plugged to A   (chain contradicts itself)              DEAD
   X plugged to B   (chain contradicts itself)              DEAD
   ...
   X plugged to X   AQ BJ CH DL EF GM IK NP OR SU           SURVIVES
   ...
   X plugged to Z   (chain contradicts itself)              DEAD
```

Twenty-five die and one hands over the exact true board. That sharpness is a
property of the **true** rotor setting. At a wrong setting the core tables are
wrong, so the chains contradict more or less at random: at 12 letters about nine
of the 26 survive by luck, each a plausible-looking but wrong board that must be
decrypted and scored. That is where §4.1's checking cost comes from, and in §7a
it is where the 26× falls.

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
548 seconds of checking in §4.1's table.

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

**6.7 Loop order: cribs outside, rotor settings inside.** With a list of, say,
50 cribs there are two ways to arrange the work. Either sweep every rotor
setting once and try all 50 cribs at each (rotor-outer), or run a complete rotor
sweep for each crib in turn (crib-outer).

Rotor-outer looks better at first, because `setup_mapping()` and `precompute()`
are shared across the cribs at a given setting. That sharing is worth almost
nothing:

| per full rotor sweep, one 10-letter crib | cost |
|---|--:|
| deduction — 129 alignments × 26 hypotheses × 10 steps | **91 s** |
| `setup_mapping()`, once per rotor setting | 0.5 s |
| `precompute()`, once per wheel order | 0.00 s |

Repeating the shared work 50 times wastes **27 s, or 0.6% of the run**. The
deduction dwarfs it by about 180×, because every rotor setting pays 26
hypotheses at each of ~129 alignments.

**Crib-outer buys early exit, and that is worth up to 50×:**

| | cost |
|---|--:|
| crib-outer, winner is crib #1 | **0.3 h** |
| crib-outer, winner is #3 | 0.8 h |
| crib-outer, winner is #10 | 2.7 h |
| crib-outer, winner is #50 | 13.6 h |
| **rotor-outer, any winner** | **13.6 h, always** |

Rotor-outer cannot stop early on a crib basis — the winning combination might
sit at the last rotor setting, so every crib is swept at every setting before
anything is known. Since §5 tiers the list cheapest-and-strongest first, the
winner is usually near the front; rotor-outer discards exactly that advantage.

Three secondary reasons agree:

- **It reuses the existing sweep unchanged.** Crib-outer is "call the sweep 50
  times". Rotor-outer means threading the crib list into the inner loop and
  touching the hot path.
- **Cribs need different modes.** A 20-letter crib solves and rejects; a
  10-letter one only seeds, at 26 climbs per setting (§7a). Mixing those inside
  one rotor sweep is awkward; crib-outer keeps them separate.
- **`-T` parallelism is untouched** — each crib gets the full parallel keyspace
  sweep the tool already does. The early exit is the one part that does touch
  it; see below.

> **The early exit was NOT built, and the question is CLOSED rather than
> deferred** (§12 step 6). What shipped is this section's own stated fallback:
> the run sweeps the whole list and ranks, which costs the worst case but never
> discards the truth. Stopping is by human inspection — which is what the
> per-crib gain table, the progress lines and `--full-text` are for, and which
> needs no threshold. The reason it is closed is that a threshold would need a
> measured per-length score margin to be trusted, and this corpus cannot supply
> one: the held-out experiment that would have produced it recovered nothing at
> all, because holding a message out removes exactly the long cribs that would
> have solved it. Everything below about the `-T` hazard therefore describes a
> mechanism that was never needed — kept because the hazard is real and would
> return with the feature. **The crib-outer decision above stands and shipped**;
> only the early exit it was partly justified by did not.

**What this needs and the tool lacks: a stop criterion.** Early exit means
deciding "this crib won" without being told the answer. The tool reports a best
result today and never concludes. At 200 letters — about 8× the unicity distance
— the true decryption's score sits far above any wrong one and a threshold is
reliable; at short lengths the margin narrows and it is not. So the exit should
be a *score threshold that can be turned off*, with the fallback being to sweep
the whole list and rank, which costs the worst case but never discards the
truth.

The other stop criterion is a person watching the progress lines and stopping
the run when the text turns into German. That needs no threshold at all, but it
needs two things the display does not give today: more of the text than the 19
characters a progress line shows, and an indication of *which* crib and
alignment produced the line — a crib run generates lines from many cribs, and
the ones from a wrong crib look much like the ones from a right one. Both are in
§8.

**The early exit must not make the answer depend on `-T`.** Stopping is a
cross-thread event: one worker crosses the threshold and the others have to
stop, so *when* they stop is thread-timing dependent, and if the winner is
simply "whoever crossed first" then two thread counts can return two different
keys. That breaks the property the whole search is built on — every other
feature here is `-T`-independent, enforced by `better_cand`'s lowest-work-index
tie-break.

The fix is to separate stopping from choosing. Crossing the threshold sets a
shared atomic flag; each worker finishes the chunk it is in and merges its
candidate through the existing mutex-guarded merge as usual, and the winner is
the merge's, not the flag-setter's. That still leaves the *set* of chunks
examined thread-dependent — a slower machine may finish more chunks before
noticing the flag — so the guarantee has to be stated honestly and narrowly:

- **guaranteed** — with the threshold off, the result is `-T`-independent, as
  today;
- **guaranteed** — with the threshold on, any key the tool returns scores above
  the threshold;
- **not guaranteed** — *which* above-threshold key it returns, when several
  exist. Early exit trades that determinism for the up-to-50× saving, which is
  the point of the flag.

Tests must therefore compare a threshold run against `-T 1` on the
*above-threshold* property, not against a fixed expected key. §10.8.

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

(§7a's table gives the same figures by crib *length* rather than by crib.) So a
12-letter crib typically hands over **seven of the ten cables** and confirms two
or three letters as unplugged, leaving about ten free letters and three cables
for the climb. A 22-letter crib leaves barely anything.

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

**The letters the deduction settles as carrying no cable are used too.** It
finds about 2.5 of them (§7's table above counts them), and they are a real
finding rather than an absence of one: the seeded climb pins them exactly as it
pins the deduced pairs, so it never wastes a move trying to plug a letter that
cannot be plugged. This is what §8 wanted `--no-plug` for, and the hybrid gets
it for free — `--no-plug` remains the way to say it by hand.

It also degrades gracefully, which matters given §4.5. If the crib is slightly
wrong, the deduction produces a bad partial board — but the follow-up score will
be poor and that alignment simply loses to another.

**7b. The climb to run after a deduction is not the recommended one.** The
standard recipe (`-c -S m4f10 -J --polish -f`) is tuned for a climb starting
from an empty board plus a random kick. A crib-seeded climb starts from five to
seven correct plugs, and four of that recipe's parts stop making sense:

- **No pre-pass.** `--score m4f10`'s mono or IC first stage exists because the
  quad/weighted surface is *nearly flat when only a plug or two is set* — that
  is the finding `-f`'s IC blend was built on. Starting from five plugs, the
  surface already has gradient and the premise is gone. Worse, a pre-pass
  optimises a **different objective**: the deduced plugs are safe in
  `plug_fixed[]`, but an IC stage will add spurious plugs that the target stage
  then has to unpick, from a board that was already good. Run the target model
  alone. (§7a's measurements were already run this way — `-R 16 -J --polish`,
  single stage — so the 55%/68% figures are pre-pass-free.)
- **Cap the climb at 10 plugs**, `--score <model>10`. The cap counts *total*
  pairs including the fixed ones, so with five deduced it leaves five to find.
  This is the regime where a cap is measured to matter most: `-J` uncapped is a
  known *loss* when few plugs are truly needed, and capping at the true count
  turns it into a large win.
- **`-M` off.** It makes the cap a strict descent target, which earns its keep
  pulling an *over*-cap board down after a big kick. Seeded, the climb grows
  from five to ten and is never over the cap, so `-M` is near-inert.
- **`-J` on.** Measured 1.8× cheaper on a seeded climb (§7a's timing table),
  which at equal wall time buys more of everything else. Its documented failure
  mode is over-plugging when few plugs are truly needed — exactly this regime —
  so pair it with the cap above rather than running it bare, and confirm the
  recovery side before recommending it outright.
- **`--random 0`.** The kick only perturbs free letters, so it cannot damage the
  seed — but scattering the remaining five when you already start near the
  answer is more likely to cost than to buy. The seeded climb wants to *finish*
  a board, not go looking for a new basin.

**`--polish` after, with the deduced plugs still fixed.** The finisher's gain
cascade skips `plug_fixed[]` letters, and that is what should happen: a deduced
plug is derived by arithmetic from the machine equation, while the cascade is
score-driven local repair, so releasing them lets the weaker evidence overwrite
the stronger — on exactly the board you least want disturbed.

A wrong hypothesis needs no help from the finisher, because it is already
handled by something decisive: each of the 26 produces its own board and score,
and the best wins. Nor could the finisher help. It completes a near-solution
board; it cannot relocate a wrong basin — the same finding that puts `-R`
restarts ahead of every finisher variant in the search playbook.

One residual case argues for measurement rather than for releasing. If the crib
is right but **garbled** at a position (§4.5), that bad edge can chain into a
wrong deduced plug without necessarily contradicting, so the seed is wrong while
the hypothesis still looks sound. Freeing the plugs would repair that — at the
cost of weakening every un-garbled run, which is the common case. Measure how
often a garbled position produces a silent wrong plug before paying that.

**Which target model — `-q`, `-a` or `-f` — is genuinely open**, and the reason
is worth stating rather than defaulting to the recommended `-f`. Its measured
+3.0–4.4pp over `-a` decomposes into **surface reshaping (+3.4pp) and selection
(−0.0pp)**: `-f` is a better *climb*, not better *discrimination*. Crib mode
needs both, in different places —

- **discrimination** picks between the 26 hypotheses and between surviving rotor
  settings, where `-f`'s advantage is measured at zero;
- **surface** drives the climb over the last few plugs, which is where `-f` wins
  — but a seeded climb has less surface left to cross, so its advantage should
  be *smaller* here than on the plain sweep.

Both pull the answer away from "obviously `-f`", and neither settles it. The A/B
is `-q` vs `-a` vs `-f` as the target of a seeded, uncapped-pre-pass climb,
scored on recovery from the correct seed. One further consideration: `-f`'s IC
term is language-independent, which is worth something when the traffic's style
is not certain to match the tables.

---

## 7a. Crib as a seed: very short cribs, 8-12 letters

§7's hybrid assumes the crib is long enough to pick out one hypothesis. Below
about 14 letters it cannot, and a different use opens up: stop trying to *solve*
the plugboard and use the crib only to *start* the hill-climb somewhere better
than random.

**What a very short crib deduces**, from the true hypothesis at the true rotor
setting, 300 random keys and boards:

| crib length | true cables found | letters shown unplugged |
|--:|--:|--:|
| **8** | **4.1 / 10** | 1.4 |
| **10** | **5.3** | 1.9 |
| 12 | 6.5 | 2.5 |
| 14 | 7.7 | 3.1 |
| 16 | 8.5 | 3.7 |
| 18 | 8.9 | 4.1 |
| 20 | 9.2 | 4.4 |
| 25 | 9.6 | 4.8 |

A 10-letter crib lands on five cables — exactly the knee of §7's value curve,
where most of the benefit already sits.

**The cost, and why it is not obviously worth paying.** A crib this short
rejects nothing (§4.1), so all 26 hypotheses survive at every rotor setting and
each one gives a *different* seed. You must climb from all of them, so the sweep
costs **26 climbs per rotor setting** instead of one. That is the entire
objection, and it has to be met head-on: is 26 seeded climbs better than 26
ordinary restarts?

**A seeded climb is far cheaper than an unseeded one**, which is most of why the
answer is yes. Fixed plugs mark their letters in `plug_fixed[]` and the climb
skips them, so the move set shrinks quadratically; there are also fewer plugs
left to find, so fewer passes. Measured at L=60, `-R 16 -J --polish`, 30 trials
— boards scored is a deterministic count, not a success rate:

| plugs given | free letters | toggles/pass | boards scored | speedup |
|--:|--:|--:|--:|--:|
| 0 | 26 | 325 | 34,551 | 1.00× |
| 3 | 20 | 190 | 18,681 | 1.85× |
| **5** | **16** | **120** | **10,469** | **3.30×** |
| 7 | 12 | 66 | 5,012 | 6.89× |
| 8 | 10 | 45 | 3,218 | 10.74× |

So the 26 climbs are not 26× the cost of the baseline. At a 5-cable seed they
come to **26 / 3.3 ≈ 8×** a single unseeded climb.

**That table counted boards, not seconds; timed, it is slightly conservative.**
`eval/crib_seed_cost.py` runs a fixed 17 576-key sweep with one deterministic
climb per key, the plugs given by `-s` so they pin exactly as a deduction's do
(min of 3 reps, `-T 1`, `-f -l wehrmacht`):

| plugs given | wall | speedup | boards scored | predicted above |
|--:|--:|--:|--:|--:|
| 0 | 15.71 s | 1.00× | 55 881 692 | — |
| **5** | **4.42 s** | **3.55×** | 15 256 095 | 3.30× |
| 8 | 1.26 s | 12.48× | 4 002 530 | 10.74× |

Wall time and the board counter move together here (3.55× against 3.66× at five
plugs), which is what should happen when nothing runs outside the score loop —
no cascade, no finisher. The measured speedups slightly exceed the predicted
ones because the arithmetic counted only the shrinking move set, while a seeded
climb also converges in fewer passes.

**`-J` is worth another 1.8× on a seeded climb.** At five preset plugs, first-
improvement with dynamic ordering takes the same sweep from 4.36 s to **2.40 s**
(15.3 M boards to 7.6 M) — so §7b's recipe should carry `-J`. **Cost only**: the
recovery side is unmeasured here, and this is the known-few-plug regime where
`-J` is documented to need a cap to win on quality, so the pairing to test is
`-J` with `--score f10` rather than `-J` alone.

**`-s` is still only a proxy, and the real deduced seed does better.** A
deduction differs from `-s` on three counts: it pins the letters it shows carry
**no** cable as well as the cables it finds, 25 of its 26 hypotheses pin plugs
that are simply *wrong*, and the search runs one climb per surviving hypothesis
rather than one per key. Measured on that path — boards scored per surviving
hypothesis, the hypotheses counted with `--crib-dump`, against the same key
space's unseeded climb:

| crib | mode | hypotheses | letters pinned | boards/climb | vs full climb |
|--:|---|--:|--:|--:|--:|
| — | unseeded | — | 0 | 3 179 | 1.0× |
| 8 | swept | 6 188 | 10.4 | 859 | 3.6× |
| **8** | **pinned** | 13 786 | 14.5 | 358 | **8.9×** |
| 12 | swept | 51 563 | 12.4 | 600 | 5.3× |
| 12 | pinned | 14 | 16.6 | 246 | 12.9× |
| 16 | swept | 9 | 15.6 | 309 | 10.3× |
| 16 | pinned | 2 | 17.0 | 155 | 20.5× |
| 20 | swept | 2 | 24.0 | 3 | 1060× |

**Cost tracks letters pinned, not whether the pins are right.** At 10.4 pinned
letters the deduction gives **3.6×** against the `-s` proxy's **3.55×** at five
plugs — the same number, although most of these seeds come from wrong
hypotheses. A wrong plug shrinks the move set exactly as a right one does. So
`-s` is a fair proxy for *cost*, and the tables above stand; it is emphatically
not a proxy for *quality*, which is §7c's separate question.

**The no-cable pins are worth more than the arithmetic credited them with.**
Against the move-set formula — `C(26 − pinned, 2)` toggles against 325 — every
row comes out 1.4–2.3× better: 5.4× predicted against 8.9× measured at 14.5
pinned letters, 8.2× against 12.9× at 16.6. Same direction and same cause as
the `-s` rows: the formula counts only the shrinking scan, while a seeded climb
also converges in fewer passes.

**Swept seeds are weaker as well as more numerous, and that is survivorship
bias.** At every length the swept survivors pin *fewer* letters than the pinned
ones (10.4 against 14.5 at 8 letters, 12.4 against 16.6 at 12) — a hypothesis
that deduces less has fewer chances to contradict itself, so the ones that
survive a wrong alignment are systematically the ones that deduced least. The
sweep therefore pays twice: more climbs, each from a poorer seed.

**Read the sparse rows with care.** The 16- and 20-letter rows rest on 2 to 9
climbs, all at or beside the true key, where convergence need not resemble the
average wrong key that dominates the unseeded baseline. The claim rests on the
well-populated rows — 8 pinned, 8 swept and 12 swept, at 6 000 to 52 000 climbs
each.

**The unplugged letters help too.** The deduction also settles letters as
carrying *no* cable, and `--no-plug` (§8) would freeze those as well:

| crib len | free | toggles | free with `--no-plug` | toggles | gain |
|--:|--:|--:|--:|--:|--:|
| 8 | 18 | 153 | 17 | 136 | 1.12× |
| **10** | 16 | 120 | **14** | **91** | **1.32×** |
| 12 | 12 | 66 | 10 | 45 | 1.47× |
| 14 | 10 | 45 | 7 | 21 | 2.14× |
| 16 | 8 | 28 | 4 | 6 | 4.67× |
| 18 | 8 | 28 | 4 | 6 | 4.67× |
| 20 | 8 | 28 | 4 | 6 | 4.67× |
| 25 | 6 | 15 | 2 | 1 | 15.0× |

At the 10-letter tier that is another **1.32×**, putting the seeded sweep at
roughly **6×** a single unseeded climb rather than 26×. It should help
*accuracy* too, by stopping the climb plugging a letter that carries no cable —
but that is **not measured**, since `--no-plug` does not exist yet. Only the
move-set arithmetic is certain.

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
currently weakest. That comparison prices the seeded side at the full 26×; the
measurements below put it nearer 6×, so the true margin is wider.

**What the corpus actually supplies at this length.** Harvesting 10-letter
windows from the 58 messages gives 150 shared by two or more, merging into 53
maximal phrases that cover **46 of 58 messages — 79%**. Coverage by harvest
length:

| harvest length | 20 | 16 | 12 | 10 |
|---|--:|--:|--:|--:|
| messages covered | 3% | 19% | 55% | **79%** |

A representative sample, measured as above:

| phrase | len | spare | loops | rejects | cables | msgs |
|---|--:|--:|--:|--:|--:|---|
| `XFORDXFORDXVIKTORXAQTX` | 22 | 11 | 3.93 | 100% | 9.3 | 18, 23 |
| `XOPOTSCHKAXOPOTSCHK` | 19 | 10 | 2.51 | 99.3% | 9.0 | 9, 35, 43 |
| `XZANDERSXZANDERS` | 16 | 8 | 1.39 | 75.5% | 8.2 | 20, 23 |
| `NULLNULLNULL` | 12 | 9 | 1.43 | 78.2% | 6.9 | 53, 57 |
| `XHOCKXHOCKX` | 11 | 6 | 0.54 | 1.1% | 6.3 | 11, 16 |
| `XSIEGFRIED` | 10 | 2 | 0.18 | **0.0%** | 5.6 | 0, 23, 25 |
| `XSCHUSTERX` | 10 | 2 | 0.15 | **0.0%** | 5.3 | 18, 23, 24 |
| `VERBINDUNG` | 10 | 1 | 0.05 | **0.0%** | 5.5 | 14, 32 |
| `VERPFLXAMT` | 10 | 0 | 0.07 | **0.0%** | 4.7 | 21, 56 |

**Every genuine 10-letter crib rejects 0.0% and still deduces 5 to 5.6 cables.**
That is precisely the shape §7a is for: they are seeds, not solvers, and the
knee of the value curve sits at five.

Two things fall out of the harvest that the plan should act on:

- **Harvest short even when you intend to use long cribs.**
  `XOPOTSCHKAXOPOTSCHK` — a doubled place name in *three* messages, rejecting
  99.3% and yielding 9 of 10 cables — never appeared at 12 or 16 letters. The
  shared *window* had to drop to 10 before the full run emerged. §5 should
  harvest at the shortest length and keep the maximal runs, not harvest at the
  target length.
- **Spare letters beat length again, and more sharply.** `NULLNULLNULL` (12
  letters, 3 distinct) rejects 78% while `XHOCKXHOCKX` (11 letters, 5 distinct)
  rejects 1%. At the bottom, `VERPFLXAMT` has ten distinct letters in ten
  positions — zero spare, zero rejection, and the weakest coverage in the table.

**Why this is a different kind of lever.** `IMPROVEMENTS.md` records that
restarts stall because the truth is a rare deep basin and no truth-free signal
exists to steer toward it — per-plug consensus across converged boards is only
~1.1 correct plugs in 10. A crib is exactly such a signal, and it comes from
*outside* the score landscape rather than being mined out of it. That is why it
can beat compute rather than merely adding to it.

**Three cautions. The first two are now measured; the third is not.**

1. ~~The table above seeds the climb with correct plugs, and a garbage seed
   could in principle out-score them.~~ **Measured, and it happens only at 8
   letters** — see §7c.
2. ~~At *wrong* rotor settings you now run 26 climbs instead of one.~~
   **Measured in §4.2b, and "26" is wrong in both directions.** The unit is
   surviving hypotheses per key, counted with `--crib-dump`, and 26 is only the
   number *tried* per alignment. **Pinned it is far cheaper than 26**: most of
   the 26 die on the diagonal board before any climb runs, leaving 1.4 per
   surviving key at 8 letters and exactly 1.0 wherever a crib rejects at all —
   which is why a pinned 8-letter crib is a 12× *saving* rather than a 26× cost.
   **Swept it is far worse than 26**: the count is `alignments × 26` tried and
   **235 surviving** per key at 8 letters, a 66× slowdown. So the real caution
   is not "26×" but "pinned or near-pinned alignment", and the cost is
   measurable before it is paid (`crib_estimate`, reported per crib).
3. Coverage assumes the crib is exactly right. An 8-letter crib is far more
   likely to be present (§4.2) *and* far more likely to be a coincidental match
   that deduces confident nonsense. Short cribs make both errors more common at
   once. **Measured, and it does not happen** — `eval/crib_absent_probe.py`, 30
   trials, 90-letter messages, the crib a real 10-letter phrase taken from
   another corpus message and checked absent, board hidden, rotor key given,
   `-J -R 64`:

   | | absent crib | no crib |
   |---|--:|--:|
   | mean winning score | −9.5588 | **−8.4389** |
   | mean %-correct | 9.9% | 44.3% |
   | false positives (crib scores higher) | **0 / 30** | — |

   Zero, and 0 of the 8 trials where the no-crib baseline actually solved. The
   mechanism is that wrong pins **handicap** the climb rather than helping it:
   a deduced plug is held fixed, so an absent crib locks in plugs the climb
   cannot undo and converges over a point *below* an unconstrained one. A
   false positive would need the wrong pins to out-bid a free climb, and they
   are strictly worse-off. So an absent crib fails visibly, on score.

   > ⚠️ **The baseline must be able to solve, or this measures nothing.** At
   > `-R 0` the same probe reported **40%** false positives — but recovery was
   > 10.1% with the crib against 11.6% without, i.e. *both* arms failing, so
   > the comparison was a coin flip between two garbage scores. Restarts are
   > what give the no-crib arm a real answer to defend.

### 7c. Does a wrong hypothesis ever win? Measured

The seeding mode rests on the solver keeping the best of 26 boards, exactly one
of which is seeded with the truth. If a wrong seed ever converges above the
right one, the run returns a confident answer that is not the message and
nothing in the output says so. §7a named this as the single thing that could
undo the mode. `eval/crib_seed_probe.py` measures it: plant a crib in an
authentic plaintext, hide the board, give the tool the rotor key, and compare
the winner's plug for the anchor letter against the truth — the anchor's partner
*is* the hypothesis, so no new diagnostic is needed.

150 trials per row, 90-letter messages, 10 cables hidden, `-f -l wehrmacht
--score f10`:

| crib | right hypothesis won | mean recovery | exact |
|--:|--:|--:|--:|
| **8** | **95%** | 74.4% | 85/150 |
| 10 | 100% | 89.7% | 119/150 |
| 12 | 100% | 94.9% | 132/150 |
| 16 | 100% | 97.4% | 138/150 |

**The risk is real and confined to 8 letters.** There a wrong hypothesis wins 5%
of the time, and when it does the failure is total: mean recovery **8.6%**,
median 7.8%, not one exact — indistinguishable from having no crib at all, with
nothing in the output to flag it. From 10 letters up it did not happen once in
450 trials.

The mechanism is the obvious one. An 8-letter crib deduces about 4 cables
(§7's table), so the correct seed is only slightly better than a wrong one and
the score has little to separate them; at 10 letters (~5.3 cables) the
advantage is already decisive.

**So the seed mode's floor is 10 letters, not 8** — a floor set by *silent
failure*, not by cost. This is a different boundary from §4.2a's: 16 letters is
where a crib can filter a swept search, 10 is where it can safely seed one.
Below 10 the answer can be wrong without looking wrong, which is worse than
being slow.

Caution 2 has since been measured in §4.2b (and found wrong in both
directions); caution 3 remains open.

**Where this sits in the plan.** It shares all the machinery of §6 — the same
menu, the same deduction, the same `--no-plug` output — and differs only in what
it does with the result: seed rather than solve. So it costs almost nothing
extra to build once §6 exists, and it should be a flag rather than a separate
path.

---

## 8. Suggested command line

Consistent with existing options; names open to discussion.

> **BUILT — and this section is the proposal, kept for its reasoning. The
> shipped surface differs in three places.** What follows below the fence is
> what was proposed; what the tool actually has is `--crib`, `--crib-at`,
> `--crib-dump`, `--crib-list`, `--no-crib-reorder`, `--no-plug` and
> `--full-text`. The deltas:
>
> - **`--crib-min-loops` was never built.** It rested on §4.1's pre-build belief
>   that menu *loops* do the rejecting. They do not — the diagonal board does,
>   and a loop-free 12-letter menu still rejects 88% of settings against 0%
>   without it (§4.1). With loops not the lever, filtering alignments by loop
>   count filters on the wrong quantity.
> - **`--crib-seed` was never built, because it turned out not to need a flag.**
>   With `-c`, the deduced plugs seed the climb automatically; without `-c`
>   there is no climb to seed. A flag would only have offered the choice of
>   throwing the deduction away.
> - **File order was reversed.** The paragraph below says the tool "should try
>   them in file order so a tier-1 hit ends the search". Both halves fell:
>   cribs run **cheapest-measured-cost first** by default (`--no-crib-reorder`
>   restores file order), because measured cost spans ~2 600× where the
>   generator's model spanned 13× (§12 step 6); and there is **no early exit**,
>   §6.7's threshold having been closed rather than deferred, so nothing "ends
>   the search" — the run sweeps the whole list and ranks.
>
> Also shipped beyond the proposal: `--crib-dump` (§12 step 3) and the per-crib
> expected-gain table (§12 step 6). **The composition matrix below shipped
> exactly as proposed**, including the four rejections.

`--no-plug` and `--full-text` were built first, as step 0 of §12: neither
depends on the crib work and both are useful on their own.

**A list is the normal input, not a single crib.** You rarely know which phrase
a message contains — you know the vocabulary of the network. §9's budget allows
several hundred cribs per message, and §5's generator produces exactly such a
file. A single crib on the command line is for testing and for the case where
you do know.

```
  --crib-list FILE     cribs to try, one per line (the generator's output)
  --crib TEXT          a single crib, as plaintext letters
  --crib-at N          pin the crib to position N (default: sweep all)
  --crib-min-loops N   skip alignments whose menu has fewer than N loops
  --no-plug LETTERS    these letters are known to carry no cable
  --crib-seed          seed the climb from the deduction instead of
                       requiring it to solve the board (§7a)
  --full-text          print the whole decrypted message on each new best,
                       not just the 19-character preview
```

The file format is §5 step 6's: one crib per line, `#` comments, blank lines
ignored. Order matters — the generator emits its tiers cheapest-first (§5 step
5), and the tool should try them in file order so a tier-1 hit ends the search
before the expensive tiers run.

Notes:

- **The naming collision was a real hazard, and is resolved.** The option that
  re-ranks *finished* boards by known-word content — unrelated to the deduction,
  and measured at about −0.1 percentage points — used to be called
  `--crib-file`, which beside `--crib-list` would have been two similar names
  for two unrelated features. It is now **`--crib-rerank`**, renamed as part of
  this work.
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
- **Say up front which flags it composes with.** §3.4 asks for the crib code to
  be self-contained and skippable like `--exhaust`; that is a statement about
  *code*, and it needs a matching statement about *option combinations*, decided
  before step 3 rather than discovered during it. The tool already has two
  precedents to follow: `--ring-stride` and `--polish` both reconstruct a key
  from `best.idx` and so are rejected against `-F` and `--exhaust`, which encode
  that index differently. Crib mode is a fourth search mode over the same sweep,
  so the proposed matrix is:

  - **`-c`, `-R`, `--random`, `-S`, `-J`, `-M` — compose.** The climb is exactly
    what §7 hands the deduction to.
  - **`--polish` — composes**, with the caveat below.
  - **`-T` — composes**, with the early-exit caveat of §6.7.
  - **`-F` — reject.** Tier 1 shortlists keys by a cheap IC climb, so a key the
    crib would have settled can be filtered out before the crib is ever
    consulted.
  - **`--exhaust` — reject.** Both force plugs from outside the climb, and the
    two pin sets can contradict each other.
  - **`--ring-stride` — reject initially, revisit.** The derived refinement is a
    second, separate sweep, so a crib hit found only in the refinement needs
    thinking through. Not worth blocking step 3 on.
  - **`-A` (annealing) — reject initially.** SA seeds itself with a built-in IC
    pre-pass, which would overwrite the deduced board.

  Rejections should be fatal at option-parsing time with a message naming both
  flags, the way the existing ones are.

- **`--polish` keeps the deduced plugs fixed**, which needs no new state: the
  finisher's gain cascade already skips `plug_fixed[]` letters. A deduced plug
  comes from arithmetic on the machine equation; the cascade is score-driven
  local repair. Letting the latter rewrite the former would put weaker evidence
  over stronger. A wrong hypothesis is handled by losing on score to the other
  25, not by being repaired — and repair could not do it anyway, since the
  finisher completes a near-solution board rather than relocating a wrong basin.
  See §7b for the one case (a garbled crib) that is worth measuring first.

- **The echoed key may be a class representative.** Crib mode reports a rotor
  key like everything else, so it inherits the §7.12 middle-wheel collapse: the
  reported ring1/start1 can be a member of the same decode-equivalent class
  rather than the true pair, and the leftmost wheel's ring is always `A`. The
  plaintext is byte-identical either way. This matters more here than elsewhere,
  because a crib run is the case where a user is most likely to compare the
  echoed key against a key they already know — so the caveat belongs in the
  crib-mode documentation, not only in `CHANGELOG.md`. `--true-key` disables the
  collapse.
- **`--full-text` supports the human stop criterion of §6.7.** A progress line
  shows 19 characters (16 under `-4`, where the wider key eats three), which is
  enough to notice a promising board but not enough to decide a run is finished.
  Four points for building it:
  - **Do not widen the fixed-width column.** The line is columnar by design and
    fits an 79-column terminal. Print the full text on its own indented line
    after the progress line instead, so the columns still line up when the
    option is off *and* on.
  - **The volume is already bounded.** A progress line is emitted only when a
    board beats everything echoed so far, so the number of full texts printed is
    the number of improvements, not the number of candidates scored — tens per
    run, not millions. Nothing on the hot path changes.
  - **Decode on the fly, as `showconfig()` already does.** It rebuilds the
    preview from the machine's *current* board rather than reading
    `m.plaintext`, which can be stale mid-climb; the full text must do the same.
    The only change is the buffer, which is a fixed 20 bytes today and would
    need to be message-length sized.
  - **It is useful on its own.** Nothing about it depends on cribs, so it can be
    built and tested first, with its own test — a run under `--full-text` must
    print a line matching the final plaintext.

- **The progress display must say which crib produced the line.** A crib run
  tries a list of cribs at many alignments each, so a bare score-and-key line is
  ambiguous in exactly the way that matters: two lines can look equally good and
  come from a right crib and a wrong one. Whoever is watching — or reading the
  log afterwards — needs to know which. Split it by what varies:

  - **The crib goes in a banner line, once per crib.** Under §6.7's crib-outer
    order a crib is fixed for a whole rotor sweep, so repeating it on every
    progress line is noise. Print it when that crib's sweep begins, with its
    position in the list so progress through the list is visible:

    ```
    crib 7/53  XOPOTSCHKAXOPOTSCHK  (19 letters, tier 1)
    ```

    Emit it under the same mutex as the progress lines, so a banner cannot land
    in the middle of one when `-T` > 1.

  - **The alignment goes in a column, because it varies line to line.** Add a
    3-wide `A` column, and take the width from the preview: the line is budgeted
    to land on exactly 80 columns and the comment above `progress_fmt_3` records
    that the preview is the field that absorbs the difference. So 19 → 15
    characters on a 3-wheel machine and 16 → 12 under `-4`. That is a real cost
    on a run *without* `--full-text` and none at all with it, which is another
    reason to build `--full-text` first.

  - **The header must gain the column too.** `showconfig_header()` prints
    through the same format string, which is what keeps the columns aligned; a
    column added to one and not the other silently misaligns every line.

  - **Only in crib mode.** With no crib option the format is unchanged, which
    §10.6's byte-identical check already enforces.

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
character. **Measured, that assumption holds**: §4.2b times a propagation at
~3 ns, and the feared 2–3× penalty for the deduction being branchier than the
tool's tight scoring loop does not appear. What the estimate *does* get wrong is
the unit — the checking cost is per surviving hypothesis, not per surviving key
— which matters only for cribs too weak to reject (§4.2b).

**The crib budget.** Since a wrong crib is cheap to reject, you can afford to
try many. At roughly two minutes each against 24.9 hours, the budget is on the
order of **several hundred cribs** per message — which sets *N* in §5 step 5.
Trying hundreds does not risk a false answer: at 200 letters the message is
about eight times longer than the point at which the true decryption becomes
unique, so wrong candidates cannot masquerade as German.

---

## 10. How to verify it works

> **BUILT — every check below was run, and all of them live in the tree.**
> §10.1 and §10.2 are `eval/crib_vectors_check.py`, which runs the binary on
> `crib_menu.py`'s vectors and compares against the true board: **40/40 exact**,
> crib lengths 8–25. §10.3 was measured and *corrected* §4.1 rather than
> agreeing with it (99.9% of a keyspace rejected pinned, 5.3% swept on a
> 12-letter crib — §4.2a). §10.4 and §10.5 are §12 step 5's end-to-end result:
> **92% of letters recovered from a 12-letter crib against 8% unseeded**.
> §10.6, §10.7 and §10.8 are checks in `tests/run_tests.sh`, which carries
> roughly two dozen crib assertions — the true key never rejected, deduced plugs
> matching the true board, rejection counts `-T`-independent, the alignment
> column present and the line inside 80 columns, and every rejected flag
> combination failing at parse time.
>
> **The "new test tier" in the last paragraph was not built, and did not need
> to be.** The role it describes — planting cribs in known plaintext — is filled
> by the `eval/crib_*.py` probes, which are run by hand for a measurement rather
> than on every commit. A `make` target would have implied per-commit cost for a
> feature whose numbers move only when the crib code does.

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

**10.7 Display.** The progress line is budgeted to exactly 80 columns, so a crib
run must be checked for width the way the existing one was: no line over 80
columns for a 3-wheel machine or under `-4`, and the header aligned with the
lines below it. Check it with a crib at the widest alignment and a full 13-pair
board.

**10.8 Flag interaction.** Two checks, both cheap, both easy to omit and
expensive to omit:

- every rejected combination in §8's matrix must fail at option-parsing time
  with a message naming both flags — one invocation each, no search performed,
  so the whole set costs milliseconds;
- every composing combination must run to completion. With the early exit off,
  `-T 1` and `-T 4` must agree exactly; with it on, the assertion is the weaker
  one §6.7 states — the returned key scores above the threshold — since which
  above-threshold key wins is not guaranteed.

Size these to the property, not to realism: a rejection check needs no keyspace
at all, and a `-T` agreement check needs the smallest one that exercises the
merge. `tests/run_tests.sh` runs under sanitizers at roughly a 10× slowdown.

A new test tier will be needed: `make crackquality` measures plugboard recovery
with the rotor key given, so it cannot see this feature at all. Planting cribs
in known plaintext is a new harness.

---

## 11. Risks, honestly stated

**The crib supply is the real constraint, not the algorithm.** The solver is
maybe 300 lines. Whether it ever fires depends on the library, and the library
is the part we cannot measure well.

**The 58-message corpus is too small to size the library.** At 20 letters it
holds exactly *one* phrase shared between two messages; at 10 letters it holds
53, covering 79% (§7a). The short end is where the supply is, and §7a is the
mode that can use it — but neither figure is an estimate of what a real network
would yield.

What stays uncertain is whether phrases shared *within* this corpus would appear
in genuinely new traffic. A real network would differ, probably favourably —
more messages, more consistent formats — but that is expectation, not evidence,
and short phrases recur partly *because* they are short. A 10-letter match is
more likely than a 20-letter one to be coincidence rather than a predictable
formula.

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

**Step 0 — `--full-text` and `--no-plug`** (§8). **Done.** Neither depends on
cribs: one prints the whole decrypted message on each new best instead of the
19-character preview, the other marks letters as carrying no cable so the climb
leaves them alone. Both are worth having on their own, and doing `--full-text`
first means the alignment column added in step 4 costs nothing that anybody
misses.

**Step 1 — the crib generator** (§5). **Done** — `eval/build_cribs.py`, output
`cribs/wehrmacht.cribs`. Needs no changes to the tool at all, and it answered
the supply question before any C++ was written: **83% held-out coverage, 0% for
a shuffled control** (§5a).

**Step 2 — a menu builder and offline analysis** (`eval/crib_menu.py`). **Done,
and it did not confirm §4.1 — it corrected it.** The rejection rates had been
measured without the diagonal board and were wrong by orders of magnitude at the
short end (§4.1). It also emits the test vectors step 3 needs (`--vectors`) and
runs §10.1/§10.2 as oracle checks on itself.

**Step 3 — deduction inside the tool**, one crib at one alignment (`--crib`,
`--crib-at`). **Done**, as a key filter: a rotor setting the crib proves
impossible is skipped without being scored. Checked against §10.1 and §10.2 by
`eval/crib_vectors_check.py`, which runs the binary on `crib_menu.py`'s vectors
and compares against the true board — 40/40 exact, crib lengths 8–25. Measured
99.9% of a start-position keyspace rejected on a 12-letter crib. `--crib-dump`
prints each surviving hypothesis and its deduced plugs.

**Step 4 — the alignment sweep**, with the self-encryption filter. **Done** —
`--crib-at` is now optional, and omitting it tries every alignment the filter
leaves. It also carries the progress line's alignment column (§8). **The sweep
changes the economics, and §4.2a records how.**

**Step 5 — the hybrid** (§7). **Done** — with `-c`, the climb starts from the
plugs each surviving hypothesis deduces, held fixed, instead of from an empty
board. Measured end to end on an 88-letter message with the plugboard hidden and
only a 12-letter crib given: **92% of letters recovered, against 8% for the same
climb unseeded and 10% at `-R 64`.** Seeding works swept as well as pinned (92%
either way), so it does not need the alignment to be known — which matters,
because a 12-letter crib cannot *filter* a swept search at all (§4.2a). **That
was measured with the rotor key given**, though, and §4.2b shows the sweep is
not free once the rotor key is unknown: each surviving hypothesis costs a climb,
so a crib too weak to reject multiplies the work rather than skipping it. The
climb it hands over to is not the recommended recipe — §7b says why, and leaves
the target model (`-q`/`-a`/`-f`) as the one thing to A/B here.

**`--polish` and `-J` on a seeded board — MEASURED** (`crib_finisher_probe.py`,
60 trials, 90-letter messages, an 8-letter crib pinned, 10 cables hidden, rotor
key given; a 12-letter crib saturates at 100% and measures nothing):

| arm | mean %-correct | exact | boards scored |
|---|--:|--:|--:|
| baseline | 73.7% | 33/60 | 319 221 |
| `--polish` | 76.1% (+2.4pp) | 35/60 | 917 461 |
| **`-J`** | **75.7% (+2.1pp)** | 35/60 | **153 321** |
| `-J --score f10` | 75.7% (+2.1pp) | 35/60 | 139 735 |
| `-J --polish` | **79.7% (+6.0pp)** | 38/60 | 800 849 |

**`-J` is a strict dominance win on a seeded climb** — +2.1pp recovery at
**2.1× fewer boards**. §7a measured only its cost and left the recovery side
open with the caveat that this is the known-few-plug regime where `-J` is
documented to need a cap; the cap turns out to be **inert here**, `-J --score
f10` recovering identically to `-J` alone. So §7b's recipe should carry `-J`
unqualified.

**`--polish` earns its keep only alongside `-J`, and it is not cheap.** Alone it
buys +2.4pp for **2.9× the boards** — the same lift `-J` gives for *less than
half* the baseline cost, so on its own it is dominated. Combined it is the best
arm at +6.0pp, and superadditive (2.1 + 2.4 = 4.5 against 6.0 measured), which
is consistent with the cheaper climb affording the finisher a better board to
work on.

> The costs here are **not matched**, and the comparison the rest of this
> project makes for finishers — against spending the same compute on more `-R`
> restarts, which `CLAUDE.md` records as dominating every finisher variant — is
> **not made**: the rotor key is given and `-R` is left at its default, so there
> is no restart axis in this measurement. `-J`'s win needs no such defence,
> being cheaper *and* better; `--polish`'s +6.0pp at 2.5× does.

**Step 5a — crib-as-seed** (§7a). **Done** — step 5's seeding is the mechanism,
and §7c is the measurement caution 1 was waiting for: a wrong hypothesis wins 5%
of the time at 8 letters and never from 10 up (450 trials), so **the seed mode's
floor is 10 letters**, set by silent failure rather than by cost.

**Step 6 — crib lists** (`--crib-list`), the cost check, the per-crib banner
(§8) and the rename of the old `--crib-file` to `--crib-rerank`. **Done.** Steps
3 to 5 take a single crib because that is the smallest testable thing; this step
is what makes the feature usable, since you normally know a network's vocabulary
rather than a particular message's contents.

`--crib-list FILE` reads the generator's output — one crib per line, `#`
comments, duplicates dropped, **file order preserved** — and runs one complete
rotor sweep per crib (crib-outer, §6.7), keeping the best board across all of
them. Three things that are fatal for a single `--crib` are ordinary for a
library and merely skip the crib: it can be longer than the ciphertext, it can
match the ciphertext at every alignment, and it can reject every key. A library
is written against a network's vocabulary, not against one message, so most of
its cribs not fitting is the normal case rather than an error.

**The budget logic became a cost check, and §4.2b is why.** The plan called for
"try cribs until the budget runs out". That would have been wrong in two ways.
The unit is not cribs or rotor settings but **surviving hypotheses**, because
under `-c` a surviving key is climbed once per surviving hypothesis — 1.0 per
key where a crib rejects, 235 where it does not, the difference between a 126×
speedup and a 66× slowdown. And a crib's cost cannot be read off the crib:
`NULLNULLNULL` (12 letters) rejects 78% while `XHOCKXHOCKX` (11) rejects 1%, so
length and spare letters — the two numbers the generator ranks by — do not
predict it. It depends on the crib *and* the ciphertext together.

**Every crib's expected gain is reported**, as one row per crib before its
sweep: `# crib len algn hyp/key gain`. `gain` is what a key costs *without* the
crib over what it costs *with* it — above 1 it saves work, below 1 it costs more
than using no crib at all. **Measured, not modelled**: `crib_unit()` and
`hillclimb_one()` are both run on the same eight sampled keys and their
plugboards-scored counters compared, so it already contains both opposing
effects — keys rejected for free, and extra climbs where they are not. Boards
rather than wall time, so a printed number stays reproducible. It omits the
deduction's own cost (outside the score loop), so a crib rejecting nearly
everything is flattered — hence `>1000x` rather than a figure; `<` marks a crib
that hit the work budget, a bound rather than a measurement.

**Cribs run cheapest-measured-cost first by default** (`--no-crib-reorder` keeps
the library's own order). This reverses §5 step 5, which priced cribs with
`build_cribs.py`'s *modelled* cost — charged by length, on the assumption that
sweep cost is roughly flat (§4.1's table: 100–117 s for every row). Measured on
one message and one key space, startup subtracted, the curve is a **cliff**:

| letters | 8 | 10 | 12 | 14 | 16 | 20 | 25 |
|---|--:|--:|--:|--:|--:|--:|--:|
| cost vs no crib | **52×** | 6.7× | 0.67× | 0.074× | 0.074× | 0.037× | 0.02× |

A ~2 600× spread against the old model's 13×, with the 16-letter point agreeing
with an independent measurement on a 5× larger key space (0.074× against
0.085×). How often a crib is *present* spans only ~26× (§4.2), so the cost term
dominates: the whole long tail of a library costs less than one short crib.
Ordering is a **preference, not a filter** — nothing is discarded, so the worst
case is a later win, never a lost one.

**Measured end to end, it is a mean win and a median loss.**
`eval/crib_order_probe.py` times the run to the first crib that recovers the
message — what a human watching progress lines waits for, there being no
automatic early exit (§6.7). Three trials, 120-letter messages, 10 cables
hidden:

| trial | file order | cost order | speedup | winning crib |
|--:|--:|--:|--:|---|
| 1 | 15.3 s | 22.4 s | 0.68× | `XNAQZIEHENX` (11) |
| 2 | 260.5 s | 6.1 s | **42.9×** | `AXOPOTSCHKAX` (12) |
| 3 | 122.5 s | 164.9 s | 0.74× | `FRIEDRICH` (9) |
| **mean** | **132.8 s** | **64.4 s** | **2.06×** | |

Cost order is faster in **1 of 3** trials and the 2.06× mean rests entirely on
trial 2, so the distribution favours it and the median does not, at a sample far
too small to separate them. What the shape shows is an **asymmetry**: the loss
is bounded by the cost of running the long tail first (a few seconds), the win
is not (260 s → 6 s). That asymmetry follows from the cost cliff rather than
from these three trials, and is the actual argument for the default.

> ⚠️ **Those are in-sample numbers, and they flatter file order.** The library's
> file order is by evidence of recurrence counted on the 69 corpus messages, and
> the probe draws its test messages from that same corpus — so file order
> front-loads phrases already known to occur in the message being attacked. On
> external traffic those counts carry no such information. Measured cost has no
> such problem: it is re-measured against the actual ciphertext every run. The
> bias runs *against* cheapest-first, so the asymmetry survives it — but a
> held-out (leave-one-out) library is what would settle the question, and has
> not been run.

**The held-out version was built, run, and abandoned — and why it failed is
itself the finding.** `crib_order_probe.py` now rebuilds the library per trial
from the corpus *minus* the message under test (`holdout_library()`), which
removes file order's unfair advantage exactly. Run that way, **no crib in the
library recovered the message in either trial before the run was stopped.**

That is not a bug in the harness, and it does not contradict §5a's 83% held-out
coverage. Coverage is **supply, not recovery**: 47 of the 57 held-out hits are
8–11 letters, which §7a's tiers call seed-only, and a seed alone will not carry
a 120-letter message with the board hidden to a 95%-correct decrypt. Holding a
message out removes precisely the long cribs that *would* have solved it,
because those are the ones harvested from the message itself.

So **the ordering question cannot be settled on this corpus at all** — not with
more trials, since honest hits are too rare to accumulate a sample against. The
cheapest-first default therefore rests on the **cost cliff** above, which is
directly measured and corpus-independent, and not on any time-to-first-hit
result. The leave-one-out code stays in the probe for a larger corpus, should
one appear — **none is available, so this is dropped rather than deferred**.
The in-sample table above stays with its warning, as the only end-to-end figure
there is.

**A `--crib-max-hyps` flag that *discarded* costly cribs was built, measured,
and removed.** It skipped any crib above a cap in surviving hypotheses per key,
measured on a fixed 256-key stride. Run against the shipped library on a message
containing four of its cribs:

| | outcome |
|---|---|
| skipping at break-even | **all four present cribs skipped**, nothing found |
| not skipping | **100% recovered**, 8 s |

Wrong twice over. The premise fails — "no crib" is not the same outcome more
cheaply, it usually *fails*: §7a measures a 5-cable seed recovering **55%
against 12%** for the same compute spent on restarts, so comparing a crib's cost
against a cheaper way of failing will always reject the crib. And no threshold
rescues it, because **cost is anti-correlated with the chance of a hit**: short
cribs reject nothing, so hypotheses survive everywhere and they are the most
expensive, while §4.2 measures **93% of messages carrying an 8-letter crib
against 3% for a 20-letter one**. Cost-ordered *pruning* removes the library's
most valuable entries first.

**Reordering captures the same throughput without the risk**, which is why it
replaced the flag rather than joining it: cheapest-first runs the long tail
early and costs nothing when it misses, because nothing is discarded and the
run continues.

This is the same shape as `--ring-stride`: a real throughput lever that trades
away recovery, correctly measured only once it was run end to end rather than
judged on its cost model.

**The score threshold for early exit (§6.7) is CLOSED, not pending.** The
run sweeps the whole list and ranks — §6.7's own fallback: worst-case
cost, but it never discards the truth. Stopping is by **human inspection**,
which is what the per-crib table, the progress lines and `--full-text` are for,
and which needs no threshold. A threshold would need a measured per-length
score margin to be trusted, and the measurement above shows this corpus cannot
supply one — so it is dropped rather than deferred.

**Step 1 was the decision point, and it passed** (§5a): the library covers 83%
of held-out messages and a shuffled control covers none, so the recurrence is
real rather than short strings colliding. The rest follows. Two qualifications
carry forward — the coverage is *supply*, not recovery, and 47 of the 57 hits
are too short to reject a rotor setting, so **step 5a (crib-as-seed) is the mode
that matters most on this corpus**, not step 5.

---

## 13. Open questions

**Four of the five are still open; question 4 is answered.** None of them blocks
the feature — everything in §12 shipped without them. Question 3 is the one that
attacks the actual constraint (§11: supply, not the algorithm) and it can be
measured offline with `eval/crib_menu.py` before any C++ is written. Questions 1
and 5 are respectively blocked on a corpus that does not exist and a throughput
detail; question 2 is buildable but needs a measurement design first.

1. **How many cribs can we usefully hold?** §9 says several hundred by compute.
   Whether the generator can produce several hundred *good* ones is untested.
2. **Should the crib mode reject or rank?** Rejecting a rotor setting outright
   is faster; keeping a score for every setting is more robust to a slightly
   wrong crib. Perhaps both, under a flag.
3. **Is the X-separator variant worth building?** Knowing only *where the
   separators are* — not what the words are — is a valid crib and an unusually
   efficient one: 14 known separator positions reject as strongly as a 22-letter
   phrase, because every deduction starts from the same letter and so needs only
   one guess. But the positions have to come from somewhere, and knowing 14 word
   boundaries may be as hard as knowing a phrase. Worth a measurement before any
   code.
4. ~~**Does a wrong seed ever beat the right one?**~~ **Answered in §7c**: at 8
   letters, 5% of the time; from 10 letters up, never in 450 trials. Left here
   for the shape of the argument. In the seeded
   sweep 25 of the 26 hypotheses seed garbage; if one of those converges to a
   board scoring above the correctly-seeded climb at the true rotor setting, the
   mode silently loses that message. The ~1% scoring-failure floor says it
   should be rare, but it is the one thing that could undo §7a and it is
   unmeasured.
5. **Can the menu be reused across alignments?** Shifting a crib by one position
   changes every edge, so probably not — but it is worth checking before
   assuming the alignment sweep costs full price each time.
