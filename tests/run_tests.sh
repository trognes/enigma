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

# Progress lines are "score W R G S... text": the score first (the only stderr
# lines starting with a decimal number), then fixed columns under a header
# (Score W R G S Text). progress_lines filters them from a stderr capture;
# last_plugboard extracts the plugboard column of the LAST one -- fields 5
# through NF-1 are the plug pairs (1-4 = score/wheels/ring/start, NF = the
# decoded-text preview), printed with a leading space per pair (" AB CD").
progress_re='^ *-{0,1}[0-9][0-9]*\.[0-9]'
progress_lines() { grep -E "$progress_re"; }
last_plugboard() {
  progress_lines | tail -1 | awk '{ for (i = 5; i < NF; i++) printf " %s", $i }'
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

# -J is the only climb-strategy flag; its "needs -c" error must name it.
check "climb-strategy without -c: -J names -J" \
  "$(printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -J 2>&1 >/dev/null | grep -c -- '(-J)')" "1"

# Plugboard hill-climb random restarts (-R): the per-key RNG is seeded from the
# flat key index, so a restarting search must still be independent of -T. Recover
# a plugboard with a wildcard start (several parallel keys) using -c -R 8.
r_pt="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERS"
r_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA -s "AB CD EF")
check "restarts: -R 8 result is -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 -T 4)"

# --crib-file (measured-down opt-in): the crib bonus is a deterministic function of the
# board, so the re-ranked winner must stay -T-independent. Also exercises loading the file.
crib_file=$(mktemp)
printf 'ENGLISH 2\nLANGUAGE 2\nANALYSIS 2\n' > "$crib_file"
check "crib: --crib-file result is -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 --crib-file "$crib_file" -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 --crib-file "$crib_file" -T 4)"
rm -f "$crib_file"

# --dump-all (diagnostic): prints the full setting (rotor key + score + plugboard) of every
# converged key x restart to stderr. The dumped SET (sorted) must be -T-invariant, and each
# line carries the rotor key -- so grep for one specific key and check it appears.
da1=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 --dump-all -T 1 2>&1 >/dev/null | grep '^dumpall' | sort)
da4=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 4 --dump-all -T 4 2>&1 >/dev/null | grep '^dumpall' | sort)
check "dump-all: dumped set is -T-independent" "$da1" "$da4"
check "dump-all: lines carry the rotor key (B123 ...)" \
  "$(printf '%s' "$da1" | grep -c '^dumpall B123 ')" "$(printf '%s' "$da1" | grep -c '^dumpall ')"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA --dump-all >/dev/null 2>&1
