#!/usr/bin/env bash
#
# Pre-pass model sweep for the HIGH-restart regime (PERFORMANCE.md section 6.10).
#
# Runs tests/eval.py over {configs} x {lengths} x {languages} x {seeds} at a fixed
# -R, and merges the per-cell rows into one timestamped shard eval/results-<ts>.tsv
# (the frozen-results.tsv sharding convention). Plugboard-recovery tier (the true
# rotor key is given, only the 10-pair plugboard is hill-climbed), quad ranking,
# prose corpora.
#
# Reproduces the two shards behind section 6.10:
#   grid  (matched R):     CONFIGS="q10 i4q10 m4q10 m6q10"  R=2560
#   matched-COMPUTE probe:  CONFIGS="q10"                    R=4240
#
# The matched-compute probe uses R=4240 for pure quad because q10 costs ~0.59x
# i4q10 per restart (no pre-pass stage; see the grid's score_iter), so q10 @ 4240
# spends the same score_iter as m4q10 @ 2560. Comparing q10@4240 vs m4q10@2560 is
# therefore matched-COMPUTE, not matched-R -- the comparison that actually decides
# whether the mono pre-pass beats pure diversity (it does not, past L~50).
#
# All knobs are env-overridable; defaults reproduce the R=2560 grid.
set -u
cd "$(dirname "$0")/.." || exit 1   # repo root

: "${CONFIGS:=q10 i4q10 m4q10 m6q10}"
: "${LENS:=40 45 50 55 60 65 70}"
: "${LANGS:=english german french danish}"
: "${SEEDS:=0 1 2}"
: "${R:=2560}"
: "${RUNS:=20}"
: "${MAXJOBS:=4}"

declare -A CORP=(
  [english]="builtin,city,mountains,ocean"
  [german]="builtin,wald,reise,wissenschaft"
  [french]="builtin,mer,montagne,ville"
  [danish]="builtin,hav,by,skov,danmark")

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for lang in $LANGS; do
  for L in $LENS; do
    for cfg in $CONFIGS; do
      for S in $SEEDS; do
        EVAL_MODEL=q EVAL_OPTS="-J -S $cfg -R $R" EVAL_LABEL="prepass.$cfg.R$R" \
          EVAL_LANGS="$lang" EVAL_CORPORA="${CORP[$lang]}" EVAL_LENGTH="$L" EVAL_PAIRS=10 \
          EVAL_RUNS="$RUNS" EVAL_THREADS=1 EVAL_SOLVER_SEED="$S" \
          EVAL_OUT="$TMP/${lang}_${L}_${cfg}_s${S}.tsv" \
          python3 tests/eval.py >/dev/null 2>&1 &
        while (( $(jobs -rp | wc -l) >= MAXJOBS )); do wait -n; done
      done
    done
  done
done
wait

TS=$(date -u +%Y%m%d-%H%M%S)
SHARD="eval/results-${TS}.tsv"
first=1
for f in "$TMP"/*.tsv; do
  [ -f "$f" ] || continue
  if [ $first -eq 1 ]; then head -1 "$f" > "$SHARD"; first=0; fi
  tail -n +2 "$f" >> "$SHARD"
done
echo "wrote $SHARD ($(( $(wc -l < "$SHARD") - 1 )) rows)"
