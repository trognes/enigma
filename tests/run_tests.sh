#!/bin/sh
#
# Test suite for the enigma tool.
#
# Run with `make test`, or directly with `sh tests/run_tests.sh` from anywhere.
# Requires the `enigma` binary to be built first (the Makefile target does this)
# and must be able to find the n-gram statistics files, so it runs from the
# repository root.
#
# The suite combines three kinds of checks:
#   * Known-answer tests (KAT) anchored to the canonical public Enigma vector
#     (Enigma I, reflector B, wheels I-II-III, rings AAA, start AAA, no plugs:
#     "AAAAA..." encrypts to "BDZGO..."). This pins the rotor/reflector wiring
#     and the basic stepping schedule to an external reference.
#   * Property tests exercising reciprocity (Enigma is its own inverse), the
#     plugboard, ring/start offsets, the double-stepping anomaly and the Norway
#     variant via encrypt-then-decrypt round trips.
#   * Behavioural tests for input handling: non-letter filtering and the
#     1024-character input limit (regression guard for the best_plaintext
#     overflow fix).
#   * End-to-end cracking tests: full matrices over every scoring model
#     (IC/mono/bi/tri/quad) x every language (german/english/danish/french), for
#     both brute-force start-position recovery and plugboard hill-climb recovery
#     (long plaintexts + a small plugboard so every combination converges).

set -u

cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)" || exit 1

# Pin the restart RNG seed so the determinism/equivalence checks are stable (the
# binary's default is a fresh random seed each run). Seed 0 also reproduces the
# historical pre-seed behaviour exactly, so the recovery KATs are unchanged. A few
# checks below pass an explicit -e to test the seed option itself.
export ENIGMA_SEED=0

# The end-to-end hill-climb checks are the bulk of the runtime under the sanitizers /
# valgrind, which slow every key-scan by ~10x -- a 676-key wildcard + -c + restarts is
# ~16 s each under ASan, and there are a dozen of them (8+ minutes total). Those tools
# only need the code PATHS exercised for memory safety; recovery/determinism is already
# covered by the fast g++/clang jobs. TEST_QUICK shrinks the recovery wildcard to 26
# keys (still contains the true start, so equality / -T-independence / recovery all
# still hold) -- ~21x faster per check -- cutting the sanitizer suite to seconds.
if [ -n "${TEST_QUICK:-}" ]; then rg="AA."; else rg="A.."; fi

ENIGMA=./enigma
if [ ! -x "$ENIGMA" ]; then
  echo "error: $ENIGMA not built; run 'make' first" >&2
  exit 1
fi

pass=0
fail=0

# repeat COUNT CHAR -> prints CHAR COUNT times
rep() { head -c "$1" < /dev/zero | tr '\0' "$2"; }

# run TEXT OPTS... -> feed TEXT on stdin, print stdout (diagnostics discarded)
run() { _t=$1; shift; printf '%s' "$_t" | "$ENIGMA" "$@" 2>/dev/null; }

# check NAME ACTUAL EXPECTED
check() {
  if [ "$2" = "$3" ]; then
    pass=$((pass + 1))
    printf 'ok   %s\n' "$1"
  else
    fail=$((fail + 1))
    printf 'FAIL %s\n       expected: %s\n       actual:   %s\n' "$1" "$3" "$2"
  fi
}

# roundtrip NAME PLAINTEXT OPTS... -> encrypt then decrypt with same settings,
# expect to get PLAINTEXT back (reciprocity).
roundtrip() {
  _name=$1; _plain=$2; shift 2
  _ct=$(run "$_plain" "$@")
  _pt=$(run "$_ct" "$@")
  check "$_name" "$_pt" "$_plain"
}

echo "== Known-answer tests =="

# Canonical Enigma I vector (reflector B, wheels I II III, rings/start AAA).
check "KAT: 25x A -> BDZGO..." \
  "$(run "$(rep 25 A)" -i -u B -w 123 -r AAA -g AAA)" \
  "BDZGOWCXLTKSBTMCDLPBMUQOF"

# Reciprocity of the canonical vector: decrypting the ciphertext returns A's.
check "KAT: BDZGO... -> 25x A" \
  "$(run "BDZGOWCXLTKSBTMCDLPBMUQOF" -i -u B -w 123 -r AAA -g AAA)" \
  "$(rep 25 A)"

# Longer golden vector (prefix is externally anchored above; the full string is
# a regression guard against any change to wiring/stepping after 25 chars).
check "golden: 50x A" \
  "$(run "$(rep 50 A)" -i -u B -w 123 -r AAA -g AAA)" \
  "BDZGOWCXLTKSBTMCDLPBMUQOFXYHCXTGYJFLINHNXSHIUNTHEO"

# Double-stepping anomaly, anchored to the documented rotor-position sequence
# for wheel order III II I:  KDO KDP KDQ KER LFS LFT LFU.  The middle rotor (II,
# notch E) turns at KDQ->KER because the right rotor (I) is at its notch Q, and
# then turns AGAIN at KER->LFS because it is now itself at its notch E, dragging
# the left rotor with it -- the double step.  Starting at KDO, encrypting 12
# letters crosses that double step at the 4th letter, so a machine that omits
# the anomaly produces a different output from the 4th letter on (verified:
# ULMHJCJJCWBY with the double step, ULMIBOYXWRWN without).
check "KAT: double-stepping anomaly (III II I, start KDO)" \
  "$(run "AAAAAAAAAAAA" -i -u B -w 321 -r AAA -g KDO)" \
  "ULMHJCJJCWBY"

