# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/): the major number changes when
existing command lines can behave differently or stop working.

## Unreleased

### Added

- **`--self-crib-seeds K` / `--self-crib-length L` / `--self-crib-signature` —
  self-crib seeding, the first lever measured to beat `-R` at matched compute.**
  A doubled word is a *self*-crib: it says only that two positions carry the
  same plaintext letter, which cancels out of `p = steck[core[steck[c]]]` and
  leaves `steck[c_j] = core_j[core_i[steck[c_i]]]` — computable from the rotor
  key alone, with no known plaintext anywhere in it.

  As a filter that is worthless (0 of 160 wrong keys rejected). As a seeder it
  is decisive. Per key the tool deduces every board the rule allows under all 26
  guesses for `steck[X]`, ranks the survivors by the **index of coincidence** of
  their decrypt, and climbs the top `K` with those plugs pinned. IC is the
  ranking because it measured as good as the fused model (150/200 against
  144/200 top-1) and needs no language or n-gram table.

  **The default hypothesises the doubled word anywhere in the message**, and
  `--self-crib-signature` narrows it to one closing the message — a signed
  surname. That is the same shape as `--crib` (sweeps every alignment) and
  `--crib-at` (pins one): the default assumes nothing, the flag adds knowledge.
  Restricting is ~15× cheaper — 20 hypotheses against ~2 200 — but only wins
  when the assumption holds. Over every corpus message carrying a doubling
  anywhere, restricting to the signature breaks **16/40** against the default's
  **26/40**, with a bare `-R 16` at 19/40.

  Measured on 676-key sweeps with a 10-pair board hidden. Against the baseline,
  `K = 10`:

  | arm | mean % | exact | per key |
  |---|---:|---:|---:|
  | `-R 1` | 17.9 | 4/32 | 334 µs |
  | `-R 16` | 48.9 | 13/32 | 3 345 µs |
  | **`--self-crib-seeds 10`** | **81.1** | **23/32** | 1 650 µs |

  — ten more messages than `-R 16` in half the wall time. `K` = 5/10/20/35/50
  breaks 21/23/23/23/24 of 32: steep to 10, flat through 35, one more at 50 for
  +120% time. **Use `K = 10`.** That plateau is the tell — raising `K` lifts the
  best-of-`K` score of the *wrong* keys too, so extra recall converts into
  discrimination only slowly.

  **`--self-crib-length` defaults to 6**, the knee of the cost curve. Over all
  20 corpus messages carrying a 4+ doubling, the floor trades cost against
  breaks monotonically — L=9 → 19/40 at 1 053 µs/key, L=7 → 24/40 at 1 588,
  **L=6 → 26/40 at 2 438**, L=5 → 27/40 at 4 224, L=4 → 28/40 at 7 573. Six sits
  just under `-R 16`'s own cost while breaking 7 more; 4 is there for maximum
  coverage when the compute is free.

  `score_iter` is the wrong axis for these flags and says the opposite of the
  truth: a swept `K=1` run scores *fewer* plugboards than a signature-
  restricted one while taking 15× the wall time, because its seeds are more
  constrained so the climbs are cheap and the uncounted deduction dominates.
  Judge on wall time.

  Rejected with `--crib`/`--crib-list`, `--exhaust`, `-A`, `--soft-plug`, `-F`
  and `--tune-phase`. `-T`-deterministic.

- **`--soft-plug AB…` — plugboard pairs that are a GUESS, not knowledge.** Same
  shape as `-s` and the opposite contract: the pairs are laid on the board each
  restart starts from and then left free, so the climb may move, merge or remove
  them. `-s` marks its letters in `plug_fixed[]` and forbids every move that
  would touch them; `--soft-plug` marks nothing.

  The reason to have both is that their failure modes are not comparable. A
  wrong `-s` pin cannot be undone by anything downstream — the pins deliberately
  survive `--polish` — so one bad guess poisons the whole run, whereas a wrong
  soft guess costs only the moves the climb spends walking back out of it. That
  is exactly the trade a *deduced* seed needs, one right most of the time but
  not always.

  The `--random` kick needed no change: it draws only from self-steckered
  letters, so a soft-seeded pair is invisible to it and the kick adds pairs
  among the letters the seed left alone. Kick size turns out to matter little —
  across `--random` 10/5/3/2/1 the mean %-correct spans 73.0–74.6 over 300
  trials, and no kick at all (`-R 0`) costs ~2pp for 35% less compute.

  Fatal on an odd number of letters, a repeated letter, a non-letter, no `-c`, a
  letter that `-s` also pins or `--no-plug` also marks, and on
  `--exhaust`/`--crib`/`--crib-list`/`-A`, each of which installs its own
  starting board. `-T`-deterministic.

