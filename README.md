# Enigma cipher tool

A command-line tool that **simulates** an Enigma cipher machine and, more
usefully, attempts to **break** Enigma ciphertext when the settings are unknown
— by brute-forcing the rotor/reflector/ring/start settings and hill-climbing the
plugboard, scoring each candidate decryption against per-language letter
statistics so the most language-like plaintext wins.

It supports three machines:

- the common **three-wheel Enigma** (wheels I–VIII, reflectors A/B/C),
- the **Norway Enigma** (Norenigma) variant (reflector N, wheels 1–5), and
- the four-rotor naval **M4** (thin reflectors UKW-b/c plus the Beta/Gamma Greek
  wheel).

The settings are the reflector (*umkehrwalze*), the wheels (*walzen*) and their
order, the ring positions (*ringstellung*), the start positions
(*grundstellung*), and the plugboard (*steckerbrett*).

## Building

You need a C++17 compiler (g++ or clang++) and `make`. There are no external
dependencies; the n-gram data files ship in this repository.

```sh
make                 # builds the ./enigma binary (g++ -O3 -pthread)
make CXX=clang++     # build with clang instead
make test            # build, then run the test suite
make bench           # build, then run the performance benchmarks
make crackquality    # build, then measure short-message cracking quality
```

## Quick start

The ciphertext (or plaintext) is read from **standard input**; the result is
written to **standard output**. Only the letters A–Z are processed — accented
Latin letters are folded to their base (`é→E`, `ü→U`, `ø→O`, `ß→S`), case is
upper-cased, and spaces/punctuation are dropped; any other non-mappable
character is skipped with an informational warning.

```sh
# Encrypt. A fully specified machine with no -c just enciphers its input --
# nothing is searched -- so no scoring options are needed. (Give an explicit
# -r: the default ring is "AA.", whose wildcard would otherwise turn this
# into a search.)
echo "ATTACK AT DAWN" | ./enigma -u B -w 531 -r ABC -g XYZ
#   -> YYHISFEPIWUP

# Decrypt. Enigma is reciprocal: the SAME settings turn ciphertext back into
# plaintext.
echo "YYHISFEPIWUP" | ./enigma -u B -w 531 -r ABC -g XYZ
#   -> ATTACKATDAWN

# Encrypt with a plugboard (pairs A<->B and C<->D):
echo "THE QUICK BROWN FOX" | ./enigma -u B -w 123 -r AAA -g AAA -s "AB CD"
```

### Cracking

```sh
# You know the rotor key but not the plugboard: hill-climb the plugboard
# (-c), scoring with the weighted all-order model (-a, the sharpest; -l
# gives the language). The default model is the index of coincidence, so
# pass -a to use it.
./enigma -c -a -l english -u B -w 241 -r AAA -g QEW < cipher.txt

# You don't know the start positions either: wildcard them with '.' and the
# program brute-forces all 26x26x26 of them, on 4 threads, while still
# hill-climbing the plugboard.
./enigma -c -a -l english -u B -w 123 -r AAA -g ... -T 4 < cipher.txt

# You know almost nothing: wildcard the reflector, wheels, ring and start,
# and let it try everything. (This is a large search — use as many threads
# as you have cores, and see "Cracking strategy" below for the recommended
# options.)
./enigma -c -a -l english -u . -w ... -r ... -g ... -T 8 < cipher.txt
```

### Other machines

```sh
# Norway Enigma (reflector N, wheels 1-5):
echo "GODDAG" | ./enigma -n -u N -w 123 -r AAA -g AAA

# M4 (4-rotor naval): -u is the thin reflector b/c, and -w/-r/-g take FOUR
# characters with the Greek wheel (B=Beta / G=Gamma) / ring / start first.
echo "WETTERBERICHT" | ./enigma -4 -u b -w B123 -r AAAA -g AAAA
```

## How it works

For a fully specified machine the tool just enciphers the input. When some
settings are left unspecified (a dot `.` wildcard), it searches:

1. For every combination of the unspecified reflector / wheel order / ring /
   start positions, it decrypts the ciphertext.
2. Each candidate plaintext is **scored** against the chosen statistical model
   (index of coincidence, or mono/bi/tri/quad-gram frequencies for a language).
   Real plaintext scores far higher than gibberish, so the highest-scoring
   settings are almost always the correct ones.
3. If `-c` is given, for each candidate key the plugboard is recovered by a
   **hill-climbing** search (greedily adding/swapping plug pairs to raise the
   score) before the key is scored. `-c` on its own climbs by **steepest
   ascent** — score all 325 plug toggles, apply the single best, repeat until
   nothing improves; `-J` swaps that rule for a cheaper first-improvement one.

The search is exhaustive over the rotor settings and heuristic over the
plugboard (whose ~150-trillion 10-pair configurations are far too many to
enumerate). The best plaintext found is printed to stdout; progress and a final
diagnostic (timing, threads, memory) go to stderr. A progress line is echoed for
every improvement — each rotor setting that beats the best so far, and, with
`-c`, each intermediate plugboard improvement inside a climb, so you can watch
the board being built up plug by plug. The lines are fixed-width columns under a
`Score W R G S Text` header — score, reflector+wheels, ring, start, plugboard
and the first characters of the decoded text (19 on a 3-wheel machine, 16 on the
wider M4 key, so the line always fits 80 columns):

```
 -7.0190 B241 AAA QEW AB IJ                                  PGFQUODLASYKYITSOEK
 -6.5572 B241 AAA QEW AB EF IJ                               PGEQUODLASYKYITSOFK
 -5.5984 B241 AAA QEW AB EF GH IJ                            PHEQUODLASYKYIISOFK
 -4.8456 B241 AAA QEW AB EF GH IJ KL                         PHEQUIDKASALYSISOFL
 -4.3335 B241 AAA QEW AB CD EF GH IJ KL                      THEQUICKANALYSISOFL
```

