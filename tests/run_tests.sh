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
# ...and $rgd is the narrow one UNCONDITIONALLY, for the checks that read a code path
# rather than a cracking result: -T-independence, "-R 0 equals the default", "-F 0 is
# off", the seed echoes. Those assert that two runs AGREE, which 26 keys establishes
# exactly as well as 676 -- and TEST_QUICK has always run them at 26 in the sanitizer
# job, so the wide sweep in the plain job was a 26x more expensive duplicate of an
# assertion already covered. $rg is kept only where breadth is the point: the recovery
# checks (the true key must beat decoys) and -F, which needs more keys than it keeps.
# Splitting the two took the plain suite from 232 s to 64 s with all 437 checks intact.
rgd="AA."

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
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 -T 4)"

# --crib-rerank (measured-down opt-in): the crib bonus is a deterministic function of the
# board, so the re-ranked winner must stay -T-independent. Also exercises loading the file.
crib_file=$(mktemp)
printf 'ENGLISH 2\nLANGUAGE 2\nANALYSIS 2\n' > "$crib_file"
check "crib: --crib-rerank result is -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 --crib-rerank "$crib_file" -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 --crib-rerank "$crib_file" -T 4)"
rm -f "$crib_file"

# --dump-all (diagnostic): prints the full setting (rotor key + score + plugboard) of every
# converged key x restart to stderr. The dumped SET (sorted) must be -T-invariant, and each
# line carries the rotor key -- so grep for one specific key and check it appears.
da1=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 --dump-all -T 1 2>&1 >/dev/null | grep '^dumpall' | sort)
da4=$(printf '%s' "$r_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 --dump-all -T 4 2>&1 >/dev/null | grep '^dumpall' | sort)
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
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 --random 10)"
# --restarts 0 (the new default) is one deterministic seed climb, no kick: an explicit
# -R 0 must equal the no-R default, and both must be -T-independent (trivially, one climb).
check "restarts: -R 0 == default (one deterministic climb)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 0)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c)"
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
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 -e 777 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 -e 777 -T 4)"
# Different explicit seeds drive different restart perturbations, so the shown seed
# tracks -e; and -e overrides the pinned $ENIGMA_SEED=0.
seed_echo=$(printf 'ABCDE' | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 -e 424242 2>&1 >/dev/null)
case "$seed_echo" in
  *"seed: 424242"*) check "seed: -e is echoed (overrides ENIGMA_SEED)" "ok" "ok" ;;
  *)                check "seed: -e is echoed (overrides ENIGMA_SEED)" "$seed_echo" "*seed: 424242*" ;;
esac
# A run with the harness's pinned ENIGMA_SEED=0 equals an explicit -e 0 (same seed).
check "seed: pinned ENIGMA_SEED=0 equals -e 0" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 8 -e 0)"

# Staged plugboard climb (--score schedule: a bigram pre-pass, then the quad target as
# the last stage). It must stay -T-independent, and recover a small plugboard on a
# long message (where the bigram pre-pass reliably steers the quad climb to the true
# board). The kick here is the --random default (10 pairs).
s_ct=$(run "$r_pt" -i -u B -w 123 -r AAA -g AAA -s "AB CD")
check "staged: --score bq -R 4 result is -T-independent" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score bq -R 4 -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score bq -R 4 -T 4)"
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
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score i3q --random 2 -R 4 -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score i3q --random 2 -R 4 -T 4)"
# The weighted all-order model (a) with its --polish finisher stays -T-independent too.
check "staged: --score m4a10 (weighted) --polish is -T-independent" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score m4a10 --random 2 -R 4 --polish -T 1)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score m4a10 --random 2 -R 4 --polish -T 4)"
# --random 0 injects no plugs, so N restarts all repeat the seed climb: --random 0 -R 8
# equals the deterministic -R 0 (one seed climb).
check "staged: --random 0 makes restarts a no-op" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score iq --random 0 -R 8)" \
  "$(run "$s_ct" -l english -u B -w 123 -r AAA -g "$rgd" -c --score iq -R 0)"
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
# The two "-F is a no-op" checks below use the narrow $rgd instead: -F 0 and -F 100%
# keep every key by construction, so there is nothing for a bigger keyspace to rank.
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
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 -S iq -F 0)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 -S iq)"

# -F N% keeps the top N% of the resolved keyspace instead of an absolute count. On
# 676 keys, -F 100% keeps every key, so it must equal a plain run with no -F; a
# generous percentage must still recover like the full crack and stay -T-independent.
check "pre-filter: -F 100% keeps all keys (matches no -F)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 -S iq -F 100%)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 -S iq)"
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
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -J -R 8 -S iq -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -J -R 8 -S iq -T 4)"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -J >/dev/null 2>&1
check "dynamic-order -J: without -c rejected (exit code)" "$?" "1"

# -M: make the plug cap a strict descent target (merge/remove only at/over the cap). A
# different (non-byte-identical) trajectory, so checked by recovery + determinism, not
# equality; deterministic so -T-independent; needs -c; still recovers on an easy message.
check "cap-target -M: recovers plaintext (long msg + capped schedule)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rg" -c -M -R 8 -S i4q10)" \
  "$f_pt"
check "cap-target -M: result is -T-independent" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -M -R 8 -S i4q10 -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -M -R 8 -S i4q10 -T 4)"
printf 'ABCDE' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA -M >/dev/null 2>&1
check "cap-target -M: without -c rejected (exit code)" "$?" "1"

# Restart-level parallelism: with a fully-specified rotor key the search has exactly ONE
# key, so -T can only speed things up by spreading the -R plugboard restarts across
# threads. Each restart draws from its own (key,restart) seed, so the result must be
# identical to -T 1 (a deterministic global best with a lowest-index tie-break) and must
# still recover the plaintext. -g is therefore PINNED to the true start, not wildcarded:
# these three checks are about the one-key case, and running them over a 676-key sweep
# did not exercise it at all while costing 56 s -- a fifth of the whole suite.
check "restart-parallel: fixed key, -R climb is -T-independent (T1==T8)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g AAA -c -R 24 -S i4q10 -T 1)" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g AAA -c -R 24 -S i4q10 -T 8)"
check "restart-parallel: fixed key + restarts recovers plaintext" \
  "$(run "$f_ct" -q -l english -u B -w 123 -r AAA -g AAA -c -R 24 -S i4q10 -T 8)" \
  "$f_pt"

# Simulated annealing (-A): an alternative plugboard optimiser. All randomness comes
# from the per-key RNG stream (seeded from the flat key index), so an SA search must
# stay independent of -T just like the restart climb. Recover the plugboard on the
# same wildcard-start key space.
check "anneal: -A result is -T-independent" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -A 20000 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -A 20000 -T 4)"
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
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -A 20000 -S q8 -T 1)" \
  "$(run "$r_ct" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -A 20000 -S q8 -T 4)"
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
d_c1=$(printf '%s' "$d_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 -S iq -T 1 2>&1 >/dev/null | grep -oE 'scored [0-9]+ plugboards')
d_c4=$(printf '%s' "$d_ct" | "$ENIGMA" -q -l english -u B -w 123 -r AAA -g "$rgd" -c -R 4 -S iq -T 4 2>&1 >/dev/null | grep -oE 'scored [0-9]+ plugboards')
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
# it recovers a plugboard on telegraphic text, the writing style it is built for.
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
# (swedish, finnish, icelandic, polish, spanish -- see fold_codepoint() in src/ngrams.cc for
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

# --ring-stride: sparse ring sampling for the rightmost wheel (archived/PERFORMANCE.md §7.11).
# Needs both -r and -g to wildcard ring2/start2; ring0 auto-collapses (§7.10), and the
# default ring "AA." pins ring0/ring1 to A -- exactly the tool's bare-default keyspace
# (26 ring2 x 26^3 starts), kept small so this stays fast under the sanitizers too.
rs_pt="THEQUICKBROWNFOXJUMPSOVERTHELAZYDOGANDTHENRANAWAYINTOTHEDARKFORESTNEARTHERIVERWHERETHEWATERWASCOLD"
rs_ct=$(run "$rs_pt" -i -u B -w 123 -r AAZ -g XKP)
# K=26 is the ceiling and the degenerate case: the coarse pass tests a SINGLE ring2
# value and the refinement carries all 25 others, so it exercises the refinement path
# with the least possible help from the coarse search. K=14 is the first stride past
# the old ceiling of 13. Both are cheaper than the smaller strides, not dearer.
for K in 2 3 5 14 26; do
  check "crack: --ring-stride $K recovers exact key" \
    "$(run "$rs_ct" -q -l english -u B -w 123 --ring-stride "$K" -T 1)" \
    "$rs_pt"
done

# The coarse set is {v < 26 : v = 0 mod K}, so it holds two values for every K in
# 13..25 and the key count is FLAT across them -- K=14 is not cheaper than K=13, and
# neither is dearer than K=5. A regression that made a large K enumerate more than it
# should would show up here as a rising count.
rs_keys() { printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "A.." -g "A.." \
            --ring-stride "$1" -T 1 2>&1 >/dev/null \
            | grep -oE 'Analysed [0-9]+' | awk '{print $2}'; }
check "--ring-stride key count is flat over K=13..25" \
  "$(test "$(rs_keys 13)" = "$(rs_keys 25)" && echo same)" "same"
check "--ring-stride key count falls monotonically to the ceiling" \
  "$(awk -v a="$(rs_keys 5)" -v b="$(rs_keys 13)" -v c="$(rs_keys 26)" \
     'BEGIN{print (a > b && b > c) ? "yes" : "no"}')" "yes"

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
# winner (archived/PERFORMANCE.md §7.11). The narrow window rested on the coarse winner landing
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

# The stride used to be a NET LOSS on a keyspace with a single wheel order and start0
# pinned: the refinement re-searched all 25 skipped ring2 values over ring1 x start1 x
# start2, which outweighed the 26/K the coarse pass saved, and the tool warned about it.
# Deriving the refinement's offsets instead of enumerating them (archived/refinement.md) shrank it
# from 25 x 130 x 26 to 25 x the start1 range, which inverts every one of those cases into
# a win -- so the warning was removed and these check the inversion instead. Verified
# against the pre-derivation binary, where the first of these cost MORE than not striding.
rs_cost() { printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "$1" -g "$2" \
            ${3:+--ring-stride "$3"} -T 1 2>&1 >/dev/null \
            | grep -oE 'Analysed [0-9]+' | awk '{print $2}'; }
for rs_case in "AA. AA." "A.. A.." "AA. ..."; do
  # shellcheck disable=SC2086  # intentional word-splitting into positional params
  set -- $rs_case
  rs_base=$(rs_cost "$1" "$2")
  for rs_k in 2 5 13; do
    rs_str=$(rs_cost "$1" "$2" "$rs_k")
    check "--ring-stride $rs_k is a net win on -r $1 -g $2" \
      "$(awk -v a="$rs_str" -v b="$rs_base" 'BEGIN{print (a < b) ? "win" : "loss"}')" "win"
  done
done
# The removed warning must be gone for good, not merely quiet: a later change that
# reinstates a cost model priced on the old enumerated refinement would start firing on
# runs that are now wins.
rs_w=$(printf '%s' "$rs_ct" | "$ENIGMA" -q -l english -u B -w 123 -r "AA." -g "AA." \
       --ring-stride 2 -T 1 2>&1 >/dev/null | grep -c 'not paying for itself')
check "--ring-stride does not warn on a keyspace it now wins" "$rs_w" "0"

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

# The refinement bands the middle wheel by OFFSET (start1 - ring1), not by raw ring1.
# Under §7.12 the reported ring1/start1 are class representatives, so raw ring1 wanders
# while the offset barely moves -- a first attempt banded raw ring1 and lost keys because
# of it. These two authentic-Wehrmacht keys are the measured counterexamples: their winning
# ring1 sits 5 and 6 away from the coarse pass's while the offset is 0 and 1 away, so a
# raw-ring1 band of any plausible width drops them and an offset band keeps them. Both
# verified to FAIL against a raw-ring1-banded build and pass here, so they guard the axis
# choice rather than restate it.
for key in "B 134 ASR AQD" "B 542 AGD ARA"; do
  # shellcheck disable=SC2086  # intentional word-splitting into positional params
  set -- $key
  ob_ct=$(run "$rs_pb_pt" -i -u "$1" -w "$2" -r "$3" -g "$4" -s "$rs_pb_board")
  check "crack: --ring-stride 2 keeps an off-centre middle ring ($1 $2 $3 $4)" \
    "$(run "$ob_ct" -f -l wehrmacht -u "$1" -w "$2" -r "A.." -g "A.." \
          -s "$rs_pb_board" --ring-stride 2 -T 1)" \
    "$rs_pb_pt"
done

# Validation: illegal K, a non-wildcarded ring2/start2, and -F/--exhaust all fail fast
# with a clear error rather than silently misbehaving.
rs_err=$(printf 'AAAA' | "$ENIGMA" --ring-stride 0 2>&1 >/dev/null)
check "--ring-stride 0 rejected" "$(printf '%s' "$rs_err" | grep -c 'Illegal ring stride')" "1"
rs_err=$(printf 'AAAA' | "$ENIGMA" --ring-stride 27 2>&1 >/dev/null)
check "--ring-stride 27 rejected" "$(printf '%s' "$rs_err" | grep -c 'Illegal ring stride')" "1"
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

# The refinement DERIVES ring1/start2/ring0 from the coarse winner's step schedules rather
# than banding them (archived/refinement.md), so the cases that exercise the STEPPING are the ones
# that exercise the derivation. None of the four below existed before it.

# M4. The refinement reuses tasks[cur_wo] verbatim, and that task carries the Greek wheel
# and its offset; a version that rebuilt the task from the machine's fields would drop
# them. Exactly like the Norway index-translation bug above, that is invisible in standard
# mode, so the whole matrix would pass over a broken -4 path.
m4_ct=$(run "$rs_pt" -4 -i -u b -w B123 -r AAAZ -g AXKP)
check "crack: M4 + --ring-stride 2 recovers exact key" \
  "$(run "$m4_ct" -4 -q -l english -u b -w B123 -r "AAA." -g "A..." --ring-stride 2 -T 1)" \
  "$rs_pt"

# Two-notch right wheel (VI, notches M and Z). The middle wheel then steps twice per
# revolution, so its turnover set is a union of TWO lattices: the step-count delta the
# derivation computes can reach 2 where a single-notch wheel bounds it at 1, and §7.12's
# class count changes from ceil(L/26)+1 to ceil(L/13)+1. Nothing else in the suite covers
# a two-notch wheel under --ring-stride, and neither do the measurements behind
# archived/refinement.md, which draw wheels from I-V only.
tn_ct=$(run "$rs_pt" -i -u B -w 126 -r AAZ -g XKP)
for rs_k in 2 3 13; do
  check "crack: --ring-stride $rs_k with a two-notch right wheel" \
    "$(run "$tn_ct" -q -l english -u B -w 126 --ring-stride "$rs_k" -T 1)" "$rs_pt"
done

# A turnover at character 1 AND a double step -- the two stepping phenomena the derivation
# exists for. Wheels 123: middle II (own notch E), right III (notch V). start2 = V puts the
# right wheel ON its notch, so the middle wheel steps at character 1: this is the case where
# a ring2 shift carries a turnover across the START of the message and changes the step
# count for the WHOLE message rather than for a delta-length window (archived/refinement.md §3).
# start1 = C reaches E after two middle steps, so the middle wheel lands on its own notch
# and the LEFT wheel steps too, exercising the left-wheel derivation that is otherwise
# never reached. Verified: this key steps wheel 0 once, the XKP key above not at all.
ds_ct=$(run "$rs_pt" -i -u B -w 123 -r AAZ -g ACV)
for rs_k in 2 3 5; do
  check "crack: --ring-stride $rs_k with a double step and a turnover at char 1" \
    "$(run "$ds_ct" -q -l english -u B -w 123 --ring-stride "$rs_k" -T 1)" "$rs_pt"
done

# The derivation must actually respond to the schedule, not emit a fixed candidate set.
# Where the left wheel steps, the left-wheel delta is non-trivial and each candidate splits,
# so the refinement is strictly larger than where it does not -- same keyspace, same K, same
# message length, differing only in the true start position. A build that banded or pinned
# instead of deriving would produce identical counts.
rs_refine() { printf '%s' "$1" | "$ENIGMA" -q -l english -u B -w 123 -r "AA." -g "..." \
              --ring-stride 2 -T 1 2>&1 >/dev/null \
              | grep -oE 'Analysed [0-9]+' | awk '{print $2}'; }
check "--ring-stride derivation responds to the stepping schedule" \
  "$(awk -v a="$(rs_refine "$ds_ct")" -v b="$(rs_refine "$rs_ct")" \
     'BEGIN{print (a > b) ? "bigger" : "same-or-smaller"}')" "bigger"

# ring1 is DERIVED only where the caller left it wildcarded; with it pinned, every start1
# in the sweep already carries a determined offset and the sweep covers them all. -r AA. is
# the tool's own default, so getting this backwards breaks the common case rather than an
# exotic one. All four combinations of (ring1, start1) pinned/wildcarded must recover.
for rs_pin in "AA. ..." "A.. ..." "AA. .C." "A.. .C."; do
  # shellcheck disable=SC2086  # intentional word-splitting into positional params
  set -- $rs_pin
  check "crack: --ring-stride 2 with -r $1 -g $2" \
    "$(run "$ds_ct" -q -l english -u B -w 123 -r "$1" -g "$2" --ring-stride 2 -T 1)" \
    "$rs_pt"
done

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

# --tune-phase N: hill-climb the rotor PHASE (the middle and right rings, with
# the offsets held) instead of enumerating it.
#
# Two costs drive every check here and both are kept as small as the property
# allows. (1) Message length: the capture radius grows with it, so the
# ~100-letter plaintexts the rest of the file uses leave nothing to find -- 140
# letters with
# the true ring 2 and 1 position off the starting phase is the cheapest setup
# that still does. (2) Climbs: each one pays a 26x26 phase scan, and the option
# requires ring1/ring2/start1/start2 wildcarded, so 676 keys is the smallest
# legal keyspace. Hence ONE run below covers four properties at once rather than
# four runs covering one each.
tp_pt="ENIGMAVARIANTREFLECTORNWHEELSANDTHEFOURROTORNAVALMTHINREFLECTORSUKWBCPLUSTHEBETAGAMMAGREEKWHEELTHESETTINGSARETHEREFLECTORUMKEHRWALZETHEWHEEL"
tp_board="ABCDEFGHIJKLMNOPQRST"
tp_ct=$(run "$tp_pt" -i -u B -w 231 -r ACB -g QMW -s "$tp_board")

# The true ring is ACB; --tune-phase 1 offers a SINGLE starting phase (ring1 =
# ring2 = A) and has to find C/B by scanning with the board frozen. The board is
# given with -s so this is about the phase and not about plugboard recovery.
# This one run establishes four things:
#   * it recovers the plaintext at all;
#   * the key it ECHOES is the tuned one, not the phase the climb started from
#     -- tune_phase() leaves the machine on the winning phase, so the merge must
#     not restore the key its work index encodes, and must not record the start
#     positions setup_mapping stepped (a first version echoed a start one
#     left-wheel step past the truth);
#   * that key actually decrypts to what was printed (the end-to-end version,
#     which catches a stale key from any source without knowing which);
#   * --polish did not re-derive the key from best.idx, which no longer
#     identifies it.
tp_errf=$(mktemp)
tp_out=$(printf '%s' "$tp_ct" | "$ENIGMA" -f -l english -c -u B -w 231 \
         -r "A.." -g "Q.." -s "$tp_board" --tune-phase 1 -R 2 --polish -T 1 \
         2>"$tp_errf")
tp_line=$(grep -E '^ *-?[0-9]+\.' "$tp_errf" | tail -1)
rm -f "$tp_errf"
check "crack: --tune-phase 1 tunes the middle and right rings" \
  "$tp_out" "$tp_pt"
# Only the RIGHT wheel's phase is asserted. The middle wheel's ring x start is
# a §7.12 equivalence class at this length -- the middle wheel never reaches its
# notch in 140 characters, so ring1=A/start1=K and the true ring1=C/start1=M
# decode identically and either may be reported. The right wheel's phase is
# identifiable, so tuning it from the starting A to the true B is the assertion
# with content here.
check "--tune-phase reports the tuned key" \
  "$(printf '%s' "$tp_line" \
     | awk '{print $2, substr($3,3,1), substr($4,3,1)}')" "B231 B W"
check "--tune-phase: the echoed key reproduces the printed plaintext" \
  "$(run "$tp_ct" -u B -w 231 \
        -r "$(printf '%s' "$tp_line" | awk '{print $3}')" \
        -g "$(printf '%s' "$tp_line" | awk '{print $4}')" \
        -s "$(printf '%s' "$tp_line" \
              | awk '{s="";for(i=5;i<NF;i++)s=s $i;print s}')")" \
  "$tp_out"
# -T-independent like every other search option -- and with -R > 1 that is not
# free: tune_phase() leaves the machine on the phase IT found, so restart 1
# would start from restart 0's phase unless the key is rebuilt per work item,
# which
# both loses the independence -R relies on and makes the result depend on which
# thread ran which restart.
check "--tune-phase is -T-independent with restarts" \
  "$(run "$tp_ct" -f -l english -c -u B -w 231 -r "A.." -g "Q.." \
        -s "$tp_board" --tune-phase 1 -R 2 --polish -T 4)" \
  "$tp_out"
# The control: without the tuning, that same starting phase (ring pinned AAA)
# cannot reach the truth, so the checks above measure the feature and not the
# keyspace. No phase scan, so this run is ~60x cheaper than the two above.
check "--tune-phase control: the fixed starting phase does not recover" \
  "$(test "$(run "$tp_ct" -f -l english -c -u B -w 231 -r "AAA" -g "Q.." \
            -s "$tp_board" -T 1)" = "$tp_pt" && echo recovered || echo no)" \
  "no"

# --tune-phase makes the search APPROXIMATE, so it is echoed in the settings for
# the same reason --ring-stride is. show_settings() runs before the search, so
# neither the keyspace nor the ciphertext matters here: the option needs ring1
# and ring2 wildcarded, so the cheapest legal run is that keyspace over a
# 4-letter input rather than the 140-letter one the recovery checks need.
tp_echo=$(printf 'AAAA' | "$ENIGMA" -q -l english -c -u B -w 231 \
          -r "A.." -g "A.." --tune-phase 2 -T 1 2>&1 >/dev/null)
check "--tune-phase is echoed in the settings" \
  "$(printf '%s' "$tp_echo" | grep -c '^Phase: .*starting phase')" "1"
tp_echo=$(printf 'AAAA' | "$ENIGMA" -q -l english -c -u B -w 231 \
          -r "A.." -g "A.." -T 1 2>&1 >/dev/null)
check "no phase line when --tune-phase is off" \
  "$(printf '%s' "$tp_echo" | grep -c '^Phase:')" "0"

# Validation. Each of these means the command line says something the search
# cannot honour, so it fails fast rather than quietly doing something else.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
tp_bad() { printf 'AAAA' | "$ENIGMA" -q -l english "$@" 2>&1 >/dev/null; }
tp_err=$(printf 'AAAA' | "$ENIGMA" --tune-phase 27 2>&1 >/dev/null)
check "--tune-phase 27 rejected" \
  "$(printf '%s' "$tp_err" | grep -c 'Illegal phase count')" "1"
tp_err=$(tp_bad --tune-phase 2)
check "--tune-phase needs -c" \
  "$(printf '%s' "$tp_err" | grep -c 'needs the plugboard hill-climb')" "1"
tp_err=$(tp_bad -c -r "AAA" -g "..." --tune-phase 2)
check "--tune-phase needs the middle/right rings wildcarded" \
  "$(printf '%s' "$tp_err" | grep -c 'tune-phase needs')" "1"
tp_err=$(tp_bad -c -r "A.." -g "..." --tune-phase 2 --ring-stride 2)
check "--tune-phase rejects --ring-stride" \
  "$(printf '%s' "$tp_err" | grep -c 'ring-stride are alternatives')" "1"
tp_err=$(tp_bad -c -r "A.." -g "..." --tune-phase 2 -F 100)
check "--tune-phase rejects -F" \
  "$(printf '%s' "$tp_err" | grep -c 'tune-phase is not supported with -F')" "1"
tp_err=$(tp_bad -c -r "A.." -g "..." --tune-phase 2 --crib HELLO)
check "--tune-phase rejects --crib" \
  "$(printf '%s' "$tp_err" | grep -c 'not supported with --crib')" "1"
tp_err=$(tp_bad -c -r "A.." -g "..." --tune-phase 2 -A 1000)
check "--tune-phase rejects -A" \
  "$(printf '%s' "$tp_err" | grep -c 'tune-phase is not supported with -A')" "1"

# --doubling-report L: report a converged climb whose decrypt carries a doubled word
# around an X ("ENGELMANN X ENGELMANN"), telegraphic German's own error
# correction.  A CONFIRMATION signal -- it never enters a ranking -- so the
# properties are that it fires on a real doubling at the true key, that it
# respects L, and that it is refused without the two things that define it (-c,
# because there must be a converged climb, and --confidence, because that is
# what defines z).
#
# The board is PINNED with -s plus --no-plug on every other letter, so the climb
# has nothing to recover and one small sweep suffices: the property under test is
# the detector and its gate, not plugboard recovery.  Keeping the key space at
# 676 rather than 26 is not decoration -- z is measured against a null sampled
# from that space, and the true key cannot stand 3 sd clear of a space barely
# bigger than the sample.
dw_pt="SNXHAUPTSTUFXOBERSCHARFXENGELMANNXENGELMANNXZURUEQXUSTUFXERBXERBXWIRDVONMIREINGEWIESENFAHREHEUTEZURARMEEXKOMMEMORGENZURXDIVISION"
dw_free="GHIJKLMNOPQRSTUVWXYZ"
dw_ct=$(run "$dw_pt" -u B -w 231 -r AAA -g QMW -s "AB CD EF")
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
dw_run() { printf '%s' "$dw_ct" | "$ENIGMA" -c -f -l wehrmacht -u B -w 231 \
           -r AAA -g "Q.." -R 1 -T "$2" -s "AB CD EF" --no-plug "$dw_free" \
           --confidence 32 --doubling-report "$1" ${3:+--doubling-z "$3"} 2>&1 >/dev/null; }
dw_out=$(dw_run 6 1)
check "--doubling-report reports the doubled word and its length" \
  "$(printf '%s' "$dw_out" | grep -c '>> 9 ENGELMANN')" "1"
check "--doubling-report report carries the true rotor key" \
  "$(printf '%s' "$dw_out" | grep '>> 9 ENGELMANN' | awk '{ print $2, $3, $4 }')" \
  "B231 AAA QMW"
# L is the whole cheap lever (chance reports fall ~16x per extra letter), so a
# threshold above the longest real doubling must silence it completely.
# Anchored on the LINE shape, not on the marker alone: the settings echo says
# 'marked ">>" below', so a bare '>>' grep counts the echo and can never read 0.
check "--doubling-report L above the doubling is silent" \
  "$(dw_run 10 1 | grep -cE '>> [0-9]+ [A-Z]')" "0"
# Display-only: -T changes thread timing, not what is found.
check "--doubling-report is reported under -T 4 as well" \
  "$(dw_run 6 4 | grep -c '>> 9 ENGELMANN')" "1"
# Identical repeats are collapsed (one call per converged restart plus one after
# --polish would otherwise print the same row R+1 times).
check "--doubling-report collapses an identical repeat" \
  "$(printf '%s' "$dw_ct" | "$ENIGMA" -c -f -l wehrmacht -u B -w 231 -r AAA \
     -g "Q.." -R 4 -T 1 -s "AB CD EF" --no-plug "$dw_free" --polish \
     --confidence 32 --doubling-report 6 2>&1 >/dev/null | grep -c '>> 9 ENGELMANN')" \
  "1"
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english --doubling-report 6 2>&1 >/dev/null)
check "--doubling-report needs -c" \
  "$(printf '%s' "$dw_err" | grep -c 'need the plugboard hill-climb')" "1"
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -c --doubling-report 6 2>&1 >/dev/null)
check "--doubling-report needs --confidence" \
  "$(printf '%s' "$dw_err" | grep -c 'need a null to gate on')" "1"