- **A live progress line for the main sweep** — percentage, key rate and ETA,
  rewriting itself in place:

  ```
  Progress:   50% (5.94M / 11.88M keys) 10.12M/s, 1s left
  ```

  The score lines report how *well* the search is doing and nothing about how
  far it has come — and because each one needs a new best, they thin out to
  nothing exactly when a run is longest, leaving no way to tell a slow sweep
  from a stuck one.

  It updates **about every 5 seconds**. An earlier version redrew on each 1%
  boundary, which is the wrong clock — 1% of the work takes longer the bigger
  the sweep, so the line updated most rarely on exactly the runs that need it:
  measured, one update every 5.8 s over 1.05M keys but 2.5 minutes over 27.4M
  and 21 minutes over 230M. The per-thread accounting block is now regime-aware
  too (64 items under `-c`, 4096 in a scan), since a climbed item costs four
  orders of magnitude more than a scanned one and a fixed block had a thread
  reporting only once every nine seconds.

  No flag: it appears whenever stderr is a terminal, exactly like the `-F`
  pre-filter's existing line, and vanishes when the sweep ends. Redirect stderr
  and it is not emitted at all — not the text and not the carriage return — so
  logs, pipelines and the test suite see byte-for-byte what they saw before. It
  is also suppressed under `--dump-all`, whose rows are the machine-readable
  form and print under a different mutex. A sweep finishing in under half a
  second draws nothing rather than flashing a line up and wiping it.

  Score lines and the progress line share stderr, and the line steps aside for
  them rather than being written over: every score line in the program is
  printed by one function, and every caller of it already holds the best-result
  mutex, so that is where the erase goes.

  Counts are shown in **keys** while the percentage runs over **work items**
  (`keys × restarts`, what the sweep actually distributes) — the two differ by a
  constant factor, so the percentage is the same either way, but reporting items
  would read `8×` high against the `Analysed N rotor combinations` line under
  `-R 8`.

- **`--confidence N`** — answers "is this score better than chance?", which the
  tool could not do before. It samples `N` keys from the resolved key space,
  scores each exactly as the search scored them, and reports the winner's
  distance above that null, the distance the **best of K keys** is expected to
  reach by chance (`μ + σ·√(2 ln K)`), and the margin between them.

  **The margin replaces the `Score` column in the progress lines**, with the
  header renamed to match, so the answer is where you are already looking and a
  saved log still explains itself. Zero is the line that matters: negative means
  the board is no better than luck over the whole sweep. `--dump-all` keeps raw
  scores as the machine-readable form.

  The margin is the number to read. A raw score means nothing on its own: every
  model scores *something* on gibberish, and because a search reports a maximum,
  the bar rises with the keyspace. A bare z-score would not do either — a
  progress line is a running maximum, so z reads 3–5 σ well before anything is
  found. The margin subtracts the chance best of the **whole** key space, which
  also keeps it a constant offset from the score: monotone, so the search order
  is untouched, and independent of thread timing.

  Measured: real English over 17 576 keys gives
  a margin of +17.0σ; the identical sweep on signal-free ciphertext gives +0.5σ;
  and a hidden plugboard searched without `-c` — which cannot recover it — gives
  −0.8σ, correctly reporting a failure.

  Samples are hill-climbed when `-c` is on, because a climbed key is drawn from
  a much higher distribution than a scanned one and calibrating against the
  wrong one would make every run look significant. The Gumbel yardstick was
  checked against 12 signal-free sweeps and matched to within 0.01 for quad and
  fused; the index of coincidence does not follow it (its null is right-skewed),
  and the printed p-value says so under `-i`.

  A second use falls out: the margin ranks the scoring **language** on one
  message. On telegraphic German it measured +15.4σ for `wehrmacht`, +8.6σ for
  `german` and +2.5σ for `english`.

  **Use `N` = 256, and never below 128.** `N` buys precision in the sampled null
  and nothing else, and too small an `N` makes the flag report the very thing it
  exists to rule out: measured over 12 seeds per cell, a signal-free ciphertext
  reports a *positive* margin at `N` ≤ 64 (+1.7σ at 16, +1.2σ at 64), while 128
  never crosses zero and 256 has real headroom. Past 512 the error left is the
  null's departure from Gaussian, not the sample size. The spread follows
  `SE ≈ √((1 + z²/2)/N)`, from which `N` needs no adjustment for keyspace size
  or message length. Calibration is free without `-c` and costs 1.5–1.7 ms per
  sample with it — ~1% of a real run, but single-threaded, so its share grows
  with `-T`.

