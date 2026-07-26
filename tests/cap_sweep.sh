#!/bin/sh
# Sweep the staged plugboard pre-pass plug-pair cap (-L) across pre-pass models and
# ciphertext lengths, emitting a tidy CSV (model,cap,length,mean,exact) on stdout.
# Reproduces the data in CODE_REVIEW.md Section 9 (and tests/cap_sweep.csv).
#
#   sh tests/cap_sweep.sh > tests/cap_sweep.csv
#
# R=1 (a single staged climb from identity) so the pair cap cleanly means "the
# pre-pass may set at most N plug pairs". Uses tests/crack_quality.py.
set -u
cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)" || exit 1

TRIALS=${TRIALS:-100}
LENGTHS=${LENGTHS:-"40 70 100 140 190"}
echo "model,cap,length,mean,exact"

# baseline: no staging
CRACKOPTS="-R 1" TRIALS=$TRIALS LENGTHS="$LENGTHS" python3 tests/crack_quality.py 2>/dev/null \
  | awk '/^ *[0-9]+ /{print "none,0,"$1","$2","$3}'

for model in i m b; do
  cap=1
  while [ "$cap" -le 13 ]; do
    CRACKOPTS="-R 1 -S $model -L $cap" TRIALS=$TRIALS LENGTHS="$LENGTHS" \
      python3 tests/crack_quality.py 2>/dev/null \
      | awk -v m="$model" -v c="$cap" '/^ *[0-9]+ /{print m","c","$1","$2","$3}'
    cap=$((cap + 1))
  done
done