check "dump-all without -c rejected (exit code)" "$?" "1"
# After random restarts the machine must hold the BEST restart's plugboard, not the
# last one's -- showconfig() prints m.steckerbrett, so decrypting the ciphertext with
# the displayed rotor + -s <plugboard> must reproduce the recovered plaintext. (It
# used to leave the last restart's board, printing a plugboard that did not match.)
pbv_ct=$(run "$r_pt" -i -u B -w 241 -r AAA -g QEW -s "AB CD EF GH IJ KL")
pbv_rec=$(run "$pbv_ct" -q -l english -u B -w 241 -r AAA -g QEW -c -R 20 -S iq)
pbv_err=$(printf '%s' "$pbv_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -R 20 -S iq 2>&1 >/dev/null)
pbv_pb=$(printf '%s\n' "$pbv_err" | last_plugboard | grep -oE '[A-Z][A-Z]' | tr '\n' ' ')
check "restart climb: displayed plugboard matches the recovered plaintext" \
  "$(run "$pbv_ct" -u B -w 241 -r AAA -g QEW -s "$pbv_pb")" \
  "$pbv_rec"
# Progress lines: the climb echoes EVERY plugboard improvement (score + machine
# settings) as it happens, not just each finished climb -- a single-key -c run used
# to print exactly one progress line (the converged board); now the board builds up
# live.
pg_err=$(printf '%s' "$pbv_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c 2>&1 >/dev/null)
pg_n=$(printf '%s\n' "$pg_err" | progress_lines | grep -c .)
check "progress: climb echoes intermediate plugboard improvements (>1 line)" \
  "$([ "$pg_n" -gt 1 ] && echo ok)" "ok"
# ...and the LAST echoed line is still the winning board: its plugboard reproduces
# the recovered plaintext (display/result consistency at the finer granularity).
pg_pb=$(printf '%s\n' "$pg_err" | last_plugboard | tr -d ' ')
check "progress: last echoed plugboard matches the recovered plaintext" \
  "$(run "$pbv_ct" -u B -w 241 -r AAA -g QEW -s "$pg_pb")" \
  "$(run "$pbv_ct" -q -l english -u B -w 241 -r AAA -g QEW -c)"
# ...and the same must hold when the --polish FINISHER improves the best board
# after all restarts: it used to replace the winner silently, so the last progress line
# showed the PRE-finisher score/wheels/plugboard while stdout held a different decrypt.
gfx_err=$(printf '%s' "$pbv_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -R 3 \
  --random 10 --polish -e 1 2>&1 >/dev/null)
gfx_pb=$(printf '%s\n' "$gfx_err" | last_plugboard | tr -d ' ')
check "progress: last echoed plugboard matches the plaintext (--polish)" \
  "$(run "$pbv_ct" -u B -w 241 -r AAA -g QEW -s "$gfx_pb")" \
  "$(run "$pbv_ct" -q -l english -u B -w 241 -r AAA -g QEW -c -R 3 --random 10 --polish -e 1)"
# Progress line FORMAT: a column header (Score W R G S Text) printed exactly once, each
# line ending in the first preview characters of the decoded text, and the whole line --
# header included -- within 80 columns even with a full 13-pair plugboard. The 4-wheel M4
# key is 3 characters wider than a 3-wheel one, so it uses its own format and a shorter
# preview (16 vs 19); both land on exactly 80 in the worst case.
fmt_pb="AB CD EF GH IJ KL MN OP QR ST UV WX YZ"
fmt_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA -s "$fmt_pb")
fmt_err=$(printf '%s' "$fmt_ct" | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -s "$fmt_pb" 2>&1 >/dev/null)
check "progress: column header printed exactly once" \
  "$(printf '%s\n' "$fmt_err" | grep -c '^ *Score ')" "1"
check "progress: line ends with the first 19 decoded characters (3-wheel)" \
  "$(printf '%s\n' "$fmt_err" | progress_lines | tail -1 | awk '{ print $NF }')" \
  "$(printf '%s' "$r_pt" | cut -c1-19)"
check "progress: lines stay within 80 columns (3-wheel, 13-pair board)" \
  "$(printf '%s\n' "$fmt_err" | awk 'length > m { m = length } END { print (m <= 80) ? "ok" : m }')" \
  "ok"
# Same, for the 4-wheel M4: the wider key must not push the line past 80 either.
fmt4_ct=$(run "$r_pt" -4 -i -u b -w B123 -r AAAA -g AAAA -s "$fmt_pb")
fmt4_err=$(printf '%s' "$fmt4_ct" | "$ENIGMA" -4 -i -u b -w B123 -r AAAA -g AAAA -s "$fmt_pb" 2>&1 >/dev/null)
check "progress: line ends with the first 16 decoded characters (M4)" \
  "$(printf '%s\n' "$fmt4_err" | progress_lines | tail -1 | awk '{ print $NF }')" \
  "$(printf '%s' "$r_pt" | cut -c1-16)"
check "progress: lines stay within 80 columns (M4, 13-pair board)" \
  "$(printf '%s\n' "$fmt4_err" | awk 'length > m { m = length } END { print (m <= 80) ? "ok" : m }')" \
  "ok"
# The --random default kick is a fixed 10 pairs: a plain kicked -R run must equal an
# explicit --random 10 run (REDESIGN Part B: default kick 8 -> 10).
check "restarts: default kick == --random 10" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 8 --random 10)"
# --restarts 0 (the new default) is one deterministic seed climb, no kick: an explicit
# -R 0 must equal the no-R default, and both must be -T-independent (trivially, one climb).
check "restarts: -R 0 == default (one deterministic climb)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 0)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c)"
# -R is validated: 0 is now legal (the default), the count may be large (well past the
# old 100000 guard), a negative value is rejected, and an absurd value over the
# 1000000000 cap is still rejected. The accept cases omit -c so only validation runs.
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c -R 0 >/dev/null 2>&1
check "restart count 0 accepted (exit code)" "$?" "0"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c -R -1 >/dev/null 2>&1
check "restart count -1 rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -R 200000 >/dev/null 2>&1
check "restart count past old 100000 cap accepted (exit code)" "$?" "0"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c -R 1000000001 >/dev/null 2>&1
check "restart count over 1000000000 rejected (exit code)" "$?" "1"
# --random is the kick size (0..13); K=0 is legal (a no-perturbation control), an
# out-of-range K is rejected, and --random without -c is an error (nothing to kick).
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c --random 0 >/dev/null 2>&1
check "kick --random 0 accepted (exit code)" "$?" "0"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -c --random 14 >/dev/null 2>&1
check "kick --random 14 (over max) rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA --random 5 >/dev/null 2>&1
check "kick --random without -c rejected (exit code)" "$?" "1"

# Partial plugboard exhaustion (--exhaust E): force E extra plug pairs among the free
# letters (on top of any -s pairs), try every combination, keep the best climb. On an easy
# long message it recovers exactly (a KAT of the exhaustion path). --exhaust 1 (no -s)
# forces each of the 325 first pairs; -s AB --exhaust 1 pins the true AB and forces one
# more. Greedy-only; the guards below enforce no -A and E within the free plug pairs.
check "exhaustion --exhaust 1 recovers an easy message" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -c --exhaust 1 --score i4q10 -T 1)" \
  "$r_pt"
check "exhaustion -s AB --exhaust 1 (1 fixed + 1 forced) recovers" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -s "AB" -c --exhaust 1 --score i4q10 -T 1)" \
  "$r_pt"
# Exhaustion composes with the kick and restarts (the old silent no-op is fixed): a kicked
# exhaustion run still recovers the easy message.
check "exhaustion --exhaust 1 --random 2 --restarts 3 recovers" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -c --exhaust 1 --random 2 --restarts 3 --score i4q10 -T 1 -e 5)" \
  "$r_pt"
# Part D: exhaustion is now parallel (first forced pair = the work unit), so it runs on
# -T > 1 and stays -T-independent -- pure exhaustion and the kicked+restart form both agree
# byte-for-byte across thread counts, and E=2 too.
check "exhaustion --exhaust 1 is -T-independent (T1==T8)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -c --exhaust 1 --score i4q10 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -c --exhaust 1 --score i4q10 -T 8)"
check "exhaustion --exhaust 1 --random 2 --restarts 3 is -T-independent (T1==T8)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -c --exhaust 1 --random 2 --restarts 3 --score i4q10 -T 1 -e 5)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -c --exhaust 1 --random 2 --restarts 3 --score i4q10 -T 8 -e 5)"
# E=2 is pinned down to 10 free letters with -s. Unpinned it is 44850 forced-pair
# combinations x a climb each, twice -- minutes per check, and ~2.7 min of the sanitizer
# job on its own. With 8 pairs fixed there are 45 first-forced-pairs (still spread across
# threads, so the parallel work-unit split this guards is still exercised) x 28 second
# pairs, which runs in ~0.25 s and gives the identical T1==T4 verdict.
check "exhaustion --exhaust 2 is -T-independent (T1==T4)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -s "AB CD EF GH IJ KL MN OP" -c --exhaust 2 --score i4q10 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g AAA -s "AB CD EF GH IJ KL MN OP" -c --exhaust 2 --score i4q10 -T 4)"
printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA -c -A 6000 --exhaust 1 -T 1 >/dev/null 2>&1
check "exhaustion --exhaust rejects -A simulated annealing (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA --exhaust 1 -T 1 >/dev/null 2>&1
check "exhaustion --exhaust without -c rejected (exit code)" "$?" "1"
# E must fit in the free plug pairs: 11 -s pairs leave 4 free letters = 2 free pairs, so
# --exhaust 3 has no room and is rejected.
printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA -s "ABCDEFGHIJKLMNOPQRSTUV" -c --exhaust 3 -T 1 >/dev/null 2>&1
check "exhaustion --exhaust E over the free pairs rejected (exit code)" "$?" "1"

