#!/bin/sh
#
# Cracking-quality benchmark for the enigma tool -- how well it recovers SHORT
# messages, which is the hard regime. This is deliberately separate from:
#
#   * tests/run_tests.sh  -- correctness (pass/fail known-answer + round-trip),
#   * tests/bench.sh      -- speed (keys/s, climbs/s).
#
# Here we measure RECOVERY QUALITY as a function of ciphertext length, so we can
# tell whether a change to the scoring or the search makes hard (short) problems
# crack better. Run with `make crackquality`, or directly with
# `sh tests/crack_quality.sh` from anywhere (it cd's to the repo root so it can
# find the n-gram files).
#
# --- What it does (the cheap "plugboard-recovery" tier) ----------------------
#
# For each length L it runs TRIALS independent random problems:
#   * a random excerpt of length L from a per-language passage,
#   * a random rotor key (reflector / 3 wheels / ring / start),
#   * a random PAIRS-pair plugboard (default 10, the historically hard case).
# Each problem is encrypted, then handed back to the tool with the TRUE rotor
# key fixed and only the plugboard hill-climbed (`-c`). This isolates plugboard
# recovery from rotor-key discrimination -- the cheap tier for the dev loop. A
# future "full-crack" tier would additionally wildcard the rotor key.
#
# --- The metric --------------------------------------------------------------
#
# We harvest the tool's own `-p` comparison ("N of M letters (P%) identical").
# Per length we report:
#   * mean %-correct -- a GRADED signal (smooth near the difficulty cliff, so it
#     detects small improvements that binary pass/fail would miss), and
#   * exact-recovery rate -- the fraction of trials recovered 100% correct.
# %-correct of the recovered PLAINTEXT (not key equality) is the right target:
# on short messages some settings are fundamentally unidentifiable (e.g. the
# ring of a rotor that never steps), so many keys yield the identical decrypt.
#
# Headline numbers: L50 / L90 -- the shortest length whose exact-recovery rate
# reaches 50% / 90%. "We moved L90 from 120 to 85 characters" is the crisp
# improvement statement; lower is better.
#
# --- Reproducibility and A/B -------------------------------------------------
#
#   make crackquality                  working-tree binary
#   make crackquality BASE=<git-ref>   same-machine A/B vs <git-ref>
#
# A fixed SEED makes the trial set deterministic, so an A/B solves the IDENTICAL
# problems with both binaries -- differences are real, not sampling noise (same
# idea as bench.sh's A/B, but for recovery rate instead of time). The cipher and
# the n-gram tables are assumed unchanged between the two revisions. (awk's rand()
# differs across awk implementations, so absolute numbers are only comparable on
# the same machine -- like bench timings -- but an A/B on one machine is valid.)
#
# --- Failure-mode split (SPLIT=1) --------------------------------------------
#
# A non-recovered short message fails for one of two reasons, which need OPPOSITE
# fixes, so SPLIT=1 labels each non-exact trial:
#   * SCORING failure -- the true plugboard does NOT score highest, so even a
#     perfect search could not pick it; only a better SCORING function helps.
#   * SEARCH failure  -- the true plugboard scores higher than what the climb
#     found, i.e. the hill-climb stuck in a local optimum; a better SEARCH
#     (restarts, simulated annealing, ...) helps.
# It is decided by an oracle: score the decrypt under the KNOWN true plugboard
# (one extra fixed-config run, no `-c`) and compare to the climb's achieved score.
# true > found => search failure; otherwise scoring failure. This tells you which
# lever to pull at each length. Adds one run per trial (roughly 2x slower); not
# combined with BASE (SPLIT is ignored when an A/B is requested).
#
# Tunables (environment): MODEL (i/m/b/t/q, default q), CLANG (crack language,
# default english), TRIALS, LENGTHS, PAIRS, SEED, BASE, SPLIT.

set -u

cd "$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)" || exit 1

HEAD_BIN=./enigma
if [ ! -x "$HEAD_BIN" ]; then
  echo "error: $HEAD_BIN not built; run 'make' first" >&2
  exit 1
fi

MODEL=${MODEL:-q}
CLANG=${CLANG:-english}
TRIALS=${TRIALS:-40}
LENGTHS=${LENGTHS:-"40 70 100 140 190 250 320"}
PAIRS=${PAIRS:-10}
SEED=${SEED:-1}
SPLIT=${SPLIT:-0}    # 1 = classify each non-recovered trial as scoring vs search failure