# The scan is capped at 30 -- the longest doubling in the authentic corpus is 13
# and nothing reaches 14, so this is 2.3x anything observed; the cap exists to
# keep the cost O(30n) rather than O(n^2), and a tighter 20 was tried and did
# not resolve against a base-vs-base control.  L is validated against the SAME
# constant, so a too-large L is refused rather than silently searching nothing.
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -c --confidence 8 \
         --doubling-report 31 2>&1 >/dev/null)
check "--doubling-report past the scan's own cap is refused" \
  "$(printf '%s' "$dw_err" | grep -c 'Illegal doubling length')" "1"
# The key is PINNED here: unlike the rejection above, a valid --doubling-report
# does not exit at validation, so without it this check would start a full
# default wildcard search under -c and hang the suite (it did).
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -c --confidence 8 \
         -u B -w 123 -r AAA -g AAA --doubling-report 30 2>&1 >/dev/null)
check "--doubling-report at the cap is accepted" \
  "$(printf '%s' "$dw_err" | grep -c 'Illegal doubling length')" "0"
# --doubling-z moves the gate.  The true key here sits far above it (z = 7..16
# once the climb has the plaintext), so raising the gate past that silences the
# report -- which is the assertion that the gate is actually consulted rather
# than the default being hard-wired.
check "--doubling-z above the true key's z silences the report" \
  "$(dw_run 6 1 99 | grep -cE '>> [0-9]+ [A-Z]')" "0"
check "--doubling-z below the default still reports" \
  "$(dw_run 6 1 1 | grep -c '>> 9 ENGELMANN')" "1"
check "--doubling-z is echoed in the settings" \
  "$(dw_run 6 1 2.5 | grep -c 'at z >= 2.5')" "1"
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -c --confidence 8 \
         --doubling-report 6 --doubling-z junk 2>&1 >/dev/null)
check "--doubling-z rejects a non-number (atof would read it as 0)" \
  "$(printf '%s' "$dw_err" | grep -c 'Illegal value for --doubling-z')" "1"
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -c --confidence 8 \
         --doubling-z 4 2>&1 >/dev/null)
check "--doubling-z without --doubling-report is refused" \
  "$(printf '%s' "$dw_err" | grep -c 'which is not on')" "1"
# --doubling-mismatches.  ENGELMANN X ENGELMANN is exact, so N=0 must still find
# it; the interesting direction is that N is CONSULTED, which the vacuous-value
# refusal and the echo establish without needing a garbled fixture.
check "--doubling-mismatches 0 still finds an exact doubling" \
  "$(printf '%s' "$dw_ct" | "$ENIGMA" -c -f -l wehrmacht -u B -w 231 -r AAA \
     -g "Q.." -R 1 -T 1 -s "AB CD EF" --no-plug "$dw_free" --confidence 32 \
     --doubling-report 6 --doubling-mismatches 0 2>&1 >/dev/null \
     | grep -c '>> 9 ENGELMANN')" "1"
check "--doubling-mismatches is echoed in the settings" \
  "$(printf '%s' "$dw_ct" | "$ENIGMA" -c -f -l wehrmacht -u B -w 231 -r AAA \
     -g "Q.." -R 1 -T 1 -s "AB CD EF" --no-plug "$dw_free" --confidence 32 \
     --doubling-report 6 --doubling-mismatches 2 2>&1 >/dev/null \
     | grep -c 'up to 2 mismatches')" "1"
# At N >= L every pair of equal-length X-free runs matches, so the test stops
# testing anything.  Refused rather than run.
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -c --confidence 8 \
         --doubling-report 6 --doubling-mismatches 6 2>&1 >/dev/null)
check "--doubling-mismatches at or above L is refused as vacuous" \
  "$(printf '%s' "$dw_err" | grep -c 'must be below')" "1"
dw_err=$(printf 'AAAA' | "$ENIGMA" -q -l english -c --confidence 8 \
         --doubling-mismatches 2 2>&1 >/dev/null)
check "--doubling-mismatches without --doubling-report is refused" \
  "$(printf '%s' "$dw_err" | grep -c 'which is not on')" "1"
# --full-text applies to the doubling report too: the report says a doubling is
# present and the whole decrypt is what lets the reader judge it, so the option
# must not skip the one line most worth expanding.  Counted as "more
# continuation lines with the report than without", which holds whatever the
# message length makes the block size.
dw_ft() { printf '%s' "$dw_ct" | "$ENIGMA" -c -f -l wehrmacht -u B -w 231 \
  -r AAA -g "Q.." -R 1 -T 1 -s "AB CD EF" --no-plug "$dw_free" --confidence 32 \
  --full-text "$@" 2>&1 >/dev/null | grep -c '^  [A-Z]*$'; }
check "--full-text expands the doubling report as well" \
  "$(test "$(dw_ft --doubling-report 6)" -gt "$(dw_ft)" && echo more)" "more"

# --confidence N: sample the null and report the winner's margin over chance.
# The property under test is DISCRIMINATION, so every check comes in a pair -- a
# ciphertext with real plaintext behind it against one with random letters
# behind it, same key, same keyspace. A line that only fired on the signal case
# would pass a build that always printed "significant".
cf_pt="ANXPANZXGRUPPEXVIERXSIEGFRIEDXTONIXDIVXSTEHTSEITXEINSZWOXSIEBENXEINSEINSNULLNULLXUHRMITANFAENGENAMUNTERKUNFTSRAUMXMELDUNGFOLGT"
cf_rnd="QHZKWMPXDVBNTLFGYRJUACOISEQWNBVXZLKMPTRDYFGHJUAICOESQZXWNMBVKLPTRDYGFHJUAICOESZQXWNVMBKLPRTDYGFHJUIACOESZQXWNVMBKLPRTDYGFHJUI"
cf_ct=$(run "$cf_pt" -i -u B -w 123 -r AAA -g AQD)
cf_rct=$(run "$cf_rnd" -i -u B -w 123 -r AAA -g AQD)
# -l wehrmacht, matching the telegraphic plaintext: the margin measures the
# model against the text, so a mismatched language understates it (measured
# +15.4 sd for wehrmacht against +2.5 for english on this very message -- the
# ranking check below turns that into an assertion rather than a footnote).
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
# Anchored on "Confidence: null", the SUMMARY, not on the label alone: the
# settings echo carries the same label, and its two following lines include
# "Threads: N" -- which made a bare '^Confidence' capture read as a -T
# difference the moment that echo was added.
cf_run() { printf '%s' "$1" | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA \
           -g "..." --confidence 64 -T 1 2>&1 >/dev/null \
           | grep -A2 '^Confidence: null'; }
cf_margin() { printf '%s' "$1" \
              | sed -n 's/.*margin \([+-][0-9.]*\) sd.*/\1/p'; }
cf_sig=$(cf_run "$cf_ct")
cf_noise=$(cf_run "$cf_rct")
# The progress lines print a MARGIN; a reader watching them cannot convert it
# back to the raw sigma count every other account of a result is quoted in
# unless the offset is stated up front.  Reported BEFORE the sweep (the summary
# repeats it afterwards), and it must be the same number in both places.
cf_bar=$(printf '%s' "$cf_ct" | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA \
         -g "..." --confidence 64 -T 1 2>&1 >/dev/null)
check "--confidence states the z that margin 0 corresponds to" \
  "$(printf '%s' "$cf_bar" | grep -c '^Confidence: margin 0 is z = ')" "1"
cf_bar_pre=$(printf '%s' "$cf_bar" \
  | sed -n 's/^Confidence: margin 0 is z = \([0-9.]*\),.*/\1/p')
cf_bar_post=$(printf '%s' "$cf_bar" \
  | sed -n 's/.*chance best of [0-9]* keys is \([0-9.]*\) sd.*/\1/p')
check "the pre-sweep bar matches the one the summary reports" \
  "$cf_bar_pre" "$cf_bar_post"

check "--confidence: a real plaintext clears chance by a wide margin" \
  "$(awk -v m="$(cf_margin "$cf_sig")" \
     'BEGIN{print (m > 5) ? "yes" : "no"}')" "yes"
check "--confidence: signal-free ciphertext does NOT clear chance" \
  "$(awk -v m="$(cf_margin "$cf_noise")" \
     'BEGIN{print (m < 2) ? "yes" : "no"}')" "yes"