- **`--tune-phase N`** (0–26, default 0 = off) — hill-climb the middle and right
  wheels' *phase* instead of enumerating it. A wheel's phase is its ring and
  start shifted together, so its offset — and with it the wheel's whole
  contribution to the substitution — stays put and the only thing that moves is
  when its own notch fires. With the flag on, the sweep enumerates offsets alone
  (26³ per wheel order instead of 26⁵) and each work item climbs the plugboard,
  **freezes that board**, scans all 26 × 26 phases, re-climbs at the winner, and
  repeats until neither improves.

  The order matters: a rotor key scored without a plugboard is noise — a
  rotor-only decrypt under a full board is ~95% scrambled — so the board is
  recovered first and only then held fixed. Measured with 10 plugs hidden at
  L=439, the frozen-board score peaks at the true phase in 8/8 trials when the
  starting phase is within 5 of it, and the capture radius grows with length
  (roughly `0.4 × length / 26`), which is what `N` starting phases per wheel are
  for. It is an *approximation* and says so in the echoed settings.

  Needs `-c`, and both `-r` and `-g` must wildcard the middle and rightmost
  positions; rejected with `--ring-stride`, `-F`, `--exhaust`, `--crib` and
  `-A`. Off by default and byte-identical to the previous release when off.

  Measured against the alternative use of the compute — the same wall time spent
  on `-R` restarts over the full ring enumeration, 80 paired trials at 200
  letters: `--tune-phase` **breaks more messages** (63/80 exact against 51/80,
  McNemar p = 0.043) but scores a **lower mean %-correct** (85.5 against 91.0,
  CI spans zero). The failure shapes are opposite and that is the whole
  difference: the exhaustive sweep always has the true rotor key in its
  keyspace, so its misses are plugboard misses that still return 76–98% of the
  letters, while `--tune-phase` can settle on the wrong offset, which nothing
  downstream can repair. It fails less often and worse. Prefer it when only a
  full break is useful; prefer the exhaustive sweep when a partial answer has
  value.

  **Below matched compute it pays outright.** Once both arms saturate the
  matched-wall-time question stops discriminating, so `-R` was swept over the
  same 40 instances at L=450: `-R 8` matches the exhaustive sweep's 38/40 for
  **23.4 s against 171.5 s, 7.3× cheaper**, and saturates there (`-R 16` is an
  identical outcome for double the time); `-R 4` gives up one break for 14.5×.
  At operational lengths the flag is therefore not a trade at all — the same
  result for a seventh of the compute — and the right operating point is a *low*
  restart count, nowhere near the `-R 42` that matched compute forced.