# --- per-language corpora (A-Z only); excerpts are drawn from these -----------
# Shared with the cracking tests in run_tests.sh.
C_english="THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUREOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDINTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERALYEARSANDSAVEDCOUNTLESSLIVES"
C_german="DIEENIGMAMASCHINEWURDEIMZWEITENWELTKRIEGVONDERDEUTSCHENWEHRMACHTVERWENDETUMGEHEIMENACHRICHTENZUVERSCHLUESSELNABERDIEALLIIERTENKONNTENDENGEHEIMENCODETROTZDEMBRECHENWEILDIEDEUTSCHENOFTDIEGLEICHENFLOSKELNVERWENDETENUNDWEILVIELEBEDIENERIMMERWIEDERDIESELBENFEHLERMACHTENDIEPOLNISCHENUNDBRITISCHENMATHEMATIKERBAUTENMASCHINENUMDIETAEGLICHENSCHLUESSELZUFINDENUNDLASENSODIEGEHEIMENFUNKSPRUECHEDESFEINDESMITUNDVERKUERZTENDADURCHDENKRIEGUMMEHREREJAHREUNDRETTETENVIELETAUSENDMENSCHENLEBEN"
C_danish="DETVARENGANGENLILLEHAVFRUESOMBOEDELANGTUDEPAAHAVETSBUNDSAMMENMEDSINFADEROGSINEFEMSOESTREHUNVARDENYNGSTEOGSMUKKESTEAFDEMALLEMENHUNLAENGTESEFTERATKOMMEOPTILMENNESKENESVERDENOGSEDENSTORESKIBEOGBYERNEOGSKOVENEHVERTAARBLEVHUNAELDREOGFIKLOVTILATSTIGEOPGENNEMDETKLAREVANDFORATSIDDEPAAKLIPPERNEISKINNETFRAMAANENOGSEUDOVERDENSTOREVIDEVERDENOGNAARSOLENGIKNEDDYKKEDEHUNNEDIGENMENHUNGLEMTEALDRIGDENDEJLIGEVERDENOVENOVERVANDETOGENDAGDAHUNREDDEDEENUNGPRINSFRADRUKNINGFORELSKEDEHUNSIGHAABLOEST"
C_french="LESSANGLOTSLONGSDESVIOLONSDELAUTOMNEBLESSENTMONCOEURDUNELANGUEURMONOTONETOUTSUFFOCANTETBLEMEQUANDSONNELHEUREJEMESOUVIENSDESJOURSANCIENSETALORSJEPLEUREETJEMENVAISAUVENTMAUVAISQUIMEMPORTEDECADELABCOMMELAFEUILLEMORTEPENDANTLONGTEMPSJEMESUISCOUCHEDEBONNEHEUREETJAIREVEDESPAYSLOINTAINSOULESHOMMESSONTLIBRESETOULAVIEESTDOUCEETBELLECHAQUEMATINJEMEPROMENAISLELONGDELARIVIEREENECOUTANTLECHANTDESOISEAUXETLEMURMUREDELEAUQUICOULAITDOUCEMENTVERSLAMER"

case "$CLANG" in
  english) corpus=$C_english ;;
  german)  corpus=$C_german  ;;
  danish)  corpus=$C_danish  ;;
  french)  corpus=$C_french  ;;
  *) echo "error: no corpus for CLANG='$CLANG' (english/german/danish/french)" >&2; exit 1 ;;
