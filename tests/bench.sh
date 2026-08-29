#!/bin/sh
#
# Performance benchmark for the enigma tool.
#
# Run with `make bench`, or directly with `sh tests/bench.sh` from anywhere.
# Requires the `enigma` binary to be built first (the `make bench` target does
# this) and must find the n-gram statistics files, so it runs from the
# repository root.
#
# It isolates the two hot paths separately, because they have very different
# cost profiles and a refactor can regress one without touching the other:
#
#   * search    -- the brute-force scan WITHOUT a plugboard hill-climb
#                  (`precompute` + `setup_mapping` + `decode` + `score`, once
#                  per key). Amplified by wildcarding the wheel order and start
#                  position so the scan dominates process startup / file I/O.
#   * icscan    -- the SAME scan under `-i`, the DEFAULT model. Identical to
#                  the `search` row except for the model, so a delta between
#                  the two is the scorer and nothing else -- the same pairing
#                  `fused` has with `hillclimb`. It exists because `-i` is what
#                  a bare `./enigma < cipher.txt` runs, and nothing else here
#                  touches `ic_score_decode`: `search` and `crib` run `-q`,
#                  `fused` runs `-f`, and under `-c` the low-order models are
#                  served by the histogram/`cooc_col` path rather than by
#                  decoding at all. So the tool's default invocation had no
#                  coverage. It also loads no n-gram table, which makes it the
#                  cleanest tier here: ~6ms of startup against a ~0.8s scan
#                  (99.2%), where `search` carries ~32ms (96.3%).
#   * hillclimb -- the plugboard optimisation loop (`-c`). A short, deliberately
#                  hard ciphertext (many plugboard pairs) is recovered while
#                  wildcarding the start position, so each of the start
#                  positions runs a full hill-climb. Per key the climb costs
#                  ~400x the bare scan, so this is ~99.7% hill-climb.
#   * fused     -- the SAME climb under `-f`, the recommended scoring model.
#                  `hillclimb` runs `-q`, which computes no index of
#                  coincidence, so it cannot see anything in `ngram_ic_decode`
#                  -- and that function is 91.9% of a `-f -c` run. A 10.5% item
#                  inside it (the IC accumulation, PR #152) went unnoticed for
#                  exactly this reason. The two rows differ ONLY in the model,
#                  so they are directly comparable.
#   * crib      -- a crib-driven sweep (`--crib`). Dominated by `crib_try`'s
#                  deduction (63.6% of the run) rather than by scoring, because
#                  a crib rejects most keys before anything is scored. Neither
#                  other tier exercises that code at all.
#
# Each benchmark has a `quick` tier (default, a few seconds) and a `long` tier
# (>=15-30s, opt in with LONG=1) that gives a stronger throughput signal.
#
# Timing is wall-clock via the minimum of several repetitions (min rejects
# scheduling noise far better than the mean); the loop is single-threaded and
# CPU-bound, so on an idle machine wall-clock ~= CPU time. A throughput figure
# (keys/s or climbs/s) is derived from the known, fixed amount of work.
#
# Regression guard -- same-machine A/B:
#
#   make bench BASE=<git-ref>          # quick tiers
#   make bench BASE=<git-ref> LONG=1   # quick + long tiers
#
# builds the binary at <git-ref> in a throwaway git worktree and runs every
# benchmark against both it and the working-tree binary, printing the percentage
# delta. Running both on the same machine back-to-back cancels out hardware /
# runner variance, which is the only reliable way to tell whether a change
# slowed the code down. The script exits non-zero if any benchmark is more than
# THRESHOLD percent (default 10) slower than BASE. (The cipher and the n-gram
# tables are assumed unchanged between the two revisions -- this benchmarks
# code, not data.)
#
# TWO LEVELS. THRESHOLD (default 10) is the REPORTING level: a cell above it is
# marked REGRESSION and the run exits non-zero, which is what a developer wants
# locally. FAIL_OVER (default: the same as THRESHOLD) is the level at which CI
# should actually BLOCK. They are separate because the measurement noise floor
# is large and per-tier -- base-vs-base controls on byte-identical code have
# measured +-4.5% on hillclimb and up to +-10% on the clang hillclimb tier in a
# container (CLAUDE.md) -- so a hard 10% gate across a 4-cell matrix would fail
# clean PRs regularly. CI therefore reports at 10 and fails at FAIL_OVER=25,
# which is 2.5x the worst floor ever recorded here and far below the kind of
# regression this is meant to stop (the +50% crib-sweep one it caught).