# Authentic message from the 1930 Enigma instruction manual: reflector A, wheel
# order II I III, ring settings XMV (24 13 22), plugboard AM FI NV PS TU WZ,
# start position ABL.  A full external known-answer vector that exercises
# reflector A, the ring offsets and the plugboard together (the German plaintext
# uses Q for "ch" and X as a separator).
check "KAT: 1930 instruction-manual message" \
  "$(run 'GCDSEAHUGWTQGRKVLFGXUCALXVYMIGMMNMFDXTGNVHVRMMEVOUYFZSLRHDRRXFJWCFHUHMUNZEFRDISIKBGPMYVXUZ' -i -u A -w 213 -r XMV -g ABL -s 'AM FI NV PS TU WZ')" \
  "FEINDLIQEINFANTERIEKOLONNEBEOBAQTETXANFANGSUEDAUSGANGBAERWALDEXENDEDREIKMOSTWAERTSNEUSTADT"

echo "== Scoring-language requirement =="

# A fully specified machine with no -c just enciphers its input -- nothing is
# ranked or hill-climbed -- so no scoring model/language is required: encryption
# works with NO -i/-l/-q at all, and matches the explicit -i result.
check "encrypt needs no -l/-i (fixed machine)" \
  "$(run "$(rep 25 A)" -u B -w 123 -r AAA -g AAA)" \
  "BDZGOWCXLTKSBTMCDLPBMUQOF"
roundtrip "round-trip with no scoring flags" \
  "ATTACKATDAWNFROMTHENORTH" -u A -w 531 -r MNB -g VCX -s "AB CD"
roundtrip "M4 round-trip with no scoring flags" \
  "WETTERBERICHT" -4 -u b -w B123 -r AAAA -g AAAA
# The default scoring model is the index of coincidence, which needs no language, so
# a wildcard search or a plugboard hill-climb with NO scoring flags now runs (scoring
# by IC) rather than being rejected.
printf 'ABCDE' | "$ENIGMA" -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "wildcard search with no scoring flags runs (default IC)" "$?" "0"
printf 'ABCDE' | "$ENIGMA" -c -u B -w 123 -r AAA -g AAA >/dev/null 2>&1
check "hill-climb with no scoring flags runs (default IC)" "$?" "0"
# But an EXPLICIT n-gram model without -l is still rejected (also checked below).
printf 'ABCDE' | "$ENIGMA" -q -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "wildcard search with -q but no -l rejected" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -q -c -u B -w 123 -r AAA -g AAA >/dev/null 2>&1
check "hill-climb with -q but no -l rejected" "$?" "1"

# A fully specified machine still scores its single decrypt for the diagnostic line,
# and must honour the requested scoring model when its prerequisites are met (an
# n-gram model needs -l) -- it used to always fall back to IC, ignoring -q/-m/etc.
q_echo=$(printf 'BDZGOWCXLT' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
case "$q_echo" in
  *quadgrams*) check "fixed machine honours -q scoring model" "ok" "ok" ;;
  *)           check "fixed machine honours -q scoring model" "$q_echo" "*quadgrams*" ;;
esac
m_echo=$(printf 'BDZGOWCXLT' | "$ENIGMA" -m -l english -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
case "$m_echo" in
  *monograms*) check "fixed machine honours -m scoring model" "ok" "ok" ;;
  *)           check "fixed machine honours -m scoring model" "$m_echo" "*monograms*" ;;
esac
# But a bare fixed decrypt (no scoring opts: default quad, no -l) still falls back to
# IC so it needs no -l -- the fallback only kicks in when the n-gram model lacks -l.
bare_echo=$(printf 'BDZGOWCXLT' | "$ENIGMA" -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
case "$bare_echo" in
  *"index of coincidence"*) check "bare fixed decrypt falls back to IC (no -l needed)" "ok" "ok" ;;
  *)                        check "bare fixed decrypt falls back to IC (no -l needed)" "$bare_echo" "*index of coincidence*" ;;
esac

echo "== Round-trip property tests =="

roundtrip "reciprocity: plain settings" \
  "THEQUICKBROWNFOXJUMPSOVERTHELAZYDOG" -i -u B -w 123 -r AAA -g AAA

roundtrip "plugboard" \
  "THEQUICKBROWNFOXJUMPS" -i -u C -w 245 -r BCD -g XYZ -s "AB CD QZ"

roundtrip "ring and start offsets" \
  "ATTACKATDAWNFROMTHENORTH" -i -u A -w 531 -r MNB -g VCX

# 130 chars also crosses a (natural) double step; complements the KAT above by
# checking reciprocity over a long message.
roundtrip "double step (long round-trip)" \
  "$(rep 130 G)" -i -u B -w 123 -r AAA -g ADV

roundtrip "Norway Enigma" \
  "DETTEERENHEMMELIGMELDING" -i -n -u N -w 123 -r AAA -g AAA

echo "== M4 (4-rotor naval) =="