- **`--doubling-report L`** — report every converged climb whose decrypt carries
  a **doubled word** of `L`+ letters around an X — `ENGELMANN X ENGELMANN`,
  telegraphic German's own error correction — printed as the ordinary progress
  line with the preview replaced by the marker, the length and the word:

  ```
  +13.97 B231 AAA QMW AB CD EF                               >> 9 ENGELMANN
  ```

  A **confirmation signal, never a score term**, and that distinction is why it
  exists in this form: it enters no ranking, so a false positive costs a second
  look and cannot promote a wrong key. The score-bonus form of the same
  evidence was swept over 140 genuine 17 576-key sweeps and measured down — a
  post-climb bonus needs a trial where the climb recovered the plaintext and
  the score still lost, and there were **zero**; the climb is steered by the
  same score a bonus would adjust, so scoring failure presents as *search*
  failure first. Reporting has no such dependency: it fires on the key that
  *is* right, whatever the search's high-water mark, so the true key can be
  reported while another board still leads (the settings echo says so).

  Two companion knobs, both documented with the numbers so they cannot be
  turned in ignorance. **`--doubling-z Z`** (default 3) gates the check on the
  raw sigma count over the `--confidence` null — only ~0.56% of keys clear
  z ≥ 3, which is what makes the check free (measured at the noise floor at
  every gate down to 0). Chance reports fall ~16× per extra letter of `L`, so
  a 230 M-key rotor sweep expects ~6 spurious reports at `L = 7` against ~90
  at `L = 6` — **raise `L` before touching the gate**; a true key whose climb
  has recovered the plaintext sits at z = 7–16, nowhere near it.
  **`--doubling-mismatches N`** (default 1) is the positions the two copies may
  differ in: 1 is the channel's error and no more (Enigma has no diffusion, so
  a garble corrupts one letter in one copy), and raising it was measured on
  2 M null texts — `N = 2` multiplies false reports ~49× and finds nothing the
  default misses, matching the corpus, where 18 of 25 real doublings have no
  mismatch, 7 have one and none has two. An indel (`SCUHNACHER` against
  `SCHUHMACHER`) misaligns the copies and is missed by design. The scan is
  capped at 30 letters (the longest real doubling is 13; the cap is what keeps
  the scan O(30·n) instead of O(n²), and `L` above it is refused rather than
  silently searching nothing). Needs `-c` and `--confidence`; `--full-text`
  expands a report like any progress line. Verified against an independent
  Python reference on 4 000 random strings — 0 mismatches.

- **The `--confidence` bar is stated before the sweep, not only after it:**

  ```
  Confidence: margin 0 is z = 6.0, the best of 75198240 keys by chance
  ```

  The progress lines print a *margin*, and a reader watching them had no way to
  convert one back to the raw sigma count — the number every other account of
  a result is quoted in — until the run finished. Same figure the summary
  reports; a test asserts the two agree.

### Changed

- **Restart is now the OUTER dimension of the work space**: the sweep does
  every key at restart 0, then every key at restart 1, and so on, instead of
  finishing each key's `-R` restarts before moving on. Throughput is unchanged
  — the per-key `setup_mapping` reuse the old order bought is under 1%,
  unresolvable above thread jitter, and `make bench BASE=` reads ±1% on every
  tier — but *when* the answer appears moves a great deal: there is no early
  exit, so front-loading is what lets a watcher kill a long sweep early. On
  the measured climb curve (87% at `R = 16`), found by the quarter mark **40%
  against 22%**, by halfway **64% against 44%**. The progress line reports per
  pass (`pass 2/4, 8728 / 17.6k keys`) — dividing items by restarts, as
  before, would read 6% of keys covered at the moment every key had been
  visited once. The rate stays key-visits per second and the ETA runs to the
  end of the whole sweep; with one restart the pass field is omitted and
  nothing changes. Exactly-tied boards can resolve differently (the tie-break
  is on the work index, which now enumerates in a different order) — still
  deterministic, still `-T`-independent, verified across `-T` 1/2/4/8. The
  winning key is reconstructed from the merged index at two sites (`--polish`
  and the `--ring-stride` refinement); both now share one `work_key()` helper,
  and three checks re-encrypt the reported plaintext under the reported key to
  prove the echoed key still matches the plaintext on stdout.

- **The test suite runs 3.6× faster — 232 s → 64 s — with all 437 checks
  intact.** `tests/run_tests.sh` had a single shared start-position fixture
  `-g $rg` (676 keys, 26 under `TEST_QUICK`) used by 48 checks, but only about
  eight of them were *recovery* checks, where a wide sweep is the point because
  the true key has to beat decoys. The rest asserted that two runs **agree**
  (`-T`-independence, `-R 0` equals the default, `-F 0` is off, the seed
  echoes), which 26 keys establishes exactly as well as 676 — and the sanitizer
  job had always run them at 26 via `TEST_QUICK`, so the plain g++/clang job was
  paying 26× for a duplicate of an assertion already covered. A second narrow
  fixture `$rgd` now serves those, and `$rg` is kept for recovery and for `-F`,
  which needs more keys than it keeps.

  Separately, the three `restart-parallel` checks — **56 s, a fifth of the whole
  suite** — did not test their own property. Their comment says "with a
  fully-specified rotor key the search has exactly ONE key, so `-T` can only
  speed things up by spreading the `-R` restarts across threads", yet they
  passed `-g $rg` and swept 676 keys, never exercising the single-key path.
  Pinning `-g AAA` made them both correct and ~500× cheaper.