esac
clen=${#corpus}

# --- BASE binary (optional A/B), built in a throwaway worktree like bench.sh --
BASE_BIN=""
base_dir=""
if [ -n "${BASE:-}" ]; then
  base_dir=$(mktemp -d) || { echo "error: mktemp failed" >&2; exit 1; }
  # shellcheck disable=SC2064
  trap "git worktree remove --force '$base_dir' >/dev/null 2>&1" EXIT INT TERM
  if ! git worktree add --detach "$base_dir" "$BASE" >/dev/null 2>&1; then
    echo "error: could not create worktree for BASE='$BASE'" >&2; exit 1
  fi
  if ! make -C "$base_dir" CXX="${CXX:-g++}" >/dev/null 2>&1; then
    echo "error: could not build BASE='$BASE'" >&2; exit 1
  fi
  BASE_BIN="$base_dir/enigma"
fi

truth=$(mktemp) || { echo "error: mktemp failed" >&2; exit 1; }
trials_f=$(mktemp) || { echo "error: mktemp failed" >&2; exit 1; }
# shellcheck disable=SC2064
trap "rm -f '$truth' '$trials_f'; [ -n '$BASE_BIN' ] && git worktree remove --force '$base_dir' >/dev/null 2>&1" EXIT INT TERM

# gen_trials L SEEDBASE -> TRIALS lines "offset reflector wheels ring grund pair..."
gen_trials() {
  awk -v trials="$TRIALS" -v L="$1" -v seed="$2" -v pairs="$PAIRS" -v clen="$clen" 'BEGIN {
    srand(seed);
    split("A B C", refl, " ");
    for (t = 0; t < trials; t++)
      {
        off = int(rand() * (clen - L + 1));
        u = refl[int(rand() * 3) + 1];
        for (i = 1; i <= 8; i++) w[i] = i;
        for (i = 8; i > 1; i--) { j = int(rand() * i) + 1; tmp = w[i]; w[i] = w[j]; w[j] = tmp; }
        ww = w[1] "" w[2] "" w[3];
        rr = ""; gg = "";
        for (i = 0; i < 3; i++) rr = rr sprintf("%c", 65 + int(rand() * 26));
        for (i = 0; i < 3; i++) gg = gg sprintf("%c", 65 + int(rand() * 26));
        for (i = 1; i <= 26; i++) a[i] = i - 1;
        for (i = 26; i > 1; i--) { j = int(rand() * i) + 1; tmp = a[i]; a[i] = a[j]; a[j] = tmp; }
        pb = "";
        for (i = 0; i < pairs; i++)
          pb = pb " " sprintf("%c%c", 65 + a[2*i+1], 65 + a[2*i+2]);
        print off, u, ww, rr, gg pb;
      }
  }'
}

# crack_pct BIN U WHEELS RING GRUND CT TRUTHFILE -> "%-correct" (recovered vs truth)
crack_pct() {
  printf '%s' "$6" | "$1" "-$MODEL" -l "$CLANG" -u "$2" -w "$3" -r "$4" -g "$5" -c -p "$7" 2>&1 >/dev/null \
    | awk -F'[()%]' '/identical/ { print $2; exit }'
}

# crack_run BIN U WHEELS RING GRUND CT TRUTHFILE -> "<pct> <climb-score>"
# (one climb run; harvest both the -p %-correct and the final best score line)
crack_run() {
  _e=$(printf '%s' "$6" | "$1" "-$MODEL" -l "$CLANG" -u "$2" -w "$3" -r "$4" -g "$5" -c -p "$7" 2>&1 >/dev/null)
  _p=$(printf '%s\n' "$_e" | awk -F'[()%]' '/identical/ { print $2; exit }')
  _s=$(printf '%s\n' "$_e" | awk '/W: / { s = $1 } END { print s }')
  printf '%s %s' "${_p:-0}" "${_s:-0}"
}

# oracle_score BIN U WHEELS RING GRUND CT PLUGBOARD -> score of the TRUE plugboard
# (fixed config, no -c: scores the one decrypt the true settings produce)
oracle_score() {
  printf '%s' "$6" | "$1" "-$MODEL" -l "$CLANG" -u "$2" -w "$3" -r "$4" -g "$5" -s "$7" 2>&1 >/dev/null \
    | awk '/W: / { s = $1 } END { print (s == "" ? 0 : s) }'
}

# SPLIT and BASE are mutually exclusive (SPLIT diagnoses the working tree).
if [ "$SPLIT" = 1 ] && [ -n "$BASE_BIN" ]; then
  echo "note: SPLIT ignored because BASE (A/B) was requested" >&2
  SPLIT=0
fi

if [ -n "$BASE_BIN" ]; then
  echo "crack quality A/B: BASE=$BASE vs working tree"
else
  echo "crack quality (working-tree binary)"
fi
printf 'model=-%s  lang=%s  trials=%s  pairs=%s  seed=%s  corpus=%s chars\n\n' \
  "$MODEL" "$CLANG" "$TRIALS" "$PAIRS" "$SEED" "$clen"

if [ -n "$BASE_BIN" ]; then
  printf '%4s  %18s  %18s\n' "len" "head mean% exact%" "base mean% exact%"