That preview is enough to notice a board turning into German and not enough to
read the message. **`--full-text`** prints the whole decryption under each
progress line, wrapped and indented, for when you are watching the output rather
than waiting for a score:

```
 -4.3335 B241 AAA QEW AB CD EF GH IJ KL                      THEQUICKANALYSISOFL
  THEQUICKANALYSISOFLONGMESSAGESISWHATMAKESTHISWORTHWATCHINGATALLXXXXXXXXXXXXXX
  XXXXXXXXXXXXXXXXXXXX
```

Those lines say how *well* the search is doing and nothing about how far it has
come — and they thin out to nothing exactly when a run is longest, since a new
line needs a new best. So underneath them sits a **live progress line** that
rewrites itself in place:

```
Progress:   50% (5.94M / 11.88M keys) 10.12M/s, 1s left
```

With `-R` restarts the sweep runs **restart-major** — every key once, then
every key again with a fresh kick — so the answer tends to appear early rather
than after each key's full budget, and the line shows which pass it is on
(`pass 2/16, 8728 / 17.6k keys`). The rate is key-visits per second and the ETA
runs to the end of the whole sweep.

There is no flag for it: it appears whenever stderr is a terminal, updates
about every 5 seconds, disappears when the search ends, and steps aside for each
score line rather than being written over. Redirect stderr to a file and it is
not there at all, so logs and scripts see exactly what they saw before. Nothing
is drawn for a sweep that finishes in under half a second.

## Options

Defaults are shown in `[brackets]`. A dot `.` is the wildcard for the reflector,
wheels, ring and start positions — any position left as `.` is brute-forced.

### Machine settings

- **`-u X`** — Reflector (*umkehrwalze*): `A`/`B`/`C`, `N` for Norway, `b`/`c`
  for M4 thin, or `.` `[.]`
- **`-w XYZ`** — Wheel order, left to right: digits `1`–`8` (`1`–`5` for Norway)
  or `.` per position `[...]`
- **`-x N`** — Highest wheel number to consider when a wheel is wildcarded `[5]`
- **`-n`** — Norway Enigma mode (reflector `N`, wheels 1–5)
- **`-4`** — M4 mode; `-u` is the thin reflector `b`/`c`, and `-w`/`-r`/`-g`
  take **four** characters, Greek wheel (`B`/`G`) / ring / start first
- **`-r XYZ`** — Ring positions (*ringstellung*), letters `A`–`Z` or `.` `[AA.]`
- **`-g XYZ`** — Start positions (*grundstellung*), letters `A`–`Z` or `.`
  `[...]`
- **`-s AB…`** — Known plugboard pairs, e.g. `-s "AB CD EF"`; held fixed during
  `-c`/`-A` (the climb keeps them and recovers the rest) `[none]`
- **`--no-plug LETTERS`** — Letters known to carry **no** cable, e.g.
  `--no-plug XQZ`; the climb leaves them unplugged, exactly as `-s` holds its
  pairs plugged. Needs `-c` `[none]`
- **`--soft-plug AB…`** — Plugboard pairs *guessed* rather than known: the climb
  starts from them each restart but may move or drop them, unlike `-s`. Needs
  `-c` `[none]`
- **`--signature-seed K`** — Seed the climb from a doubled word closing the
  message (a signed surname): deduce candidate boards per key, rank by index of
  coincidence, climb the top `K`. Needs `-c` `[0 = off]`
- **`--signature-length L`** — Shortest signature to hypothesise `[4]`