# --dump-all: one 'dumpall' line per converged (key, restart) climb.
da_n=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA -c -R 4 --dump-all -T 1 2>&1 >/dev/null | grep -c '^dumpall ')
check "--dump-all prints one line per restart (-R 4)" "$da_n" "4"

# --true-key (§2 diagnostic): needs -F and reports the true key's tier-1 rank. r_ct was
# encrypted at reflector B, wheels 123, ring AAA, start AAA => true key B123AAAAAA, which
# is inside the -g A.. keyspace here (first start letter fixed A).
printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA -c --true-key B123AAAAAA -T 1 >/dev/null 2>&1
check "--true-key without -F rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA -c -F 5 --true-key TOOSHORT -T 1 >/dev/null 2>&1
check "--true-key with a malformed key rejected (exit code)" "$?" "1"
tk_line=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g A.. -c -F 5 --true-key B123AAAAAA -T 2 2>&1 >/dev/null | grep -c 'true-key tier1 rank [0-9][0-9]* of ')
check "--true-key reports a tier-1 rank line" "$tk_line" "1"
# The reported rank must be -T-independent (deterministic tier-1).
tk1=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g A.. -c -F 5 --true-key B123AAAAAA -T 1 2>&1 >/dev/null | grep -oE 'rank [0-9]+ of [0-9]+')
tk4=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g A.. -c -F 5 --true-key B123AAAAAA -T 4 2>&1 >/dev/null | grep -oE 'rank [0-9]+ of [0-9]+')
check "--true-key rank is -T-independent" "$tk1" "$tk4"

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

# Staged plugboard climb (--score schedule: a bigram pre-pass, then the quad target as
# the last stage). It must stay -T-independent, and recover a small plugboard on a
# long message (where the bigram pre-pass reliably steers the quad climb to the true
# board). The kick here is the --random default (10 pairs).
s_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA -s "AB CD")
check "staged: --score bq -R 4 result is -T-independent" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score bq -R 4 -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score bq -R 4 -T 4)"
check "staged: --score bq recovers plugboard (long message)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g AAA -c --score bq)" \
  "$r_pt"
# --score schedule is validated: a non-model letter is rejected. The old r token is gone
# (moved to --random); a is now a valid MODEL token (the weighted all-order model, -a).
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c --score z >/dev/null 2>&1
check "staged: bad --score schedule rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c --score r8q >/dev/null 2>&1
check "staged: --score no longer accepts r token (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c --score a1q >/dev/null 2>&1
check "staged: --score accepts a (weighted) model token (exit code)" "$?" "0"

# Per-stage plug-pair caps (the number after a model letter) composed with the kick
# (--random) and restarts must stay -T-independent.
check "staged: --score i3q --random 2 -R 4 is -T-independent" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score i3q --random 2 -R 4 -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score i3q --random 2 -R 4 -T 4)"
# The weighted all-order model (a) with its --polish finisher stays -T-independent too.
check "staged: --score m4a10 (weighted) --polish is -T-independent" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score m4a10 --random 2 -R 4 --polish -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score m4a10 --random 2 -R 4 --polish -T 4)"
# --random 0 injects no plugs, so N restarts all repeat the seed climb: --random 0 -R 8
# equals the deterministic -R 0 (one seed climb).
check "staged: --random 0 makes restarts a no-op" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score iq --random 0 -R 8)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rg" -c --score iq -R 0)"
# Schedule grammar is validated: an out-of-range stage cap is rejected.
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c --score q14 >/dev/null 2>&1
check "staged: --score stage cap over max (q14) rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -l english -u B -w 123 -r AAA -g AAA -c --score q0 >/dev/null 2>&1
check "staged: --score stage cap 0 (q0) rejected (exit code)" "$?" "1"

# A --score climb schedule (more than one stage, or any cap) without -c is a non-fatal
# warning (nothing to climb), and the run proceeds ranking by the target model.
sc_warn=$(printf '%s' "$s_ct" | "$ENIGMA" -l english -u B -w 123 -r AAA -g "..A" --score i4q10 2>&1 >/dev/null)
case "$sc_warn" in
  *"climb schedule ignored without -c"*) check "staged: --score without -c warns" "ok" "ok" ;;
  *)                                      check "staged: --score without -c warns" "$sc_warn" "*climb schedule ignored without -c*" ;;
esac

# Model selectors (-i/-m/-b/-t/-q) are aliases for a single uncapped --score <model>
# stage (REDESIGN Part C). Setting the scoring model to *conflicting* values is a fatal
# error: two disagreeing selectors, or a selector vs a different --score target. Agreement
# (a repeated selector, or a selector matching the --score target) is accepted silently.
printf 'ABCDE' | "$ENIGMA" -m -q -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "model: two disagreeing selectors (-m -q) rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -m --score q -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "model: selector vs --score target (-m --score q) rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -i --score q -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "model: -i vs --score q rejected (exit code)" "$?" "1"
printf 'ABCDE' | "$ENIGMA" -q -q -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "model: repeated agreeing selector (-q -q) accepted (exit code)" "$?" "0"
printf 'ABCDE' | "$ENIGMA" -q --score q -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "model: selector matching --score target (-q --score q) accepted (exit code)" "$?" "0"
printf 'ABCDE' | "$ENIGMA" -q --score i4q10 -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1
check "model: selector matching --score target/last stage (-q --score i4q10) accepted (exit code)" "$?" "0"
# A selector alone sets the scan ranking model identically to the equivalent --score:
# with a fixed key + wildcard start, -q and --score q must rank the same best decrypt.
qsel_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA)
check "model: -q selector == --score q (same ranking, no -c)" \
  "$(run "$qsel_ct" -l english -u B -w 123 -r AAA -g ..A -q)" \
  "$(run "$qsel_ct" -l english -u B -w 123 -r AAA -g ..A --score q)"

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