# Backward-compatibility KAT (externally anchored): the M4 thin reflector b with
# the Greek wheel Beta at ring/position A is wiring-equivalent to the standard
# 3-rotor reflector B; likewise thin c + Gamma at A == reflector C. This ties the
# Greek-folding effective-reflector composition to the standard reflector path,
# which is itself pinned by the BDZGO KAT above (so the check is not circular).
m4kat="THEQUICKBROWNFOXJUMPSOVERTHELAZYDOGENIGMA"
check "M4: thin b + Beta@A == reflector B" \
  "$(run "$m4kat" -i -4 -u b -w B142 -r AAAA -g AAAA)" \
  "$(run "$m4kat" -i -u B -w 142 -r AAA -g AAA)"
check "M4: thin c + Gamma@A == reflector C" \
  "$(run "$m4kat" -i -4 -u c -w G142 -r AAAA -g AAAA)" \
  "$(run "$m4kat" -i -u C -w 142 -r AAA -g AAA)"

# Reciprocity with a non-zero Greek offset and a plugboard.
roundtrip "M4: round-trip (Greek offset + plugboard)" \
  "ENIGMAMFOURNAVALCIPHERMACHINETEST" -i -4 -u c -w G317 -r AQXP -g BMNL -s "AB CD"

# Distinct Greek positions and Greek wheels must produce distinct ciphertext
# (guards against the Greek offset / wheel silently not being applied).
m4a=$(run "$m4kat" -i -4 -u b -w B317 -r AAAA -g AQXP)
m4b=$(run "$m4kat" -i -4 -u b -w B317 -r AAAA -g KQXP)
check "M4: Greek position changes output" "$([ "$m4a" != "$m4b" ] && echo differ)" "differ"
m4g=$(run "$m4kat" -i -4 -u b -w G317 -r AAAA -g AQXP)
check "M4: Greek wheel changes output" "$([ "$m4a" != "$m4g" ] && echo differ)" "differ"

# End-to-end: encrypt at a known Greek position (K), recover it by wildcarding the
# Greek position (quad scoring); must return the exact plaintext.
m4pt="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCE"
m4ct=$(run "$m4pt" -i -4 -u b -w B317 -r AAAA -g KQXP)
check "M4: crack the Greek position" \
  "$(run "$m4ct" -4 -u b -w B317 -r AAAA -g .QXP -l english)" \
  "$m4pt"

# M4 -T determinism: a wildcard (thin x Greek x Greek-position) search must give
# the same result with 1 and 4 threads.
check "M4: -T1 == -T4" \
  "$(run "$m4ct" -4 -u . -w .317 -r AAAA -g .QXP -l english -T 1)" \
  "$(run "$m4ct" -4 -u . -w .317 -r AAAA -g .QXP -l english -T 4)"

# Validation: M4 requires 4-character -w; a 3-character -w is rejected.
printf 'ABCDEFGHIJ' | "$ENIGMA" -i -4 -u b -w 123 -r AAAA -g AAAA >/dev/null 2>&1
check "M4: 3-char -w rejected (exit code)" "$?" "1"
# -n and -4 together is rejected.
printf 'ABCDEFGHIJ' | "$ENIGMA" -i -n -4 >/dev/null 2>&1
check "M4: -n with -4 rejected (exit code)" "$?" "1"

# An over-large precompute (here a full M4 wildcard at -x 4, ~1.1 GB) must fail
# cleanly with exit 1 and a helpful message rather than a std::terminate, when the
# allocator refuses the block. Forced with a tight virtual-memory cap.
#
# This check is pass-or-SKIP, never a hard fail. It is skipped when `ulimit -v` is
# a no-op (e.g. macOS), and -- importantly -- on SANITIZER builds: ASan/TSan/etc.
# reserve a huge virtual address space, so running them under the cap crashes at
# startup (SIGSEGV / core dump) before reaching our handler. We detect an
# instrumented binary by its sanitizer runtime markers and skip WITHOUT invoking
# it under the cap, so no crash is triggered.
sanitized=no
if LC_ALL=C grep -qaE 'libasan|libtsan|libubsan|libmsan|__asan_|__tsan_|AddressSanitizer|ThreadSanitizer' "$ENIGMA" 2>/dev/null; then
  sanitized=yes
fi
# shellcheck disable=SC3045  # ulimit -v is non-POSIX but works where this guard passes (Linux)
if [ "$sanitized" = no ] && [ "$( (ulimit -v 524288 2>/dev/null; ulimit -v) )" = 524288 ]; then
  # shellcheck disable=SC3045
  alloc_err=$( (ulimit -v 524288
    printf 'ABCDEFGHIJKL' | "$ENIGMA" -i -4 -u . -w .... -r AAAA -g .AAA -x 4 2>&1 >/dev/null) )
  alloc_code=$?
  alloc_ok=no
  case "$alloc_err" in
    *"Could not allocate"*) [ "$alloc_code" = 1 ] && alloc_ok=yes ;;
  esac
  if [ "$alloc_ok" = yes ]; then
    check "M4: oversized allocation rejected cleanly (exit 1 + message)" "ok" "ok"
  else
    printf 'skip M4 oversized-allocation test (no clean failure under cap: code=%s)\n' "$alloc_code"
  fi
else
  printf 'skip M4 oversized-allocation test (sanitizer build, or ulimit -v not effective)\n'
fi

echo "== Input handling =="

# Non-letters are stripped and input is upper-cased before encryption.
check "non-letter filtering / case folding" \
  "$(run 'hello, World! 123' -i -u B -w 123 -r AAA -g AAA)" \
  "$(run 'HELLOWORLD' -i -u B -w 123 -r AAA -g AAA)"

