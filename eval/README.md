# eval/ — persistent per-run evaluation log

`results.tsv` is a committed, append-only benchmark of **short-message plugboard
recovery** — the hard regime this project cares about. Every row is one crack
run, recorded with enough detail to reproduce and to diagnose it. It is filled
by `tests/eval.py` and grows over time; new experiments append, nothing is
rewritten.

Focus of the current campaign: **L=50, 10 plugs, quad final score, English +
German**. The tier is *plugboard-recovery*: the true rotor key is handed to the
search and only the plugboard is hill-climbed (the same cheap tier
`tests/crack_quality.py` uses). This isolates plugboard search from rotor-key
discrimination.

## How it differs from `crack_quality.py`

`crack_quality.py` prints per-length **aggregates** over a fixed, `SEED`-
deterministic trial set — the right tool for an A/B on a code change.
`eval.py` records **one row per individual run** with the plaintext, the full
key, the recovered board and both scores — the right tool for building a durable
corpus of solved/unsolved instances you can slice, re-score, and replay later.

## Running

```sh
make                                   # build ./enigma first
python3 tests/eval.py                  # 40 runs/lang, recommended recipe
EVAL_RUNS=60 python3 tests/eval.py     # more runs
EVAL_OPTS='-S i4q10 -R 10' EVAL_LABEL='i4q10.R10' python3 tests/eval.py
```

Env knobs: `EVAL_LANGS` (default `english german`), `EVAL_RUNS` (40),
`EVAL_LENGTH` (50), `EVAL_PAIRS` (10), `EVAL_OPTS` (climb strategy, default
`-J -S i4q10 -R 10`), `EVAL_LABEL`, `EVAL_THREADS` (1), `EVAL_SOLVER_SEED` (0),
`EVAL_CORPORA` (comma-separated corpus names to restrict to, e.g.
`doenitz1945`; default all), `EVAL_MODEL` (final/ranking model letter, default
`q`; set with a matching `-S` target, e.g. `EVAL_MODEL=t EVAL_OPTS='-J -S i4t10
-R 10'`), `EVAL_OUT`. Problems are drawn from fresh OS entropy, so each
invocation adds new random instances.

## Corpora (`eval/corpora/`)

Each run draws its excerpt from a randomly chosen source passage for the
language (weighted so every valid excerpt across all passages is equally
likely), and records which one in the `corpus` column. Passages live as plain
`<language>_<name>.txt` files in `eval/corpora/`; the loader keeps only A–Z
(uppercased), so **you can add a corpus by dropping a raw text file there** — no
code change. `<name>` becomes the `corpus` value.

Most seeded passages are **authored** (original prose on varied topics), not
fetched literature: this session's egress proxy denies general web hosts
(`www.gutenberg.org`, `cryptocellar.org` → 403), so external corpora could not
be downloaded. Add real ones as files when you have them. German umlauts/eszett
are transliterated `ae/oe/ue/ss` (the project convention).
`eval/corpora/build_corpora.py` holds the human-readable sources and
regenerates the `.txt` files.

Two **genuine** historical Enigma message plaintexts are included (retrieved via
web search, provenance in `build_corpora.py`): `german_doenitz1945` (the May
1945 Doenitz-succession M4 message P1030681 — real WW2 telegraphic German with
`X`/`Y`/`KK` separators and procedure words) and `german_manual1930` (the
canonical 1930 instruction-manual message, ~90 chars). Telegraphic German
scores very differently under the prose-trained tables — use `EVAL_CORPORA` to
test them in isolation.

## Columns