`-n` and `-4` are mutually exclusive. In M4 mode only the Greek wheel's `start −
ring` offset is recoverable, so a full M4 wildcard search enumerates the
distinct offsets rather than every ring×start pair.

**Reported ring positions may differ from the key you enciphered with.** The
same identifiability limit applies to the stepping wheels, and the search
exploits it to skip provably redundant keys:

- **Leftmost wheel** — nothing downstream depends on its window position, so
  only `start − ring` is recoverable. Its ring is always reported as `A`.
- **Middle wheel** — shifting its ring and start together changes the decode
  only if its notch is reached, and in a short message most start positions
  never reach it. Those settings are indistinguishable from the ciphertext
  alone, so the search tests one member of each equivalence class and may report
  that one instead of yours.

In both cases the **decrypted text is byte-identical** — the reported setting is
simply one of several that produce it. The middle-wheel case is length-dependent
and disappears for long messages (past roughly 676 letters every setting is
distinguishable again), and it only arises when both `-r` and `-g` wildcard that
wheel. If you need the literal key rather than an equivalent one, pin the
positions you already know.

### Scoring (which plaintext "looks like a language")

- **`-i`** — Index of coincidence — language-independent, needs no `-l`
  (**default**)
- **`-m` / `-b` / `-t` / `-q`** — Mono- / bi- / tri- / quad-gram statistics
- **`-a`** — Weighted all-order score — log-linear mixture of quad/tri/bi/mono
- **`-f`** — Weighted all-order score **plus the index of coincidence**
  (**recommended** when the language is known)
- **`-l lang`** — Scoring language: `english`, `german`, `danish`, `french`,
  `swedish`, `finnish`, `icelandic`, `polish`, `spanish`, or `wehrmacht`
  (telegraphic military German — see below). **Required** for
  `-m`/`-b`/`-t`/`-q`/`-a`/`-f`; ignored by `-i`

The **default model is the index of coincidence** (`-i`) — the only one that
needs no language, so the tool runs out of the box with no scoring options. When
you know the language, **`-f` (fused) is the recommended model**. It takes the
weighted all-order score `-a` — a log-linear (geometric) mixture of all four
n-gram orders, itself measurably better than plain quadgrams (`-q`) on short
messages — and adds a weighted **index of coincidence** term. Measured **+3 to
+4.4 pp** mean letters correct over `-a` alone, on English, German *and*
telegraphic traffic alike.

The IC term works differently from the four n-gram orders. Those are combined
*per quadgram window* and folded into one table when the tables load, so they
cost nothing extra at run time. IC cannot be folded in that way — it is
quadratic in the whole-message letter histogram rather than additive per
position — so it is computed alongside the n-gram sum and added to the
per-symbol score. What it buys is a better **climb**, not better discrimination:
IC is language-independent and, being blind to which letter is which, it is the
one signal the plugboard cannot fake, so it supplies gradient where the n-gram
surface is flat. Its weight is baked in (the optimum is a broad plateau, so
there is nothing to tune). Plain quadgrams (`-q`) remain the simple single-order
alternative.

Each selector is an alias for a single-stage `--score <model>` (`-f` is also a
schedule token, e.g. `--score m4f10`), so setting the model to **conflicting**
values — two disagreeing selectors (`-m -q`, `-q -f`), or a selector against a
different `--score` target (`-m --score q`) — is a **fatal error**; agreement
(`-f --score m4f10`, `-q --score i4q10`) is fine. The n-gram tables are highly
language-specific — **`-l` must match the language of the plaintext**,
especially for `-q`/`-a` (scoring an English message with `-l german` typically
fails). Note that `-l` on its own does nothing: it only takes effect with an
n-gram model, so it is `-a -l english`, not `-l english`, that scores with the
English tables.

### Plugboard cracking

- **`-c`** — Hill-climb the plugboard for each candidate key. The climb rule is
  **steepest ascent** by default: score all 325 plug toggles, apply the single
  best, repeat to convergence
- **`-J`** — Change `-c`'s climb rule to **first-improvement in best-first
  order**: apply the first improving toggle instead of scanning for the best,
  ~2.8× cheaper per climb, so **pair with more `-R`**. A matched-compute win on
  the realistic ~10-plug case, may lose with few plugs (needs `-c`; off by
  default)
- **`-M`** — Make the plug cap a strict **descent target**: at/over the cap only
  merge/remove moves (no adds or reshuffles). A matched-compute win with a tight
  `-S` cap, biggest on **known-few-plug** boards; also cheaper per climb (needs
  `-c`; off by default)
- **`--polish`** — **Recommended finisher**: runs a directed quadgram-gain
  repair (plus a deeper 3-plug-tangle escalation) once on the best board after
  all restarts. It runs once after all restarts, so its cost is fixed —
  negligible at a high `-R`, a few % of a low-`R` run — for a small quality bump
  on top of `-R` (needs `-c`; off by default)
- **`--no-plug LETTERS`** — Letters known to carry no cable: the climb, the
  re-pair and the `--random` kick all leave them unplugged. The counterpart
  of `-s` — that one says "these two are plugged to each other", this one says
  "this one is plugged to nothing" — and worth more than it looks, since a
  cable has two ends: each marked letter removes 25 of the 325 candidate plugs
  (needs `-c`; off by default)
- **`--soft-plug AB…`** — Plugboard pairs that are a **guess**, not knowledge.
  The pairs are laid on the board each restart starts from and then left free,
  so the climb can move, merge or remove them. The point is the failure mode: a
  wrong `-s` pin cannot be undone by anything downstream, while a wrong soft
  guess costs only the moves spent walking back out of it — which is what a
  *deduced* seed needs. A soft-seeded board starts good, so it wants a smaller
  `--random` kick than the default (needs `-c`; off by default)
- **`--signature-seed K` / `--signature-length L`** — **Terminal-signature
  seeding**, the one lever measured to beat `-R` at matched compute. Half of
  authentic traffic's doubled words are a surname signing off the message, and a
  doubled word is a *self*-crib: it says two positions share an unknown letter,
  which cancels out of the machine equation and leaves a rule computable from
  the rotor key alone. Per key the tool deduces the candidate boards that rule
  allows, ranks them by index of coincidence, and climbs the top `K` with the
  deduced plugs pinned. On a 676-key sweep `K = 1` recovered 20 of 30 messages
  against a bare `-R 16`'s 16 — at **87× less** compute (needs `-c`; off by
  default)
- **`--crib TEXT` / `--crib-at N`** — **Known plaintext**, and where it sits. A
  rotor setting that cannot have produced the crib is rejected by arithmetic and
  never scored — measured **99.9%** of a start-position keyspace on a 12-letter
  crib. The deduction chains `steck[p] = core[steck[c]]` from a single guessed
  plug and enforces plugboard reciprocity (Welchman's diagonal board), which is
  what makes even short, loop-free cribs bite. **`--crib-at` is 1-based and
  optional**: omit
  it and every alignment is tried, but rejections then multiply across
  alignments, so a swept crib needs **16 letters or more** to be worth anything
  (12 rejects 99.9% pinned and 5.3% swept). Not combinable with `-F`,
  `--exhaust`, `--ring-stride` or `-A`. Add `--crib-dump` to print the plugs
  each surviving hypothesis deduces `[off]`. **With `-c` the crib also seeds the
  climb**: the deduced plugs are held fixed and the climb finds the rest, which
  is where a short crib earns its keep even though it cannot filter — measured
  92% of letters recovered against 8% unseeded, on an 88-letter message with the
  plugboard hidden
- **`--crib-list FILE`** — A whole **crib library** instead of one crib: one per
  line, `#` comments, one complete rotor sweep each, best board kept — run
  **cheapest measured cost first** unless `--no-crib-reorder` is given.
  You rarely know which phrase a message contains — you know the vocabulary of
  the network, which is what `eval/build_cribs.py` harvests. A crib that is too
  long, cannot sit anywhere, or rejects every key is skipped rather than fatal,
  because most of a library not fitting a given message is the normal case.
  Each crib gets a row before its sweep — length, viable alignments, measured
  cost, and **expected gain** (what a key costs without the crib over what it
  costs with it, so above 1 saves work and below 1 costs more than no crib),
  which is the guide to why a crib is worth running or was skipped