# 1024 letters is the maximum and must be accepted.
out1024=$(run "$(rep 1024 A)" -i -u B -w 123 -r AAA -g AAA)
check "1024-letter input accepted (length)" "${#out1024}" "1024"

# Input larger than the read buffer must be consumed across multiple read()
# calls (guards the short-read loop): 70000 spaces then 10 letters, so the
# letters only appear after the first 64 KiB chunk.
check "input larger than read buffer is fully read" \
  "$( { printf '%*s' 70000 ''; printf 'BDZGOWCXLT'; } | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA 2>/dev/null )" \
  "AAAAAAAAAA"

# 1025 letters must be rejected with a non-zero exit (best_plaintext overflow
# regression guard).
err=$(rep 1025 A | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
code=$?
check "1025-letter input rejected (exit code)" "$code" "1"
case "$err" in
  *"too long"*) check "1025-letter input rejected (message)" "ok" "ok" ;;
  *)            check "1025-letter input rejected (message)" "$err" "*too long*" ;;
esac

# Illegal -l language names are rejected before any file is opened (guards the
# fixed-size filename buffer against overflow and path traversal).
err=$(printf 'ABC' | "$ENIGMA" -q -l "../../etc/passwd" -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
code=$?
check "illegal -l rejected (exit code)" "$code" "1"
case "$err" in
  *"Illegal language"*) check "illegal -l rejected (message)" "ok" "ok" ;;
  *)                    check "illegal -l rejected (message)" "$err" "*Illegal language*" ;;
esac

# There is no default language: n-gram scoring (-m/-b/-t/-q) requires -l when
# scoring actually runs, and must fail loudly when it is missing. (A wildcard makes
# this a real search; a fully fixed machine with no -c needs no score -- see the
# "Scoring-language requirement" section above. -i needs no language and is
# exercised throughout the known-answer tests.)
err=$(printf 'ABC' | "$ENIGMA" -q -u B -w 123 -r AAA -g ..A 2>&1 >/dev/null)
code=$?
check "n-gram scoring without -l rejected (exit code)" "$code" "1"
case "$err" in
  *"language is required"*) check "n-gram scoring without -l rejected (message)" "ok" "ok" ;;
  *)                        check "n-gram scoring without -l rejected (message)" "$err" "*language is required*" ;;
esac

# Input with no A-Z letters is rejected rather than running a degenerate,
# empty-ciphertext search (and dividing by zero in the -p comparison).
err=$(printf '12345 .,!?' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
code=$?
check "empty ciphertext rejected (exit code)" "$code" "1"
case "$err" in
  *"empty"*) check "empty ciphertext rejected (message)" "ok" "ok" ;;
  *)         check "empty ciphertext rejected (message)" "$err" "*empty*" ;;
esac

# A wheel cannot be used in two positions: an explicit -w with a repeated digit
# is rejected at validation time (otherwise bruteforce's permutation guard skips
# every combination and the search silently finds nothing).
err=$(printf 'BDZGOWCXLT' | "$ENIGMA" -i -u B -w 112 -r AAA -g AAA 2>&1 >/dev/null)
code=$?
check "duplicate wheels rejected (exit code)" "$code" "1"
case "$err" in
  *"two positions"*) check "duplicate wheels rejected (message)" "ok" "ok" ;;
  *)                 check "duplicate wheels rejected (message)" "$err" "*two positions*" ;;
esac

# Threading: -T must not change the result. Crack the same ciphertext with 1 and
# 4 worker threads (a wildcard-wheel search, so there are several parallel tasks)
# and require identical recovered plaintext.
t_pt="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERS"
t_ct=$(run "$t_pt" -i -u B -w 123 -r AAA -g QXP)
check "threads: -T 4 matches -T 1 (wheel-order search)" \
  "$(run "$t_ct" -q -l english -u B -w ... -r AAA -g QXP -T 4)" \
  "$(run "$t_ct" -q -l english -u B -w ... -r AAA -g QXP -T 1)"

# Fixed wheels + wildcard start: one wheel order, so parallelism comes entirely
# from the ring/start sweep (the case the old scheme left single-threaded).
check "threads: -T 4 matches -T 1 (ring/start search)" \
  "$(run "$t_ct" -q -l english -u B -w 123 -r AAA -g ... -T 4)" \
  "$(run "$t_ct" -q -l english -u B -w 123 -r AAA -g ... -T 1)"

# -T is validated: 0 and 257 (> max 256) are rejected.
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -T 0 >/dev/null 2>&1
check "thread count 0 rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -T 257 >/dev/null 2>&1
check "thread count 257 rejected (exit code)" "$?" "1"