# The null it measures must be the model's, not something rescaled per run: quad
# on random text sits near -8.0 whatever the ciphertext (measured -7.99 +/- 0.17
# over 250 offline samples), so both arms must agree on it.
cf_null() { printf '%s' "$1" \
            | sed -n 's/^Confidence: null \(-*[0-9.]*\) .*/\1/p'; }
check "--confidence: the sampled null is the model's, not the message's" \
  "$(awk -v a="$(cf_null "$cf_sig")" -v b="$(cf_null "$cf_noise")" \
     'BEGIN{d=a-b; if (d<0) d=-d; print (d < 0.5) ? "close" : "far"}')" "close"

# The margin ranks the scoring LANGUAGES on one message, which is the flag's
# second use: this plaintext is telegraphic German, and CLAUDE.md's advice is
# wehrmacht for real traffic, german for prose, english not at all. The margins
# must order the same way, or it is not measuring the model against the text.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_lang() { printf '%s' "$cf_ct" | "$ENIGMA" -q -l "$1" -u B -w 123 -r AAA \
            -g "..." --confidence 64 -T 1 2>&1 >/dev/null \
            | sed -n 's/.*margin \([+-][0-9.]*\) sd.*/\1/p'; }
check "--confidence: the margin ranks wehrmacht > german > english here" \
  "$(awk -v w="$(cf_lang wehrmacht)" -v g="$(cf_lang german)" \
       -v e="$(cf_lang english)" \
     'BEGIN{print (w > g && g > e) ? "ordered" : "not-ordered"}')" "ordered"

# Under -c the samples must be CLIMBED, or a climbed search is calibrated
# against a scanned null and everything looks significant. The climbed null sits
# well above the scanned one on the same ciphertext, which is what this asserts.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_c_null=$(printf '%s' "$cf_ct" | "$ENIGMA" -q -l wehrmacht -c -u B -w 123 \
            -r AAA -g "AQ." --confidence 16 -T 1 2>&1 >/dev/null \
            | grep '^Confidence')
check "--confidence: -c calibrates against a CLIMBED null" \
  "$(awk -v a="$(cf_null "$cf_c_null")" -v b="$(cf_null "$cf_sig")" \
     'BEGIN{print (a > b + 0.3) ? "higher" : "not-higher"}')" "higher"

# IC's null is right-skewed on top of the tail error every model has, so the
# Gaussian form understates its best-of-K worst of all (6.1 sigma observed
# against 4.4 predicted). It earns an EXTRA clause the other models do not get.
# Asserted as the property -- one caveat for -q, a longer one for -i -- rather
# than by grepping a particular word, which is what made this check fail when
# the wording changed while the behaviour did not.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_pline() { printf '%s' "$cf_ct" | "$ENIGMA" "$@" -u B -w 123 -r AAA -g "..." \
             --confidence 64 -T 1 2>&1 >/dev/null | grep '(Gaussian tail'; }
check "--confidence: -i's p-value carries a longer caveat than -q's" \
  "$(awk -v a="$(cf_pline -i)" -v b="$(cf_pline -q -l wehrmacht)" \
     'BEGIN{print (length(a) > length(b)) ? "yes" : "no"}')" "yes"
check "--confidence: -i names itself in the caveat" \
  "$(cf_pline -i | grep -c 'IC')" "1"
# The Gaussian tail understates the false-positive rate near zero for EVERY
# model, not only IC: measured on 2000 signal-free ciphertexts at L=200,
# K=17576, a margin of +0.54 came up 2.35% of the time against the 0.70% the
# p-value implies (eval/confidence_false_positive.py). So the caveat is
# unconditional, and IC only earns an extra clause.
cf_opt() { printf '%s' "$cf_rct" | "$ENIGMA" -u B -w 123 -r AAA \
           -g "..." --confidence 64 -T 1 "$@" 2>&1 >/dev/null \
           | grep -c 'optimistic near zero'; }
check "--confidence: the p-value is flagged optimistic for quad too" \
  "$(cf_opt -q -l wehrmacht)" "1"
check "--confidence: the p-value is flagged optimistic for IC" \
  "$(cf_opt -i)" "1"
# The "not a find" note is the actionable half, and it has to fire exactly where
# the p-value misleads and nowhere else. Signal-free text lands near zero and
# must get it; a real break reads +15 to +17 sd and must not, or the line would
# be noise on every successful run.
cf_note() { printf '%s' "$1" | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA \
            -g "..." --confidence 64 -T 1 2>&1 >/dev/null \
            | grep -c 'below +2 sd is not a find'; }
check "--confidence: signal-free text gets the 'not a find' note" \
  "$(cf_note "$cf_rct")" "1"
check "--confidence: a real break does NOT get the 'not a find' note" \
  "$(cf_note "$cf_ct")" "0"
# NO confidence line may look like a progress line, for exactly the reason the
# pre-flight block asserts the same of its own: the documented way to pull a
# run's margin off stderr is to grep '^ *[+-][0-9]', and this note used to wrap
# as "... a margin of\n            +0.5 sd came up ...".  That continuation IS
# such a line, so an extractor read the CAVEAT back as the run's result -- and
# silently, since it looks like a plausible margin.  Found when a sweep of 33
# known day keys reported "+0.5 sd came up in 2-5% of runs" for every one.
# Anchored on the signal-free arm, the only one that prints the note.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
check "confidence summary lines cannot be read as progress lines" \
  "$(printf '%s' "$cf_rct" | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA \
     -g "..." --confidence 64 -T 1 2>&1 >/dev/null \
     | sed -n '/^Confidence: null/,$p' \
     | grep -cE '^ *[+-]?[0-9]')" "0"
check "confidence summary stays within 80 columns" \
  "$(printf '%s' "$cf_rct" | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA \
     -g "..." --confidence 64 -T 1 2>&1 >/dev/null \
     | sed -n '/^Confidence: null/,$p' | awk '{ print length }' \
     | sort -rn | head -1 | awk '{ print ($1 <= 80) ? "ok" : $1 }')" "ok"
check "--confidence: -q does not carry the IC clause" \
  "$(cf_pline -q -l wehrmacht | grep -c 'IC')" "0"

check "--confidence is -T-independent" \
  "$(cf_run "$cf_ct")" \
  "$(printf '%s' "$cf_ct" \
     | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA -g "..." \
       --confidence 64 -T 4 2>&1 >/dev/null | grep -A2 '^Confidence: null')"
check "no confidence line when the option is off" \
  "$(printf '%s' "$cf_ct" \
     | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA -g "..." -T 1 \
       2>&1 >/dev/null | grep -c '^Confidence')" "0"
# The PROGRESS LINES carry the margin, not the raw score, and the header says
# so -- the point being that zero is the meaningful line: negative means the
# board is no better than what the whole sweep produces by luck. Both arms
# again: a column that only behaved on the signal case would be worse than the
# raw score it replaced.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_lines() { printf '%s' "$1" | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA \
             -g "..." --confidence 64 -T 1 2>&1 >/dev/null; }
cf_sig_l=$(cf_lines "$cf_ct")
cf_noise_l=$(cf_lines "$cf_rct")
check "--confidence renames the score column to Margin" \
  "$(printf '%s' "$cf_sig_l" | grep -c '^ *Margin W')" "1"
check "no Margin column when --confidence is off" \
  "$(printf '%s' "$cf_ct" \
     | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA -g "..." -T 1 \
       2>&1 >/dev/null | grep -c '^ *Score W')" "1"
# The winning line must cross zero on signal and must NOT on noise.
cf_last() { printf '%s' "$1" | grep -E '^ *[+-][0-9]' | tail -1 \
            | awk '{print $1}'; }
check "--confidence: the winning progress line crosses zero on signal" \
  "$(awk -v m="$(cf_last "$cf_sig_l")" \
     'BEGIN{print (m > 5) ? "yes" : "no"}')" "yes"
check "--confidence: no progress line crosses zero far on noise" \
  "$(awk -v m="$(cf_last "$cf_noise_l")" \
     'BEGIN{print (m < 2) ? "yes" : "no"}')" "yes"
# Monotone in the score, so the merge order is untouched: the margins a run
# prints must be strictly increasing, exactly as the raw scores were.
check "--confidence: margins increase down the run, as scores did" \
  "$(printf '%s' "$cf_sig_l" | grep -E '^ *[+-][0-9]' | awk '{print $1}' \
     | awk 'NR>1 && $1<=p {bad=1} {p=$1} END{print bad ? "no" : "yes"}')" "yes"
# The 80-column budget holds with the new column and a full 13-pair board.
check "--confidence: progress lines stay within 80 columns" \
  "$(printf '%s' "$cf_ct" \
     | "$ENIGMA" -q -l wehrmacht -u B -w 123 -r AAA -g "..." \
       -s ABCDEFGHIJKLMNOPQRSTUVWXYZ --confidence 64 -T 1 2>&1 >/dev/null \
     | awk 'length($0) > 80' | wc -l | tr -d ' ')" "0"
# --dump-all keeps RAW scores and must be untouched by the calibration in both
# senses. It is the machine-readable form the harnesses parse, so a margin
# there -- or an extra row per calibration sample -- would silently change what
# every probe measures. The row COUNT catches the second: hillclimb_one() dumps
# unconditionally, so the sampling climbs landed in the diagnostic until it was
# suppressed for them (16 spurious rows at --confidence 16).
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_dump() { printf '%s' "$cf_ct" | "$ENIGMA" -q -l wehrmacht -c -u B -w 123 \
            -r AAA -g "AQ." --dump-all "$@" -T 1 2>&1 >/dev/null; }
check "--dump-all keeps raw scores under --confidence" \
  "$(cf_dump --confidence 16 | grep -c '^dumpall .* -[0-9]')" \
  "$(cf_dump --confidence 16 | grep -c '^dumpall ')"
check "--confidence adds no rows to --dump-all" \
  "$(cf_dump --confidence 16 | grep -c '^dumpall ')" \
  "$(cf_dump | grep -c '^dumpall ')"

cf_err=$(printf 'AAAA' | "$ENIGMA" --confidence -1 2>&1 >/dev/null)
check "--confidence negative rejected" \
  "$(printf '%s' "$cf_err" | grep -c 'Illegal sample count')" "1"

# --confidence changes what the first column MEANS, so the settings echo has to
# say so: a reader joining a saved log at the progress lines cannot otherwise
# tell a margin from a raw score, and on the same run the two differ by ~20.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_set() { printf '%s' "$cf_ct" | "$ENIGMA" -q -l wehrmacht -u B -w 123 \
           -r AAA -g "AQ." "$@" -T 1 2>&1 >/dev/null; }
check "--confidence is echoed in the settings" \
  "$(cf_set --confidence 32 | grep -c '^Confidence: 32 null samples')" "1"
check "no confidence setting line when the option is off" \
  "$(cf_set | grep -c '^Confidence:')" "0"
# Under -c each sample is a whole plugboard climb rather than one score, which
# is the difference between free and seconds -- so the echo distinguishes them.
check "--confidence settings line says when samples are climbed" \
  "$(cf_set -c --confidence 32 | grep -c 'null samples, each climbed')" "1"
check "--confidence settings line omits 'climbed' without -c" \
  "$(cf_set --confidence 32 | grep -c 'each climbed')" "0"

# The sampling progress line is a live \r line and must appear ONLY on a TTY,
# so redirected logs and this harness stay clean (the same rule -F's tier-1
# line follows). Nothing here runs on a TTY, so it must never be seen.
check "--confidence sampling progress stays off a redirected stderr" \
  "$(cf_set -c --confidence 32 | grep -c 'sampling the null')" "0"

# A null needs more than one key to be a null. With the rotor key FULLY
# specified the key space is one key, every sample climbs it to the identical
# score, and sd came out as float noise (~1e-15) rather than 0 -- so a bare
# `sd > 0` guard passed and the margin became score/1e-15, about 1e13, which
# also blew the 8-wide first column out to 87 characters. The run must instead
# say there is nothing to calibrate against and fall back to raw scores.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_one=$(printf '%s' "$cf_ct" | "$ENIGMA" -q -l wehrmacht -c -R 1 -u B -w 123 \
         -r AAA -g AQD --confidence 32 -T 1 2>&1 >/dev/null)
check "--confidence: a one-key space reports no null rather than a huge margin" \
  "$(printf '%s' "$cf_one" | grep -c 'sampled keys scored alike')" "1"
check "--confidence: a degenerate null falls back to the raw Score column" \
  "$(printf '%s' "$cf_one" | grep -cE '^ *Score +W')" "1"
check "--confidence: a degenerate null prints no margin summary" \
  "$(printf '%s' "$cf_one" | grep -c 'margin')" "0"
check "--confidence: a degenerate null keeps progress lines within 80 columns" \
  "$(printf '%s' "$cf_one" | awk 'length($0) > 80' | wc -l | tr -d ' ')" "0"

# The key count the summary prints must be the one its own bar was computed
# from. It used to be passed in separately, and under --ring-stride the caller
# passed the REFINEMENT's keys too -- so the line read "chance best of 1528334
# keys is 5.3 sd" with 5.3 built from 1527084. Recomputing sqrt(2 ln K) here
# from the printed K and comparing it to the printed bar ties the two halves of
# the line together whatever the cause.
#
# It catches drift at the size that would CHANGE THE ANSWER, not the 0.00015 sd
# of the instance above -- zk grows as sqrt(ln K), so that one was four orders
# below the printed precision and no output-level check can see it. A K wrong by
# a factor (work items instead of keys, say) moves the bar by ~0.1 sd and does
# get caught. The tolerance is the rounding bound of the one decimal printed.
# (A small key space keeps this quick; the property does not need a big one.)
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
cf_stride() { printf '%s' "$cf_ct" | "$ENIGMA" -q -l wehrmacht -u B -w 123 \
              -r "AA." -g "AA." --confidence 32 -e 1 --ring-stride "$1" \
              -T 1 2>&1 >/dev/null; }