| column | meaning |
|---|---|
| `git_sha` | binary/code version (`-dirty` if the tree differs from HEAD, ignoring the results file itself) |
| `timestamp_utc`, `host` | when/where the run happened |
| `language`, `length`, `num_plugs` | the regime (english/german, length, 10) |
| `corpus` | which source passage the excerpt was drawn from (see `eval/corpora/`) |
| `true_reflector`, `true_rotors`, `true_ring`, `true_grund`, `true_plugs` | the answer key (reflector A/B/C, 3 wheel digits, ring, start, plug pairs) |
| `plaintext` | the true excerpt (A–Z only) |
| `cli_options` | the strategy options used, with `<lang>` filled in — **excludes** the rotor key, which lives in the `true_*` columns |
| `config_label` | short language-independent tag for grouping runs of one strategy |
| `solver_seed` | `-e` restart seed (pinned 0, recorded) |
| `threads` | `-T` (affects wall-time only; `score_iter` is thread-independent) |
| `letters_matched_count`, `letters_matched_pct` | recovered-vs-true letter agreement over the message positions |
| `exact_match` | 1 iff every letter matches |
| `score_iter` | plugboards scored (the compute spent — compare configs at matched `score_iter`, never matched `-R`) |
| `wall_time_ms` | crack wall-time (machine-dependent, unlike `score_iter`) |
| `recovered_plugs` | the plugboard the climb settled on |
| `recovered_plaintext` | the tool's best decrypt |
| `recovered_score` | target-model log-prob of the recovered board (model per `cli_options` / `EVAL_MODEL`) |
| `true_score` | target-model log-prob of the true board (from an oracle decrypt, same model) |

## Two things the data gives you for free

- **Scoring vs search failure**, per row, without re-running: if
  `recovered_score ≥ true_score` but `exact_match = 0`, the truth didn't win →
  **scoring failure**; if `true_score > recovered_score`, the climb never
  reached the truth → **search failure**. (At L50/10-plug most misses are search
  failures; short German shows some scoring failures.)
- **A growing paired benchmark suite.** Because every instance is fully stored,
  a new strategy can be replayed on the *same* problems already in the file for a
  low-variance paired comparison — you never have to pre-commit a seed list.

## Current contents

The log holds **all four languages** (english, german, danish, french), 2800–2840
rows each, 10 plugs. The buggy pre-fix German rows were removed (see below) and
the non-English rows were regenerated under the fold-and-accumulate binary
(`git_sha` `ad4113e`). Configs present: quad greedy (`-J -S i4q10 -R 10`), quad
steepest (`-S i4q10 -R 10`), and a trigram slice (`-J -S i4t10 -R 10`), across
L40–L300. English uses 4 prose corpora, german its 4 prose corpora; danish/french
each use a single built-in passage (less excerpt diversity).

## Key findings so far (10 plugs, `-J -R 10`)

- **A table-loading bug crippled non-English scoring (found via this log, now
  fixed).** `load_counts` stopped reading at the first accented gram, so the
  German quad table loaded only its 29 most frequent entries (4.9%). This
  produced a spurious "German wants bigram" reading — a truncation artifact
  (lower order = less truncated), **now retracted**. The parser was fixed to fold
  accented grams to their A–Z base and accumulate counts (`PERFORMANCE.md` §6.9);
  **model order is not meaningfully language-dependent — use `-q`.**
- **After the fix every language cracks comparably** (quad greedy), all reaching
  100% exact by ~L200. At short lengths the non-English languages are actually
  *easier* than english (L50 mean %-correct: english 15, french 34, german 35,
  danish 33). English is search-bound (0 scoring failures at every length).
- **Accent-folding convention.** The tool folds accents to a single base letter
  (`ä→A`). French matches this (its corpus strips accents to the base); the
  german/danish corpora *expand* (`ä→ae`, `å→aa`), a small mismatch on accented
  words (in practice negligible — german scores among the best). Single-letter
  corpora would make the cross-language comparison fully clean.
- **Genuine telegraphic German** remains a separate §6.6 residual (operational
  `Q`-for-`CH` / `X`-separator orthography), to be requantified under the fix.

## Reproducing a single row

A row is self-contained. Let the `true_*` columns be `U W R G` and `PLUGS`, and
`CT` the ciphertext:

```sh
# 1. reconstruct the ciphertext by enciphering the stored plaintext under the true key
CT=$(printf '%s' "<plaintext>" | ./enigma -i -u U -w W -r R -g G -s "PLUGS")
# 2. re-run the exact crack (cli_options + the true rotor key, held fixed)
printf '%s' "$CT" | ./enigma <cli_options> -u U -w W -r R -g G
```

Check out the row's `git_sha` first for a byte-exact replay; `score_iter` is a
deterministic checksum of `(git_sha, cli_options, solver_seed, instance)`.