- **`--no-crib-reorder`** — Keep a `--crib-list` in the library's own order.
  By default it is run **cheapest measured cost first**. The cost of a crib
  against its length is a *cliff*: relative to a sweep with no crib, 8 letters
  costs **52×**, 12 costs 0.67×, 25 costs 0.02×. Since how often a crib is
  present spans only ~26×, cost dominates — the whole long tail of a library
  costs less than one short crib, so running it first is nearly free. Ordering
  discards nothing, so the worst case is finding the answer later, never not at
  all
- **`-R N` / `--restarts N`** — Random restart attempts: `0` = one deterministic
  climb from the seed (no kick); `N` = exactly `N` kicked climbs, keep the best
  `[0]`
- **`--random K`** — Random-kick size — plug pairs injected per restart (needs
  `-c`; `0` = no kick, a control) `[10]`
- **`-S sched` / `--score sched`** — Staged climb schedule — model stages only
  (see below)
- **`--cascade[=GATE]`** — Per-convergence quadgram-gain directed-repair
  cascade, gated by a near-solution score threshold (needs `-c`; quad-only)
  `[off]`. **Not recommended** — `--polish` supersedes it on a plain sweep; kept
  because it's the only cascade variant that composes with `-F`/`--exhaust`
- **`--no-repair`** — Disable the always-on 2-plug re-pair barrier cross (needs
  `-c`) `[off]`. **Not recommended** — an ablation/measurement flag, not a
  quality lever
- **`--ring-stride K`** — Test only every `K`th ring position of the rightmost
  wheel, then refine every skipped position; needs both `-r` and `-g` to
  wildcard that wheel, and rejects `-F`/`--exhaust` `[1..26, 1 = off]`.
  **Worth using at `K=2` or `K=3`** when throughput matters: on authentic
  telegraphic German they analyse 1.9× and 2.6× fewer keys while costing only
  about half a percentage point to two percentage points of exact recovery.
  `K=3` is the better of the two. `K` of 5 or more costs considerably more
  accuracy and is not recommended. Above 13 the cost stops falling — every `K`
  from 13 to 25 samples two ring positions and analyses the same number of
  keys — so only `K=26` changes anything, and it buys a further 15% for about
  ten percentage points of exact recovery. The refinement pass is *derived*
  rather than searched — the skipped positions' ring/start settings follow from
  the coarse winner's stepping schedule — so it costs a few hundred keys rather
  than tens of thousands, and there is no keyspace where the stride costs more
  than it saves. That derivation is verified to recover everything the old
  exhaustive refinement did, on the standard machine and on M4, with one- and
  two-notch wheels alike. The search is still an *approximation*, so a run says
  so in its echoed settings
- **`--confidence N`** — Before the sweep, sample `N` keys to measure what this
  scoring model produces on this ciphertext with **no signal**, and report how
  far the winner sits above that `[0 = off, use 256]`. A raw score cannot answer
  "did I find anything?" on its own, because every model scores *something* on
  gibberish, and a search reports the best of everything it tried — so the bar
  rises with the size of the keyspace. The line gives three numbers: the
  winner's distance above the null in standard deviations, the distance the best
  of that many keys is expected to reach **by chance**, and the margin between
  them. **Read the margin** — and it is put where you are already looking: with
  `--confidence` on, the first column of every progress line becomes that margin
  instead of the raw score, and the header says `Margin` so a saved log still
  explains itself. **Zero is the line that matters.** Over 17 576 keys, a real
  message reads negative on every line before the true key and then jumps to
  **+17.0**; a ciphertext with random letters behind it tops out at **+0.5**;
  and a hidden plugboard searched without `-c`, which cannot recover it, ends at
  **−0.8** — correctly reporting that nothing was found. Samples are
  hill-climbed when `-c` is on, so the comparison is against what the search
  actually does. **Do not read a small positive margin as a find**: the printed
  p-value comes from a Gaussian tail and is optimistic near zero, measured over
  2000 signal-free runs at **2.35%** for a margin of +0.54 against the 0.70% it
  implies, and rising to 4.83% on a larger key space. A run under +2 σ says so
  itself. The gap is unmistakable in practice — a genuine break reads +15 to
  +17 σ, not +0.5. It also ranks the scoring **language** on a single message:
  on telegraphic German the margin measured +15.4 for `-l wehrmacht`, +8.6 for
  `german` and +2.5 for `english`. **Use `N` = 256, and never below 128**: `N`
  buys nothing but precision in the null, and at `N` ≤ 64 a signal-free
  ciphertext reports a *positive* margin on some seeds — a false "significant",
  which is worse than no answer at all. Bigger is not better either; past 512
  the error left is the null's own shape rather than the sample size. The
  calibration is free without `-c` and costs 1.5–1.7 ms per sample with it —
  slow enough at a large `N` that the sampling shows a progress line, and
  reported in the settings echo so a saved log says what its first column means.
  It needs a key space to sample: with the rotor key fully specified there is no
  null to measure against, and the run says so and reports raw scores