- **`--tune-phase` is now measured across message length, and the trade it makes
  dissolves by ~450 letters.** It shipped measured at one length (L=200), where
  at matched wall time it broke more messages than an exhaustive ring sweep but
  scored a lower mean %-correct. Re-run at L=300 (80 paired trials) and L=450
  (40), with the budget re-calibrated at each length as that comparison
  requires:

  | L | B mean / exact | A mean / exact | McNemar |
  |---:|---|---|---|
  | 200 | 91.0, 51/80 | 85.5, **63/80** | p = 0.043 |
  | 300 | **98.8**, 65/80 | 94.7, **74/80** | p = 0.049 |
  | 450 | 100.0, 38/40 | 100.0, 39/40 | p = 1.0 |

  The split survives at 300 letters and is gone at 450, where the arms are
  indistinguishable. **It dissolves because the problem stops being hard, not
  because the flag pulls ahead** — `--tune-phase`'s catastrophic misses (a wrong
  offset, under 20% of letters) fall 12/80 → 4/80 → 0/40, which is what the
  capture radius predicts, but the exhaustive arm's reach zero *first*, at
  L=300: past that length the true key is always in its keyspace and a plugboard
  miss still returns ~94% of the letters. So the guidance is unchanged in kind
  and narrowed in scope: choose between them below roughly 400 letters, and
  above that it does not matter which you pick.

  The obvious mechanism explains the rate but not which trials fail. Bucketing
  by the cyclic distance from the true ring to the nearest starting phase — the
  quantity the capture radius is about — leaves the catastrophes spread over
  distances 3–5 at both lengths, with the *worst* bucket recovering 29/32 at
  L=300, so raising `N` is not the fix that reading would suggest.

  The report script now prints the failure-shape table (misses, of which
  catastrophic, and the partial-miss mean) that made this legible; the headline
  means and exact counts alone show a shrinking gap and hide the fact that the
  two arms fail in opposite ways. Raw data `eval/results-tune-phase-L300.jsonl`
  and `-L450.jsonl`, with `.txt` summaries.

- **The unknown-key break rate is now measured**
  (`eval/unknown_key_headroom.py`) — 55% at 17 576 keys and 53% at 230 million,
  for a 167-letter message with a
  10-pair board hidden at `-R 8`. Every other result in this repo measures
  plugboard recovery with the rotor key *given*, so a negative sweep could not
  previously be read as evidence about the message.

  **Keyspace size is nearly irrelevant**: four orders of magnitude of `K` cost
  two points, because the chance bar grows as `√(2 ln K)` (4.42 → 6.21) while
  the true key's z has a median of 11.5. The limits are climb failure at the
  true key and a scoring floor, both independent of `K` — and separating them
  needs a **high** `-R`, since at a single `-R` a failed climb also produces a
  low z and the two are the same trials. Judged at `-R 64`, **95%** of messages
  are intrinsically breakable at L=167; the floor is 5%, and the rest of the
  residual is climb failure.

  **At matched wall time the middle option wins.** Per 24 h: `-r A..` exact
  affords `-R 4` for a 66% break rate, `-r AA.` affords `-R 34` for 65%, and
  `-r A.. --ring-stride 3` affords `-R 12` for **80%** — because the climb curve
  flattens (50/68/79/87/95/100% at R=2…64) before the coverage penalty does.
  Spend on `-R` until it flattens, then buy coverage.

  It avoids sweeping at all: a break needs the climb to work at the true key
  *and* that key's score to clear the bar, and the second is arithmetic once the
  z is known — 3 s per trial against the ~10 h a real 80M-key sweep costs.