set -u

cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)" || exit 1

# Pin the restart RNG so the hill-climb timing is stable and the A/B is fair (the
# default is a fresh random seed each run). Seed 0 matches pre-seed BASE refs too.
export ENIGMA_SEED=0

HEAD_BIN=./enigma
if [ ! -x "$HEAD_BIN" ]; then
  echo "error: $HEAD_BIN not built; run 'make' first" >&2
  exit 1
fi

LONG=${LONG:-0}
SCALE=${SCALE:-0}
THRESHOLD=${THRESHOLD:-10}
FAIL_OVER=${FAIL_OVER:-$THRESHOLD}
QUICK_REPS=3
LONG_REPS=2

regressed=0
hard_regressed=0
skipped=0        # tiers the BASE binary could not run at all
head_failed=0    # the head binary itself failed: a broken benchmark

# --- GitHub Actions job summary (markdown) -----------------------------------
# When running under Actions, GITHUB_STEP_SUMMARY names a file whose markdown is
# rendered on the workflow run page. We mirror each benchmark row into a table
# there so the results are an obvious report, not buried in the log. Empty (and a
# no-op) for local runs, so `make bench` on a workstation is unchanged.
GH_SUMMARY="${GITHUB_STEP_SUMMARY:-}"
sumln() {
  [ -n "$GH_SUMMARY" ] || return 0
  printf '%s\n' "$1" >> "$GH_SUMMARY"
}
sum_header() {
  [ -n "$GH_SUMMARY" ] || return 0
  _mach="${CXX:-g++} / $(uname -m)"
  sumln "## Bench — $_mach"
  sumln ""
  if [ -n "$BASE_BIN" ]; then
    sumln "A/B vs \`$BASE\` · regression threshold ${THRESHOLD}% · LONG=$LONG (min of ${QUICK_REPS}/${LONG_REPS} reps)"
    sumln ""
    sumln "| benchmark | tier | base | head | Δ | |"
    sumln "|:--|:--|--:|--:|--:|:-:|"
  else
    sumln "working-tree timings · LONG=$LONG (min of ${QUICK_REPS}/${LONG_REPS} reps)"
    sumln ""
    sumln "| benchmark | tier | time | throughput | solved |"
    sumln "|:--|:--|--:|--:|:-:|"
  fi
}

# Build the BASE binary in a throwaway worktree, if an A/B was requested.
BASE_BIN=""
if [ -n "${BASE:-}" ]; then
  base_dir=$(mktemp -d) || { echo "error: mktemp failed" >&2; exit 1; }
  # shellcheck disable=SC2064
  trap "git worktree remove --force '$base_dir' >/dev/null 2>&1" EXIT INT TERM
  if ! git worktree add --detach "$base_dir" "$BASE" >/dev/null 2>&1; then
    echo "error: could not create worktree for BASE='$BASE'" >&2
    exit 1
  fi
  # Build BASE with the same compiler as the working-tree binary, so the A/B
  # compares like for like. `make bench CXX=clang++ BASE=...` exports CXX into
  # this script's environment; otherwise fall back to the Makefile default.
  if ! make -C "$base_dir" CXX="${CXX:-g++}" >/dev/null 2>&1; then
    echo "error: could not build BASE='$BASE'" >&2
    exit 1
  fi
  BASE_BIN="$base_dir/enigma"
fi

# Each binary loads its n-gram tables from its own tree: the working-tree binary
# from ./ngrams, and a BASE binary from its worktree (ngrams/ if that ref carries
# the folder, else the worktree root -- so an A/B spanning the data-dir move still
# finds the tables for both refs).
HEAD_DATA="$PWD/ngrams"
BASE_DATA=""
if [ -n "${BASE:-}" ]; then
  if [ -d "$base_dir/ngrams" ]; then BASE_DATA="$base_dir/ngrams"; else BASE_DATA="$base_dir"; fi
fi
data_for() {
  if [ -n "${BASE_BIN:-}" ] && [ "$1" = "$BASE_BIN" ]; then printf '%s' "$BASE_DATA"
  else printf '%s' "$HEAD_DATA"; fi
}