# -J: first-improvement with dynamic best-first move ordering. A different
# trajectory again, so checked by recovery + determinism, not equality; deterministic
# (order derived from the fixed board, fixed tie-break) so -T-independent; needs -c.
check "dynamic-order -J: recovers plaintext (long msg + restarts)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -J -R 8 -S iq)" \
  "$f_pt"
check "dynamic-order -J: result is -T-independent" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -J -R 8 -S iq -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -J -R 8 -S iq -T 4)"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -J >/dev/null 2>&1
check "dynamic-order -J: without -c rejected (exit code)" "$?" "1"

# -M: make the plug cap a strict descent target (merge/remove only at/over the cap). A
# different (non-byte-identical) trajectory, so checked by recovery + determinism, not
# equality; deterministic so -T-independent; needs -c; still recovers on an easy message.
check "cap-target -M: recovers plaintext (long msg + capped schedule)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -M -R 8 -S i4q10)" \
  "$f_pt"
check "cap-target -M: result is -T-independent" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -M -R 8 -S i4q10 -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -M -R 8 -S i4q10 -T 4)"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -M >/dev/null 2>&1
check "cap-target -M: without -c rejected (exit code)" "$?" "1"

# Restart-level parallelism: with a fully-specified rotor key the search has exactly ONE
# key, so -T can only speed things up by spreading the -R plugboard restarts across
# threads. Each restart draws from its own (key,restart) seed, so the result must be
# identical to -T 1 (a deterministic global best with a lowest-index tie-break) and must
# still recover the plaintext.
check "restart-parallel: fixed key, -R climb is -T-independent (T1==T8)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 24 -S i4q10 -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 24 -S i4q10 -T 8)"
check "restart-parallel: fixed key + restarts recovers plaintext" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -R 24 -S i4q10 -T 8)" \
  "$f_pt"

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
sa_pb=$(printf '%s\n' "$sa_err" | last_plugboard | grep -oE '[A-Z][A-Z]' | tr '\n' ' ')
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
cap_pairs=$(printf '%s\n' "$cap_err" | last_plugboard | grep -oE '[A-Z][A-Z]' | grep -c .)
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
frz_climb=$(printf '%s' "$frz_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -R 20 -S iq -s "YZ" 2>&1 >/dev/null | last_plugboard)
case "$frz_climb" in
  *YZ*) check "fixed -s plug survives the greedy climb" "ok" "ok" ;;
  *)    check "fixed -s plug survives the greedy climb" "$frz_climb" "*YZ*" ;;
esac
frz_sa=$(printf '%s' "$frz_ct" | "$ENIGMA" -q -l english -u B -w 241 -r AAA -g QEW -c -A 30000 -s "YZ" 2>&1 >/dev/null | last_plugboard)
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
  "$(run 'BDZGOWCXLT' -m -l zztest -d . -u B -w 123 -r AAA -g AAA)" \
  "AAAAAAAAAA"
rm -f zztest_monograms.txt

# Data directory: the n-gram files can live somewhere other than the default
# "ngrams" subdirectory, selected by -d or $ENIGMA_DATA. Run from a different
# CWD (/) with an absolute binary so only the resolved data dir can find them.
# A small wildcard search (-g ..A) forces the n-gram table to be loaded from the
# resolved data dir -- a fully fixed machine would not score, so it would not load
# anything. A successful exit means the files were found there.
root=$(pwd)
( cd / && printf 'BDZGOWCXLT' | "$root/enigma" -m -l english -d "$root/ngrams" -u B -w 123 -r AAA -g ..A >/dev/null 2>&1 )
check "-d finds n-gram files from another CWD (exit code)" "$?" "0"
( cd / && printf 'BDZGOWCXLT' | ENIGMA_DATA="$root/ngrams" "$root/enigma" -m -l english -u B -w 123 -r AAA -g ..A >/dev/null 2>&1 )
check "ENIGMA_DATA finds n-gram files from another CWD (exit code)" "$?" "0"
( cd / && printf 'BDZGOWCXLT' | ENIGMA_DATA=/nonexistent "$root/enigma" -m -l english -d "$root/ngrams" -u B -w 123 -r AAA -g ..A >/dev/null 2>&1 )
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
# wildcarding the start. All languages x 6 models must recover when -l matches the
# plaintext language. (This also guards the IC formula fix: the old -i formula could
# not distinguish plaintext from gibberish and returned the wrong key. Note quadgrams
# need the matching -l -- they are the most language-specific model; see CLAUDE.md.)
for lang in $crack_langs; do
  plain=$(plain_for "$lang")
  ct=$(run "$plain" -i -u B -w 123 -r AAA -g QXP)
  for mode in -i -m -b -t -q -a -f; do
    check "crack: start position, $lang $mode" \
      "$(run "$ct" $mode -u B -w 123 -r AAA -g "$crack_scan_g" -l "$lang")" \
      "$plain"
  done
done

# The 'wehrmacht' scoring language (telegraphic military German -- ngrams/wehrmacht_*.txt,
# generated from the published Appendix-C statistics). Check its four tables load and that
# it recovers a plugboard on telegraphic text, the register it is built for.
pt_wehrmacht="ANROEMEINSBERTAXWIRTSQAFTLIQEUNTERSTELLUNGUNTERROEMXZEHNXARMKORPSDAUERTNURXZWOXTAGEXSTUERZBEQERX"
w_ct=$(run "$pt_wehrmacht" -i -u B -w 123 -r AAA -g AAA -s "AB CD")
# Capped at the true 2 pairs: uncapped, the climb adds a spurious third plug on this text
# (JQ -- J is rare in telegraphic German, so the model tolerates it). -l german does the
# same here, so it is over-plugging, not a wehrmacht-table problem.
check "crack: hill-climb plugboard, wehrmacht -a" \
  "$(run "$w_ct" -a -c --score a2 -u B -w 123 -r AAA -g AAA -l wehrmacht)" \
  "$pt_wehrmacht"