elif [ "$SPLIT" = 1 ]; then
  printf '%4s  %8s  %8s  %12s  %12s\n' "len" "mean%" "exact%" "search-fail%" "scoring-fail%"
else
  printf '%4s  %8s  %8s\n' "len" "mean%" "exact%"
fi

# accumulate "L headexact" pairs for the L50/L90 headline (head binary)
head_curve=""

for L in $LENGTHS; do
  if [ "$L" -gt "$clen" ]; then
    echo "  skip len=$L (longer than corpus $clen)" >&2
    continue
  fi
  gen_trials "$L" "$((SEED * 1000003 + L))" > "$trials_f"

  hp=""; bp=""; cls=""
  while IFS= read -r line; do
    # shellcheck disable=SC2086
    set -- $line
    off=$1; u=$2; ww=$3; rr=$4; gg=$5; shift 5; pb="$*"
    excerpt=$(printf '%s' "$corpus" | cut -c "$((off + 1))-$((off + L))")
    printf '%s' "$excerpt" > "$truth"
    ct=$(printf '%s' "$excerpt" | "$HEAD_BIN" -i -u "$u" -w "$ww" -r "$rr" -g "$gg" -s "$pb" 2>/dev/null)

    if [ "$SPLIT" = 1 ]; then
      run=$(crack_run "$HEAD_BIN" "$u" "$ww" "$rr" "$gg" "$ct" "$truth")
      p=${run% *}; csc=${run#* }
      hp="$hp ${p:-0}"
      if awk -v p="${p:-0}" 'BEGIN { exit !(p >= 99.95) }'; then
        cls="$cls exact"
      else
        osc=$(oracle_score "$HEAD_BIN" "$u" "$ww" "$rr" "$gg" "$ct" "$pb")
        cls="$cls $(awk -v o="${osc:-0}" -v c="${csc:-0}" 'BEGIN { print (o > c + 0.01) ? "search" : "scoring" }')"
      fi
    else
      p=$(crack_pct "$HEAD_BIN" "$u" "$ww" "$rr" "$gg" "$ct" "$truth")
      hp="$hp ${p:-0}"
      if [ -n "$BASE_BIN" ]; then
        q=$(crack_pct "$BASE_BIN" "$u" "$ww" "$rr" "$gg" "$ct" "$truth")
        bp="$bp ${q:-0}"
      fi
    fi
  done < "$trials_f"

  # shellcheck disable=SC2086
  hstat=$(printf '%s\n' $hp | awk '{ s += $1; if ($1 >= 99.95) e++ } END { printf "%.1f %.1f", s/NR, 100.0*e/NR }')
  hmean=${hstat% *}; hexact=${hstat#* }
  head_curve="$head_curve $L:$hexact"

  if [ -n "$BASE_BIN" ]; then
    # shellcheck disable=SC2086
    bstat=$(printf '%s\n' $bp | awk '{ s += $1; if ($1 >= 99.95) e++ } END { printf "%.1f %.1f", s/NR, 100.0*e/NR }')
    bmean=${bstat% *}; bexact=${bstat#* }
    printf '%4s  %8s %8s   %8s %8s\n' "$L" "$hmean" "$hexact" "$bmean" "$bexact"
  elif [ "$SPLIT" = 1 ]; then
    # shellcheck disable=SC2086
    fstat=$(printf '%s\n' $cls | awk '{ n++; c[$1]++ } END { printf "%.1f %.1f", 100.0*c["search"]/n, 100.0*c["scoring"]/n }')
    fsearch=${fstat% *}; fscoring=${fstat#* }
    printf '%4s  %8s  %8s  %12s  %12s\n' "$L" "$hmean" "$hexact" "$fsearch" "$fscoring"
  else
    printf '%4s  %8s  %8s\n' "$L" "$hmean" "$hexact"
  fi
done

# L50 / L90: shortest length whose head exact-recovery rate reaches the threshold
lcross() {
  # shellcheck disable=SC2086
  printf '%s\n' $head_curve | awk -F: -v thr="$1" '$2 >= thr { print $1; exit }'
}
echo
l50=$(lcross 50); l90=$(lcross 90)
printf 'headline (head): L50=%s  L90=%s  (shortest length reaching that exact-recovery rate; lower is better)\n' \
  "${l50:-none}" "${l90:-none}"

exit 0