# Plugboard hill-climb random restarts (-R): the per-key RNG is seeded from the
# flat key index, so a restarting search must still be independent of -T. Recover
# a plugboard with a wildcard start (several parallel keys) using -c -R 8.
r_pt="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERS"
r_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA -s "AB CD EF")
check "restarts: -R 8 result is -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -T 4)"
# After random restarts the machine must hold the BEST restart's plugboard, not the
# last one's -- showconfig() prints m.steckerbrett, so decrypting the ciphertext with
# the displayed rotor + -s <plugboard> must reproduce the recovered plaintext. (It
# used to leave the last restart's board, printing a plugboard that did not match.)
pbv_ct=$(run "$r_pt" -i -u B -w 241 -r AAA -g QEW -s "AB CD EF GH IJ KL")
pbv_rec=$(run "$pbv_ct" -q -l english -u B -w 241 -r AAA -g QEW -c -R 20 -S iq)
pbv_err=$(printf '%s' "$pbv_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -R 20 -S iq 2>&1 >/dev/null)
pbv_pb=$(printf '%s\n' "$pbv_err" | grep "W:" | tail -1 | sed -n 's/.*S://p' | grep -oE '[A-Z][A-Z]' | tr '\n' ' ')
check "restart climb: displayed plugboard matches the recovered plaintext" \
  "$(run "$pbv_ct" -u B -w 241 -r AAA -g QEW -s "$pbv_pb")" \
  "$pbv_rec"
# The no-r-token default kick is a fixed 8 pairs (CODE_REVIEW §9): a plain -R run
# must equal an explicit -S r8 run.
check "restarts: default kick == -S r8 (fixed 8 pairs)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -S r8q)"
# A bare r token (no count) takes the default kick, just as a bare model token is
# uncapped: with the model stages held equal, -S riq must equal -S r8iq.
check "restarts: bare r == default kick (-S riq == -S r8iq)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -S riq)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -S r8iq)"
# -R is validated: 0 is rejected, the count may be large (well past the old 100000
# guard), but an absurd value over the 1000000000 cap is still rejected. The accept
# case omits -c so only the validation runs (no actual restart climbs).
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c -R 0 >/dev/null 2>&1
check "restart count 0 rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -R 200000 >/dev/null 2>&1
check "restart count past old 100000 cap accepted (exit code)" "$?" "0"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c -R 1000000001 >/dev/null 2>&1
check "restart count over 1000000000 rejected (exit code)" "$?" "1"

# Random seed (-e / $ENIGMA_SEED): the restart perturbation is seeded from it mixed
# with the key index, so a fixed seed is reproducible and stays -T-independent, an
# explicit -e overrides $ENIGMA_SEED, and the seed is echoed so a run can be repeated.
check "seed: -e 777 is reproducible and -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -e 777 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -e 777 -T 4)"
# Different explicit seeds drive different restart perturbations, so the shown seed
# tracks -e; and -e overrides the pinned $ENIGMA_SEED=0.
seed_echo=$(printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -e 424242 2>&1 >/dev/null)
case "$seed_echo" in
  *"seed: 424242"*) check "seed: -e is echoed (overrides ENIGMA_SEED)" "ok" "ok" ;;
  *)                check "seed: -e is echoed (overrides ENIGMA_SEED)" "$seed_echo" "*seed: 424242*" ;;
esac
# A run with the harness's pinned ENIGMA_SEED=0 equals an explicit -e 0 (same seed).
check "seed: pinned ENIGMA_SEED=0 equals -e 0" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -e 0)"

# Staged plugboard climb (-S schedule: a bigram pre-pass, then the quad target as
# the last token). It must stay -T-independent, and recover a small plugboard on a
# long message (where the bigram pre-pass reliably steers the quad climb to the true
# board). The restart perturbation here is the full-random default (no r token).
s_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA -s "AB CD")
check "staged: -S bq -R 4 result is -T-independent" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c -S bq -R 4 -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c -S bq -R 4 -T 4)"
check "staged: -S bq recovers plugboard (long message)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g AAA -c -S bq)" \
  "$r_pt"
# -S schedule is validated: a non-model/-r letter is rejected.
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c -S z >/dev/null 2>&1
check "staged: bad -S schedule rejected (exit code)" "$?" "1"

# Per-stage plug-pair caps (the number after a model letter) and the per-restart
# random token (r). Both must stay -T-independent.
check "staged: -S r2i3q -R 4 result is -T-independent" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c -S r2i3q -R 4 -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c -S r2i3q -R 4 -T 4)"
# r0 injects no plugs, so restarts are a no-op: -S r0iq -R 8 == -S iq -R 1.
check "staged: -S r0 makes restarts a no-op" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c -S r0iq -R 8)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c -S iq -R 1)"
# Schedule grammar is validated: out-of-range stage cap and r token are rejected.
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c -S q14 >/dev/null 2>&1
check "staged: -S stage cap over max (q14) rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c -S q0 >/dev/null 2>&1
check "staged: -S stage cap 0 (q0) rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c -S r99 >/dev/null 2>&1
check "staged: -S r token over max (r99) rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c -S rir >/dev/null 2>&1
check "staged: -S two r tokens rejected (exit code)" "$?" "1"

# Key pre-filter (-F N): tier 1 ranks every key by a cheap IC climb and keeps the
# top N; tier 2 runs the full -c/-R/-S climb on only those keys. On a long message
# the true rotor key sits comfortably inside a generous top-N, so the pre-filter
# must recover the same plaintext as the full crack, and (like every search path)
# stay independent of -T. Wildcard start positions (-g $rg: 676 keys normally, 26
# under TEST_QUICK) so the filter has a keyspace to rank; F=50 keeps the top ~7%.
f_pt="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSE"
f_ct=$(run "$f_pt" -i -u B -w 123 -r AAA -g AAA -s "AB CD EF")
check "pre-filter: -F 50 recovers plaintext like the full crack" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 50)" \
  "$f_pt"