check "crack: start position, wehrmacht -a" \
  "$(run "$(run "$pt_wehrmacht" -i -u B -w 123 -r AAA -g QXP)" -a -u B -w 123 -r AAA -g "$crack_scan_g" -l wehrmacht)" \
  "$pt_wehrmacht"

# Non-English-corpus languages added after the original english/german/danish/french set
# (swedish, finnish, icelandic, polish, spanish -- see fold_codepoint() in enigma.cc for
# the accented-letter folding this exercises, e.g. Polish ogonek/stroke/acute letters and
# Icelandic thorn, which once silently dropped up to ~20%/~5% of a table's mass). Each
# language's four tables must load with ZERO "non-mappable character" records -- a cheap
# guard against a future table reintroducing an unfolded code point -- and a plugboard
# must be recoverable under its own table. Not folded into the full crack_langs matrix
# above: these languages don't have a curated long public-domain passage yet, so this is
# a lighter smoke test, not the full start-position + hill-climb x 7-model matrix.
for lang in swedish finnish icelandic polish spanish; do
  for suffix in monograms bigrams trigrams quadgrams; do
    case $suffix in
      monograms) mode=-m ;; bigrams) mode=-b ;; trigrams) mode=-t ;; quadgrams) mode=-q ;;
    esac
    err=$(printf 'ABCDE' | "$ENIGMA" "$mode" -l "$lang" -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null)
    check "language table loads cleanly: $lang $suffix" \
      "$(printf '%s' "$err" | grep -c 'non-mappable')" "0"
  done
done
# A representative sentence per language, already folded to plain A-Z the way the
# plaintext reader folds real accented input (the Polish sentence below reads
# "Wlasciwie nie wiem..." with plain L/E/S standing in for the original Polish
# text's L-stroke/E-ogonek/S-acute letters), plugboard hidden, hill-climbed and
# expected back exactly.
pt_swedish="JAGVETINTERIKTIGTVARFORDETSVENSKASPRAKETARSAVARTATTLARASIGMENDETARNOGFORATTVIHARSAMANGAORD"
pt_finnish="ENTIEDAMIKSISUOMENKIELIONNIINVAIKEAAOPPIAMUTTASESSAONVARMASTIPALJONKAUNIITAJASOINTUVIASANOJA"
pt_icelandic="EKKIVEITEGHVERSVEGNAISLENSKAERSVONAERFIDENHUNERLIKAMJOGFALLEGOGFULLAFGOMLUMNORRAENUMORDUM"
pt_polish="WLASCIWIENIEWIEMDLACZEGOJEZYKPOLSKIJESTTAKTRUDNYALEJESTTEZBARDZOPIEKNYIPELENCIEKAWYCHSLOW"
pt_spanish="NOSEMUYBIENPORQUEELIDIOMAESPANOLESTANDIFICILDEAPRENDERPEROTIENEMUCHASPALABRASMUYBONITAS"
for lang in swedish finnish icelandic polish spanish; do
  case $lang in
    swedish) plain=$pt_swedish ;; finnish) plain=$pt_finnish ;;
    icelandic) plain=$pt_icelandic ;; polish) plain=$pt_polish ;; spanish) plain=$pt_spanish ;;
  esac
  ct=$(run "$plain" -i -u B -w 123 -r AAA -g AAA -s "AB CD")
  check "crack: hill-climb plugboard, $lang -q" \
    "$(run "$ct" -q -c -u B -w 123 -r AAA -g AAA -l "$lang")" \
    "$plain"
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
  for mode in -i -m -b -t -q -a -f; do
    check "crack: hill-climb plugboard, $lang $mode" \
      "$(run "$ct" $mode -c -u B -w 123 -r AAA -g AAA -l "$lang")" \
      "$plain"
  done
done

# --ring-stride: sparse ring sampling for the rightmost wheel (PERFORMANCE.md §7.11).
# Needs both -r and -g to wildcard ring2/start2; ring0 auto-collapses (§7.10), and the
# default ring "AA." pins ring0/ring1 to A -- exactly the tool's bare-default keyspace
# (26 ring2 x 26^3 starts), kept small so this stays fast under the sanitizers too.
rs_pt="THEQUICKBROWNFOXJUMPSOVERTHELAZYDOGANDTHENRANAWAYINTOTHEDARKFORESTNEARTHERIVERWHERETHEWATERWASCOLD"
rs_ct=$(run "$rs_pt" -i -u B -w 123 -r AAZ -g XKP)
for K in 2 3 5; do
  check "crack: --ring-stride $K recovers exact key" \
    "$(run "$rs_ct" -q -l english -u B -w 123 --ring-stride "$K" -T 1)" \
    "$rs_pt"
done

# Regression: true ring2=Z with a K=2 coarse winner landing at the A(0) boundary needs
# TWO things the initial implementation got wrong. (1) The refinement window must WRAP
# at the 0/25 boundary rather than clamp -- a clamped [0,1] window only ever checks
# {A,B}, never Z, even though ring2=Z is "recoverable" per the documented |K/2| distance
# metric. (2) On the plain-scan path, search_worker() leaves the machine's
# ringstellung/grundstellung in a stale, stepped state after scanning (a "lazy restore"
# perf optimisation -- only the hillclimb path restores per key), so re-reading
# ring0/start0 from the live machine between refinement segments picks up whatever key
# the PRIOR segment's scan last touched rather than the coarse winner's actual pin.
# These specific keys were found by a random sweep to trigger both bugs; -T 1 keeps it
# deterministic and reproducible.
for key in "B 451 AAZ VKZ" "B 351 AAZ NLV" "C 324 AAZ JEY"; do
  # shellcheck disable=SC2086  # intentional word-splitting into positional params
  set -- $key
  wr_ct=$(run "$rs_pt" -i -u "$1" -w "$2" -r "$3" -g "$4")
  check "crack: --ring-stride 2 wraps at ring2=Z boundary ($1 $2 $3 $4)" \
    "$(run "$wr_ct" -q -l english -u "$1" -w "$2" --ring-stride 2 -T 1)" \
    "$rs_pt"
done

