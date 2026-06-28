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
#   * End-to-end cracking tests: a full matrix of brute-force start-position
#     recovery for every scoring model (IC/mono/bi/tri/quad) in every language
#     (german/english/danish/french), plus plugboard hill-climb recovery for the
#     combinations that reliably converge (trigram in every language; bigram and
#     quadgram in english).

set -u

cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)" || exit 1

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

# The n-gram parser tolerates blank lines and irregular whitespace without
# hanging or erroring (guards the fscanf field-count / leading-space fix).
printf '\n  E 529117365\n\nT 390965105\nA 374061888\n   \n' > zztest_monograms.txt
check "n-gram parser tolerates messy file" \
  "$(run 'BDZGOWCXLT' -m -l zztest -u B -w 123 -r AAA -g AAA)" \
  "AAAAAAAAAA"
rm -f zztest_monograms.txt

echo "== End-to-end cracking =="

# Genuine per-language plaintexts (A-Z only; accents/umlauts transliterated, e.g.
# ae oe ue / aa). Used for both the brute-force and hill-climb matrices below.
pt_german="DIEENIGMAMASCHINEWURDEIMZWEITENWELTKRIEGVONDERDEUTSCHENWEHRMACHTVERWENDETUMGEHEIMENACHRICHTENZUVERSCHLUESSELNABERDIEALLIIERTENKONNTENDENGEHEIMENCODETROTZDEMBRECHEN"
pt_english="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTEN"
pt_danish="DETVARENGANGENLILLEHAVFRUESOMBOEDELANGTUDEPAAHAVETSBUNDSAMMENMEDSINFADEROGSINEFEMSOESTREHUNVARDENYNGSTEOGSMUKKESTEAFDEMALLEMENHUNLAENGTESEFTERATKOMMEOPTILMENNESKENE"
pt_french="LESSANGLOTSLONGSDESVIOLONSDELAUTOMNEBLESSENTMONCOEURDUNELANGUEURMONOTONETOUTSUFFOCANTETBLEMEQUANDSONNELHEUREJEMESOUVIENSDESJOURSANCIENSETALORSJEPLEUREETJEMENVAIS"
plain_for() {
  case $1 in
    german)  printf '%s' "$pt_german"  ;;
    english) printf '%s' "$pt_english" ;;
    danish)  printf '%s' "$pt_danish"  ;;
    french)  printf '%s' "$pt_french"  ;;
  esac
}

# (1) Brute-force the start position with every scoring model in every language.
# Each plaintext is encrypted at start QXP (no plugboard) and recovered by
# wildcarding the start (-g ...). All 4 languages x 5 models must recover when
# -l matches the plaintext language. (This also guards the IC formula fix: the
# old -i formula could not distinguish plaintext from gibberish and returned the
# wrong key. Note quadgrams need the matching -l -- they are the most
# language-specific model; see the gotcha in CLAUDE.md.)
for lang in german english danish french; do
  plain=$(plain_for "$lang")
  ct=$(run "$plain" -i -u B -w 123 -r AAA -g QXP)
  for mode in -i -m -b -t -q; do
    check "crack: start position, $lang $mode" \
      "$(run "$ct" $mode -u B -w 123 -r AAA -g ... -l "$lang")" \
      "$plain"
  done
done

# (2) Hill-climb the plugboard (rotor/ring/start known, plugboard unknown).
# Trigram scoring recovers a 3-pair plugboard in every language; bigram and
# quadgram recover it for English. IC and monogram scoring lack the signal to
# recover a plugboard by hill-climbing, and bigram/quadgram hit
# language-dependent local optima (hill-climbing is a greedy heuristic), so those
# combinations are intentionally not asserted -- the brute-force matrix above
# already exercises every scoring model in every language.
for lang in german english danish french; do
  plain=$(plain_for "$lang")
  ct=$(run "$plain" -i -u B -w 123 -r AAA -g AAA -s "AB CD EF")
  check "crack: hill-climb plugboard, $lang -t" \
    "$(run "$ct" -t -c -u B -w 123 -r AAA -g AAA -l "$lang")" \
    "$plain"
done
en_hc_ct=$(run "$pt_english" -i -u B -w 123 -r AAA -g AAA -s "AB CD EF")
check "crack: hill-climb plugboard, english -b" \
  "$(run "$en_hc_ct" -b -c -u B -w 123 -r AAA -g AAA -l english)" \
  "$pt_english"
check "crack: hill-climb plugboard, english -q" \
  "$(run "$en_hc_ct" -q -c -u B -w 123 -r AAA -g AAA -l english)" \
  "$pt_english"

echo
echo "passed: $pass, failed: $fail"
[ "$fail" -eq 0 ]