check "pre-filter: -F 50 result is -T-independent" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 50 -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 50 -T 4)"
# -F is validated: it needs -c, a negative count is rejected, and 0 just means "off"
# (so -F 0 must match a plain run with no -F).
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -F 10 >/dev/null 2>&1
check "pre-filter: -F without -c rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c -F -1 >/dev/null 2>&1
check "pre-filter: -F negative rejected (exit code)" "$?" "1"
check "pre-filter: -F 0 is off (matches no -F)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 0)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq)"

# -F N% keeps the top N% of the resolved keyspace instead of an absolute count. On
# 676 keys, -F 100% keeps every key, so it must equal a plain run with no -F; a
# generous percentage must still recover like the full crack and stay -T-independent.
check "pre-filter: -F 100% keeps all keys (matches no -F)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 100%)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq)"
check "pre-filter: -F 15% recovers plaintext like the full crack" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 15%)" \
  "$f_pt"
check "pre-filter: -F 15% result is -T-independent" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 15% -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -F 15% -T 4)"
# -F percentage is validated: over 100% and (like the count form) without -c reject.
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c -F 150% >/dev/null 2>&1
check "pre-filter: -F over 100% rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -F 8% >/dev/null 2>&1
check "pre-filter: -F N% without -c rejected (exit code)" "$?" "1"

# Simulated annealing (-A): an alternative plugboard optimiser. All randomness comes
# from the per-key RNG stream (seeded from the flat key index), so an SA search must
# stay independent of -T just like the restart climb. Recover the plugboard on the
# same wildcard-start key space.
check "anneal: -A result is -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -A 20000 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -A 20000 -T 4)"
# It must recover the true plugboard on a comfortable (long, fully-specified) message.
sa_rec=$(run "$pbv_ct" -q -l english -u B -w 241 -r AAA -g QEW -c -A 60000)
check "anneal: recovers plaintext on a long message" "$sa_rec" "$r_pt"
# Like the restart climb, after annealing the displayed board must be the one that
# actually produced the recovered plaintext (showconfig prints m.steckerbrett).
sa_err=$(printf '%s' "$pbv_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -A 60000 2>&1 >/dev/null)
sa_pb=$(printf '%s\n' "$sa_err" | grep "W:" | tail -1 | sed -n 's/.*S://p' | grep -oE '[A-Z][A-Z]' | tr '\n' ' ')
check "anneal: displayed plugboard matches the recovered plaintext" \
  "$(run "$pbv_ct" -u B -w 241 -r AAA -g QEW -s "$sa_pb")" \
  "$sa_rec"
# -A is validated: it needs -c, and a non-positive budget is rejected.
printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA -A 20000 >/dev/null 2>&1
check "anneal: -A without -c rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA -c -A -5 >/dev/null 2>&1
check "anneal: negative -A budget rejected (exit code)" "$?" "1"

# SA honours the -S target-stage plug cap (a known-plug-count prior): -A -S qN caps the
# whole trajectory (pre-pass, anneal moves, quench) at N pairs. Still -T-independent.
check "anneal: -A -S q8 result is -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -A 20000 -S q8 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -A 20000 -S q8 -T 4)"
# The cap is actually enforced: with -S q3 the recovered board must hold <= 3 plug pairs.
cap_err=$(printf '%s' "$pbv_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -A 40000 -S q3 2>&1 >/dev/null)
cap_pairs=$(printf '%s\n' "$cap_err" | grep "W:" | tail -1 | sed -n 's/.*S://p' | grep -oE '[A-Z][A-Z]' | grep -c .)
check "anneal: -S q3 caps the board at <= 3 plug pairs" "$([ "${cap_pairs:-0}" -le 3 ] && echo ok)" "ok"
# With the cap matched to the true count, SA still recovers on a comfortable message.
sa8_ct=$(run "$r_pt" -i -u B -w 241 -r AAA -g QEW -s "AB CD EF GH IJ KL MN OP")
check "anneal: -A -S q8 recovers an 8-plug board (long message)" \
  "$(run "$sa8_ct" -q -l english -u B -w 241 -r AAA -g QEW -c -A 60000 -S q8)" \
  "$r_pt"

# Fixed -s plugs are frozen: the climb and SA must never remove or rewire them, not even
# a spurious one. Encrypt with a single real plug (AB), then force an unrelated plug (YZ)
# and confirm YZ survives in the recovered board (a plain seed would drop it).
frz_ct=$(run "$r_pt" -i -u B -w 241 -r AAA -g QEW -s "AB")
frz_climb=$(printf '%s' "$frz_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -R 20 -S iq -s "YZ" 2>&1 >/dev/null | grep "W:" | tail -1 | sed -n 's/.*S://p')
case "$frz_climb" in
  *YZ*) check "fixed -s plug survives the greedy climb" "ok" "ok" ;;
  *)    check "fixed -s plug survives the greedy climb" "$frz_climb" "*YZ*" ;;
esac
frz_sa=$(printf '%s' "$frz_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -A 30000 -s "YZ" 2>&1 >/dev/null | grep "W:" | tail -1 | sed -n 's/.*S://p')
case "$frz_sa" in
  *YZ*) check "fixed -s plug survives simulated annealing" "ok" "ok" ;;
  *)    check "fixed -s plug survives simulated annealing" "$frz_sa" "*YZ*" ;;
esac

