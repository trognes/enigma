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
#   * hillclimb -- the plugboard optimisation loop (`-c`). A short, deliberately
#                  hard ciphertext (many plugboard pairs) is recovered while
#                  wildcarding the start position, so each of the start
#                  positions runs a full hill-climb. Per key the climb costs
#                  ~400x the bare scan, so this is ~99.7% hill-climb.
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

set -u

cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)" || exit 1

HEAD_BIN=./enigma
if [ ! -x "$HEAD_BIN" ]; then
  echo "error: $HEAD_BIN not built; run 'make' first" >&2
  exit 1
fi

LONG=${LONG:-0}
SCALE=${SCALE:-0}
THRESHOLD=${THRESHOLD:-10}
QUICK_REPS=3
LONG_REPS=2

regressed=0

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

# trunc LEN -> first LEN characters of the English benchmark plaintext (shares
# the passage used by the cracking tests in run_tests.sh).
PT="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTEN"
trunc() { printf '%s' "$PT" | cut -c1-"$1"; }

# encrypt PLAIN PLUGBOARD -> ciphertext at the fixed reference settings.
encrypt() { printf '%s' "$1" | "$HEAD_BIN" -i -u B -w 123 -r AAA -g AAA -s "$2" 2>/dev/null; }

# min_time BIN REPS WARMUP CT ARGS... -> minimum wall-clock seconds over REPS
# runs (after WARMUP discarded runs), feeding CT on stdin.
min_time() {
  _bin=$1; _reps=$2; _warm=$3; _ct=$4; shift 4
  _w=0
  while [ "$_w" -lt "$_warm" ]; do
    printf '%s' "$_ct" | "$_bin" "$@" >/dev/null 2>&1
    _w=$((_w + 1))
  done
  _min=""
  _i=0
  while [ "$_i" -lt "$_reps" ]; do
    _t0=$(date +%s.%N)
    printf '%s' "$_ct" | "$_bin" "$@" >/dev/null 2>&1
    _t1=$(date +%s.%N)
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
  _out=$(printf '%s' "$_ct" | "$_bin" "$@" 2>/dev/null)
  [ "$_out" = "$_exp" ] && echo ok || echo MISS
}

# bench NAME TIER WORK UNIT EXPECT CT ARGS... -> run one benchmark and print a
# row (HEAD only, or a base/head A/B row when BASE is set). EXPECT is "-" to skip
# the solve check.
bench() {
  _name=$1; _tier=$2; _work=$3; _unit=$4; _exp=$5; _ct=$6; shift 6
  if [ "$_tier" = quick ]; then _reps=$QUICK_REPS; _warm=1; else _reps=$LONG_REPS; _warm=0; fi

  _ht=$(min_time "$HEAD_BIN" "$_reps" "$_warm" "$_ct" "$@")
  _sol=""
  [ "$_exp" != "-" ] && _sol=$(solved "$HEAD_BIN" "$_ct" "$_exp" "$@")

  if [ -n "$BASE_BIN" ]; then
    _bt=$(min_time "$BASE_BIN" "$_reps" "$_warm" "$_ct" "$@")
    _delta=$(awk -v b="$_bt" -v h="$_ht" 'BEGIN { printf "%+.1f", (h - b) / b * 100 }')
    _flag=""
    if awk -v b="$_bt" -v h="$_ht" -v t="$THRESHOLD" 'BEGIN { exit !((h - b) / b * 100 > t) }'; then
      _flag="  REGRESSION"
      regressed=1
    fi
    printf '%-10s %-5s base %8.2fs  head %8.2fs  %7s%%%s\n' \
      "$_name" "$_tier" "$_bt" "$_ht" "$_delta" "$_flag"
  else
    _rate=$(awk -v w="$_work" -v t="$_ht" 'BEGIN { printf "%.0f", w / t }')
    printf '%-10s %-5s %8.2fs  %10s %-8s %s\n' \
      "$_name" "$_tier" "$_ht" "$_rate" "$_unit/s" "$_sol"
  fi
}

if [ -n "$BASE_BIN" ]; then
  echo "A/B: BASE=$BASE vs working tree (regression threshold ${THRESHOLD}%)"
else
  echo "benchmark (working-tree binary)"
fi
printf 'LONG=%s  SCALE=%s  quick reps=%s  long reps=%s\n\n' \
  "$LONG" "$SCALE" "$QUICK_REPS" "$LONG_REPS"

# --- search: brute-force scan, no plugboard (wildcard wheels + start) ---------
ct_s=$(encrypt "$(trunc 80)" "")
# 3-permutations of wheels 1..3 (-x 3) x 26^3 start positions = 105456 keys
bench search quick 105456 keys - "$ct_s" -q -u B -w ... -g ... -x 3 -l english

# --- hillclimb: recover a 6-pair plugboard, wildcard start (-c) ---------------
pb="AB CD EF GH IJ KL"
pt_h=$(trunc 80)
ct_h=$(encrypt "$pt_h" "$pb")
# 26^2 start positions (first letter fixed to the true A), each a full climb
bench hillclimb quick 676 climbs "$pt_h" "$ct_h" -q -c -u B -w 123 -r AAA -g A.. -l english

if [ "$LONG" = 1 ]; then
  ct_sl=$(encrypt "$(trunc 120)" "")
  # 3-permutations of wheels 1..5 (-x 5) x 26^3 = 1054560 keys
  bench search long 1054560 keys - "$ct_sl" -q -u B -w ... -g ... -x 5 -l english

  pt_hl=$(trunc 160)
  ct_hl=$(encrypt "$pt_hl" "$pb")
  # full 26^3 start positions, each a full climb
  bench hillclimb long 17576 climbs "$pt_hl" "$ct_hl" -q -c -u B -w 123 -r AAA -g ... -l english
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
if [ "$regressed" -eq 1 ]; then
  echo "RESULT: regression detected (> ${THRESHOLD}% slower than BASE)"
  exit 1
fi
[ -n "$BASE_BIN" ] && echo "RESULT: no regression > ${THRESHOLD}%"
exit 0