- **`--double-length L`** — report any converged climb whose decrypt carries a
  **doubled word** of `L`+ letters around an X (`ENGELMANN X ENGELMANN` —
  telegraphic German doubles important words as its own error correction), as
  a progress line with the preview replaced by `>> <length> <word>`
  `[0 = off; needs -c and --confidence]`. It is a **confirmation, not a score
  term**: it enters no ranking, so it cannot promote a wrong key — it simply
  makes the true key unmistakable in a long log (`grep '>>'`) even while some
  other board still leads on score. One mismatched letter between the copies
  is allowed (`--double-mismatches`, default 1) — the error a transmission
  garble makes — and the z gate (`--double-z`, default 3) keeps the check
  free by only examining the ~0.5% of keys that score well. Chance reports
  fall ~16× per extra letter: a full 230 M-key sweep expects ~6 spurious
  reports at `L = 7` and ~90 at `L = 6`, so raise `L` rather than the gate —
  a true key whose climb has recovered the plaintext sits at z = 7–16, far
  above it.

- **`--tune-phase N`** — Stop enumerating the middle and right wheels' *phase*
  and optimise it instead: keep `N` starting phases per wheel, hill-climb the
  plugboard as usual, then scan all 26 × 26 phases with that board **frozen**,
  re-climb at the winner, and repeat until neither improves `[0..26, 0 = off]`.
  A wheel's phase is its ring and start shifted together, so its offset — and
  with it the wheel's whole contribution to the substitution — is unchanged and
  the only thing moving is when its own notch fires. The sweep is then over
  offsets alone, 26³ per wheel order instead of 26⁵. The plugboard has to be
  recovered *before* the phase is scored: without a board the decrypt is ~95%
  scrambled and the phase score is noise. Needs `-c`, and both `-r` and `-g`
  must wildcard the middle and rightmost positions; rejects `--ring-stride`,
  `-F`, `--exhaust`, `--crib` and `-A`. An *approximation* — the scan has a
  capture radius of roughly `0.4 × length / 26`, so it wants long messages, and
  a run says so in its echoed settings. Measured against spending the same wall
  time on `-R` restarts over the full ring enumeration (80 paired trials, 200
  letters): it recovers the message exactly more often — 63 times against 51 —
  but gets a lower average fraction of the letters right, because when it fails
  it has usually settled on the wrong offset and the answer is worthless, where
  the exhaustive sweep normally returns a nearly-correct plaintext. Reach for it
  when only a full break is useful to you
- **`--exhaust E`** — Force `E` extra plug pairs among the free letters, try
  every combination, keep the best climb (needs `-c`; parallel over the first
  forced pair, so `-T` helps) `[off]`. **Not recommended** — a
  measured-dominated exploration tool; a higher `-R` beats it at equal compute
- **`-A N` / `--anneal N`** — Recover the plugboard by simulated annealing (move
  budget `N`) instead of the greedy climb (needs `-c`; `0` = off) `[0]`
- **`-F N` / `-F N%` / `--prefilter`** — Key pre-filter: full climb only the top
  `N` keys, or top `N%` of the keyspace (needs `-c`; `0` = off) `[0]`. **Not
  recommended** for short messages — a throughput tool for long (~300+ letter)
  traffic, unreliable on the short/hard end (see "Cracking strategy" below)
- **`-e N` / `--seed N`** — Random seed for restarts / annealing (also
  `$ENIGMA_SEED`); default is a fresh random seed each run

Every option also has a long name (`--restarts`, `--score`, `--climb`, …), and
unambiguous prefixes work (`--restart`, `--lang`); the short forms are kept.

The plugboard climb gets stuck in local optima on short messages, so `-R N` runs
`N` climbs, each from a randomly **kicked** board, and keeps the best; `--random
K` sets the kick size and `-S`/`--score` runs the climb in stages. (`-R 0`, the
default, is a single deterministic climb from the seed — no kick.) `-A N` is an
alternative recovery method that anneals the plugboard (accepting some worsening
moves early to escape local optima, cooling to a greedy finish); at equal
compute it is a peer of `-R … --score iq`. `-A` honours the `--score` target
cap, so if you **know** the plugboard uses fewer than the usual pairs you can
tell it — `-A N --score q8` anneals toward at most 8 pairs, which improves
recovery on short messages (and is a wash otherwise; don't set it below the real
count). When you are also brute-forcing rotor settings, `-F N` shortlists the
most promising keys with a cheap pass so the expensive climb runs only on those.
See **Cracking strategy** below.

The restarts are seeded from a **fresh random seed each run** (`/dev/urandom`
via `std::random_device`), so repeated runs explore different perturbations. The
seed is echoed in the settings line; pass it back with `-e N` (or
`$ENIGMA_SEED`) to reproduce a run exactly. Results stay independent of the
thread count for a given seed.

### Data and performance

- **`-d dir`** — Directory holding the n-gram files (else `$ENIGMA_DATA`, else
  `ngrams`) `[ngrams]`
- **`-T N`** — Worker threads for the search, 1–256 `[1]`
- **`-p file`** — Compare the recovered plaintext against a known plaintext file
- **`-v` / `-h`** — Version / help