# The final diagnostic reports how many rotor combinations were analysed and how many
# plugboards were scored. A pure scan (no -c) scores exactly one plugboard per key...
d_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA -s "AB CD")
d_scan=$(printf '%s' "$d_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AA. 2>&1 >/dev/null)
case "$d_scan" in
  *"Analysed 26 rotor combinations, scored 26 plugboards"*)
    check "diagnostic: scan scores one plugboard per key" "ok" "ok" ;;
  *) check "diagnostic: scan scores one plugboard per key" "$d_scan" \
       "*Analysed 26 rotor combinations, scored 26 plugboards*" ;;
esac
# ...a fully specified decrypt is one combination, one plugboard (singular grammar)...
d_one=$(printf '%s' "$d_ct" | "$ENIGMA" -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
case "$d_one" in
  *"Analysed 1 rotor combination, scored 1 plugboard"*)
    check "diagnostic: fixed decrypt is 1 combination, 1 plugboard" "ok" "ok" ;;
  *) check "diagnostic: fixed decrypt is 1 combination, 1 plugboard" "$d_one" \
       "*Analysed 1 rotor combination, scored 1 plugboard*" ;;
esac
# ...and the plugboard-scored count is the same regardless of thread count (same work).
d_c1=$(printf '%s' "$d_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -T 1 2>&1 >/dev/null | grep -oE 'scored [0-9]+ plugboards')
d_c4=$(printf '%s' "$d_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 -S iq -T 4 2>&1 >/dev/null | grep -oE 'scored [0-9]+ plugboards')
check "diagnostic: plugboard-scored count is -T-independent" "$d_c1" "$d_c4"

# Usage/exit conventions: -h prints help to stdout and exits 0; running with no
# arguments is a usage error (help to stderr, exit 1).
hout=$("$ENIGMA" -h 2>/dev/null); hcode=$?
check "-h exits 0" "$hcode" "0"
case "$hout" in
  *"Usage: enigma"*) check "-h writes help to stdout" "ok" "ok" ;;
  *)                 check "-h writes help to stdout" "$hout" "*Usage: enigma*" ;;
esac
"$ENIGMA" </dev/null >/dev/null 2>&1
check "no arguments exits 1" "$?" "1"

# The settings echo (stderr) prints the plugboard as spaced pairs (AB CD),
# not the internal de-spaced form (ABCD).
pb_echo=$(printf 'BDZGOWCXLT' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -s "AB CD EF" 2>&1 >/dev/null)
case "$pb_echo" in
  *"AB CD EF"*) check "settings echo spaces plugboard pairs" "ok" "ok" ;;
  *)            check "settings echo spaces plugboard pairs" "$pb_echo" "*AB CD EF*" ;;
esac

# The n-gram parser tolerates blank lines and irregular whitespace without
# hanging or erroring (guards the fscanf field-count / leading-space fix).
printf '\n  E 529117365\n\nT 390965105\nA 374061888\n   \n' > zztest_monograms.txt
check "n-gram parser tolerates messy file" \
  "$(run 'BDZGOWCXLT' -m -l zztest -u B -w 123 -r AAA -g AAA)" \
  "AAAAAAAAAA"
rm -f zztest_monograms.txt

# Data directory: the n-gram files can live somewhere other than the current
# directory, selected by -d or $ENIGMA_DATA (default "."). Run from a different
# CWD (/) with an absolute binary so only the resolved data dir can find them.
# A small wildcard search (-g ..A) forces the n-gram table to be loaded from the
# resolved data dir -- a fully fixed machine would not score, so it would not load
# anything. A successful exit means the files were found there.
root=$(pwd)
( cd / && printf 'BDZGOWCXLT' | "$root/enigma" -m -l english -d "$root" -u B -w 123 -r AAA -g ..A >/dev/null 2>&1 )
check "-d finds n-gram files from another CWD (exit code)" "$?" "0"
( cd / && printf 'BDZGOWCXLT' | ENIGMA_DATA="$root" "$root/enigma" -m -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1 )
check "ENIGMA_DATA finds n-gram files from another CWD (exit code)" "$?" "0"
( cd / && printf 'BDZGOWCXLT' | ENIGMA_DATA=/nonexistent "$root/enigma" -m -l english -d "$root" -u B -w 123 -r AAA -g ..A >/dev/null 2>&1 )
check "-d overrides ENIGMA_DATA (exit code)" "$?" "0"

# A missing data directory fails with the full path it tried (and before stdin).
err=$(printf 'ABC' | "$root/enigma" -q -l english -d /nonexistent -u B -w 123 -r AAA -g ..A 2>&1 >/dev/null)
code=$?
check "missing data dir rejected (exit code)" "$code" "1"
case "$err" in
  */nonexistent/english_quadgrams.txt*) check "missing data dir names the path" "ok" "ok" ;;
  *) check "missing data dir names the path" "$err" "*/nonexistent/english_quadgrams.txt*" ;;
esac

echo "== End-to-end cracking =="