# The refinement covers EVERY skipped ring2, not just the +/-K/2 neighbours of the coarse
# winner (PERFORMANCE.md §7.11). The narrow window rested on the coarse winner landing
# within K/2 of the truth; this authentic-Wehrmacht key is a measured counterexample --
# the coarse pass at K=3 wins on a ring2 well outside K/2 of the true T, so the narrow
# window never tested the truth and returned a partially-wrong plaintext, while the full
# sweep recovers it. Verified to FAIL against the pre-widening binary.
ws_pt=NNAHMEHOPOZSCHAAXOPOTSCHKAXUNVWIEDDRHERSTELLCNGXWELDKAJAXWEL
ws_board="RG VJ KF AC BX SY OH NQ DP WZ"
ws_ct=$(run "$ws_pt" -i -u A -w 123 -r DLT -g ACG -s "$ws_board")
check "crack: --ring-stride 3 refines beyond the K/2 window" \
  "$(run "$ws_ct" -f -l wehrmacht -u A -w 123 -r "..." -g "..." -s "$ws_board" \
        --ring-stride 3 -T 1)" \
  "$ws_pt"

# --ring-stride must not touch the PLUGBOARD. The --polish finisher shares its enclosing
# `if` with the refinement (both reconstruct the winner from best.idx once) and used to run
# whenever EITHER was requested -- so a strided run got a full plugboard climb plus an
# unconditional gain cascade even with no -c, adding spurious plugs to a board supplied via
# -s and corrupting the decrypt. With no -c the board must come back exactly as given,
# stride or no stride, so compare the two runs rather than hard-coding the normalised form.
# This needs a case where the finisher actually FINDS an improving plug -- on an easy
# board it converges immediately and a buggy build looks identical to a fixed one. This
# authentic-Wehrmacht key is a measured one: the pre-fix binary adds an 11th plug (FV) and
# returns a corrupted decrypt, the fixed one returns the board as given and recovers
# exactly. ring1/start1 are pinned to the true key to keep it at 338 keys.
rs_pb_pt=XBEFINDEMIQINXROSENOWROSENOWXSOFORTQUNKQNTWORTXWASCHBUSCHWIS
rs_pb_board="SX JI AB HT RW QK UM ZG EN LY"
rs_pb_ct=$(run "$rs_pb_pt" -i -u A -w 145 -r FFR -g RTB -s "$rs_pb_board")
rs_pb_plain=$(printf '%s' "$rs_pb_ct" | "$ENIGMA" -f -l wehrmacht -u A -w 145 -r "FF." -g "RT." \
              -s "$rs_pb_board" -T 1 2>&1 >/dev/null | last_plugboard)
rs_pb_stride=$(printf '%s' "$rs_pb_ct" | "$ENIGMA" -f -l wehrmacht -u A -w 145 -r "FF." -g "RT." \
               -s "$rs_pb_board" --ring-stride 2 -T 1 2>&1 >/dev/null | last_plugboard)
check "--ring-stride leaves the plugboard alone without -c" "$rs_pb_stride" "$rs_pb_plain"
check "--ring-stride without -c keeps exactly the 10 -s pairs" \
  "$(printf '%s' "$rs_pb_stride" | wc -w | tr -d ' ')" "10"
check "crack: --ring-stride 2 recovers with the given plugboard untouched" \
  "$(run "$rs_pb_ct" -f -l wehrmacht -u A -w 145 -r "FF." -g "RT." -s "$rs_pb_board" \
        --ring-stride 2 -T 1)" \
  "$rs_pb_pt"

# INVARIANT: with no -c, NOTHING may touch the plugboard. The check above pins one option
# at one K; this is the general property, because the defect was a class rather than an
# instance -- a feature running when it was not requested, because it shared an enclosing
# `if` with one that was and never re-checked its own flag. That `if` exists to host work
# needing best.idx reconstructed once, so it is likely to gain more members, and the next
# one can make exactly the same mistake. Every option below is legal without -c and
# reaches that block; the whole sweep is sensitive (all four K values report 11 plugs on
# the pre-fix binary against the fixed one's 10), so it guards the defect rather than
# restating the fix.
inv_ref=$(printf '%s' "$rs_pb_ct" | "$ENIGMA" -f -l wehrmacht -u A -w 145 -r "FF." -g "RT." \
          -s "$rs_pb_board" -T 1 2>&1 >/dev/null | last_plugboard)
for inv_o in "--ring-stride 2" "--ring-stride 3" "--ring-stride 5" "--ring-stride 13"; do
  # shellcheck disable=SC2086  # intentional word-splitting of the option under test
  inv_got=$(printf '%s' "$rs_pb_ct" | "$ENIGMA" -f -l wehrmacht -u A -w 145 -r "FF." -g "RT." \
            -s "$rs_pb_board" $inv_o -T 1 2>&1 >/dev/null | last_plugboard)
  check "no -c: plugboard untouched ($inv_o)" "$inv_got" "$inv_ref"
done

# INVARIANT: -s pairs are HELD FIXED by every climb variant, not just the two that were
# spot-checked (greedy and -A). Distinct from the invariant above -- that one says the
# board must not change without -c, this one says the given pairs must survive when a
# climb IS requested -- and unlike it, this passes on the pre-fix binary too: it fills a
# coverage gap rather than guarding the finisher defect. Each variant routes through a
# different move path (cascade, cap-as-target, first-improvement, restarts, exhaustion,
# the -F tiers, the stride refinement), and every one of them must skip plug_fixed[].
for inv_c in "--polish" "--cascade" "-M" "-J" "-R 3" "-A 3000" "--exhaust 1" "-F 5" \
             "--ring-stride 2"; do
  # shellcheck disable=SC2086  # intentional word-splitting of the option under test
  inv_pb=$(printf '%s' "$rs_pb_ct" | "$ENIGMA" -f -l wehrmacht -u A -w 145 -r "FF." -g "RT." \
           -s "$rs_pb_board" -c $inv_c -T 1 2>&1 >/dev/null | last_plugboard)
  inv_missing=""
  for inv_p in $rs_pb_board; do
    # the display sorts each pair's letters, so compare against both orderings
    inv_rev=$(printf '%s' "$inv_p" | cut -c2)$(printf '%s' "$inv_p" | cut -c1)
    case " $inv_pb " in
      *" $inv_p "*|*" $inv_rev "*) ;;
      *) inv_missing="$inv_missing $inv_p" ;;
    esac
  done
  check "-c $inv_c: every -s pair held fixed" "$inv_missing" ""