For **authentic telegraphic German traffic** (real Wehrmacht messages: `X`
separators, `Q` for *ch*, spelled-out numbers) the prose German tables mis-score
the plaintext. Use **`-l wehrmacht`**, a domain-matched scoring language built
from the published statistics of ~20 000 letters of 1941 Enigma decrypts: it
recovers **+20.9 pp** more (mean %-letters-correct) over a 69-message held-out
set of real 1941 messages. It is a *writing style*, not a separate language — on
prose German it is a domain **mis**match and measured **−10.2 pp**, so ordinary
German text (and `make crackquality`) should stay on `-l german`. Regenerate the
tables with `python3 eval/build_telegraphic_ngrams.py`; see
`eval/MODERN_BREAKING_NOTES.md` §6.

A known-word (**crib**) finisher, `--crib-rerank <f>`, re-ranks converged
plugboards by `score + --crib-weight × (known words present in the decrypt)`,
with the word list read from `<f>` (vocabulary in `cribs/`) and `--crib-weight
X` setting how much that bonus counts against the n-gram score `[0.5]`. It is
**opt-in and not recommended** — measured net-neutral on real traffic (the
residual short-message failures are wrong-basin, so re-ranking can't reach the
truth); kept as a diagnostic. See `cribs/README.md` and
`eval/MODERN_BREAKING_NOTES.md` §7.

For the complete, authoritative option list — including the advanced finishers
(`--polish`, `-M`, `-A`) and the internal diagnostic flags — run `./enigma -h`.
The list above describes the everyday options; `-h` groups the rest as
**recommended**, **advanced**, **non-recommended** (opt-in; dominated or
situational), and **diagnostic** (measurement only), and prints the recommended
short-message recipe at the end.

## Cracking strategy

A plain `-c` climb recovers the plugboard reliably on long messages but gets
stuck in local optima on short ones. Two options improve this and **compose**:

- **`-R N` — random restarts.** Runs `N` climbs, each from a randomly **kicked**
  board, and keeps the best result. This is the biggest lever for short
  messages, and it keeps paying as `N` grows (there is no practical plateau) —
  at the cost of roughly `N`× the work. `-R 0` (the default) is a single
  deterministic climb from the seed, no kick; `-R N` runs exactly `N` kicked
  climbs (the un-kicked seed climb is not additionally run).

- **`--random K` — kick size.** The per-restart kick is `K` random plug pairs,
  defaulting to **10** (close to a typical plug count, which works best).
  `--random 0` is a legal control (no perturbation — `N` restarts then repeat
  the seed climb).

- **`-S <schedule>` / `--score <schedule>` — staged climb.** A schedule is a
  string of `<letter><optional cap>` model tokens `i`/`m`/`b`/`t`/`q`/`a`, climb
  stages run in order; an optional number caps how many plug pairs that stage
  may set (omitted = uncapped). The **last** model token is the target/ranking
  model. Climbing a low-order model first (its scoring surface is smoother when
  only a few plugs are set) steers the early plugs into a better basin. With the
  recommended fused target the staging is a **mono pre-pass**: `--score m4f10`
  climbs monograms (capped at 4 plugs), then refines under the fused model
  (capped at 10). For a plain **quad** target an index-of-coincidence pre-pass
  measured best instead (`--score i4q10`). **On telegraphic traffic at
  operational length, use `--score i4f10`**: over 2000 paired trials at 167
  letters on authentic 1941 Army messages it beat `m4f10` by 2.8 pp of letters
  correct and 3 pp of exact recovery (72.2% → 75.2%). (The kick and the
  exhaustion are their own options, `--random` / `--exhaust`, not schedule
  tokens.)

  > **Which pre-pass is best depends mildly on the writing style.** `m4a10` is
  > the general recommendation and is what the recipes below use, but a paired
  > A/B against the IC pre-pass `i4a10` (same weighted target, matched compute,
  > 1800 trials each) put mono ahead on telegraphic traffic (**−2.2 pp** for
  > IC), level on English prose (−1.4 pp, CI spanning zero), and **behind on
  > German prose** (**+2.2 pp** for IC). So on German prose specifically,
  > `--score i4a10` is worth a try; everywhere else keep `m4a10`. (Measured with
  > the `-a` target; not re-checked against `-f`, whose IC term may change the
  > calculus.) The gap is ~2 pp either way, so this is a fine-tuning note, not a
  > reason to change your default. The reason is that mono scores letter
  > *identities* against one language's frequencies, while IC only measures how
  > uneven the distribution is and is therefore language-blind. See
  > `archived/PERFORMANCE.md` §6.10.

- **`-F N` (or `-F N%`) — key pre-filter.** With `-c` the full `-R`/`-S` climb
  is paid on *every* candidate key, which dominates runtime when you wildcard
  rotor settings. `-F` instead runs a single **cheap index-of-coincidence
  climb** on every key, keeps the best `N` (or the best `N%` of the keyspace),
  and pays the full climb on only those — a ~8–20× throughput win, so you can
  afford more restarts per surviving key. (A plugboard-free IC *scan* does not
  work here: under a full plugboard the rotor-only decrypt is almost entirely
  scrambled, so it cannot rank keys; a cheap IC *climb* partially recovers the
  plugboard and does.) Prefer the `N%` form — recall depends on the *fraction*
  of the keyspace you keep, so a percentage stays meaningful as the wildcard
  grows; keep a generous slice (≥ ~10%), since too tight a shortlist on a large
  keyspace or a weakly-discriminated wildcard can drop the true key. It is a
  throughput tool, not lossless: on a large keyspace with a full plugboard even
  a good `N` recovers only around half of the hardest keys.