- **Two-notch wheels (VI, VII, VIII) now collapse the RIGHT wheel's ring ×
  start by 13** — always on, no flag. Those three notch at `M` and `Z`, exactly
  13 apart, so their notch *set* survives a shift of 13; and since a stepping
  wheel's absolute position is read by nothing but that notch test, shifting its
  ring and start together by 13 gives a byte-identical decode. The search now
  tests one member of each pair.

  Exact and unconditional — unlike the middle wheel's collapse it has no length
  term, so it is worth **2× at 40 letters and 2× at 900 alike**. It composes
  with the middle-wheel collapse, giving **4×** when VI–VIII sit in both
  positions. Applies only when ring2 and start2 are both wildcarded, and is
  **0% under the default `-x 5`**, since none of wheels I–V has two notches — so
  it pays for Kriegsmarine traffic and nothing else.

  The middle wheel needed no work: its collapse derives classes by simulating
  the stepping rather than from a formula, so it had been picking this up all
  along. Reported ring2/start2 may now be either member of a pair, the same
  class-representative contract the other collapses already carry; the decrypt
  is identical either way, and the settings echo names which collapses fired.

- **`--score i4f10` is now the measured pick for telegraphic traffic at
  operational length**, in place of the recommended `m4f10`. The existing advice
  — a monogram pre-pass beats an index-of-coincidence one on telegraphic German
  by 2.2 pp — was measured with **`-a`** as the target, and `-f` differs from
  `-a` precisely by folding IC into the target score, so the recommendation had
  never been checked against the model the tool actually recommends.

  Against `-f` the ordering **reverses**. On authentic HG Nord decrypts at 167
  letters, 2000 paired trials across five independent seeds: `i4f10` beats
  `m4f10` by **2.81 pp** mean %-correct (95% CI [−4.80, −0.82], z = 2.76) and
  **3.1 pp** of exact recovery (72.2% → 75.2%; McNemar p = 0.021 over the 1800
  trials with logged discordants). All five seeds agree, heterogeneity is
  Q = 1.65 on 4 df, and `score_iter` matched within 2% in every run.

  `m4f10` remains the default elsewhere.

  **The target matters about twice as much as the pre-pass, and the two do NOT
  interact.** The full `{m4,i4} × {a,f}` square was measured at L=167, 1000
  paired trials per cell. Fused over weighted is **+6.56 pp** with a mono
  pre-pass and **+5.20 pp** with an IC one — both above the +3.0…+4.4 pp
  recorded for `-f` over `-a` — while the difference between those two target
  effects is +1.35 pp, 95% CI [−1.25, +3.95], **z = 1.02**.

  The IC pre-pass therefore wins under **both** targets at this length,
  confirmed directly for `-a` as well (−6.40 pp, McNemar p = 0.009). So the
  documented "mono beats IC by 2.2 pp on telegraphic" does not reproduce at
  L=167 under either target, and the explanation points back at **message
  length** rather than an interaction between the two knobs. A single L=60 run
  did lean mono under `-f`, consistent with a crossover somewhere between.
  Reproducer: `eval/prepass_ab.py`, with `--arms` for any two schedules.

- **`--confidence N` is echoed in the settings** (with whether its samples are
  climbed), and the sampling shows a live progress line on a TTY. The flag
  changes what the first column *means* — margin, not score, a difference of
  ~20 on the same run — so a saved log has to say so up front rather than leave
  it to be inferred. The progress line matters because under `-c` each sample is
  a whole plugboard climb: at `N` = 1024 that is a couple of seconds before the
  search prints anything. It is erased when sampling finishes, since the echo
  already gave `N` and the summary gives the result.

- **BREAKING: `--crib-file` is now `--crib-rerank`.** It re-ranks *finished*
  boards by known-word content and has nothing to do with the crib deduction;
  beside the new `--crib-list` the two names would have been one letter apart
  for two unrelated features. The old name is not accepted — **the next release
  carrying this needs a major version bump.**

### Fixed

- **`--full-text` wrapped 2 columns short of the progress line.** The
  continuation width dated from a 79-column target; the progress lines were
  later budgeted to land on exactly 80 in every mode (61+19 for 3 wheels,
  64+16 under `-4`, 65+15 and 68+12 with the crib column) and the two were
  never reconciled, so the wrapped text stopped short of the right margin of
  the preview it replaces. The suite's one-sided "stays within 80" check
  passed 78 as happily as 80 — it now compares the widest continuation against
  the progress line from the same run, so the two cannot drift apart again.