done

# The refinement re-searches all 25 skipped ring2 values over ring1 x start1 x start2, so
# it is cheaper than a coarse ring2 value only by tasks.size() * rc[0] * gc[0]. When that
# factor is 1 -- a single wheel order AND start0 pinned -- the stride costs MORE than it
# saves, and the tool must say so rather than silently doing extra work. The two -g forms
# below straddle it: with start0 pinned the stride is a net loss, with start0 open it is a
# 1.9x win, and the warning tracks that exactly.
rs_w=$(printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "AA." -g "AA." \
       --ring-stride 2 -T 1 2>&1 >/dev/null | grep -c 'not paying for itself')
check "--ring-stride warns when the refinement outweighs the coarse pass" "$rs_w" "1"
rs_w=$(printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "AA." -g "..." \
       --ring-stride 2 -T 1 2>&1 >/dev/null | grep -c 'not paying for itself')
check "--ring-stride stays silent when it is paying off" "$rs_w" "0"

# The "Analysed N" line must count keys the refinement actually SCORED, not its index
# space: the §7.12 collapse applies inside the refinement too, so counting the index space
# claimed credit for skipped start1 values and overstated --ring-stride's cost by the
# collapse factor (439400 enumerated against 106600 scored on a wildcarded keyspace).
# ring1/start1 are both wildcarded here so the collapse is active; on a plain scan one
# plugboard is scored per surviving key, so the two counts must agree exactly.
rs_diag=$(printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "A.." -g "A.." \
          --ring-stride 2 -T 1 2>&1 >/dev/null \
          | grep -oE 'Analysed [0-9]+ rotor combinations, scored [0-9]+ plugboards')
check "--ring-stride: analysed count matches keys scored (collapse active)" \
  "$(printf '%s' "$rs_diag" | awk '{print ($2 == $6)}')" "1"

# Validation: illegal K, a non-wildcarded ring2/start2, and -F/--exhaust all fail fast
# with a clear error rather than silently misbehaving.
rs_err=$(printf 'AAAA' | "$ENIGMA" --ring-stride 0 2>&1 >/dev/null)
check "--ring-stride 0 rejected" "$(printf '%s' "$rs_err" | grep -c 'Illegal ring stride')" "1"
rs_err=$(printf 'AAAA' | "$ENIGMA" --ring-stride 14 2>&1 >/dev/null)
check "--ring-stride 14 rejected" "$(printf '%s' "$rs_err" | grep -c 'Illegal ring stride')" "1"
rs_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -u B -w 123 -r AAZ -g AAA --ring-stride 2 2>&1 >/dev/null)
check "--ring-stride needs ring2/start2 wildcarded" \
  "$(printf '%s' "$rs_err" | grep -c 'ring-stride needs')" "1"
rs_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -u B -w 123 -r "..." -g "..." -c --ring-stride 2 -F 100 2>&1 >/dev/null)
check "--ring-stride rejects -F" "$(printf '%s' "$rs_err" | grep -c 'not supported with -F')" "1"

# NORWAY mode with --ring-stride. Regression for a bug that every other --ring-stride
# test missed because they all used the standard machine: wheel_task carries RAW wheel
# numbers, which init_walzen() translates (Norway adds norway_rotor_base). The refinement
# used to rebuild its task from the machine's ALREADY-TRANSLATED walzenlage[], so
# search_worker translated a second time and searched the wrong rotors -- and the §7.12
# mask, keyed on raw indices, hit an unbuilt row and skipped every key, so the refinement
# silently found nothing. Invisible outside Norway, where raw == translated.
nw_pt="STRENGTHEMMELIGSTOPMULIGMILITAERAKTIVITETOBSERVERTIEKMANFJORDENSTOPEKSPERTISEREKVIRERESOMGAAENDESTOPDOKUMENTERFUNNETMEDREFERANSETILOBJEKTIOSLO"
nw_ct=$(run "$nw_pt" -n -i -u N -w 352 -r LYR -g OSL)
check "crack: Norway + --ring-stride 2 refines to the exact key" \
  "$(run "$nw_ct" -n -f -l danish -u N -w 352 -r "L.." -g "O.." --ring-stride 2 -T 1)" \
  "$nw_pt"
check "crack: Norway + --ring-stride 2 matches the unstrided search" \
  "$(run "$nw_ct" -n -f -l danish -u N -w 352 -r "L.." -g "O.." --ring-stride 2 -T 1)" \
  "$(run "$nw_ct" -n -f -l danish -u N -w 352 -r "L.." -g "O.." -T 1)"

# The refinement runs its own search with a private best_result, which used to restart the
# progress display: a second column header mid-run, and each improvement echoed TWICE --
# once by the climb (report_climb_progress, gated on the OUTER best via g_progress) and
# once by that search's merge (gated on rbest). One shared gate now, so: exactly one
# header, and no progress line repeated verbatim.
# ring1/start1 are PINNED here (-r LY. -g OS.), unlike the recovery checks above: this
# check is about the progress DISPLAY, and the refinement echoes the same way whether it
# searches 338 keys or 228k. Wildcarding them costs ~60 s per run (~6.5 min of the
# sanitizer job on its own -- every key gets a hill-climb) for no extra display coverage.
# Verified against the pre-fix binary: this keyspace still reports 2 headers and 1
# repeated line, so the regression is still caught.
rs_disp=$(printf '%s' "$nw_ct" | "$ENIGMA" -n -c -f -l danish -u N -w 352 -r "LY." -g "OS." \
          --ring-stride 2 -T 2 2>&1 >/dev/null)
check "--ring-stride prints exactly one progress header" \
  "$(printf '%s' "$rs_disp" | grep -c 'Score W')" "1"