# Read every n-gram table into the page cache before any timing starts, and
# fault in both binaries.
#
# `min_time` wraps the WHOLE invocation, so a cold table read lands inside a
# measured run rather than beside it. The two arms do not share the cost: an
# A/B has two SEPARATE copies on disk (HEAD_DATA and BASE_DATA, ~27 MB each),
# so whichever arm touches a table first pays for it, and the long tiers take
# no warm-up run to absorb it. `icscan` reads no table at all -- ~6ms of
# startup against ~32ms for `search` -- so leaving this to chance would make
# the two scan tiers differ by more than the model they are meant to isolate.
#
# Cheap insurance rather than a proven fix: on a LONG=1 run the quick tiers
# have already touched these files by the time the long tiers start, so this
# only bites when the cache is evicted in between or a subset is run.
warm_cache() {
  for _d in "$HEAD_DATA" "$BASE_DATA"; do
    { [ -n "$_d" ] && [ -d "$_d" ]; } || continue
    cat "$_d"/*.txt >/dev/null 2>&1 || true
  done
  for _b in "$HEAD_BIN" "$BASE_BIN"; do
    { [ -n "$_b" ] && [ -x "$_b" ]; } || continue
    printf 'ABCDEFGHIJ' | "$_b" -i -u B -w 123 -r AAA -g AAA \
      >/dev/null 2>&1 || true
  done
}

# trunc LEN -> first LEN characters of the English benchmark plaintext (shares
# the passage used by the cracking tests in run_tests.sh).
PT="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTEN"
trunc() { printf '%s' "$PT" | cut -c1-"$1"; }

# encrypt PLAIN PLUGBOARD -> ciphertext at the fixed reference settings.
encrypt() { printf '%s' "$1" | "$HEAD_BIN" -i -u B -w 123 -r AAA -g AAA -s "$2" 2>/dev/null; }

# Sub-second wall clock, probed once at startup. Not every date(1) supports %N:
# GNU date does, recent macOS does (verified on Sequoia), but older macOS and
# other BSDs emit a literal "N". awk then parses "1753920000.N" as a whole number
# and the timer silently drops to 1-SECOND resolution on a ~2s benchmark, which
# manufactures confident-looking deltas out of pure quantisation. Probing beats
# guessing by platform, and an unusable clock is a hard error rather than a
# silent fallback to whole seconds.
#   date +%s.%N    -> used directly when it really returns fractional seconds
#   gdate          -> Homebrew coreutils, if date(1) cannot
#   python3        -> works everywhere, but each call pays interpreter startup
#                     (~30ms), which inflates ABSOLUTE times; A/B deltas are
#                     affected far less, since both sides pay it equally
case "$(date +%s.%N 2>/dev/null)" in
  *.[0-9]*) now() { date +%s.%N; } ;;
  *)
    if command -v gdate >/dev/null 2>&1 && [ -n "$(gdate +%N 2>/dev/null)" ]; then
      now() { gdate +%s.%N; }
    elif command -v python3 >/dev/null 2>&1; then
      now() { python3 -c 'import time; print(time.time())'; }
      echo "note: no GNU date; timing via python3, so absolute times include" >&2
      echo "      ~30ms of interpreter startup per sample. For clean numbers:" >&2
      echo "      brew install coreutils" >&2
    else
      echo "error: no sub-second clock available. This platform's date(1) lacks" >&2
      echo "       %N, so timings would have 1-second resolution and every delta" >&2
      echo "       would be quantisation noise. Install GNU coreutils (macOS:" >&2
      echo "       brew install coreutils) or python3." >&2
      exit 1
    fi
    ;;
esac

# min_time BIN REPS WARMUP CT ARGS... -> minimum wall-clock seconds over REPS
# runs (after WARMUP discarded runs), feeding CT on stdin.
min_time() {
  _bin=$1; _reps=$2; _warm=$3; _ct=$4; shift 4
  _data=$(data_for "$_bin")
  _w=0
  while [ "$_w" -lt "$_warm" ]; do
    printf '%s' "$_ct" | ENIGMA_DATA="$_data" "$_bin" "$@" >/dev/null 2>&1
    _w=$((_w + 1))
  done
  _min=""
  _i=0
  while [ "$_i" -lt "$_reps" ]; do
    _t0=$(now)
    printf '%s' "$_ct" | ENIGMA_DATA="$_data" "$_bin" "$@" >/dev/null 2>&1
    _st=$?
    _t1=$(now)
    # A binary that REJECTS these options exits at once, times at ~0.00s, and
    # would then be reported as an infinite regression for a tier that never
    # ran -- which is exactly what a BASE older than a flag does. Comparing dev
    # against v2.1.0 printed "crib +13268.6% REGRESSION" because --crib
    # postdates that tag. Return empty so the caller can say n/a instead.
    if [ "$_st" -ne 0 ]; then
      printf ''
      return 0
    fi
    _dt=$(awk -v a="$_t0" -v b="$_t1" 'BEGIN { printf "%.4f", b - a }')
    if [ -z "$_min" ] || awk -v d="$_dt" -v m="$_min" 'BEGIN { exit !(d < m) }'; then
      _min=$_dt
    fi
    _i=$((_i + 1))
  done
  printf '%s' "$_min"
}

# solved BIN CT EXPECT ARGS... -> "ok" if the recovered plaintext matches.
solved() {
  _bin=$1; _ct=$2; _exp=$3; shift 3
  _out=$(printf '%s' "$_ct" | ENIGMA_DATA="$(data_for "$_bin")" "$_bin" "$@" 2>/dev/null)
  [ "$_out" = "$_exp" ] && echo ok || echo MISS
}

# bench NAME TIER WORK UNIT EXPECT CT ARGS... -> run one benchmark and print a
# row (HEAD only, or a base/head A/B row when BASE is set). EXPECT is "-" to skip
# the solve check.
bench() {
  _name=$1; _tier=$2; _work=$3; _unit=$4; _exp=$5; _ct=$6; shift 6
  if [ "$_tier" = quick ]; then _reps=$QUICK_REPS; _warm=1; else _reps=$LONG_REPS; _warm=0; fi

  _ht=$(min_time "$HEAD_BIN" "$_reps" "$_warm" "$_ct" "$@")
  # The head binary failing is a broken benchmark, not a skippable row.
  if [ -z "$_ht" ]; then
    printf '%-10s %-5s HEAD FAILED -- binary rejected these options or crashed\n' \
      "$_name" "$_tier"
    head_failed=1
    return 0
  fi
  _sol=""
  [ "$_exp" != "-" ] && _sol=$(solved "$HEAD_BIN" "$_ct" "$_exp" "$@")

  if [ -n "$BASE_BIN" ]; then
    _bt=$(min_time "$BASE_BIN" "$_reps" "$_warm" "$_ct" "$@")
    if [ -z "$_bt" ]; then
      # Not a regression: there is nothing to compare against. Counted and
      # reported at the end so partial coverage is never silent.
      skipped=$((skipped + 1))
      printf '%-10s %-5s base %9s  head %8.2fs  %8s  base lacks these options\n' \
        "$_name" "$_tier" "n/a" "$_ht" "--"
      if [ -n "$GH_SUMMARY" ]; then
        sumln "$(awk -v n="$_name" -v ti="$_tier" -v h="$_ht" \
          'BEGIN { printf "| `%s` | %s | n/a | %.2fs | -- | base lacks these options |", n, ti, h }')"
      fi
      return 0
    fi
    _delta=$(awk -v b="$_bt" -v h="$_ht" 'BEGIN { printf "%+.1f", (h - b) / b * 100 }')
    _flag=""
    if awk -v b="$_bt" -v h="$_ht" -v t="$THRESHOLD" 'BEGIN { exit !((h - b) / b * 100 > t) }'; then
      _flag="  REGRESSION"
      regressed=1
      if awk -v b="$_bt" -v h="$_ht" -v t="$FAIL_OVER" \
             'BEGIN { exit !((h - b) / b * 100 > t) }'; then
        _flag="  REGRESSION (over ${FAIL_OVER}%)"
        hard_regressed=1
      fi
    fi
    printf '%-10s %-5s base %8.2fs  head %8.2fs  %7s%%%s\n' \
      "$_name" "$_tier" "$_bt" "$_ht" "$_delta" "$_flag"
    if [ -n "$GH_SUMMARY" ]; then
      _mk="✅"; [ -n "$_flag" ] && _mk="⚠️"
      sumln "$(awk -v n="$_name" -v ti="$_tier" -v b="$_bt" -v h="$_ht" -v d="$_delta" -v e="$_mk" \
        'BEGIN { printf "| `%s` | %s | %.2fs | %.2fs | %s%% | %s |", n, ti, b, h, d, e }')"
    fi
  else
    _rate=$(awk -v w="$_work" -v t="$_ht" 'BEGIN { printf "%.0f", w / t }')
    printf '%-10s %-5s %8.2fs  %10s %-8s %s\n' \
      "$_name" "$_tier" "$_ht" "$_rate" "$_unit/s" "$_sol"
    if [ -n "$GH_SUMMARY" ]; then
      sumln "$(awk -v n="$_name" -v ti="$_tier" -v h="$_ht" -v r="$_rate" -v u="$_unit" -v so="$_sol" \
        'BEGIN { printf "| `%s` | %s | %.2fs | %s %s/s | %s |", n, ti, h, r, u, so }')"
    fi
  fi
}

if [ -n "$BASE_BIN" ]; then
  echo "A/B: BASE=$BASE vs working tree (regression threshold ${THRESHOLD}%)"
else
  echo "benchmark (working-tree binary)"
fi
printf 'LONG=%s  SCALE=%s  quick reps=%s  long reps=%s\n\n' \
  "$LONG" "$SCALE" "$QUICK_REPS" "$LONG_REPS"
sum_header

warm_cache

# --- search: brute-force scan, no plugboard (wildcard wheels + start) ---------
ct_s=$(encrypt "$(trunc 80)" "")
# 3-permutations of wheels 1..3 (-x 3) x 26 ring2 x 26^3 starts = 2741856 keys.
# The ring2 factor is easy to miss: no -r is given, so the default "AA." leaves
# the rightmost ring wildcarded. Verified against the binary's own count.
bench search quick 2741856 keys - "$ct_s" -q -u B -w ... -g ... -x 3 -l english

# --- icscan: the same scan under -i, the default model -----------------------
# Identical to the row above except for the model, so a delta between them is
# the scorer and nothing else. No -l: IC needs no language, and passing one
# would load a table this model never reads.
bench icscan quick 2741856 keys - "$ct_s" -i -u B -w ... -g ... -x 3

# --- hillclimb: recover a 6-pair plugboard, wildcard start (-c) ---------------
pb="AB CD EF GH IJ KL"
pt_h=$(trunc 80)
ct_h=$(encrypt "$pt_h" "$pb")
# 26^2 start positions (first letter fixed to the true A), each a full climb
bench hillclimb quick 676 climbs "$pt_h" "$ct_h" -q -c -u B -w 123 -r AAA -g A.. -l english

# --- fused: the same climb under -f, the recommended model -------------------
# Identical to the row above except for the model, so a delta between them is
# the scorer and nothing else.
bench fused quick 676 climbs "$pt_h" "$ct_h" -f -c -u B -w 123 -r AAA -g A.. -l english

# --- crib: crib-driven sweep, deduction-bound rather than scoring-bound ------
# CRIB is a 12-letter stretch of the benchmark plaintext at a pinned position
# (1-based), so the true key survives and the run still recovers the message.
# -r AAA pins the rings to keep the quick tier at 421824 keys; 96% are rejected
# by arithmetic before being scored, which is the property under test.
CRIB=LANGUAGESTAT
CRIB_AT=19
bench crib quick 421824 keys "$(trunc 80)" "$ct_s" -q -u B -w ... -g ... -x 4 -r AAA \
      -l english --crib "$CRIB" --crib-at "$CRIB_AT"

if [ "$LONG" = 1 ]; then
  ct_sl=$(encrypt "$(trunc 120)" "")
  # 3-permutations of wheels 1..5 (-x 5) x 26 ring2 x 26^3 = 27418560 keys
  bench search long 27418560 keys - "$ct_sl" -q -u B -w ... -g ... -x 5 -l english
  bench icscan long 27418560 keys - "$ct_sl" -i -u B -w ... -g ... -x 5

  pt_hl=$(trunc 160)
  ct_hl=$(encrypt "$pt_hl" "$pb")
  # full 26^3 start positions, each a full climb
  bench hillclimb long 17576 climbs "$pt_hl" "$ct_hl" -q -c -u B -w 123 -r AAA -g ... -l english
  bench fused     long 17576 climbs "$pt_hl" "$ct_hl" -f -c -u B -w 123 -r AAA -g ... -l english

  # -x 4 with the default rings wildcarded: 24 wheel orders x 26 x 26^3 keys
  bench crib long 10967424 keys "$(trunc 120)" "$ct_sl" -q -u B -w ... -g ... -x 4 \
        -l english --crib "$CRIB" --crib-at "$CRIB_AT"
fi

# --- thread scaling (opt in with SCALE=1) ------------------------------------
# Sweep -T over powers of two up to ~2x the core count and report the wall-clock
# speedup over a single thread. Two workloads, to show both parallel axes:
#   * wheel-order: wildcard wheels (many independent wheel-order tasks)
#   * ring/start:  fixed wheels, wildcard rings+starts (ONE wheel order) -- this
#                  is the case the old wheel-order-only scheme left serial.
# Informational only (no pass/fail).
if [ "$SCALE" = 1 ]; then
  # portable online-CPU count: getconf works on Linux and macOS, then fall back
  # to nproc (Linux) / sysctl (macOS/BSD); default to 4 if all fail or the
  # result is not a number
  cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null \
            || nproc 2>/dev/null \
            || sysctl -n hw.ncpu 2>/dev/null \
            || echo 4)
  case "$cores" in
    '' | *[!0-9]*) cores=4 ;;
  esac
  ct_sc=$(encrypt "$(trunc 100)" "")

  sweep() {  # label  args...
    _lbl=$1; shift
    echo
    echo "thread scaling -- $_lbl (${cores} cores):"
    _base=""
    _t=1
    while [ "$_t" -le $((cores * 2)) ] && [ "$_t" -le 256 ]; do
      _s=$(min_time "$HEAD_BIN" 2 0 "$ct_sc" "$@" -T "$_t")
      [ -z "$_base" ] && _base=$_s
      awk -v s="$_s" -v b="$_base" -v th="$_t" \
        'BEGIN { printf "  -T %-3d  %7.2fs   %.2fx\n", th, s, b / s }'
      _t=$((_t * 2))
    done
  }

  # wheel-order axis: 60 wheel orders x 26^3 starts
  sweep "wheel order (-w ... -x5 = 60 tasks x 26^3 starts)" \
    -q -u B -w ... -r AAA -g ... -x 5 -l english
  # ring/start axis: 1 wheel order x 26 rings x 26^3 starts (was serial before)
  sweep "ring/start (-w 123, 26 rings x 26^3 starts, 1 wheel order)" \
    -q -u B -w 123 -r AA. -g ... -l english
fi

echo
sumln ""
# Partial coverage is reported, never silent: a run where the base could not
# execute half the tiers must not read like a clean bill of health.
if [ "$skipped" -gt 0 ]; then
  echo "NOTE: $skipped benchmark(s) not compared -- the BASE binary rejected"
  echo "      their options, so those features postdate \`$BASE\` and there is"
  echo "      nothing to compare against."
  sumln "**ℹ️ $skipped benchmark(s) not compared: the base binary rejected their options (feature postdates \`$BASE\`).**"
fi
if [ "$head_failed" -eq 1 ]; then
  echo "RESULT: the benchmark itself is broken -- the head binary failed to run"
  sumln "**❌ the head binary failed to run at least one benchmark.**"
  exit 1
fi
if [ "$hard_regressed" -eq 1 ]; then
  echo "RESULT: regression detected (> ${FAIL_OVER}% slower than BASE) -- past the noise floor"
  sumln "**❌ regression >${FAIL_OVER}% slower than base on at least one benchmark. That is far past any measured noise floor, so this is a real regression, not scatter.**"
  exit 1
fi
if [ "$regressed" -eq 1 ]; then
  echo "RESULT: regression detected (> ${THRESHOLD}% slower than BASE)"
  sumln "**⚠️ regression detected — >${THRESHOLD}% slower than base on at least one benchmark (advisory: re-check on quiet hardware; the shared runners are bimodal on the climb tier).**"
  [ "$FAIL_OVER" = "$THRESHOLD" ] && exit 1
  echo "  (under the ${FAIL_OVER}% hard limit, so not failing the run -- re-check on quiet hardware)"
  exit 0
fi
if [ -n "$BASE_BIN" ]; then
  echo "RESULT: no regression > ${THRESHOLD}%"
  sumln "**✅ no regression > ${THRESHOLD}% vs base.**"
fi
exit 0