cf_zk_ok() { printf '%s' "$1" | awk '
  /chance best of/ { k = $6; sd = $9 }
  END { if (k == "" || sd == "") { print "missing"; exit }
        d = sqrt(2 * log(k)) - sd
        print (d < 0.06 && d > -0.06) ? "match" : "drift(" d ")" }'; }
cf_keys() { printf '%s' "$1" | sed -n 's/.*chance best of \([0-9]*\) keys.*/\1/p'; }
cf_analysed() { printf '%s' "$1" | sed -n 's/^Analysed \([0-9]*\) rotor.*/\1/p'; }
cf_s1=$(cf_stride 1)
cf_s2=$(cf_stride 2)
check "--confidence: the printed bar matches the printed key count" \
  "$(cf_zk_ok "$cf_s1")" "match"
check "--confidence: the bar matches the key count under --ring-stride too" \
  "$(cf_zk_ok "$cf_s2")" "match"
# Unstrided there is no refinement, so the two counts coincide; strided the
# summary must report the COARSE sweep while the diagnostic reports the total.
check "--confidence: unstrided, the summary counts every analysed key" \
  "$(cf_keys "$cf_s1")" "$(cf_analysed "$cf_s1")"
check "--confidence: strided, the summary excludes the refinement's keys" \
  "$(awk -v c="$(cf_keys "$cf_s2")" -v a="$(cf_analysed "$cf_s2")" \
     'BEGIN{print (c > 0 && a > c) ? "yes" : "no"}')" "yes"

# Live sweep progress: a \r line carrying percentage, rate and ETA. It is
# TTY-only, so nothing this harness runs may ever see it -- a redirected log or
# a parsed stderr stream must be byte-for-byte what it was before the line
# existed. That is the whole assertion available here; the drawing and the
# handoff with the score lines need a pty and are checked outside the suite.
#
# Checked on THREE shapes because the line is armed for the main sweep only: a
# plain scan, a climb (where score lines share the stream), and --dump-all,
# which is excluded outright because its rows are machine-readable and print
# under a different mutex.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
sp_run() { printf '%s' "$cf_ct" | "$ENIGMA" -q -l wehrmacht -u B -w 123 \
           -r AAA -g "A.." "$@" -T 2 2>&1 >/dev/null; }
check "no sweep progress line on a redirected stderr (scan)" \
  "$(sp_run | grep -c 'Progress:')" "0"
check "no sweep progress line on a redirected stderr (climb)" \
  "$(sp_run -c -R 2 | grep -c 'Progress:')" "0"
check "no sweep progress line on a redirected stderr (--dump-all)" \
  "$(sp_run -c -R 2 --dump-all | grep -c 'Progress:')" "0"
# The carriage return is the mechanism, so its ABSENCE is the sharper check:
# a \r escaping into a redirected stream would corrupt any log or parser
# downstream even if the word "Progress:" never appeared.
check "no carriage returns on a redirected stderr" \
  "$(sp_run -c -R 2 | tr -dc '\r' | wc -c | tr -d ' ')" "0"

# Right-wheel ring x start collapse by 13 (CLAUDE.md "Two-notch wheels" -- cited
# by name, since ENHANCEMENTS.md renumbers as issues close). VI, VII and
# VIII notch at M and Z, exactly 13 apart, so their notch SET survives a shift
# of 13 -- and a stepping wheel's absolute position is read by nothing but that
# notch test. So for such a wheel on the RIGHT, (ring2, start2) and
# (ring2+13, start2+13) decode identically and one of the pair is skipped.
#
# The equivalence itself is checked FIRST and by encryption alone, because that
# is the fact the skip rests on: if it were false the skip would drop real keys
# silently, which no recovery test reliably catches. Single-notch wheels are the
# control -- without one, a build that shifted nothing would pass.
tn_pt="ANXPANZXGRUPPEXVIERXSIEGFRIEDXTONIXDIVXSTEHTSEITXEINSZWOXSIEBEN"
for tn in "126 two-notch same" "123 single-notch differ" "186 two-notch same"
do
  # shellcheck disable=SC2086  # intentional word-splitting into params
  set -- $tn
  check "right wheel $2 (w$1): ring2/start2 shifted by 13 must $3" \
    "$(test "$(run "$tn_pt" -i -x 8 -u B -w "$1" -r AAC -g AAF)" \
          = "$(run "$tn_pt" -i -x 8 -u B -w "$1" -r AAP -g AAS)" \
       && echo same || echo differ)" \
    "$3"
done
# ...and in the MIDDLE position the shift is equally exact but needs no code
# here: §7.12 derives its classes by simulating the stepping, so it already
# picks this up.
check "middle wheel two-notch (w163): shifted by 13 is identical too" \
  "$(test "$(run "$tn_pt" -i -x 8 -u B -w 163 -r ACA -g AFA)" \
        = "$(run "$tn_pt" -i -x 8 -u B -w 163 -r APA -g ASA)" \
     && echo same || echo differ)" "same"

# The key count. -r AA. -g AA. pins everything but ring2/start2, so the 676 keys
# isolate this collapse from §7.12's (which needs ring1 AND start1 wildcarded,
# and is inert here).
tn_ct=$(run "$tn_pt" -i -x 8 -u B -w 126 -r AAC -g AAF)
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
tn_keys() { printf '%s' "$tn_ct" \
            | "$ENIGMA" -i -x 8 -u B -w "$1" -r "$2" -g "$3" -T 1 \
              2>&1 >/dev/null \
            | grep -oE 'Analysed [0-9]+' | awk '{print $2}'; }
check "right-wheel collapse halves the keys (two-notch)" \
  "$(tn_keys 126 "AA." "AA.")" "338"
check "no collapse with a single-notch right wheel" \
  "$(tn_keys 123 "AA." "AA.")" "676"
# The precondition, both halves: with either ring2 or start2 pinned the skipped
# key's twin may be absent from the sweep, so the collapse must not fire.
check "right-wheel collapse inert when ring2 is pinned" \
  "$(tn_keys 126 "AAC" "AA.")" "26"
check "right-wheel collapse inert when start2 is pinned" \
  "$(tn_keys 126 "AA." "AAF")" "26"

# Recovery must be unaffected: whichever member of the pair the true key is, its
# twin decodes identically, so the plaintext still comes out exact. Both ring2
# values below are in the DROPPED half (>= 13, N..Z), the case that would fail.
for tn_r in AAP AAZ; do
  tn_ct2=$(run "$tn_pt" -i -x 8 -u B -w 126 -r "$tn_r" -g AAS)
  check "crack: right-wheel collapse recovers with true ring2 = $tn_r" \
    "$(run "$tn_ct2" -f -l wehrmacht -x 8 -u B -w 126 -r "AA." -g "AA." -T 1)" \
    "$tn_pt"
done
check "right-wheel collapse is -T-independent" \
  "$(run "$tn_ct" -f -l wehrmacht -x 8 -u B -w 126 -r "AA." -g "AA." -T 1)" \
  "$(run "$tn_ct" -f -l wehrmacht -x 8 -u B -w 126 -r "AA." -g "AA." -T 4)"
# Analysed must count keys SCORED, the same contract §7.12's accounting carries.
tn_diag=$(printf '%s' "$tn_ct" \
          | "$ENIGMA" -f -l wehrmacht -x 8 -u B -w 126 -r "AA." -g "AA." -T 1 \
            2>&1 >/dev/null \
          | grep -oE 'Analysed [0-9]+ rotor combinations, scored [0-9]+ plugb')
check "right-wheel collapse: analysed count matches keys scored" \
  "$(printf '%s' "$tn_diag" | awk '{print ($2 == $6)}')" "1"

# The echo names which collapses fired, and must not name one that did no work.
# Here §7.12 is inert (ring1/start1 pinned) so the line must read "right" alone.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
tn_line() { printf '%s' "$tn_ct" \
            | "$ENIGMA" -i -x 8 -u B -w "$1" -r "$2" -g "$3" -T 1 \
              2>&1 >/dev/null \
            | grep -c "^Collapse:   $4"; }
check "right-wheel collapse is echoed alone when §7.12 is inert" \
  "$(tn_line 126 "AA." "AA." "right ring x start")" "1"
check "no collapse line with a single-notch right wheel" \
  "$(tn_line 123 "AA." "AA." "")" "0"

# M4 and Norway, for the reason every other keyspace check carries them: notch[]
# and notch_halfperiod[] are indexed by TRANSLATED rotor numbers, so a mode that
# translates (-n adds norway_rotor_base) is where an index slip hides. Norway's
# wheels 1-5 are all single-notch, so the collapse must NOT fire there; M4
# reaches VI-VIII, so it must.
tn_m4=$(run "$tn_pt" -4 -i -x 8 -u b -w B126 -r AAAC -g AAAF)
check "M4: right-wheel collapse fires with a two-notch right wheel" \
  "$(printf '%s' "$tn_m4" \
     | "$ENIGMA" -4 -i -x 8 -u b -w B126 -r "AAA." -g "AAA." -T 1 \
       2>&1 >/dev/null \
     | grep -oE 'Analysed [0-9]+' | awk '{print $2}')" "338"
tn_nw=$(run "$tn_pt" -n -i -u N -w 123 -r AAC -g AAF)
check "Norway: no right-wheel collapse (wheels 1-5 are single-notch)" \
  "$(printf '%s' "$tn_nw" | "$ENIGMA" -n -i -u N -w 123 -r "AA." -g "AA." -T 1 \
     2>&1 >/dev/null | grep -oE 'Analysed [0-9]+' | awk '{print $2}')" "676"

# Middle-wheel ring x start collapse (archived/PERFORMANCE.md §7.12). Shifting ring1 and start1
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
            | grep -c '^Collapse: .*middle ring'; }
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

echo "== Work-space ordering: restart-major =="

# Restart is the OUTER dimension: every key at restart 0, then every key at
# restart 1, and so on.  Restart-innermost would share a key's setup_mapping
# across its restarts, but that saving is under 1% while the ordering decides
# WHEN an answer appears -- there is no early exit, so front-loading the
# probability is what lets a watcher kill a long sweep early.
#
# Observable deterministically at -T 1: --dump-all emits one line per work item
# in work order, so the first items are successive KEYS, not one key repeated.
wo_ct=$(run "ANXPANZXGRUPPEXVIERXSIEGFRIEDXTONIXDIVXSTEHTSEIT" \
        -u B -w 123 -r AAA -g QEW -s ABCDEFGH)