check "--ring-stride repeats no progress line" \
  "$(printf '%s' "$rs_disp" | grep -E '^ *-?[0-9]+\.' | sort | uniq -d | wc -l | tr -d ' ')" "0"

# --ring-stride makes the search APPROXIMATE, so a run must say so in the echoed settings:
# without this, a saved log is indistinguishable from an exhaustive run. It must stay
# silent when the option is off, so the default output is unchanged.
# show_settings() emits this BEFORE the search runs, so the keyspace is irrelevant to
# what is being checked: -r AA. -g AA. is the smallest one --ring-stride accepts (it
# requires ring2/start2 wildcarded) and is ~20x cheaper than the full sweep these used.
rs_echo=$(printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "AA." -g "AA." --ring-stride 2 -T 1 2>&1 >/dev/null)
check "--ring-stride is echoed in the settings" \
  "$(printf '%s' "$rs_echo" | grep -c '^Stride: .*ring-stride')" "1"
rs_echo=$(printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "AA." -g "AA." -T 1 2>&1 >/dev/null)
check "no stride line when --ring-stride is off" \
  "$(printf '%s' "$rs_echo" | grep -c '^Stride:')" "0"

# Middle-wheel ring x start collapse (PERFORMANCE.md §7.12). Shifting ring1 and start1
# together only changes the decode through the middle notch, which most start1 values
# never reach in a short message -- so those start1 values are skipped as duplicates.
# The risk being guarded is SILENT KEY LOSS: a wrong partition drops the true key from
# the search with no error, so every case below must still recover exactly.
#
# The rotor choices are deliberate: 126/168 put a TWO-NOTCH rotor (VI, notches MZ) on the
# right, doubling the middle's step rate, and 132 exercises the double-step extras --
# both are cases where a closed-form class count is wrong, so they are exactly where a
# formula-based implementation would silently lose keys.
mw_pt="ANXPANZXGRUPPEXVIERXSIEGFRIEDSIEGFRIEDTONIXDIVXSTEHTSEITXEINSZWOXSIEBENXEINSEINSNULLNULLXUHRMITANFAENGENAMUNTERKUNFTSRAUMX"
for mw in "123 AQL ADT" "132 AZC AKP" "126 AMM AJY" "168 ABX AWD" "145 AKK ARR"; do
  # shellcheck disable=SC2086  # intentional word-splitting into positional params
  set -- $mw
  mw_ct=$(run "$mw_pt" -i -u B -w "$1" -r "$2" -g "$3")
  # ring1 wildcarded (-r A..) -> the collapse is active here
  check "crack: middle-wheel collapse recovers exactly (w$1, ring1 wildcarded)" \
    "$(run "$mw_ct" -f -l wehrmacht -u B -w "$1" -r "A.." -g "A.." -T 1)" \
    "$mw_pt"
  # ring1 PINNED (to its true value) -> collapse must not fire; recovery unaffected
  mw_r1=$(printf '%s' "$2" | cut -c2)
  check "crack: middle-wheel collapse inert when ring1 pinned (w$1)" \
    "$(run "$mw_ct" -f -l wehrmacht -u B -w "$1" -r "A$mw_r1." -g "A.." -T 1)" \
    "$mw_pt"
done

# -T-independence with the collapse active: skipped keys make the per-thread chunks
# uneven, so this guards the work split as much as the collapse itself.
mw_ct=$(run "$mw_pt" -i -u B -w 123 -r AQL -g ADT)
check "middle-wheel collapse is -T-independent" \
  "$(run "$mw_ct" -f -l wehrmacht -u B -w 123 -r "A.." -g "A.." -T 1)" \
  "$(run "$mw_ct" -f -l wehrmacht -u B -w 123 -r "A.." -g "A.." -T 4)"

# The "Analysed N" line must count keys actually scored, not the index-space size --
# it previously claimed credit for keys the collapse never touched. N must equal the
# plugboards scored on a plain scan (one score per surviving key).
mw_diag=$(printf '%s' "$mw_ct" | "$ENIGMA" -f -l wehrmacht -u B -w 123 -r "A.." -g "A.." -T 1 2>&1 >/dev/null \
          | grep -oE 'Analysed [0-9]+ rotor combinations, scored [0-9]+ plugboards')
check "middle-wheel collapse: analysed count matches keys scored" \
  "$(printf '%s' "$mw_diag" | awk '{print ($2 == $6)}')" "1"

# The collapse must announce itself when applied -- it explains a reported ring/start that
# differs from the true key -- and stay silent otherwise. Its gate has three parts, so all
# three are checked: ring1 wildcarded, start1 wildcarded, and no --true-key (that
# diagnostic ranks a specific key, so the collapse is disabled for it).
mw_line() { printf '%s' "$mw_ct" | "$ENIGMA" -f -l wehrmacht -u B -w 123 "$@" -T 1 2>&1 >/dev/null \
            | grep -c '^Collapse: .*middle wheel'; }
check "middle-wheel collapse is echoed when applied" \
  "$(mw_line -r "A.." -g "A..")" "1"
check "no collapse line when ring1 is pinned" \
  "$(mw_line -r "AA." -g "A..")" "0"
check "no collapse line when start1 is pinned" \
  "$(mw_line -r "A.." -g "AA.")" "0"
# ring2/start2 are pinned to the true key here (-r A.L -g A.T). --true-key disables the
# collapse, so this run gets no key reduction at all: over the full A.. / A.. space that
# is 456976 keys through a -c climb -- ~100 s plain and minutes under the sanitizers, the
# single most expensive check in the suite. The gate only cares that ring1 and start1 are
# BOTH wildcarded, which -r A.L -g A.T still satisfies (676 keys). The positive control on
# the same keyspace keeps the negative from passing vacuously.
check "collapse applies on the pinned-ring2 keyspace (control)" \
  "$(mw_line -r "A.L" -g "A.T")" "1"
check "no collapse line under --true-key" \
  "$(mw_line -r "A.L" -g "A.T" -c -F 5 --true-key B123AQLADT)" "0"

echo
echo "passed: $pass, failed: $fail"
[ "$fail" -eq 0 ]