- **`-J` — swap the climb rule for first-improvement in best-first order.** `-c`
  alone climbs by **steepest ascent**: every step scores all 325 plug toggles
  and applies the single best one. `-J` instead applies the **first** toggle
  that improves and sweeps the move list circularly, with the order rebuilt per
  restart from the starting board — so a climb costs ~2.8× less. A single `-J`
  climb recovers a bit *worse* than steepest ascent, so it only pays off when
  you **spend the saved time on more restarts**: pair it with a larger `-R` and,
  at equal compute, it recovers noticeably more of a short message. Leave it off
  for a single climb (`-R 0`).

The recipes below build the schedule and plug-cap mechanics up on the
**recommended** fused target (`-f`, staged as `--score m4f10`). The percentages
quoted alongside them were measured with a quad target (`--score iq` / `i4q10`);
what they illustrate — pre-pass, plug cap, cap-as-target — is a property of the
schedule rather than of the model, and `-a` is the sharper model to run it on
(see "Scoring" above).

A good general recipe for a hard (short) message with a known rotor key:

```sh
./enigma -c -R 20 -f --score mf -l english \
         -u B -w 241 -r AAA -g QEW < cipher.txt

# faster climbs → more restarts for the same time: add -J and raise -R
./enigma -c -J -R 55 -f --score mf -l english \
         -u B -w 241 -r AAA -g QEW < cipher.txt
```

**Cap the plug count in the schedule.** A real Wehrmacht board has ~10 plugs, so
capping the final (weighted) stage at 10 and the pre-pass lower — `--score
m4f10` — keeps the climb from adding spurious plugs on the noisy short-message
score, and (being cheaper) buys more restarts for the same budget. Measured with
a quad target on ~50–80-letter 10-plug messages, capping recovered several
percentage points more than the uncapped schedule at equal compute. The
**final** cap of 10 is what matters; the pre-pass cap is a **flat plateau**
(measured for the IC pre-pass: ≈3–6 all tie, so the exact value barely matters —
`4` is a fine representative). The kick stays the default — a *small* kick like
`--random 3` hurts:

```sh
./enigma -c -R 26 -f --score m4f10 -l english \
         -u B -w 241 -r AAA -g QEW < cipher.txt
```

If you know the board uses **few** plugs (say 6), cap at that count instead
(`--score m4f6` or so) — there the cap is a large win, and adding **`-M`** makes
it larger still: `-M` turns the cap into a strict descent target (at/over the
cap the climb may only merge or remove plugs, never add or reshuffle), so a
restart's random kick is cleanly pruned back down to the true count instead of
leaving spurious plugs. On known-few-plug short messages this adds several more
points (up to ~+20pp at the hardest lengths) at equal compute, and it climbs
faster too:

```sh
./enigma -c -R 26 -f --score m4f6 -M -l english \
         -u B -w 241 -r AAA -g QEW < cipher.txt
```

When you also brute-force the rotor key, add `-F` to shortlist keys and `-T` for
threads:

```sh
./enigma -c -R 20 -f --score m4f10 -F 10% -T 4 -l english \
         -u . -w ... -r AAA -g ... < cipher.txt
```

Increase `-R` for harder messages.

### Two recommended recipes (standard ~10-plug board)

**Use the weighted all-order model `-a` when you know the language** — it
recovers short messages measurably better than plain quad (`-q`) at no extra
cost (a log-linear mixture of all four n-gram orders; see "Scoring" above),
staged as `--score m4f10`. There are two strong plugboard solvers, and at
**matched compute** they are **peers with a length-dependent crossover** — pick
either, or run both:

- **Greedy** — the tuned restart climb: dynamic move ordering (`-J`) over a
  capped staged schedule (`--random 10` kick → mono pre-pass → weighted capped
  at 10 plugs), plus the best-board finisher `--polish` (one fixed-cost pass
  after all restarts). Very cheap per restart, so it affords many.

  ```sh
  ./enigma -c -J --score m4f10 --polish --random 10 -R 40 -f -l english \
           -u B -w 241 -r AAA -g QEW < cipher.txt
  ```

- **Simulated annealing** (`-A`) — a *deep* anneal per restart (small `-A`
  starves it) with the same 10-plug cap:

  ```sh
  ./enigma -c -A 12000 --score a10 -R 12 -a -l english \
           -u B -w 241 -r AAA -g QEW < cipher.txt
  ```

Measured **on English prose**, 50–70-letter 10-plug messages at equal
`score_iter`, **SA tends to win the very shortest / hardest lengths and greedy
the slightly longer ones**, within a few points either way — so on prose neither
dominates. Scale `-R` up together for harder messages (and `-A` is the SA depth
knob; keep it deep). Both compose with `-F`/`-T` when the rotor key is also
unknown.

> **On telegraphic traffic (`-l wehrmacht`), that parity does not hold — use
> greedy.** The peer framing above was measured on prose; re-measured on
> authentic telegraphic plaintext it does not transfer. Over 3000 paired trials
> (L50–90, 10-plug boards, both solvers at the recipes above and equal
> `score_iter`), greedy wins **every** length with a mean advantage of **10.4
> pp** letters correct — from **5.7 pp** at L50 to **15.4 pp** at L90, every 95%
> CI excluding zero. The *direction* of the prose result survives (SA closes the
> gap as messages get shorter, and its per-trial win rate rises from 22% to
> 36%), but there is **no crossover**: SA never reaches parity in this range.
> Part of the gap is structural rather than algorithmic — `-A` uses only the
> *last* `--score` stage's plug cap and seeds itself with a built-in IC
> pre-pass, so it cannot use greedy's mono pre-pass, which is worth ~3–4 pp over
> IC. See `eval/eval_sa_vs_greedy.py` and `archived/PERFORMANCE.md` §3.11.

## Input, output and diagnostics

- **Input** comes from stdin; only A–Z are kept (accented letters folded to
  their base, case upper-cased, spaces/punctuation dropped, other non-mappable
  characters skipped with a warning).