# Genuine per-language plaintexts (A-Z only; accents/umlauts transliterated, e.g.
# ae oe ue / aa). Long passages (~450-480 letters) so that even the weakest
# scoring models have enough signal to converge during hill-climbing.
pt_german="DIEENIGMAMASCHINEWURDEIMZWEITENWELTKRIEGVONDERDEUTSCHENWEHRMACHTVERWENDETUMGEHEIMENACHRICHTENZUVERSCHLUESSELNABERDIEALLIIERTENKONNTENDENGEHEIMENCODETROTZDEMBRECHENWEILDIEDEUTSCHENOFTDIEGLEICHENFLOSKELNVERWENDETENUNDWEILVIELEBEDIENERIMMERWIEDERDIESELBENFEHLERMACHTENDIEPOLNISCHENUNDBRITISCHENMATHEMATIKERBAUTENMASCHINENUMDIETAEGLICHENSCHLUESSELZUFINDENUNDLASENSODIEGEHEIMENFUNKSPRUECHEDESFEINDESMITUNDVERKUERZTENDADURCHDENKRIEGUMMEHREREJAHREUNDRETTETENVIELETAUSENDMENSCHENLEBEN"
pt_english="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUREOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDINTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERALYEARSANDSAVEDCOUNTLESSLIVES"
pt_danish="DETVARENGANGENLILLEHAVFRUESOMBOEDELANGTUDEPAAHAVETSBUNDSAMMENMEDSINFADEROGSINEFEMSOESTREHUNVARDENYNGSTEOGSMUKKESTEAFDEMALLEMENHUNLAENGTESEFTERATKOMMEOPTILMENNESKENESVERDENOGSEDENSTORESKIBEOGBYERNEOGSKOVENEHVERTAARBLEVHUNAELDREOGFIKLOVTILATSTIGEOPGENNEMDETKLAREVANDFORATSIDDEPAAKLIPPERNEISKINNETFRAMAANENOGSEUDOVERDENSTOREVIDEVERDENOGNAARSOLENGIKNEDDYKKEDEHUNNEDIGENMENHUNGLEMTEALDRIGDENDEJLIGEVERDENOVENOVERVANDETOGENDAGDAHUNREDDEDEENUNGPRINSFRADRUKNINGFORELSKEDEHUNSIGHAABLOEST"
pt_french="LESSANGLOTSLONGSDESVIOLONSDELAUTOMNEBLESSENTMONCOEURDUNELANGUEURMONOTONETOUTSUFFOCANTETBLEMEQUANDSONNELHEUREJEMESOUVIENSDESJOURSANCIENSETALORSJEPLEUREETJEMENVAISAUVENTMAUVAISQUIMEMPORTEDECADELABCOMMELAFEUILLEMORTEPENDANTLONGTEMPSJEMESUISCOUCHEDEBONNEHEUREETJAIREVEDESPAYSLOINTAINSOULESHOMMESSONTLIBRESETOULAVIEESTDOUCEETBELLECHAQUEMATINJEMEPROMENAISLELONGDELARIVIEREENECOUTANTLECHANTDESOISEAUXETLEMURMUREDELEAUQUICOULAITDOUCEMENTVERSLAMER"
plain_for() {
  case $1 in
    german)  printf '%s' "$pt_german"  ;;
    english) printf '%s' "$pt_english" ;;
    danish)  printf '%s' "$pt_danish"  ;;
    french)  printf '%s' "$pt_french"  ;;
  esac
}

# The end-to-end cracking matrix below is the bulk of the suite's runtime under the
# sanitizers / valgrind, which slow every key-scan by ~10x -- 8+ minutes, almost all
# of it in the 26^3-key brute force x 4 languages here. Those tools only need the code
# PATHS exercised for memory safety, not the full recovery matrix (recovery/determinism
# is covered by the fast g++/clang jobs). TEST_QUICK trims the matrix to one language
# and a small keyspace while still exercising every scoring model and both the scan and
# hill-climb paths, cutting the sanitizer suite from minutes to seconds.
if [ -n "${TEST_QUICK:-}" ]; then
  crack_langs="english"
  crack_scan_g="Q.."          # 676 keys, still contains the true start QXP
else
  crack_langs="german english danish french"
  crack_scan_g="..."          # full 26^3 keyspace
fi

# (1) Brute-force the start position with every scoring model in every language.
# Each plaintext is encrypted at start QXP (no plugboard) and recovered by
# wildcarding the start. All languages x 5 models must recover when -l matches the
# plaintext language. (This also guards the IC formula fix: the old -i formula could
# not distinguish plaintext from gibberish and returned the wrong key. Note quadgrams
# need the matching -l -- they are the most language-specific model; see CLAUDE.md.)
for lang in $crack_langs; do
  plain=$(plain_for "$lang")
  ct=$(run "$plain" -i -u B -w 123 -r AAA -g QXP)
  for mode in -i -m -b -t -q; do
    check "crack: start position, $lang $mode" \
      "$(run "$ct" $mode -u B -w 123 -r AAA -g "$crack_scan_g" -l "$lang")" \
      "$plain"
  done
done

# (2) Hill-climb the plugboard (rotor/ring/start known, plugboard unknown), for
# every scoring model in every language. With long plaintext and a small (2-pair)
# plugboard, even IC and monogram scoring have enough signal to converge to the
# exact plugboard. (Hill-climbing is a greedy heuristic, so it is sensitive to
# text length and plug count -- a larger plugboard or a shorter message can leave
# it in a local optimum, especially for quadgrams; this configuration is chosen
# so that all 4 languages x 5 models recover exactly.)
for lang in $crack_langs; do
  plain=$(plain_for "$lang")
  ct=$(run "$plain" -i -u B -w 123 -r AAA -g AAA -s "AB CD")
  for mode in -i -m -b -t -q; do
    check "crack: hill-climb plugboard, $lang $mode" \
      "$(run "$ct" $mode -c -u B -w 123 -r AAA -g AAA -l "$lang")" \
      "$plain"
  done
done

echo
echo "passed: $pass, failed: $fail"
[ "$fail" -eq 0 ]