wo_starts=$(printf '%s' "$wo_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA \
            -g "AA." -R 2 -T 1 --dump-all 2>&1 >/dev/null \
            | grep '^dumpall' | head -4 | awk '{ print $4 }' | tr '\n' ' ')
check "restart is the outer dimension of the work space" \
  "$wo_starts" "AAA AAB AAC AAD "

# The failure this ordering could cause is the one that matters: the echoed
# rotor key is reconstructed from the merged work index, and --polish and the
# --ring-stride refinement each decode it independently.  Decode it as
# idx/restarts instead of idx%keys and the tool prints a key that does not
# decrypt to the plaintext it just wrote to stdout.
#
# THE ROUND-TRIP ASSERTION ALONE DOES NOT CATCH THAT, and for a while these
# three checks swept 456 976 keys through a -c climb (189 s, 75% of the whole
# suite) to establish nothing.  Re-encrypting the reported plaintext under the
# reported key tests SELF-CONSISTENCY, and the line it reads is the last
# progress line -- which the ordinary climb emits.  Both finishers re-echo only
# when they IMPROVE on the climb's best, so with a wrong reconstruction they
# simply score worse, never echo, and the climb's own (correct, consistent) line
# stands.  Verified by injecting the historical bug at the two reconstruction
# sites: all three passed, at 676 keys AND at 456 976, byte-identical output.
#
# So each finisher needs a fixture where its reconstruction DETERMINES the
# result, and then the assertion is that the finisher still delivers:
#
#   --polish       10 plugs, so the climb converges near-but-incomplete and the
#                  finisher completes it.  Asserted as a strict score
#                  improvement over the same run without --polish.  (The 3-plug
#                  fixture used here reaches the true key unaided -- -6.9317 at
#                  every -R and seed tried -- so polish can never bite on it.)
#   --ring-stride  true ring2 = N (13), which K=3's coarse set {0,3,6,...}
#                  SKIPS, so only the refinement can find it.  Asserted as
#                  recovery of the plaintext.
#
# Both discriminate against the injected build -- --polish's gain vanishes
# exactly (score identical to no-polish) and --ring-stride's recovery is lost --
# and both run on 676 keys, so all five checks here cost ~1.3 s rather than 189.
#
# Which half was actually uncovered is worth recording, because it is not the
# half the expensive checks were aimed at.  Under the injection 23 of the 502
# checks fail, and 21 of those are the pre-existing "crack: --ring-stride N ..."
# recovery checks: the REFINEMENT's reconstruction was already well covered
# elsewhere, and the check below mainly anchors that property where the bug
# lives.  --polish's reconstruction had NO coverage at all -- the improvement
# check below is the only one in the suite that catches it.
wo_pt=DASOBERKOMMANDODERWEHRMACHTGIBTBEKANNTXAACHENXISTGERETTETXDURCHDENEINSATZ
wo_c=$(run "$wo_pt" -u B -w 123 -r AAN -g AAW -s "AB CD EF")
wo_c10=$(run "$wo_pt" -u B -w 123 -r AAN -g AAW -s "AB CD EF GH IJ KL MN OP QR ST")
TMP_WO=/tmp/enigma_wo.$$
# Runs the sweep over the 676 keys around the true one and leaves stderr in
# $TMP_WO; echoes the recovered plaintext.
wo_run() {
  _c=$1; shift
  printf '%s' "$_c" | "$ENIGMA" -c -f -l wehrmacht -S i4f10 -J -u B -w 123 \
    -r "AA." -g "AA." -R 2 -T 4 "$@" 2>"$TMP_WO"
}
wo_score() { grep -E '^ *-?[0-9.]+ [A-Za-z]' "$TMP_WO" | tail -1 | awk '{ print $1 }'; }
# Re-encrypt the recovered plaintext under the REPORTED key: it must give the
# ciphertext back.  Cheap, and still worth keeping -- it catches an echoed key
# that is simply garbage, which the improvement assertions would not notice.
wo_roundtrip() {
  _o=$(wo_run "$@")
  _l=$(grep -E '^ *-?[0-9.]+ [A-Za-z]' "$TMP_WO" | tail -1)
  [ -n "$_l" ] || { echo "no-progress-line"; return; }
  _w=$(printf '%s' "$_l" | awk '{ print $2 }')
  _r=$(printf '%s' "$_l" | awk '{ print $3 }')
  _g=$(printf '%s' "$_l" | awk '{ print $4 }')
  _s=$(printf '%s' "$_l" | awk '{ for (i = 5; i < NF; i++) printf "%s ", $i }')
  printf '%s' "$_o" | "$ENIGMA" -u "$(printf %s "$_w" | cut -c1)" \
    -w "$(printf %s "$_w" | cut -c2-)" -r "$_r" -g "$_g" -s "$_s" 2>/dev/null
}
# The finisher's reconstruction is load-bearing: --polish must beat the same
# run without it.  A wrong key makes the finisher inert, not merely worse.
wo_run "$wo_c10" >/dev/null; wo_np=$(wo_score)
wo_run "$wo_c10" --polish >/dev/null; wo_p=$(wo_score)
check "--polish improves on the converged best (its key is reconstructed)" \
  "$(awk -v a="$wo_np" -v b="$wo_p" 'BEGIN { print (b > a) ? "better" : "no-gain" }')" \
  "better"
check "the echoed key reproduces the ciphertext (--polish)" \
  "$(wo_roundtrip "$wo_c10" --polish)" "$wo_c10"
# The refinement's reconstruction is load-bearing: the true ring2 is in the half
# K=3 skips, so the coarse pass cannot reach it and only the refinement can.
check "--ring-stride refinement recovers a ring2 the coarse pass skipped" \
  "$(wo_run "$wo_c" --ring-stride 3)" "$wo_pt"
check "the echoed key reproduces the ciphertext (--ring-stride)" \
  "$(wo_roundtrip "$wo_c" --ring-stride 3)" "$wo_c"
check "the echoed key reproduces the ciphertext (both)" \
  "$(wo_roundtrip "$wo_c" --polish --ring-stride 3)" "$wo_c"
rm -f "$TMP_WO"

echo
echo "== Seed deduplication: --seed-dedup =="

# Skip the target climb when this restart's stage-0 seed was already climbed for
# this key.  The seed determines the climb, so a repeat is byte-identical work.
#
# THE FIXTURE IS --random 0, AND THAT IS THE WHOLE POINT.  With no kick every
# restart climbs from the same unperturbed seed, so the duplicate count is an
# EXACT expected value -- R-1 of R -- on 26 keys in a fraction of a second.  The
# natural fixture (real kicks) duplicates ~1.4% at -R 8, which needs a large
# sweep to assert anything and would be a slow check that catches less.  It also
# pins the DENOMINATOR, which is the easy thing to get wrong: the unit is a SEED
# (one per key x restart), not a key, so dividing by the key count would
# misreport by a factor of R -- invisible at -R 1 and unmistakable across
# -R 1/2/4.
sd_pt="ENIGMACIPHERTOOLACOMMANDLINETOOLTHATSIMULATESANENIGMACIPHERMACHINEANDMOREUSEFULLYATTEMPTSTOBREAKENIGMACIPHERTEXT"
sd_ct=$(run "$sd_pt" -i -u B -w 231 -r AAC -g QWE -s "AB CD EF GH IJ")
# stderr of a dedup run: OPTS... -> the "Skipped ..." line
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
sd_err() { printf '%s' "$sd_ct" | "$ENIGMA" -c -q -l english -u B -w 231 \
             -r AAC -g "$rgd" -S i4q10 "$@" 2>&1 >/dev/null; }
sd_pct() { sd_err "$@" | sed -n 's/^Skipped .*(\([0-9.]*\)%)$/\1/p'; }
sd_cnt() { sd_err "$@" | sed -n 's/^Skipped \([0-9]*\) .*/\1/p'; }
sd_den() { sd_err "$@" | sed -n 's/^Skipped .* of \([0-9]*\) .*/\1/p'; }

check "--seed-dedup skips nothing at -R 1" \
  "$(sd_pct --seed-dedup -R 1 --random 0)" "0.0"
check "--seed-dedup skips half at -R 2 (no kick: every seed repeats)" \
  "$(sd_pct --seed-dedup -R 2 --random 0)" "50.0"
check "--seed-dedup skips 3 of 4 at -R 4" \
  "$(sd_pct --seed-dedup -R 4 --random 0)" "75.0"
# The denominator is seeds, not keys: 26 keys x 4 restarts.
check "--seed-dedup reports seeds, not keys, as the denominator" \
  "$(sd_den --seed-dedup -R 4 --random 0)" "104"
check "--seed-dedup skip count is exact (3 of every 4 seeds)" \
  "$(sd_cnt --seed-dedup -R 4 --random 0)" "78"

# The skip must not change the answer.  With --random 0 the skipped climbs would
# have reproduced the kept one exactly, so this is an equivalence, not an
# approximation -- if it fails, the filter is skipping something it should not.
sd_plain=$(run "$sd_ct" -c -q -l english -u B -w 231 -r AAC -g "$rgd" \
             -S i4q10 -R 4 --random 0 --polish)
check "--seed-dedup does not change the recovered plaintext" \
  "$(run "$sd_ct" -c -q -l english -u B -w 231 -r AAC -g "$rgd" -S i4q10 \
       -R 4 --random 0 --polish --seed-dedup)" "$sd_plain"
# ...and with no duplicates possible (-R 1) the staged climb it runs must be the
# ordinary one, which is what makes the second copy of the stage loop safe.
sd_one=$(run "$sd_ct" -c -q -l english -u B -w 231 -r AAC -g "$rgd" \
           -S i4q10 -R 1)
check "--seed-dedup at -R 1 equals the ordinary staged climb" \
  "$(run "$sd_ct" -c -q -l english -u B -w 231 -r AAC -g "$rgd" -S i4q10 \
       -R 1 --seed-dedup)" "$sd_one"

# -T independence: the pass barrier exists so that the skip decision -- and so
# the whole run -- does not depend on the thread count.  The COUNT is part of
# that contract, not a by-product: a -T-dependent total means threads are
# straddling passes on one key even if the winning board happens to agree.
sd_t1=$(sd_cnt --seed-dedup -R 4 --random 0 -T 1)
check "--seed-dedup skip count is -T-independent (2)" \
  "$(sd_cnt --seed-dedup -R 4 --random 0 -T 2)" "$sd_t1"
check "--seed-dedup skip count is -T-independent (4)" \
  "$(sd_cnt --seed-dedup -R 4 --random 0 -T 4)" "$sd_t1"
check "--seed-dedup result is -T-independent" \
  "$(run "$sd_ct" -c -q -l english -u B -w 231 -r AAC -g "$rgd" -S i4q10 \
       -R 4 --random 0 --seed-dedup -T 4)" \
  "$(run "$sd_ct" -c -q -l english -u B -w 231 -r AAC -g "$rgd" -S i4q10 \
       -R 4 --random 0 --seed-dedup -T 1)"

# Off is off: no line, and the flag absent must leave the run alone.
check "no --seed-dedup, no report line" \
  "$(sd_err -R 4 --random 0 | grep -c '^Skipped')" "0"

# A bigger filter cannot skip MORE than a smaller one: extra bits only remove
# false positives.  Monotonicity is the cheap end-to-end sanity check on the
# sizing arithmetic, and it holds whatever the true duplicate rate is.
sd_lo=$(sd_cnt --seed-dedup --seed-dedup-bits 4 -R 8)
sd_hi=$(sd_cnt --seed-dedup --seed-dedup-bits 24 -R 8)
check "--seed-dedup-bits 24 skips no more than bits 4 (fewer false positives)" \
  "$([ "$sd_hi" -le "$sd_lo" ] && echo yes)" "yes"

# Refusals.  Each of these either installs its own starting board (so the
# stage-0 board is not a function of (key, restart) alone) or re-encodes the
# work index so "one key per pass" stops holding.  --ring-stride is NOT among
# them: its coarse pass dedups normally and its refinement runs unfiltered.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
sd_bad() { printf '%s' "$sd_ct" | "$ENIGMA" -q -l english -u B -w 231 \
             -r AAC -g "$rgd" "$@" 2>&1 >/dev/null; }
check "--seed-dedup needs -c" \
  "$(sd_bad --seed-dedup -S i4q10 | grep -c 'needs -c')" "1"
check "--seed-dedup needs a staged schedule" \
  "$(sd_bad -c --seed-dedup -S q | grep -c 'needs a staged')" "1"
check "--seed-dedup rejects -A" \
  "$(sd_bad -c --seed-dedup -S i4q10 -A 100 | grep -c 'not work with -A')" "1"
check "--seed-dedup rejects -F" \
  "$(sd_bad -c --seed-dedup -S i4q10 -F 10 | grep -c 'not work with -F')" "1"
check "--seed-dedup rejects --exhaust" \
  "$(sd_bad -c --seed-dedup -S i4q10 --exhaust 1 | grep -c 'exhaust')" "1"
check "--seed-dedup rejects --crib" \
  "$(sd_bad -c --seed-dedup -S i4q10 --crib ENIGMA | grep -c 'crib')" "1"
check "--seed-dedup rejects --tune-phase" \
  "$(sd_bad -c --seed-dedup -S i4q10 -r "A.." --tune-phase 2 \
     | grep -c 'tune-phase')" "1"
check "--seed-dedup-bits rejects 3 (too few for a usable filter)" \
  "$(sd_bad -c --seed-dedup --seed-dedup-bits 3 -S i4q10 \
     | grep -c 'Illegal bits per item')" "1"
check "--seed-dedup-bits rejects 25" \
  "$(sd_bad -c --seed-dedup --seed-dedup-bits 25 -S i4q10 \
     | grep -c 'Illegal bits per item')" "1"
# Silently doing nothing is the failure mode CLAUDE.md warns about, so a
# sub-option without the flag it configures is fatal rather than ignored.
check "--seed-dedup-bits without --seed-dedup is fatal" \
  "$(sd_bad -c --seed-dedup-bits 8 -S i4q10 | grep -c 'need --seed-dedup')" "1"
check "--seed-dedup-max without --seed-dedup is fatal" \
  "$(sd_bad -c --seed-dedup-max 1G -S i4q10 | grep -c 'need --seed-dedup')" "1"
# The ceiling REFUSES rather than thinning the filter, and names what would fit.
sd_small=$(sd_bad -c --seed-dedup -S i4q10 -R 8 --seed-dedup-max 100)
check "--seed-dedup-max refuses when the request does not fit" \
  "$(printf '%s' "$sd_small" | grep -c 'over the --seed-dedup-max')" "1"
check "--seed-dedup-max says what would fit instead" \
  "$(printf '%s' "$sd_small" | grep -c 'seed-dedup-bits\|Nothing above')" "1"
# Byte suffixes, since a memory budget misread by 1024x is a swap-death.
check "--seed-dedup-max accepts a G suffix" \
  "$(sd_bad -c --seed-dedup -S i4q10 -R 4 --seed-dedup-max 4G \
     | grep -c 'over the --seed-dedup-max')" "0"
check "--seed-dedup-max rejects junk" \
  "$(sd_bad -c --seed-dedup -S i4q10 --seed-dedup-max 4Q \
     | grep -c 'trailing characters')" "1"

# --ring-stride composes: the coarse pass is filtered, the refinement is not.
# -r AA. -g AA. is the smallest keyspace that satisfies --ring-stride's
# precondition (ring2 AND start2 wildcarded) -- 676 keys rather than the
# millions that wildcarding ring1/start1 as well would sweep under -c.
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
sd_rs=$(printf '%s' "$sd_ct" | "$ENIGMA" -c -q -l english -u B -w 231 \
          -r "AA." -g "AA." -S i4q10 -R 4 --random 0 --ring-stride 3 \
          --seed-dedup -T 1 2>&1 >/dev/null)
check "--seed-dedup works with --ring-stride (coarse pass filtered)" \
  "$(printf '%s' "$sd_rs" | sed -n 's/^Skipped .*(\([0-9.]*\)%)$/\1/p')" "75.0"
# 234 coarse keys x 4 restarts.  The refinement's 25 keys must be in NEITHER
# counter: it reuses search_worker over its own key space, whose indices start
# again at 0 and alias the coarse per-key regions, so an unsuspended filter both
# consumes those regions and skips refinement climbs against seeds they never
# produced.  A wrong denominator here is that bug, and it is otherwise silent.
check "--ring-stride refinement is outside the filter (seed count)" \
  "$(printf '%s' "$sd_rs" | sed -n 's/^Skipped .* of \([0-9]*\) .*/\1/p')" "936"
check "--seed-dedup does not change a --ring-stride result" \
  "$(run "$sd_ct" -c -q -l english -u B -w 231 -r "AA." -g "AA." -S i4q10 \
       -R 4 --random 0 --ring-stride 3 --seed-dedup -T 1)" \
  "$(run "$sd_ct" -c -q -l english -u B -w 231 -r "AA." -g "AA." -S i4q10 \
       -R 4 --random 0 --ring-stride 3 -T 1)"

echo
echo "== Pre-flight: is this ciphertext even Enigma? =="

# Enigma is a permutation cipher, so its output is near-flat.  A ciphertext
# carrying residual language structure was not produced by one and has no key
# to find -- which a 28-hour 75.2M-key sweep of QTXMA established the expensive
# way.  Two free statistics, both with closed-form nulls that were checked
# against real Enigma encryptions (eval/preflight_null.py).
#
# THE GATE MATTERS AS MUCH AS THE TEST, and it is why the report is not simply
# unconditional.  With a fully-specified key the tool is encrypting or
# decrypting, and on encryption the input is PLAINTEXT -- language-like by
# definition -- so reporting there would print a scary-looking line about a
# ciphertext that is not one, on every encryption this suite performs.  The
# report is ON BY DEFAULT for a SEARCH (a wildcarded key), silent otherwise,
# and --no-preflight turns it off entirely.
pf_qtxma=JVMOYCZAYMRVLCBSOQXYBATSXJBQLAEJKYTYXJOEMYBLOEMYOKSRMTAVLBCXJAMOESRXYTVAOEYAVYXKCJVCMEISHTBAYVXXAJWCZQCYPXMEHABLKYJYASOEIJYXOQXYTLBASYEESTAQXJVNWCBJZBYQYTM
pf_byqmz=NYZKYDOEMGPSDUHMLHJATWMYCHIFYMAESTAVLCGCNLGMZIQUSQNRAIKYJDETUEXOJQPGXQSCEXENOSFASJVTGBHXTVGQTWKEWPPRIVYJEHEWNGPFUEAZTUWZUQBLNBYETZVSUAJSEASZXYFTUMOSHURQESSTQMPAOPBFTY
# stderr only.  The braces make the order explicit: stdout is discarded
# INSIDE the group, then the group's stderr becomes the pipeline's stdout.
# 26 keys ("-g AA.") is a search as far as the gate is concerned, and every
# assertion reads a line printed BEFORE the sweep, so breadth buys nothing.
pf_run() {
  _t=$1; shift
  { printf '%s' "$_t" | "$ENIGMA" -u B -w 123 -r AAA "$@" >/dev/null; } 2>&1
}

# QTXMA: IC 0.0577 at 155 letters, z = +10.9, and 4 letters of A-Z unused
# (P = 8.5e-08).  Both tests fire.
check "pre-flight warns on a non-Enigma ciphertext" \
  "$(pf_run "$pf_qtxma" -g "AA." \
     | grep -c '^WARNING: this does not look like Enigma')" "1"
# BYQMZ is the control that keeps the above from passing for the wrong reason:
# same length class, genuinely Enigma-like (z = +0.6, no unused letters).  It
# must still REPORT, the report being on by default, but say it is fine.
check "pre-flight passes an Enigma-like ciphertext" \
  "$(pf_run "$pf_byqmz" -g "AA." | grep -c 'consistent with Enigma output')" "1"
check "pre-flight does not warn on an Enigma-like ciphertext" \
  "$(pf_run "$pf_byqmz" -g "AA." | grep -c '^WARNING')" "0"
# The gate: a fully-specified key is an encrypt/decrypt, not a search.  Feeding
# it the very ciphertext that trips the warning above must stay silent.
check "pre-flight does not fire when the key is fully specified" \
  "$(pf_run "$pf_qtxma" -g AAA | grep -c 'Pre-flight\|WARNING')" "0"
# Encrypting PLAINTEXT is the case the gate exists for.
check "pre-flight does not fire when encrypting plaintext" \
  "$(pf_run "DASOBERKOMMANDODERWEHRMACHTGIBTBEKANNTXAACHENXISTGERETTET" \
     -g QEW | grep -c 'Pre-flight\|WARNING')" "0"
# --no-preflight turns off both halves, warning included.
check "--no-preflight silences the report" \
  "$(pf_run "$pf_byqmz" --no-preflight -g "AA." | grep -c '^Pre-flight:')" "0"
check "--no-preflight silences the warning" \
  "$(pf_run "$pf_qtxma" --no-preflight -g "AA." \
     | grep -c 'Pre-flight\|WARNING')" "0"
# The statistics themselves, to 4 places -- these are the numbers the thresholds
# are calibrated against, so a change in either is a change in the contract.
# Anchored on ^Pre-flight: because show_settings() also prints the phrase
# "index of coincidence" (it is the name of the default scoring model).
check "pre-flight reports the index of coincidence" \
  "$(pf_run "$pf_qtxma" -g "AA." \
     | sed -n 's/^Pre-flight:.*index of coincidence \([0-9.]*\).*/\1/p')" "0.0577"
check "pre-flight counts the unused letters" \
  "$(pf_run "$pf_qtxma" -g "AA." \
     | sed -n 's/.*), and \([0-9]*\) of 26 letters unused.*/\1/p')" "4"
# NO pre-flight line may look like a progress line.  The margin extractor for
# --confidence greps stderr for '^ *[+-][0-9]', and a continuation line reading
# "  +10.95 sd; ..." was picked up as the run's last margin -- breaking a
# --confidence check in a way that pointed at --confidence rather than here.
check "pre-flight lines cannot be read as progress lines" \
  "$(pf_run "$pf_qtxma" -g "AA." \
     | sed -n '/^Pre-flight:/,/Proceeding anyway/p' \
     | grep -cE '^ *[+-]?[0-9]')" "0"
# The null is LENGTH-DEPENDENT, and that is the whole reason it is not a fixed
# IC threshold: two of the four broken (genuinely Enigma) 1941 messages reach
# z = +4.2 at 47 and 74 letters.  WEUWY is one of them -- 9 unused letters of
# 26, which is normal at that length -- and must not be flagged.
check "pre-flight does not flag a short genuine Enigma message" \
  "$(pf_run "WCZIEDSYTCDXOICDSXOXASIMEIORSRKRISSPCCOUIMDZYDM" -g "AA." \
     | grep -c '^WARNING')" "0"

# The pre-flight block is part of the settings echo, so it must align with it:
# a 12-wide label field and 12-space continuations, exactly like "Confidence: "
# and "Machine:    ".  It shipped with 13 in both places and so sat one column
# right of everything above it.  Asserted against the echo's OWN continuation
# indent rather than the literal 12, so the two cannot drift apart.
check "pre-flight continuations align with the rest of the settings echo" \
  "$(pf_run "$pf_qtxma" -g "AA." | awk '
     /^Machine:/   { getline; match($0, /^ */); echo = RLENGTH }
     /^Pre-flight:/{ getline; match($0, /^ */); pf = RLENGTH }
     END { print (echo > 0 && pf == echo) ? "aligned" : "echo=" echo " pf=" pf }')" \
  "aligned"
# The label field itself, same rule: "Pre-flight:" plus padding must occupy the
# same width as "Machine:" plus its padding.
check "pre-flight label field is the same width as the echo's" \
  "$(pf_run "$pf_qtxma" -g "AA." | awk '
     /^Machine:/    { match($0, /^Machine: */);    a = RLENGTH }
     /^Pre-flight:/ { match($0, /^Pre-flight: */); b = RLENGTH }
     END { print (a > 0 && a == b) ? "same" : "machine=" a " preflight=" b }')" \
  "same"

check "pre-flight output stays within 80 columns" \
  "$(pf_run "$pf_qtxma" -g "AA." \
     | sed -n '/^Pre-flight:/,/Proceeding anyway/p' | awk '{ print length }' \
     | sort -rn | head -1 | awk '{ print ($1 <= 80) ? "ok" : $1 }')" "ok"

echo
echo "== Progress display: --full-text =="

# --full-text prints the whole decrypted message below each progress line, wrapped and
# indented, instead of the 19-character preview the fixed-width line has room for.
# One fully-specified key, so this costs a single decrypt: the option is about DISPLAY,
# and the display code runs identically over any keyspace.
ft_pt=DASOBERKOMMANDODERWEHRMACHTGIBTBEKANNTXAACHENXISTGERETTETXDURCHDENEINSATZDERHILFSKRAEFTEXMELDUNGFOLGT
ft_ct=$(run "$ft_pt" -i -u B -w 123 -r AAA -g QEW -s ABCDEFGH)
ft_key='-i -u B -w 123 -r AAA -g QEW -s ABCDEFGH'
# The continuation lines are the indented all-letter ones; joining them must give back
# the plaintext exactly -- that is the whole contract of the option.
# shellcheck disable=SC2086
ft_lines() { printf '%s' "$ft_ct" | "$ENIGMA" $ft_key "$@" 2>&1 >/dev/null | grep '^  [A-Z]*$'; }
check "--full-text prints the whole message" \
  "$(ft_lines --full-text | tr -d ' \n')" "$ft_pt"

# Off by default: without the flag there are no continuation lines at all. Guards the
# byte-identical-when-absent property at the point where it is easiest to break.
check "--full-text is off by default" "$(ft_lines | wc -l | tr -d ' ')" "0"

# The progress line is budgeted to exactly 80 columns and the wrapped text must stay
# inside it too, so nothing wraps a second time in an 80-column terminal.
check "--full-text stays within 80 columns" \
  "$(printf '%s' "$ft_ct" | "$ENIGMA" -i -u B -w 123 -r AAA -g QEW -s ABCDEFGH --full-text 2>&1 >/dev/null \
     | awk '{ if (length($0) > 80) n++ } END { print n+0 }')" "0"

# ... and it must REACH 80, not merely stay under it: the continuation wraps
# against the same right margin as the preview it replaces, so the two read as
# one block.  A one-sided bound missed this for a long time -- the width was 76
# (78 with the indent) from a time when the target was a 79-column terminal,
# while every progress_fmt variant lands on exactly 80.  Compare the widest
# continuation line against the progress line itself rather than against a
# literal, so the two can never drift apart again.
ft_widths=$(printf '%s' "$ft_ct" | "$ENIGMA" -i -u B -w 123 -r AAA -g QEW \
            -s ABCDEFGH --full-text 2>&1 >/dev/null \
            | awk '/^ *[-0-9]+\.[0-9]+ / { if (length($0) > p) p = length($0) }
                   /^  [A-Z]*$/          { if (length($0) > c) c = length($0) }
                   END { print p, c }')
# The score column is 8 wide and BOTH printers must respect it: the margin
# (--confidence) and the raw score.  Only the margin was guarded, so an
# oversized raw score shifted every column after it -- reachable with
# ENIGMA_LOGLIN, which scales the quad table.  The default weights leave zero
# slack (about -14, exactly 8 characters), so this is the check that keeps the
# 80-column budget true rather than merely true today.
sc_ct=$(run "$ft_pt" -i -u B -w 123 -r AAA -g QEW -s ABCDEFGH)
sc_width() { printf '%s' "$sc_ct" \
  | ENIGMA_LOGLIN="$1" "$ENIGMA" -q -l english -u B -w 123 -r AAA -g AAA 2>&1 \
    >/dev/null | awk '/^ *-?[0-9]/ { print length($0); exit }'; }
check "score column holds 80 columns at the default weights" \
  "$(sc_width '1,0.6,0.3,0.15')" "80"
check "score column holds 80 columns on an oversized raw score" \
  "$(sc_width '400,240,120,60')" "80"

check "--full-text wraps to the progress line's own width" \
  "$(printf '%s' "$ft_widths" \
     | awk '{ print ($1 == $2) ? "same" : $1 " vs " $2 }')" "same"

echo
echo "== Known-unplugged letters: --no-plug =="

# --no-plug marks letters as carrying no cable, so nothing may ever plug them: not the
# climb, not the re-pair, and not the --random kick (which draws from self-steckered
# letters, exactly what a --no-plug letter looks like). --dump-all shows every converged
# board across the restarts, so one run covers all three.
np_boards() { printf '%s' "$ft_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW \
                -c -R 8 --random 10 --dump-all "$@" 2>&1 >/dev/null \
              | grep '^dumpall' | awk '{ for (i = 6; i <= NF; i++) printf "%s", $i }'; }
check "--no-plug letters are never plugged" \
  "$(np_boards --no-plug XYZQ | grep -c '[XYZQ]')" "0"
# Without the option the same run does plug them -- otherwise the check above would pass
# for the wrong reason (a 10-pair kick covers 20 of the 26 letters, so this is not close).
check "--no-plug control: unmarked letters do get plugged" \
  "$(np_boards | grep -c '[XYZQ]')" "1"

# Determinism is the standing contract for every search option.
check "--no-plug is -T-independent" \
  "$(run "$ft_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 8 --no-plug XYZQ -T 1)" \
  "$(run "$ft_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 8 --no-plug XYZQ -T 4)"

# Four ways to get the option wrong, all fatal. The overlap case is the interesting one:
# -s says the letter carries a cable and --no-plug says it does not, so the command line
# contradicts itself and there is no sensible reading to pick.
np_rejects() { printf '%s' "$ft_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW \
                 "$@" >/dev/null 2>&1; echo $?; }
check "--no-plug rejects a letter also plugged by -s" \
  "$(np_rejects -c -s XZ --no-plug XY)" "1"
check "--no-plug rejects a repeated letter" "$(np_rejects -c --no-plug XX)" "1"
check "--no-plug rejects a non-letter" "$(np_rejects -c --no-plug X7)" "1"
check "--no-plug rejects a run with no climb" "$(np_rejects --no-plug XY)" "1"

# --no-plug letters are unavailable to --exhaust as well, so they come off its free-letter
# count: 26 - 16 marked = 10 free letters = 45 first pairs, and 5 forced pairs is the most
# that fits. E=6 must be refused rather than silently enumerating nothing.
np_many=XYZQKLMNOPRSTUVW
check "--exhaust sees --no-plug letters as unavailable" \
  "$(printf '%s' "$ft_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW -c \
       --exhaust 1 --no-plug "$np_many" 2>&1 >/dev/null \
     | grep -oE '\([0-9]+ combinations\)')" "(45 combinations)"

echo
echo "== Guessed plugs the climb may revise: --soft-plug =="

# --soft-plug lays pairs on the board each restart starts from and then leaves them free,
# where -s pins them.  The WHOLE point is that difference, so the tests are about it: the
# same wrong pairs must survive under -s and must not survive under --soft-plug.  One key
# throughout (-g QEW pins it), so every check below is a single climb's worth of work.

# That the seed is APPLIED needs a problem the unseeded climb cannot already solve, so
# this uses its own 10-pair fixture -- ft_ct's 4-pair board is recovered by a single
# unkicked climb, which would let the check pass with the seed silently dropped.
# Proximity rather than exact recovery, because at 104 letters the true board is not quite
# the scoring optimum: the seeded climb walks a couple of plugs off it and lands at ~93%.
# That is the documented scoring floor, not a seeding failure, and 80/20 straddles it with
# room to spare.
sp_true=ABCDEFGHIJKLMNOPQRST
sp_ct=$(run "$ft_pt" -i -u B -w 123 -r AAA -g QEW -s "$sp_true")
sp_pct() { awk -v a="$1" -v b="$2" 'BEGIN { n = length(b); c = 0;
             for (i = 1; i <= n; i++) if (substr(a, i, 1) == substr(b, i, 1)) c++;
             print int(100 * c / n) }'; }
sp_seeded=$(run "$sp_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 0 \
              --soft-plug "$sp_true")
sp_bare=$(run "$sp_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 0)
check "--soft-plug seeds the climb (correct seed, -R 0 lands on the plaintext)" \
  "$(test "$(sp_pct "$sp_seeded" "$ft_pt")" -ge 80 && echo near || echo far)" "near"
check "--soft-plug control: the same unseeded climb does not" \
  "$(test "$(sp_pct "$sp_bare" "$ft_pt")" -lt 20 && echo far || echo near)" "far"

# The semantic difference from -s, stated as the two halves of one experiment: give both
# options the same two WRONG pairs and look at the converged boards.  -s must keep both on
# every board; --soft-plug must not keep them on all of them.  (grep -c counts LINES, so
# this asks how many converged boards still carry the pair.)
sp_boards() { printf '%s' "$ft_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW \
                -c -R 4 --random 10 --dump-all "$@" 2>&1 >/dev/null | grep '^dumpall'; }
check "-s keeps its (wrong) pairs on every converged board" \
  "$(sp_boards -s XZYQ | grep -c 'XZ' )" "4"
check "--soft-plug lets the climb move its (wrong) pairs" \
  "$(test "$(sp_boards --soft-plug XZYQ | grep -c 'XZ')" -lt 4 && echo moved \
     || echo stuck)" "moved"

check "--soft-plug is -T-independent" \
  "$(run "$ft_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 4 \
       --soft-plug XZYQ -T 1)" \
  "$(run "$ft_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 4 \
       --soft-plug XZYQ -T 4)"

# Malformed, contradictory, or pointless: all fatal.  The two contradictions are the
# interesting ones -- -s asserts a letter's partner is KNOWN and --no-plug asserts it has
# none, so in each case the command line says two incompatible things about one letter.
sp_rejects() { printf '%s' "$ft_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW \
                 "$@" >/dev/null 2>&1; echo $?; }
check "--soft-plug rejects an odd number of letters" \
  "$(sp_rejects -c --soft-plug XYZ)" "1"
check "--soft-plug rejects a repeated letter" "$(sp_rejects -c --soft-plug XYXZ)" "1"
check "--soft-plug rejects a non-letter" "$(sp_rejects -c --soft-plug X7)" "1"
check "--soft-plug rejects a letter also pinned by -s" \
  "$(sp_rejects -c -s XZ --soft-plug XY)" "1"
check "--soft-plug rejects a letter also marked by --no-plug" \
  "$(sp_rejects -c --no-plug X --soft-plug XY)" "1"
check "--soft-plug rejects a run with no climb" "$(sp_rejects --soft-plug XY)" "1"
# Every other seeding mechanism installs its own board at its own site, so combining them
# would let one silently overwrite the other.
check "--soft-plug rejects --exhaust" \
  "$(sp_rejects -c --soft-plug XY --exhaust 1)" "1"
check "--soft-plug rejects -A" "$(sp_rejects -c --soft-plug XY -A 100)" "1"
check "--soft-plug rejects --crib" \
  "$(sp_rejects -c --soft-plug XY --crib DASOBERKOMMANDO)" "1"

echo
echo "== Terminal-signature seeding: --self-crib-seeds =="

# The mode deduces candidate boards per key from a doubled word closing the message,
# ranks them by IC and climbs the top K with the deduced plugs pinned.  Its own fixture,
# because it needs a plaintext that ENDS with a doubled surname; one key throughout
# (-g QEW pins it), so each check below is a handful of climbs.
sig_pt=DASOBERKOMMANDODERWEHRMACHTGIBTBEKANNTXAACHENXISTGERETTETXDURCHDENEINSATZDERHILFSKRAEFTEXGEZXRENNERXRENNER
sig_ct=$(run "$sig_pt" -i -u B -w 123 -r AAA -g QEW -s ABCDEFGHIJKLMNOPQRST)
sig_pct() { awk -v a="$1" -v b="$2" 'BEGIN { n = length(b); c = 0;
              for (i = 1; i <= n; i++) if (substr(a, i, 1) == substr(b, i, 1)) c++;
              print int(100 * c / n) }'; }
sig_run() { run "$sig_ct" -q -l german -u B -w 123 -r AAA -g QEW -c "$@"; }

# The deduction is arithmetic on the machine equation, so with a 10-pair board hidden it
# hands the climb most of the answer: one unkicked seeded climb recovers the plaintext
# where the same unseeded climb returns noise.
check "--self-crib-seeds recovers a 10-pair board from one climb" \
  "$(sig_pct "$(sig_run -R 0 --self-crib-seeds 1 --self-crib-signature)" \
       "$sig_pt")" "100"
check "--self-crib-seeds control: the same unseeded climb does not" \
  "$(test "$(sig_pct "$(sig_run -R 0)" "$sig_pt")" -lt 20 && echo far || echo near)" \
  "far"
# K is how many ranked seeds get climbed, so a larger K must not lose what K=1 found.
check "--self-crib-seeds K=5 keeps what K=1 found" \
  "$(sig_pct "$(sig_run -R 0 --self-crib-seeds 5 --self-crib-signature)" \
       "$sig_pt")" "100"

# The hypothesis count is a property of the ciphertext, so the echo must report it AFTER
# the list is built -- it read 0 while show_settings() ran first.
check "--self-crib-seeds echoes a non-zero hypothesis count" \
  "$(printf '%s' "$sig_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW -c \
       -R 0 --self-crib-seeds 3 --self-crib-signature 2>&1 >/dev/null \
     | grep -c '^Self-crib: .* [1-9][0-9]* hypotheses')" "1"
# Raising the floor past what the message can hold leaves nothing to deduce, which would
# make every key return "no seed": that is fatal, not a silent empty search.
check "--self-crib-seeds rejects a length no signature can fit" \
  "$(printf 'ABCDEFGHIJ' | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW -c \
       --self-crib-seeds 1 --self-crib-signature --self-crib-length 13 \
       >/dev/null 2>&1; echo $?)" "1"

check "--self-crib-seeds is -T-independent" \
  "$(sig_run -R 0 --self-crib-seeds 5 --self-crib-signature -T 1)" \
  "$(sig_run -R 0 --self-crib-seeds 5 --self-crib-signature -T 4)"

# Malformed values, and the modes that install their own starting board at their own site
# -- running two of them would silently let one overwrite the other.
sig_rejects() { printf '%s' "$sig_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA \
                  -g QEW "$@" >/dev/null 2>&1; echo $?; }
check "--self-crib-seeds rejects a negative K" \
  "$(sig_rejects -c --self-crib-seeds -1)" "1"
check "--self-crib-seeds rejects an out-of-range length" \
  "$(sig_rejects -c --self-crib-seeds 1 --self-crib-length 14)" "1"
check "--self-crib-seeds rejects a run with no climb" \
  "$(sig_rejects --self-crib-seeds 1)" "1"
check "--self-crib-seeds rejects --crib" \
  "$(sig_rejects -c --self-crib-seeds 1 --crib DASOBERKOMMANDO)" "1"
check "--self-crib-seeds rejects --exhaust" \
  "$(sig_rejects -c --self-crib-seeds 1 --exhaust 1)" "1"
check "--self-crib-seeds rejects -A" \
  "$(sig_rejects -c --self-crib-seeds 1 -A 100)" "1"
check "--self-crib-seeds rejects --soft-plug" \
  "$(sig_rejects -c --self-crib-seeds 1 --soft-plug AB)" "1"
check "--self-crib-seeds rejects -F" \
  "$(sig_rejects -c --self-crib-seeds 1 -F 10)" "1"

# --confidence must calibrate its null against the climb the SEARCH runs, not the plain
# one: a seeded climb starts from a pinned board and is drawn from a different score
# distribution entirely.  Before this was routed through climb_unit() the seeded run
# sampled plain climbs, so its null line was byte-identical to the unseeded run's -- which
# is exactly what this compares.  The bias is not even one-directional: near the null the
# plain null understates the margin and far out it overstates, so a real break would have
# been reported as more significant than it is.
sig_null() { printf '%s' "$sig_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g A.. \
               -c -T 1 --confidence 24 "$@" 2>&1 >/dev/null \
             | sed -n 's/^Confidence: null \([^ ]*\) .*/\1/p'; }
check "--confidence calibrates against the SEEDED climb, not the plain one" \
  "$(test "$(sig_null --self-crib-seeds 1)" != "$(sig_null -R 1)" \
     && echo differs || echo same)" "differs"
check "--confidence null is unchanged for an unseeded run" \
  "$(sig_null -R 1)" "$(sig_null -R 1)"

# --self-crib-signature hypothesises the doubled word ANYWHERE, not only closing the message.
# The property that matters is exactly that: a message whose doubling sits in the MIDDLE
# is invisible to the terminal mode and recoverable by the swept one.
# A NINE-letter doubled word, not six: swept reliability tracks the signature length,
# because that is what sets the number of equality edges.  Measured on this fixture, a
# 6-letter mid-message doubling is not recovered even at K=100 while 9 and 13 recover at
# K=1 -- so a 6-letter fixture would assert something the mode does not deliver.
swp_pt=DASOBERKOMMANDOXGEZXSTEINECKEXSTEINECKEXMELDETXAACHENXISTGERETTETXDURCHDENEINSATZXENDE
swp_ct=$(run "$swp_pt" -i -u B -w 123 -r AAA -g QEW -s ABCDEFGHIJKLMNOPQRST)
swp_run() { run "$swp_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 0 "$@"; }
check "the default (anywhere) recovers a MID-message doubling" \
  "$(sig_pct "$(swp_run --self-crib-seeds 5 --self-crib-length 7)" "$swp_pt")" "100"
check "--self-crib-signature control: restricting to the end loses it" \
  "$(test "$(sig_pct "$(swp_run --self-crib-seeds 5 --self-crib-signature \
       --self-crib-length 7)" "$swp_pt")" -lt 30 && echo far || echo near)" "far"
# Sweeping is the whole cost difference, so the hypothesis count must actually grow.
swp_hyps() { printf '%s' "$swp_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW \
               -c -R 0 --self-crib-seeds 1 --self-crib-length 7 "$@" 2>&1 >/dev/null \
             | sed -n 's/^Self-crib:.* \([0-9]*\) hypotheses.*/\1/p'; }
check "--self-crib-signature enumerates far fewer hypotheses than the default" \
  "$(test "$(swp_hyps)" -gt "$(( $(swp_hyps --self-crib-signature) * 10 ))" \
     && echo fewer || echo similar)" "fewer"
check "the swept default is -T-independent" \
  "$(swp_run --self-crib-seeds 5 --self-crib-length 7 -T 1)" \
  "$(swp_run --self-crib-seeds 5 --self-crib-length 7 -T 4)"
check "--self-crib-signature rejects a run with no --self-crib-seeds" \
  "$(printf '%s' "$swp_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW -c \
       --self-crib-signature >/dev/null 2>&1; echo $?)" "1"
check "--exhaust E is bounded by the remaining free pairs" \
  "$(np_rejects -c --exhaust 6 --no-plug "$np_many")" "1"
check "--exhaust E within the remaining free pairs is accepted" \
  "$(np_rejects -c --exhaust 5 --no-plug "$np_many")" "0"

# --self-crib-tandem: a doubled word with NO separator between the copies.  The
# default cannot see one at all -- its 26 guesses are on steck[X] and the separator
# anchor is what carries that guess into the message -- so SIEGFRIEDSIEGFRIED forms
# no hypothesis.  Three of the 66 corpus messages carry such a doubling and no
# X-separated one, and this is one of them.
tdm_pt=ANXPANZXGRUPPEXVIERXSIEGFRIEDSIEGFRIEDTONIXDIVXSTEHTSEITXEINSZWOXSIEBENXEINSEINSNULLNULLXUHRMITANFAENGENAMUNTERKUNFTSRAUMXKANNNIQTEINFLIESZENXDAXDRITTEXINFXDIVXUNDXAQTEXPANZXDIVXBLOQIERENUNDRANMBELEGTHALTEXDIVXKDRX
tdm_ct=$(run "$tdm_pt" -u B -w 342 -r ALZ -g VAT -s "AZ DV ET FS GQ JP LX MY NR OW")
# Plugboard-recovery tier: true rotor key, board hidden, one climb from the seed.
tdm_run() { printf '%s' "$tdm_ct" | "$ENIGMA" -c -f -l wehrmacht -u B -w 342 \
            -r ALZ -g VAT --self-crib-seeds 10 --self-crib-length 6 -R 0 "$@" 2>/dev/null; }
# The property that justifies the flag: it recovers what the default cannot.
check "--self-crib-tandem recovers a separator-free doubling" \
  "$(tdm_run --self-crib-tandem -T 1)" "$tdm_pt"
# The control keeps that from passing for the wrong reason -- the message must be
# genuinely out of the default's reach, not merely easy.
check "--self-crib-tandem control: the default does not" \
  "$(test "$(tdm_run -T 1)" = "$tdm_pt" && echo same || echo different)" "different"
check "--self-crib-tandem is -T-independent" \
  "$(tdm_run --self-crib-tandem -T 1)" "$(tdm_run --self-crib-tandem -T 4)"
# It adds hypotheses rather than replacing them: a separated doubling must still be
# hypothesised with the flag on, so the count strictly grows.
tdm_hyps() { { printf '%s' "$tdm_ct" | "$ENIGMA" -c -i -u B -w 342 -r ALZ -g VAT \
              --self-crib-seeds 1 --self-crib-length 6 "$@" >/dev/null; } 2>&1 \
              | sed -n 's/^Self-crib:.* \([0-9]*\) hypotheses.*/\1/p'; }
check "--self-crib-tandem adds hypotheses, does not replace them" \
  "$(awk -v a="$(tdm_hyps)" -v b="$(tdm_hyps --self-crib-tandem)" \
     'BEGIN { print (b > a) ? "more" : "not-more" }')" "more"
check "--self-crib-tandem says so in the settings echo" \
  "$(tdm_hyps --self-crib-tandem >/dev/null; { printf '%s' "$tdm_ct" | "$ENIGMA" -c -i \
     -u B -w 342 -r ALZ -g VAT --self-crib-seeds 1 --self-crib-tandem >/dev/null; } 2>&1 \
     | grep -c 'separated or tandem')" "1"
# Rejections.  --signature says the copies are separated by an X closing the message;
# --tandem says they are not separated at all.  That is a contradiction, not a
# narrowing, so it is refused rather than silently preferring one.
check "--self-crib-tandem rejects a run with no --self-crib-seeds" \
  "$(sig_rejects -c --self-crib-tandem)" "1"
check "--self-crib-tandem rejects --self-crib-signature" \
  "$(sig_rejects -c --self-crib-seeds 1 --self-crib-tandem --self-crib-signature)" "1"

echo
echo "== Crib deduction: --crib =="

# A crib is a guess at part of the plaintext WITH its position. Rotor settings that
# cannot have produced it are rejected by arithmetic, before anything is scored.
# One fixed key and one short wildcard, per the rule that a check is sized to the
# property and not to realism.
cb_pt=DASOBERKOMMANDODERWEHRMACHTGIBTBEKANNTXAACHENXISTGERETTETXDURCHDENEINSATZDERHILFSKRAEFTE
cb_plugs="AB CD EF GH IJ KL MN OP QR ST"
cb_ct=$(run "$cb_pt" -i -u B -w 123 -r AAA -g QEW -s "$cb_plugs")
cb_key='-i -u B -w 123 -r AAA -g QEW'
# Braces so the stdout redirect is unambiguous: this keeps stderr and drops stdout,
# which shellcheck flags as SC2069 when written the other way round on an unpiped
# command (the suite's other uses of that idiom all feed a pipe).
# shellcheck disable=SC2086
cb_err() { { printf '%s' "$cb_ct" | "$ENIGMA" $cb_key "$@" >/dev/null; } 2>&1; }

# The true key must survive its own crib. This is the zero-tolerance property: a
# deduction that rejects the truth loses the message outright.
check "--crib: the true key is not rejected" \
  "$(cb_err --crib OBERKOMMANDO --crib-at 4 | grep -c 'rejected 0 of 1 key')" "1"

# Every plug the surviving hypothesis deduces must match the true board. AB/EF/GH/MN/OP
# are real cables; YY and ZZ say Y and Z carry none, which is true (the board plugs
# A-T only). A single wrong pair here would be a bug, not a near miss.
check "--crib: deduced plugs match the true board" \
  "$(cb_err --crib OBERKOMMANDO --crib-at 4 --crib-dump | grep '^cribstop' \
     | sed 's/^cribstop \([^ ]* \)\{6\}//')" \
  "AB BA EF FE GH HG MN NM OP PO YY ZZ"

# Against a wildcarded start the crib throws away almost everything unscored -- that is
# the whole point -- and the run still recovers the plaintext.
check "--crib: rejects most keys but keeps the answer" \
  "$(run "$cb_ct" -i -u B -w 123 -r AAA -g ... -s "$cb_plugs" --crib OBERKOMMANDO --crib-at 4)" \
  "$cb_pt"
cb_rej=$(printf '%s' "$cb_ct" | "$ENIGMA" -i -u B -w 123 -r AAA -g ... \
         --crib OBERKOMMANDO --crib-at 4 2>&1 >/dev/null \
         | sed -n 's/^Crib: .*rejected \([0-9]*\) of \([0-9]*\).*/\1 \2/p')
check "--crib: rejection is a large majority of the keyspace" \
  "$(printf '%s' "$cb_rej" | awk '{print ($1 > 0.9 * $2)}')" "1"

# The rejection count must not depend on thread count either. A key's restarts can
# straddle a chunk boundary, so two workers can each see it as new -- counting there
# rather than at the key's first work item made this drift with -T.
cb_count() { printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g ... \
             -c -R 4 --crib OBERKOMMANDO --crib-at 4 -T "$1" 2>&1 >/dev/null \
             | sed -n 's/^Crib: .*rejected \([0-9]*\) .*/\1/p'; }
check "--crib: rejection count is -T-independent" "$(cb_count 1)" "$(cb_count 4)"

# Without --crib-at the crib is tried at every alignment the self-encryption filter
# leaves. That filter is pure arithmetic on the ciphertext -- an Enigma never encrypts a
# letter to itself -- and removes roughly half of them.
cb_sweep=$(printf '%s' "$cb_ct" | "$ENIGMA" -i -u B -w 123 -r AAA -g ... \
           --crib OBERKOMMANDO 2>&1 >/dev/null \
           | sed -n 's/^Crib: \([0-9]*\) alignment.*/\1/p')
check "--crib sweeps more than one alignment" \
  "$(printf '%s' "$cb_sweep" | awk '{print ($1 > 1)}')" "1"
check "--crib: the sweep still recovers the plaintext" \
  "$(run "$cb_ct" -i -u B -w 123 -r AAA -g ... -s "$cb_plugs" --crib OBERKOMMANDO)" \
  "$cb_pt"

# --crib-seeds K: IC-rank the surviving hypotheses and climb only the best K, as
# --self-crib-seeds does.  A SWEPT crib leaves many survivors per key and every one
# currently gets a full plugboard climb, which is what puts short cribs out of reach.
# shellcheck disable=SC2086
cs_run() { { printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA \
             -g "AA." -R 0 -T 1 --crib OBERKOMMANDO "$@" >/dev/null; } 2>&1; }
cs_iters() { cs_run "$@" | sed -n 's/.*scored \([0-9]*\) plugboard.*/\1/p'; }
# The saving is the point, so assert it rather than the flag merely being accepted:
# capping at 3 must cost strictly fewer plugboards than climbing every survivor.
check "--crib-seeds climbs fewer plugboards than every survivor" \
  "$(awk -v a="$(cs_iters)" -v b="$(cs_iters --crib-seeds 3)" \
     'BEGIN { print (b < a) ? "fewer" : "no-saving" }')" "fewer"

# A crib-REJECTED key reports unit_no_score (-1e300), which is a sentinel and not a
# score.  Feeding those to the null put ~99% of samples at -1e300: the mean sat at
# ~-1e300, the variance OVERFLOWED to +inf, and (s - mu)/sd came out exactly 0 for
# every board -- so every progress line printed the identical margin -z_k and the
# summary printed a 300-digit null.  The three assertions below are the three
# visible symptoms, so any one of them regressing is caught.
cf_crib() { { printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA \
              -g "..." -R 0 -T 1 --crib OBERKOMMANDO --crib-at 4 --confidence 32 \
              --no-preflight >/dev/null; } 2>&1; }
cf_crib_out=$(cf_crib)
check "--confidence: a crib-rejected key is not counted in the null" \
  "$(printf '%s' "$cf_crib_out" \
     | sed -n 's/^Confidence: null \(-*[0-9.]*\) .*/\1/p' \
     | awk '{ print ($1 > -100) ? "sane" : "overflowed" }')" "sane"
check "--confidence: the null spread stays finite under a crib" \
  "$(printf '%s' "$cf_crib_out" \
     | sed -n 's/.*+\/- \([0-9a-zA-Z.]*\) over.*/\1/p' \
     | awk '{ print ($1 == "inf" || $1 + 0 <= 0) ? "broken" : "finite" }')" "finite"
# The clinching symptom: with the null broken EVERY line read the same margin.
check "--confidence: margins vary from board to board under a crib" \
  "$(printf '%s' "$cf_crib_out" | grep -E '^ *[+-][0-9]' \
     | awk '{ print $1 }' | sort -u | wc -l \
     | awk '{ print ($1 > 1) ? "vary" : "all-identical" }')" "vary"
# ... and the answer must survive the cut.  This is the property that matters: a cheaper
# run that stops finding the key is worthless.  Scored by IC with the board given, which
# also shows the ranking needs NO language and no n-gram table -- it is the index of
# coincidence of the decrypt, exactly as --self-crib-seeds ranks.
check "--crib-seeds still recovers the plaintext" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -c -i -u B -w 123 -r AAA -g ... \
     -s "$cb_plugs" -R 0 -T 1 --crib OBERKOMMANDO --crib-seeds 5 2>/dev/null)" "$cb_pt"
# Cutting the list must not change the answer when the winner is inside the cut.  Run
# under n-grams too, so both scoring families are covered.
check "--crib-seeds agrees with climbing every survivor" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA -g "AA." \
     -R 0 -T 1 --crib OBERKOMMANDO --crib-seeds 8 2>/dev/null)" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA -g "AA." \
     -R 0 -T 1 --crib OBERKOMMANDO 2>/dev/null)"
# K=0 is off, and must leave the historical path byte-identical -- including the count
# of plugboards scored, which the seeded path's deduplication would change.
check "--crib-seeds 0 is exactly the unseeded run" \
  "$(cs_iters --crib-seeds 0)" "$(cs_iters)"
# REGRESSION: --crib with -s used to build a board that was not an involution and then
# SMASH THE STACK formatting it.  The deduction started from an empty board, so a
# hypothesis could deduce A-D while -s said A-B; seeding overwrote steck[A] and left
# steck[B] pointing at A, and format_plugboard -- sized for the 13 pairs an involution
# can have -- walked off the end of its buffer on the 14+ that a corrupt board yields.
# The deduction now starts from what -s and --no-plug already fix, so such a hypothesis
# is rejected by crib_set instead.  Asserted two ways: the run must exit 0, and no
# progress line may carry more than 13 pairs.
check "--crib with -s does not crash" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA -g ... \
     -s "$cb_plugs" -R 0 -T 1 --crib OBERKOMMANDO >/dev/null 2>&1; echo $?)" "0"
check "--crib with -s never echoes an impossible plugboard" \
  "$({ printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA -g ... \
     -s "$cb_plugs" -R 0 -T 1 --crib OBERKOMMANDO >/dev/null; } 2>&1 \
     | progress_lines | awk '{ n = 0; for (i = 5; i < NF; i++) n++; \
       if (n > 13) bad++ } END { print bad+0 }')" "0"
# The deduction must also still agree with the true board when -s is given -- rejecting
# contradictions must not reject the TRUTH, which is the zero-tolerance direction.
check "--crib with -s does not reject the true key" \
  "$(cb_err --crib OBERKOMMANDO --crib-at 4 -s "$cb_plugs" -c \
     | grep -c 'rejected 0 of 1 key')" "1"
# -T-independence: the ranking is per key and deterministic, so thread count cannot
# move the result.
check "--crib-seeds is -T-independent" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA -g ... \
     -R 0 -T 1 --crib OBERKOMMANDO --crib-seeds 4 2>/dev/null)" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -c -q -l german -u B -w 123 -r AAA -g ... \
     -R 0 -T 4 --crib OBERKOMMANDO --crib-seeds 4 2>/dev/null)"
check "--crib-seeds echoes what it will do" \
  "$(cs_run --crib-seeds 7 | grep -c '^Crib seeds: top 7 ')" "1"
# Rejections, all fatal: each names something the command line asks for and the search
# cannot honour.
cs_rejects() { printf '%s' "$cb_ct" | "$ENIGMA" -u B -w 123 -r AAA -g AAA "$@" \
               >/dev/null 2>&1; echo $?; }
check "--crib-seeds rejects a run with no climb" \
  "$(cs_rejects --crib OBERKOMMANDO --crib-seeds 3)" "1"
check "--crib-seeds rejects a run with no crib" \
  "$(cs_rejects -c --crib-seeds 3)" "1"
check "--crib-seeds rejects a negative count" \
  "$(cs_rejects -c --crib OBERKOMMANDO --crib-seeds -1)" "1"
check "--crib-seeds rejects an absurd count" \
  "$(cs_rejects -c --crib OBERKOMMANDO --crib-seeds 99999)" "1"

# The alignment the crib survived at goes in its own progress-line column, since a swept
# run produces lines from many alignments and they are otherwise indistinguishable. The
# line is budgeted to exactly 80 columns either way.
check "--crib: progress lines carry an alignment column" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g ... -c \
     --crib OBERKOMMANDO 2>&1 >/dev/null | grep -c '^ *Score .*  A Text$')" "1"
check "--crib: progress lines stay within 80 columns" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g ... -c \
     --crib OBERKOMMANDO 2>&1 >/dev/null | grep -E "$progress_re|^ *Score " \
     | awk '{ if (length($0) > 80) n++ } END { print n+0 }')" "0"

# The hybrid (archived/cribs.md §7): with -c the climb starts from the plugs each surviving
# hypothesis deduces, held fixed, instead of from an empty board. The plugboard is
# HIDDEN here -- only the crib is given -- which is the case the mode exists for.
cb_pct() { python3 -c 'import sys
t, g = sys.argv[1], sys.argv[2]
print(int(100.0 * sum(a == b for a, b in zip(t, g)) / len(t)))' "$cb_pt" "$1"; }
check "--crib: the seeded climb recovers most of a hidden board" \
  "$(cb_pct "$(run "$cb_ct" -q -l german -u B -w 123 -r AAA -g QEW -c \
                  --crib OBERKOMMANDO --crib-at 4)" | awk '{print ($1 > 80)}')" "1"
# The control: the same climb without the crib gets nowhere on this message, so the
# check above cannot pass for some reason other than the seeding.
check "--crib control: the same climb unseeded does not" \
  "$(cb_pct "$(run "$cb_ct" -q -l german -u B -w 123 -r AAA -g QEW -c -R 64)" \
     | awk '{print ($1 < 40)}')" "1"
# Sweeping costs nothing for seeding: a 12-letter crib cannot filter a swept search
# (§4.2a), but the right alignment's hypothesis still produces the best board.
check "--crib: seeding works swept, not just pinned" \
  "$(cb_pct "$(run "$cb_ct" -q -l german -u B -w 123 -r AAA -g QEW -c \
                  --crib OBERKOMMANDO)" | awk '{print ($1 > 80)}')" "1"

# Deterministic, like every other search option.
check "--crib is -T-independent" \
  "$(run "$cb_ct" -q -l german -u B -w 123 -r AAA -g ... -c -R 4 --crib OBERKOMMANDO --crib-at 4 -T 1)" \
  "$(run "$cb_ct" -q -l german -u B -w 123 -r AAA -g ... -c -R 4 --crib OBERKOMMANDO --crib-at 4 -T 4)"

# A crib cannot sit where it matches the ciphertext: an Enigma never encrypts a letter
# to itself. Silently rejecting every key would look like a hard message rather than a
# bad alignment, so it is fatal.
cb_self=$(printf '%s' "$cb_ct" | cut -c4-15)
check "--crib: self-encrypting alignment rejected" \
  "$(cb_err --crib "$cb_self" --crib-at 4 >/dev/null 2>&1; echo $?)" "1"

# The combinations archived/cribs.md §8 rules out, each fatal at option-parsing time.
cb_reject() { printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g ... \
                "$@" >/dev/null 2>&1; echo $?; }
check "--crib without --crib-at sweeps (accepted)" \
  "$(cb_reject --crib OBERKOMMANDO)" "0"
check "--crib-at without --crib rejected" "$(cb_reject --crib-at 4)" "1"
# --crib-at is 1-based, so 0 is not a position. It must be rejected at PARSE
# time: 0 - 1 is -1, which is the "not given" sentinel, so a fall-through would
# silently mean "sweep every alignment" instead of erroring.
check "--crib-at 0 rejected (1-based)" \
  "$(cb_reject --crib OBERKOMMANDO --crib-at 0)" "1"
check "--crib-at negative rejected" \
  "$(cb_reject --crib OBERKOMMANDO --crib-at -2)" "1"
check "--crib with -F rejected" \
  "$(cb_reject -c --crib OBERKOMMANDO --crib-at 4 -F 5)" "1"
check "--crib with --exhaust rejected" \
  "$(cb_reject -c --crib OBERKOMMANDO --crib-at 4 --exhaust 1)" "1"
check "--crib with -A rejected" \
  "$(cb_reject -c --crib OBERKOMMANDO --crib-at 4 -A 100)" "1"
check "--crib past the end of the ciphertext rejected" \
  "$(cb_reject --crib OBERKOMMANDO --crib-at 900)" "1"
check "--crib with a non-letter rejected" \
  "$(cb_reject --crib "OBERKOMM4NDO" --crib-at 4)" "1"

echo
echo "== Crib libraries: --crib-list =="

# A library is written against a network's vocabulary, not against one message, so most
# of its cribs do not fit any given message. The list runs one rotor sweep per crib
# (crib-outer, archived/cribs.md §6.7) and keeps the best board across all of them.
cl_file=$(mktemp)
printf '# a library\nOBERKOMMANDO\nXSIEGFRIEDXX\n\nNOTINTHISMESSAGE\n' > "$cl_file"
cl_run() { printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW \
             --crib-list "$cl_file" "$@"; }

# The winning crib's answer is the run's answer: a later, worse crib must not displace
# an earlier, better one. Graded like the single-crib checks above -- the board is
# hidden here, so most-of-it is the property, not exactness.
check "--crib-list recovers most of a hidden board" \
  "$(cb_pct "$(cl_run -c 2>/dev/null)" | awk '{print ($1 > 80)}')" "1"

# Comments, blank lines and duplicates are dropped; the count of table rows is what the
# loader actually kept, so an off-by-one in the parser shows up here.
check "--crib-list loads the cribs it should" \
  "$(cl_run 2>&1 >/dev/null | grep -cE '^ +[1-9][0-9]*  [A-Z]')" "3"

# Every crib gets a row carrying its measured cost and expected gain, which is what
# makes a skip legible rather than arbitrary.
check "--crib-list reports a gain column" \
  "$(cl_run -c 2>&1 >/dev/null | grep -c 'hyp/key .*gain')" "1"
check "--crib-list reports a gain for each crib" \
  "$(cl_run -c 2>&1 >/dev/null | grep -cE '^ +[1-9][0-9]*  [A-Z].*([0-9]x|-)$')" "3"

# Deterministic like every other search option -- including the estimate, which is
# single-threaded and counts boards rather than timing them precisely so that a
# reported number, and any skip decision, cannot depend on thread timing.
check "--crib-list is -T-independent" \
  "$(cl_run -c -T 1 2>/dev/null)" "$(cl_run -c -T 4 2>/dev/null)"
check "--crib-list cost table is -T-independent" \
  "$(cl_run -c -T 1 2>&1 >/dev/null | grep -E '^ +[1-9][0-9]*  [A-Z]')" \
  "$(cl_run -c -T 4 2>&1 >/dev/null | grep -E '^ +[1-9][0-9]*  [A-Z]')"

# A crib longer than the ciphertext, or one that cannot sit anywhere, is fatal for a
# single --crib but ORDINARY for a library: skip it and try the next. The ciphertext
# itself is the one crib guaranteed to have no viable alignment -- it has exactly one,
# and an Enigma never encrypts a letter to itself.
cl_bad=$(mktemp)
printf '%s\n%s\nOBERKOMMANDO\n' "$(printf 'A%.0s' $(seq 1 200))" "$cb_ct" > "$cl_bad"
cl_bad_run() { printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA \
                 -g QEW -c --crib-list "$cl_bad"; }
check "--crib-list skips unusable cribs instead of dying" \
  "$(cb_pct "$(cl_bad_run 2>/dev/null)" | awk '{print ($1 > 80)}')" "1"
check "--crib-list says why each crib was skipped" \
  "$(cl_bad_run 2>&1 >/dev/null | grep -c 'skipped: \(longer than\|cannot sit\)')" "2"
rm -f "$cl_bad"

# The column header is printed once for the whole run, not once per crib, and a later
# crib must not re-echo boards worse than the best already shown.
check "--crib-list prints one progress header for the run" \
  "$(cl_run -c 2>&1 >/dev/null | grep -c '^ *Score .*Text$')" "1"

# Ordering: cheapest measured cost first by default, since the cost spread across
# crib lengths is ~90x and runs opposite to intuition (a 20-letter crib swept in
# 0.15 s where a 10-letter one took 13.65 s). Ordering discards nothing, so it can
# default on: it discards nothing, so the worst case is a later win.
cl_order() { cl_run -c "$@" 2>&1 >/dev/null \
               | grep -E '^ +[1-9][0-9]*  [A-Z]' | awk '{print $2}'; }
check "--crib-list keeps file order under --no-crib-reorder" \
  "$(cl_order --no-crib-reorder | tr '\n' ' ')" \
  "OBERKOMMANDO XSIEGFRIEDXX NOTINTHISMESSAGE "
# NOTINTHISMESSAGE (16) rejects the whole sample and so costs nothing; the two shorter
# cribs cost real climbs. Cheapest-first therefore has to move it to the front.
check "--crib-list runs the cheapest crib first by default" \
  "$(cl_order | head -1)" "NOTINTHISMESSAGE"
# Order changes which crib runs first, never which board wins.
check "--crib-list result does not depend on the order" \
  "$(cl_run -c 2>/dev/null)" "$(cl_run -c --no-crib-reorder 2>/dev/null)"

# The option combinations, per archived/cribs.md §8. --crib-at pins ONE alignment and the cribs
# in a list differ in length, so the two cannot be combined.
cl_reject() { printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW \
                "$@" >/dev/null 2>&1; echo $?; }
check "--crib-list with --crib rejected" \
  "$(cl_reject --crib-list "$cl_file" --crib OBERKOMMANDO)" "1"
check "--crib-list with --crib-at rejected" \
  "$(cl_reject --crib-list "$cl_file" --crib-at 4)" "1"
check "--crib-list with -F rejected" \
  "$(cl_reject -c --crib-list "$cl_file" -F 5)" "1"
check "--no-crib-reorder without --crib-list rejected" \
  "$(cl_reject --crib OBERKOMMANDO --no-crib-reorder)" "1"
check "--crib-order is no longer accepted" \
  "$(cl_reject --crib-list "$cl_file" --crib-order cost)" "1"
check "--crib-max-hyps is no longer accepted" \
  "$(cl_reject --crib-list "$cl_file" --crib-max-hyps 5)" "1"
check "--crib-list with a missing file rejected" \
  "$(cl_reject --crib-list /nonexistent/crib/library)" "1"
cl_empty=$(mktemp)
printf '# only comments\n\n' > "$cl_empty"
check "--crib-list with no usable cribs rejected" \
  "$(cl_reject --crib-list "$cl_empty")" "1"
rm -f "$cl_empty" "$cl_file"

# The old --crib-file name is gone: it re-ranked finished boards and now collides in
# meaning with --crib-list, so it was renamed (archived/cribs.md §8).
check "--crib-file is no longer accepted" \
  "$(printf '%s' "$cb_ct" | "$ENIGMA" -q -l german -u B -w 123 -r AAA -g QEW -c \
     --crib-file /dev/null >/dev/null 2>&1; echo $?)" "1"

# Every long option must appear in --help. --crib-dump was absent for four
# releases because nothing checked, so this compares the getopt table in the
# source against the help text rather than trusting a human to notice.
help_missing=$("$ENIGMA" -h 2>&1 > /tmp/enigma_help.$$ ; \
  grep -ohE '\{ "[a-z-]+",' "$(dirname "$0")"/../src/*.cc \
  | sed 's/{ "//;s/",//' | sort -u \
  | while read -r o; do grep -q -- "--$o" /tmp/enigma_help.$$ || echo "$o"; done)
rm -f /tmp/enigma_help.$$
check "help lists every long option" "$help_missing" ""

# A numeric option given a non-number must FAIL, not read as 0.  atoi/atof
# cannot report failure, and 0 is "off" for most of these -- so a typo used to
# silently disable what was asked for, with no trace: at -R 0 the settings echo
# omits the restart line entirely, and --confidence 0 prints nothing at all.
#
# Only -T, -x and --ring-stride caught it before, and only by accident: their
# valid ranges exclude 0, so the BOUNDS check rejected what the PARSE had let
# through.  Those three are in the list below to keep them covered for the
# right reason.
#
# Each case asserts the exit status AND that the message names the option, so
# a future refactor cannot satisfy this by failing for some unrelated reason.
num_reject()   # num_reject <option-name-in-message> <args...>
{
  want=$1
  shift
  err=$(printf 'AAAA' | "$ENIGMA" -i "$@" 2>&1 >/dev/null)
  st=$?
  if [ "$st" -ne 0 ] && printf '%s' "$err" | grep -q "Illegal value for $want"; then
    echo 1
  else
    echo 0
  fi
}
for spec in "-R:-R junk" "-T:-T junk" "-x:-x junk" "-A:-A junk" \
            "-F:-F junk" "-e:-e junk" "--confidence:--confidence junk" \
            "--random:--random junk" "--exhaust:--exhaust junk" \
            "--ring-stride:--ring-stride junk" "--tune-phase:--tune-phase junk" \
            "--crib-weight:--crib-weight junk" "--crib-at:--crib-at junk" \
            "--crib-seeds:--crib-seeds junk" \
            "--self-crib-seeds:--self-crib-seeds junk" \
            "--self-crib-length:--self-crib-length junk" \
            "--doubling-report:--doubling-report junk" \
            "--doubling-mismatches:--doubling-mismatches junk"
do
  nr_name=${spec%%:*}
  nr_args=${spec#*:}
  # shellcheck disable=SC2086
  check "$nr_name rejects a non-number" "$(num_reject "$nr_name" $nr_args)" "1"
done

# Trailing junk is the realistic typo -- "64O" for "640" -- and atoi read it as
# 64, which is a plausible number and so leaves nothing to notice.
check "-R rejects trailing characters" "$(num_reject '-R' -R 64O)" "1"
check "-R rejects an empty argument"   "$(num_reject '-R' -R '')" "1"

# ...while the values that were always legal must still be.  -R 0 and -e 0 are
# the ones that matter: 0 is a real setting for both (one deterministic climb;
# the historical RNG stream), so a parser that rejected it would be worse than
# the bug.
num_ok()
{
  printf 'AAAA' | "$ENIGMA" -i -u B -w 123 -r AAA -g AAA "$@" >/dev/null 2>&1
  echo $?
}
check "-R 0 is still legal"            "$(num_ok -R 0)" "0"
check "-e 0 is still legal"            "$(num_ok -e 0)" "0"
check "-e takes a full 64-bit seed"    "$(num_ok -e 18446744073709551615)" "0"
check "-e rejects a negative seed"     "$(num_reject '-e' -e -5)" "1"
check "-F N% is still legal"           "$(num_ok -c -F 10%)" "0"
check "-F rejects junk before the %"   "$(num_reject '-F' -F abc%)" "1"

# The measurement-only environment overrides are the same hazard with a worse
# consequence: ENIGMA_IC_BLEND=typo silently set the blend to 0, turning -f
# into -a, so a probe would have quietly measured the baseline instead of the
# variant it was testing.
env_reject()
{
  ev=$1
  shift
  err=$(printf 'AAAA' | env "$ev" "$ENIGMA" -u B -w 123 -r AAA -g AAA "$@" \
        2>&1 >/dev/null)
  st=$?
  if [ "$st" -ne 0 ] && printf '%s' "$err" | grep -q 'Illegal value for \$'; then
    echo 1
  else
    echo 0
  fi
}
check "\$ENIGMA_SEED rejects a non-number" \
  "$(env_reject ENIGMA_SEED=junk -i)" "1"
check "\$ENIGMA_IC_BLEND rejects a non-number" \
  "$(env_reject ENIGMA_IC_BLEND=junk -f -l english)" "1"
check "\$ENIGMA_MONOIC_BLEND rejects a non-number" \
  "$(env_reject ENIGMA_MONOIC_BLEND=junk -c -S k -l english -g AAA)" "1"
check "-S k (mono+IC) is accepted and named in the settings echo" \
  "$(printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH' | "$ENIGMA" -c -S k -l english \
     -u B -w 123 -r AAA -g AAA 2>&1 >/dev/null | grep -c 'monograms + IC')" "1"
check "-S k4f10 (mono+IC pre-pass, fused target) parses" \
  "$(printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH' | "$ENIGMA" -c -S k4f10 -l english \
     -u B -w 123 -r AAA -g AAA >/dev/null 2>&1; echo $?)" "0"
check "-S k needs a language, like every other n-gram model" \
  "$(printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH' | "$ENIGMA" -c -S k \
     -u B -w 123 -r AAA -g AAA >/dev/null 2>&1; echo $?)" "1"
check "\$ENIGMA_LOGLIN rejects a partial weight vector" \
  "$(printf 'AAAA' | env ENIGMA_LOGLIN=1,0.6 "$ENIGMA" -q -l english \
     >/dev/null 2>&1; echo $?)" "1"

echo
echo "passed: $pass, failed: $fail"
[ "$fail" -eq 0 ]

echo "== Biased restart kick: --biased-random =="

# Draw the kick's pairs from exp(z / T) over the 325 single-plug IC z-scores
# instead of uniformly.  Measured worth ~+8% of breaks at -R 3..5 and nothing
# from -R 6 up (eval/results-weighted-kick.txt).
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
br_bad() { printf 'AAAA' | "$ENIGMA" "$@" 2>&1 >/dev/null; }

br_err=$(br_bad -c -q -l english -R 2 --biased-random 0.001)
check "--biased-random rejects an out-of-range temperature" \
  "$(printf '%s' "$br_err" | grep -c 'Illegal --biased-random')" "1"
br_err=$(br_bad -q -l english -R 2 --biased-random 1)
check "--biased-random needs -c" \
  "$(printf '%s' "$br_err" | grep -c 'biased-random needs -c')" "1"
br_err=$(br_bad -c -q -l english -R 0 --biased-random 1)
check "--biased-random needs at least one restart" \
  "$(printf '%s' "$br_err" | grep -c 'needs --restarts 1 or more')" "1"
br_err=$(br_bad -c -q -l english -R 2 --random 0 --biased-random 1)
check "--biased-random needs a non-empty kick" \
  "$(printf '%s' "$br_err" | grep -c 'needs --random 1 or more')" "1"
br_err=$(br_bad -c -q -l english -R 2 -A 100 --biased-random 1)
check "--biased-random rejects -A" \
  "$(printf '%s' "$br_err" | grep -c 'not supported with -A')" "1"
br_err=$(br_bad -c -q -l english -R 2 --exhaust 1 --biased-random 1)
check "--biased-random rejects --exhaust" \
  "$(printf '%s' "$br_err" | grep -c 'not supported with --exhaust')" "1"

# A 60-letter message with a 10-pair board hidden: hard enough that the kick
# actually matters, small enough to be instant.
br_pt="ANXPANZXGRUPPEXVIERXSIEGFRIEDSIEGFRIEDTONIXDIVXSTEHTSEITXEIN"
br_ct=$(run "$br_pt" -i -u B -w 231 -r AAA -g QMW -s "AH BR CM DE FJ NZ PX QU ST VW")
# shellcheck disable=SC2069  # deliberate: keep stderr, discard stdout
br_run() { printf '%s' "$br_ct" | ENIGMA_SEED=0 "$ENIGMA" -c -q -l english \
             -u B -w 231 -r AAA -g QMW -R 4 "$@" 2>&1 >/dev/null; }
br_scored() { br_run "$@" | sed -n 's/.*scored \([0-9]*\) plugboards.*/\1/p'; }

check "--biased-random is echoed with its temperature" \
  "$(br_run --biased-random 1 | grep -c 'kick bias: softmax T = 1')" "1"
check "--biased-random marks the kick line" \
  "$(br_run --biased-random 1 | grep -c '4 restarts, 10-pair kick, IC-biased')" "1"
check "no --biased-random leaves the kick line unmarked" \
  "$(br_run | grep -c '4 restarts, 10-pair kick$')" "1"

# THE CHECK THAT CAN ACTUALLY FAIL.  Everything above passes if the flag is
# parsed and then ignored; this one does not.  A no-op biased_perturb() -- or
# one wired so the uniform path still runs -- gives the SAME plugboard count as
# the unbiased run, because the RNG stream and the climb are otherwise
# identical.  Verified by stubbing biased_perturb() to call
# perturb_steckerbrett(): this check fails and the six validation checks above
# do not.
check "--biased-random actually changes the search" \
  "$([ "$(br_scored --biased-random 0.35)" != "$(br_scored)" ] && echo differs)" \
  "differs"

# -T independence: the weights come from the key alone and the draw from the
# per-(key,restart) stream, so neither depends on how the work was split.
br_t1=$(printf '%s' "$br_ct" | ENIGMA_SEED=0 "$ENIGMA" -c -q -l english \
          -u B -w 231 -r AAA -g QM. -R 4 --biased-random 1 -T 1 2>/dev/null)
br_t4=$(printf '%s' "$br_ct" | ENIGMA_SEED=0 "$ENIGMA" -c -q -l english \
          -u B -w 231 -r AAA -g QM. -R 4 --biased-random 1 -T 4 2>/dev/null)
check "--biased-random is -T independent" "$br_t1" "$br_t4"

# And it still recovers, which a badly-normalised weight vector would break by
# concentrating every kick on one pair.  A LONGER fixture than the one above:
# 60 letters against a 10-pair board recovers only ~15% of the time even
# working correctly, so a check on it would fail for reasons that have nothing
# to do with the flag.
br_pt2="ANXPANZXGRUPPEXVIERXSIEGFRIEDSIEGFRIEDTONIXDIVXSTEHTSEITXEINSZWOXSIEBENXEINSEINSNULLNULLXUHRMITANFAENGENAMUNTERKUNFTSRAUMXKANNNIQTEINFLIESZENXDAXDRITT"
br_ct2=$(run "$br_pt2" -i -u B -w 231 -r AAA -g QMW -s "AH BR CM DE FJ NZ PX QU ST VW")
check "--biased-random still recovers the plaintext" \
  "$(printf '%s' "$br_ct2" | ENIGMA_SEED=0 "$ENIGMA" -c -f -l wehrmacht \
       -S k4f10 -u B -w 231 -r AAA -g QMW -R 8 --biased-random 1 2>/dev/null)" \
  "$br_pt2"

echo "== Histogram-form low-order climb (-S i/m/k) =="

# IC, mono and k are functions of one 26-bin histogram of the decrypt taken
# BEFORE the exit plugboard, and that histogram is a sum of columns of a
# per-key co-occurrence table -- so a toggle costs O(26) instead of O(L).  The
# whole feature rests on the two paths being BYTE-IDENTICAL, so what is checked
# here is exactly that: ENIGMA_HIST=0 sends the same climb back through the
# decoders, and every converged board, every plug, and the plugboards-scored
# counter must come out the same.
#
# THE COUNTER IS PART OF THE ASSERTION, not decoration.  The dumped boards
# compare the ENDPOINTS of each climb; the counter compares how many scorings
# it took to get there, so a fast path that reached the same answer by a
# different route -- or that silently skipped work -- fails here even when the
# boards agree.
#
# VERIFIED BY INJECTION, and two of the three do not fail, they HANG.  Removing
# the hist_resync() in the first-improvement loop fails 8 of these 11 checks
# outright.  Removing the one in the steepest-ascent scan, or building the
# co-occurrence table only once so it goes stale across keys, makes the climb
# NEVER CONVERGE: the steepest loop repeats while `best_score > last_best`, and
# with a histogram that no longer matches the board that comparison can stay
# true forever.  So the resync is load-bearing for TERMINATION and not only for
# the answer -- and a CI failure here may present as a timeout rather than as a
# FAIL line.
#
# 26 keys, deliberately: this asserts that two runs AGREE, which needs no
# breadth (see $rgd above).  --dump-all reports every converged climb rather
# than only the winner, so it compares -R x 26 boards, not one.
hist_both() {
  _h=$1; shift
  printf '%s' "$hist_ct" | ENIGMA_SEED=0 ENIGMA_HIST="$_h" "$ENIGMA" \
    -c -l wehrmacht -R 3 --dump-all "$@" 2>&1 \
    | grep -E 'dumpall|Analysed' | sort
}
hist_same() {
  _name=$1; shift
  check "$_name" "$(hist_both 1 "$@" | md5sum)" "$(hist_both 0 "$@" | md5sum)"
}

hist_pt="ANXPANZXGRUPPEXVIERXSIEGFRIEDSIEGFRIEDTONIXDIVXSTEHTSEITXEINSZWOXSIEBENXEINSEINSNULLNULLXUHRMITANFAENGENAMUNTERKUNFT"
hist_ct=$(run "$hist_pt" -i -u B -w 231 -r AAA -g QMW \
           -s "AH BR CM DE FJ NZ PX QU ST VW")

# A non-empty comparison is the precondition for the rest: an option typo would
# make both arms print nothing and every check below would pass vacuously.
check "histogram identity fixture produces boards to compare" \
  "$(hist_both 1 -u B -w 231 -r AAA -g "$rgd" -S k4f10 -J \
       | grep -c dumpall)" "78"

# The three models, each as a bare target and as the cap stage of the
# recommended recipe.  -J and steepest ascent are different move loops and both
# carry the fast path.
hist_same "histogram climb: -S i is byte-identical" \
  -u B -w 231 -r AAA -g "$rgd" -S i -J
hist_same "histogram climb: -S m is byte-identical" \
  -u B -w 231 -r AAA -g "$rgd" -S m -J
hist_same "histogram climb: -S k is byte-identical" \
  -u B -w 231 -r AAA -g "$rgd" -S k -J
hist_same "histogram climb: -S k4f10 -J is byte-identical" \
  -u B -w 231 -r AAA -g "$rgd" -S k4f10 -J --polish
hist_same "histogram climb: -S i4f10 steepest is byte-identical" \
  -u B -w 231 -r AAA -g "$rgd" -S i4f10
# -M makes the cap a strict descent target, so the pre-pass converges from a
# different direction and exercises the merge/remove cases the default's
# over-cap board does not.
hist_same "histogram climb: -S i4q6 -M is byte-identical" \
  -u B -w 231 -r AAA -g "$rgd" -S i4q6 -M
# -s pins letters the climb may not rewire, so the toggle plan must agree with
# the mutate/restore path about which moves exist at all.
hist_same "histogram climb: -s pins are byte-identical" \
  -u B -w 231 -r AAA -g "$rgd" -S k4f10 -J -s "AH BR"
# --biased-random shares the co-occurrence table with the climb; if either
# rebuilt it at the wrong moment the other would read a stale one.
hist_same "histogram climb: --biased-random shares the table safely" \
  -u B -w 231 -r AAA -g "$rgd" -S k4f10 -J --biased-random 1

# The table is keyed on the ROTOR STACK, and Norway and M4 reach it through a
# rotor-index translation and a folded Greek wheel respectively -- the two
# places a wheel-table bug hides while every standard-mode test passes.
hist_n_ct=$(run "$hist_pt" -i -n -u N -w 123 -r AAA -g QMW \
             -s "AH BR CM DE FJ NZ")
hist_ct=$hist_n_ct
hist_same "histogram climb: Norway is byte-identical" \
  -n -u N -w 123 -r AAA -g "$rgd" -S k4f10 -J
hist_m4_ct=$(run "$hist_pt" -i -4 -u b -w B317 -r AAAA -g BQMW \
              -s "AH BR CM DE FJ NZ")
hist_ct=$hist_m4_ct
hist_same "histogram climb: M4 is byte-identical" \
  -4 -u b -w B317 -r AAAA -g B"$rgd" -S k4f10 -J