- **An oversized raw score shifted every column after it.** The score field is
  8 characters with zero slack — the weighted models bottom out around −14,
  which fills it exactly — and of the two printers sharing it only the margin
  (`--confidence`) had a width guard. Reachable via `ENIGMA_LOGLIN`, which
  scales the quad table: ×10 printed `-142.3724` and broke the 80-column
  budget. The raw branch now falls back to `%.1e` exactly as the margin does,
  with one check per printer.

- **`make bench BASE=<old tag>` reported a tier the base could not run as an
  infinite regression.** Comparing `dev` against `v2.1.0` printed
  `crib +13268.6% REGRESSION` — but `--crib` postdates that tag, so the base
  binary exited immediately on the unknown option, was timed at 0.00 s, and the
  ratio blew up. The row now reads `n/a — base lacks these options` and does not
  count toward the threshold; skipped tiers are counted and reported at the end
  so partial coverage is never silent. A **head** binary that fails is still a
  hard error, since that is a broken benchmark rather than a skippable row.

  Two drift checks were run with it. Against `v2.1.0` (160 commits back)
  `search` is **−60.8%** and the climb tiers flat. Against `46d4999` — the merge
  of PR #151, the first commit holding all four search optimisations, 58 commits
  back — `search` is **+0.2%** and `hillclimb` **+0.7%**, so the win has been
  fully retained, while `fused` (−6.0%) and `crib` (−10.3%) are faster still.
  The second comparison is the better-conditioned one: against the release a 5%
  regression would hide inside a −62% victory, whereas both sides of the
  post-optimisation pair start from the same baseline and the expectation is 0%.

- **`--confidence`'s p-value was optimistic near zero, and said so only under
  `-i`.** It is the Gaussian upper tail, and the statistic it models — the
  maximum over `K` keys — lives at ~4.4 σ, exactly where a central-limit
  approximation is weakest: the score is a sum over positions, so the CLT gives
  the centre of the null quickly and the tail slowly. Measured on **2000
  signal-free ciphertexts** (L=200, K = 17 576, `--confidence 1000`), a margin
  of **+0.54 came up 2.35% of the time against the 0.70%** the p-value implies,
  and at K = 3 163 680 it came up **4.83%** — the rate rises with the keyspace.
  The null's best-of-K sits +0.21 σ above a Gaussian of the same μ/σ, with a
  95th percentile of +0.40 against +0.11 predicted.

  The caveat is now unconditional rather than IC-only (IC keeps an extra clause,
  being worse again), and a run whose margin is under +2 σ — the measured 99th
  percentile of noise — additionally prints *"below +2 sd is not a find"* with
  the measured rate. Nothing changes far out, where a real break reads +15 to
  +17 σ and a factor of three on 1e-98 means nothing, so the note fires only
  where the number actually misleads. Raising `N` does not help: at N=1000 the
  estimation error is ~0.10 σ and nearly all the spread is the genuine
  fluctuation of the best of K. Reproducer:
  `eval/confidence_false_positive.py`.

- **`--confidence`'s summary reported a key count its own chance bar was not
  built from.** The count was passed to the summary separately from the bar, and
  under `--ring-stride` the caller passed the refinement's keys too — so the
  line read "chance best of 1528334 keys is 5.3 sd" with 5.3 computed for
  1 527 084. The bar now reports the `K` it was built from, and the inclusive
  total stays with the `Analysed N rotor combinations` diagnostic where it
  belongs. Worth 0.00015 σ — `√(2 ln K)` barely moves with `K` — so this closes
  a drift the output could not show rather than a visible error. The refinement
  cannot be folded into the bar instead: it has to exist before the sweep, which
  is what keeps the margin a constant offset from the score, and the
  refinement's keys are chosen conditional on the coarse winner rather than
  drawn independently.