- **Output** is the single best-scoring plaintext, on stdout.
- **Diagnostics** go to stderr: the resolved configuration is echoed at the
  start, the running best is shown during the search, and two final lines report
  the number of rotor combinations analysed and plugboards scored, then
  wall-clock time, thread count, the precomputed-table memory and peak memory.
  When stderr is a terminal the main sweep also shows a **live progress line**
  (percentage, key rate and ETA) that rewrites itself in place and steps aside
  for each score line; with `-F`, the pre-filter's ranking phase shows one of
  its own (percentage of keys ranked). Neither appears when stderr is
  redirected, so a saved log is unchanged. **`--full-text`** adds the whole
  decrypted message under each progress line instead of the 19-character
  preview.
- A scoring model is needed only when the run actually scores — a wildcard
  search or a plugboard hill-climb (`-c`). Those require `-l` (or `-i`). Pure
  encryption/decryption — a fully specified machine with no `-c` — needs no
  scoring options at all. (The default ring is `AA.`, so give an explicit `-r`
  to encrypt; otherwise the wildcard makes it a search and `-l`/`-i` is required
  again.) A fully specified run still prints a single-candidate score on stderr,
  and it honours the model you ask for: `-q -l english` scores that one decrypt
  with quadgrams; with no scoring options (or an n-gram model but no `-l`) it
  falls back to the index of coincidence, which needs no language.

## n-gram data files

Scoring uses letter-frequency tables read from
`<datadir>/<language>_<ngram>.txt`, where `<ngram>` is
`monograms`/`bigrams`/`trigrams`/`quadgrams`. Each line is `<LETTERS> <count>`,
e.g. `TION 13168375`. The tables ship in the `ngrams/` subdirectory, and the
data directory is resolved as `-d <dir>` → `$ENIGMA_DATA` → `ngrams` (the
bundled default, found when run from the repo root) — pass `-d` or set
`$ENIGMA_DATA` to run from another working directory.

At load time each count is converted to a log10 probability
`log10(count/total)`, with n-grams never seen in the corpus floored at
`log10(0.01/total)` so that gibberish (which contains impossible n-grams) is
penalised rather than ignored. The model score is the per-symbol average of
these — a cross-entropy in dits/char, so scores are negative and roughly
length-independent (a more language-like decrypt scores closer to zero). The
index of coincidence (`-i`) is a separate normalised statistic and is
unaffected.

Tables for `english`, `german`, `danish`, `french`, `swedish`, `finnish`,
`icelandic`, `polish` and `spanish` ship in this repository. They were obtained
from the [Practical
Cryptography](http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/)
website, where additional languages are available in the same format.

The tenth language, **`wehrmacht`** (telegraphic German), is derived instead
from the published work of Geoff Sullivan and Frode Weierud. Its **monogram**
table is Figure 17 of Appendix C of *Breaking German Army Ciphers* (Cryptologia
29(3):193–232, 2005) verbatim — single-letter frequencies over ~20 000 letters
of 1941 Enigma decrypts — and its **bigram and trigram** tables are the prose
German tables reweighted toward those statistics together with Figure 18, the
400 most frequent trigrams from the same corpus. Both figures are transcribed in
`eval/appendix-c-fig17-monograms.txt` and `eval/appendix-c-fig18-trigrams.txt`,
and `eval/build_telegraphic_ngrams.py` regenerates the tables from them. These
are *aggregate published statistics*, not the plaintext of any individual
message, so the authentic-message sets in `eval/` remain held out.
Language-specific letters outside plain A-Z are folded to a base A-Z letter when
a table loads (diacritics stripped: `Ä`/`Å`/`Ö` → `A`/`A`/`O`, `Ñ` → `N`, `Ł` →
`L`, Icelandic `Þ` → `T` pairing with `Ð` → `D`, etc.) — see `fold_codepoint()`
in `enigma.cc`.

## Performance

The search is parallelised over the whole `keys × restarts` work space —
reflectors, wheel orders, ring settings, start positions, **and the `-R`
plugboard restarts** — so `-T N` uses N worker threads even when the wheels are
fixed and only the rings/starts are being searched, **and even when the rotor
key is fully specified and you are only recovering the plugboard** (there the
restarts are what get spread across threads). The default is a single thread; on
a 4-core machine a search runs about 3× faster with `-T 4`, and scaling can be
measured with `make bench SCALE=1`. Results are independent of the thread count
(`-T` does not change which plaintext is found).

## References

The hill-climbing strategy is based on the algorithms described in the
[publications by Frode Weierud et al.](http://cryptocellar.org/Enigma/)

The authentic-message material in `eval/` — ciphertexts, recovered day keys,
message keys and indicators, and the scanned message forms — comes from **Geoff
Sullivan and Frode Weierud's *Breaking German Wehrmacht Ciphers* project**:

> **<https://cryptocellar.org/bgac/index.html>**

Specifically: *Breaking German Army Ciphers* (Cryptologia 29(3):193–232, 2005)
and its Appendix C statistics; the July 1941 message and key pages; the
June–October 1941 key page; the Ultimate Enigma Challenge page; and Olaf Ostwald
and Frode Weierud, *Modern breaking of Enigma ciphertexts* (Cryptologia
41(5):395–421, 2017). The `wehrmacht` scoring language is built from the
Appendix C figures (see "n-gram data files" above). The material is published by
its authors under CC BY-NC-SA; it is included here for research and is credited
to them, not covered by this program's GPL.

The software is available under the GNU GPL version 3 license. Copyright (C)
2017–2026 Torbjørn Rognes.