- **`--confidence` on a fully-specified rotor key printed a margin of ~1e13 and
  broke the progress-line columns.** With the key given there is exactly one key
  to sample, so every sample scores the same and σ̂ came out as floating-point
  noise (~1e-15) rather than 0 — passing the `sd > 0.0` guard and making the
  margin `score/1e-15`. The 15-digit result overflowed the 8-wide first column
  and pushed every line to 87 characters. The guard is now relative,
  `sd > 1e-9·(|μ| + 1)`, which sits nine orders below any real null (~0.17) and
  six above the noise; a degenerate null now says there is nothing to measure
  against and the run falls back to raw scores. `showconfig` also falls back to
  `%+.1e` — exactly 8 characters — if a margin ever fails to fit, so no
  arithmetic surprise can shift the columns again.

## 2.1.0

36 commits on the tool since 2.0.0. It adds a **fused scoring model** that is
the first scoring gain here not tied to a writing style, cuts the **rotor
keyspace** on identifiability grounds, and adds **`--ring-stride`** for trading
a little accuracy for throughput on the rightmost wheel.

### Added

- **`-f` / `--fused`, fused weighted all-order + index of coincidence**, and the
  new recommended model when the language is known. It takes `-a`'s table
  unchanged and adds `lambda * IC` to the per-symbol score. Measured **+3.0 to
  +4.4 percentage points** over `-a` on english, german *and* wehrmacht — the
  first scoring change in this tool that does not depend on the writing style,
  which is expected, since IC is language-independent. Wall-time neutral. Note
  it is a better *climb* rather than better discrimination: the gain is surface
  reshaping, so it does not lift the scoring-failure floor. Recommended recipe:

  ```sh
  ./enigma -c -S m4f10 -J --polish -f -l <lang> -T <cores> -R <restarts>
  ```

- **`--ring-stride K`** (1–26, default 1 = off) — test only every `K`th ring
  position of the rightmost wheel, then refine every skipped position. Worth
  using at **`K=2` or `K=3`** when throughput matters: on authentic telegraphic
  German they analyse 1.9× and 2.6× fewer keys for about half a percentage point
  to two percentage points of exact recovery. `K` of 5 or more is not
  recommended. Needs both `-r` and `-g` to wildcard the rightmost wheel's
  position, and is rejected together with `-F`/`--exhaust`.

  The refinement is **derived rather than searched**: the skipped positions'
  ring/start settings follow from the coarse winner's stepping schedule, so it
  costs a few hundred keys rather than tens of thousands, and there is no
  keyspace where the stride costs more than it saves. Verified to recover
  everything an exhaustive refinement does, across wheels I–VIII, M4, `K` up to
  26, and messages long enough for the left wheel to step.
- **Five more scoring languages**: `swedish`, `finnish`, `icelandic`, `polish`,
  `spanish`.

### Changed

- **Settings that provably decode identically are no longer enumerated.** Two
  always-on keyspace reductions, both exact:
  - the **leftmost stepping wheel's** ring × start collapses totally (26×), in
    every mode, whenever both are wildcarded;
  - the **middle wheel's** collapses partially (3–5× at short lengths).

  Both are lossless — no key that decodes differently is dropped — and they
  apply to `--ring-stride`'s coarse pass as well.

- **The reported ring and start for those two wheels may differ from previous
  releases, on the same command line.** This is the one change that can surprise
  you, so read it before diffing output against 2.0.0:
  - the leftmost stepping wheel's ring is now always reported as `A`, since only
    its offset from the start position is recoverable at all;
  - the middle wheel's ring/start may be reported as a **class representative**
    rather than the true pair, because class members are indistinguishable from
    ciphertext alone. It is length-dependent: past L≈676 every class is a
    singleton and the true key is reported exactly.

  **The recovered plaintext is byte-identical either way** — an affected run
  decrypts exactly as before. Only the echoed key and the `Analysed N rotor
  combinations` count change. Set `--true-key` to disable the middle-wheel
  collapse.

- **N-gram loading is about twice as fast** (hand-rolled parser; the log value
  evaluated once per entry rather than twice). This is startup cost, not the hot
  path, but it is paid by every invocation.
- The resolved-settings echo reports `--ring-stride` and the middle-wheel
  collapse when they are active.

### Fixed

- **The `wehrmacht` table had an unbounded reweighting and a silent 32-bit
  overflow**, so some counts wrapped instead of saturating.

Bugs found and fixed in `--ring-stride` during its development are not listed:
the option is new in this release, so they were never in a shipped version.
