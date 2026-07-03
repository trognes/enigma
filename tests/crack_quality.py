#!/usr/bin/env python3
#
# Cracking-quality benchmark for the enigma tool -- how well it recovers SHORT
# messages, which is the hard regime. Deliberately separate from:
#
#   * tests/run_tests.sh  -- correctness (pass/fail known-answer + round-trip),
#   * tests/bench.sh      -- speed (keys/s, climbs/s).
#
# Run with `make crackquality`, or directly with `python3 tests/crack_quality.py`
# from anywhere (it cd's to the repo root so it can find the n-gram files).
#
# (Rewritten from a shell+awk version: that relied on awk's srand()/rand(), whose
# seeded sequence is not reproducible in every awk -- mawk re-seeds from entropy --
# so trials were not deterministic across runs. Python's random.Random(seed) is
# deterministic and cross-platform, so a SEED-matched A/B compares identical
# problems on any machine.)
#
# --- What it does (the cheap "plugboard-recovery" tier) ----------------------
#
# For each length L it runs TRIALS independent random problems:
#   * a random excerpt of length L from a per-language passage,
#   * a random rotor key (reflector / 3 wheels / ring / start),
#   * a random PAIRS-pair plugboard (default 10, the historically hard case).
# Each problem is encrypted, then handed back with the TRUE rotor key fixed and
# only the plugboard hill-climbed (`-c`). This isolates plugboard recovery from
# rotor-key discrimination -- the cheap tier for the dev loop.
#
# --- The metric --------------------------------------------------------------
#
# %-correct is computed directly: the recovered plaintext (the tool's stdout)
# compared letter-by-letter to the excerpt. Per length we report the mean
# %-correct (a graded signal, smooth near the difficulty cliff) and the
# exact-recovery rate (fraction recovered 100% correct), plus headline L50/L90
# (shortest length reaching that recovery rate; lower is better). %-correct of the
# recovered plaintext (not key equality) is the right target: on short messages
# some settings are unidentifiable (e.g. the ring of a rotor that never steps).
#
# --- Reproducibility and A/B -------------------------------------------------
#
#   make crackquality                  working-tree binary
#   make crackquality BASE=<git-ref>   same-machine A/B vs <git-ref>
#
# A fixed SEED makes the trial set deterministic, so an A/B solves the IDENTICAL
# problems with both binaries (same idea as bench.sh's A/B, but for recovery rate).
#
# --- Failure-mode split (SPLIT=1) --------------------------------------------
#
# A non-recovered short message fails for one of two reasons needing OPPOSITE
# fixes; SPLIT=1 labels each non-exact trial via an oracle (score the decrypt
# under the KNOWN true plugboard, no -c, and compare to the climb's score):
#   * SCORING failure -- true plugboard does not score highest; only better
#     scoring helps.
#   * SEARCH failure  -- true plugboard scores higher than the climb reached
#     (stuck in a local optimum); only better search helps.
# true > found => search failure, else scoring failure. Adds one run per non-exact
# trial; ignored when BASE (A/B) is requested.
#
# Tunables (environment): MODEL (i/m/b/t/q, default q), CLANG (crack language,
# default english), TRIALS, LENGTHS, PAIRS, SEED, BASE, SPLIT, CRACKOPTS (extra
# options appended to the climb invocation, e.g. "-R 10").

import os
import random
import shlex
import string
import subprocess
import sys
import tempfile

# The binary's restart RNG now defaults to a fresh random seed each run, which would
# make this A/B non-deterministic. Pin it so BASE and working-tree solve every trial
# with the same restart sequence. Seed 0 also matches pre-seed BASE refs (which ignore
# ENIGMA_SEED and use the historical key-index seed), keeping the A/B comparable across
# the change. Overridable from the environment.
os.environ.setdefault("ENIGMA_SEED", "0")

CORPORA = {
    "english": "THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUREOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDINTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERALYEARSANDSAVEDCOUNTLESSLIVES",
    "german": "DIEENIGMAMASCHINEWURDEIMZWEITENWELTKRIEGVONDERDEUTSCHENWEHRMACHTVERWENDETUMGEHEIMENACHRICHTENZUVERSCHLUESSELNABERDIEALLIIERTENKONNTENDENGEHEIMENCODETROTZDEMBRECHENWEILDIEDEUTSCHENOFTDIEGLEICHENFLOSKELNVERWENDETENUNDWEILVIELEBEDIENERIMMERWIEDERDIESELBENFEHLERMACHTENDIEPOLNISCHENUNDBRITISCHENMATHEMATIKERBAUTENMASCHINENUMDIETAEGLICHENSCHLUESSELZUFINDENUNDLASENSODIEGEHEIMENFUNKSPRUECHEDESFEINDESMITUNDVERKUERZTENDADURCHDENKRIEGUMMEHREREJAHREUNDRETTETENVIELETAUSENDMENSCHENLEBEN",
    "danish": "DETVARENGANGENLILLEHAVFRUESOMBOEDELANGTUDEPAAHAVETSBUNDSAMMENMEDSINFADEROGSINEFEMSOESTREHUNVARDENYNGSTEOGSMUKKESTEAFDEMALLEMENHUNLAENGTESEFTERATKOMMEOPTILMENNESKENESVERDENOGSEDENSTORESKIBEOGBYERNEOGSKOVENEHVERTAARBLEVHUNAELDREOGFIKLOVTILATSTIGEOPGENNEMDETKLAREVANDFORATSIDDEPAAKLIPPERNEISKINNETFRAMAANENOGSEUDOVERDENSTOREVIDEVERDENOGNAARSOLENGIKNEDDYKKEDEHUNNEDIGENMENHUNGLEMTEALDRIGDENDEJLIGEVERDENOVENOVERVANDETOGENDAGDAHUNREDDEDEENUNGPRINSFRADRUKNINGFORELSKEDEHUNSIGHAABLOEST",
    "french": "LESSANGLOTSLONGSDESVIOLONSDELAUTOMNEBLESSENTMONCOEURDUNELANGUEURMONOTONETOUTSUFFOCANTETBLEMEQUANDSONNELHEUREJEMESOUVIENSDESJOURSANCIENSETALORSJEPLEUREETJEMENVAISAUVENTMAUVAISQUIMEMPORTEDECADELABCOMMELAFEUILLEMORTEPENDANTLONGTEMPSJEMESUISCOUCHEDEBONNEHEUREETJAIREVEDESPAYSLOINTAINSOULESHOMMESSONTLIBRESETOULAVIEESTDOUCEETBELLECHAQUEMATINJEMEPROMENAISLELONGDELARIVIEREENECOUTANTLECHANTDESOISEAUXETLEMURMUREDELEAUQUICOULAITDOUCEMENTVERSLAMER",
}


def env(name, default):
    v = os.environ.get(name)
    return v if v not in (None, "") else default


MODEL = env("MODEL", "q")
CLANG = env("CLANG", "english")
TRIALS = int(env("TRIALS", "40"))
LENGTHS = [int(x) for x in env("LENGTHS", "40 70 100 140 190 250 320").split()]
PAIRS = int(env("PAIRS", "10"))
SEED = int(env("SEED", "1"))
SPLIT = env("SPLIT", "0") == "1"
BASE = env("BASE", "")
CRACKOPTS = shlex.split(env("CRACKOPTS", ""))


# Per-binary n-gram data directory: each binary reads its tables from its own
# tree (the working-tree binary from ./ngrams, a BASE binary from its worktree),
# so an A/B spanning the data-dir move finds the tables for both refs.
DATADIRS = {}


def run(binary, args, text):
    """Feed `text` on stdin, return (stdout, stderr) as strings."""
    env = os.environ.copy()
    datadir = DATADIRS.get(binary)
    if datadir:
        env["ENIGMA_DATA"] = datadir
    p = subprocess.run([binary] + args, input=text,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True, env=env)
    return p.stdout, p.stderr


def last_score(stderr):
    """The final best score the search printed (first field of the last 'W:' line,
    e.g. '  392.9191 W: B137 R: ...')."""
    score = None
    for line in stderr.splitlines():
        if "W:" in line:
            try:
                score = float(line.split()[0])
            except (ValueError, IndexError):
                pass
    return score


def encrypt(binary, key, plain):
    u, w, r, g, pb = key
    out, _ = run(binary, ["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", pb], plain)
    return out.strip()


def climb(binary, key, ct):
    """Run the plugboard hill-climb; return (recovered_plaintext, best_score)."""
    u, w, r, g, _ = key
    args = ["-" + MODEL, "-l", CLANG, "-u", u, "-w", w, "-r", r, "-g", g, "-c"]
    args += CRACKOPTS
    out, err = run(binary, args, ct)
    return out.strip(), last_score(err)


def oracle_score(binary, key, ct):
    """Score the decrypt under the TRUE plugboard (fixed config, no -c)."""
    u, w, r, g, pb = key
    _, err = run(binary, ["-" + MODEL, "-l", CLANG, "-u", u, "-w", w,
                          "-r", r, "-g", g, "-s", pb], ct)
    return last_score(err)


def pct_correct(recovered, truth):
    if not truth:
        return 0.0
    same = sum(1 for a, b in zip(recovered, truth) if a == b)
    return 100.0 * same / len(truth)


def gen_trials(length, corpus):
    """Deterministic trial set for a length: (excerpt, key) tuples."""
    rng = random.Random(SEED * 1000003 + length)
    trials = []
    for _ in range(TRIALS):
        off = rng.randrange(len(corpus) - length + 1)
        excerpt = corpus[off:off + length]
        u = rng.choice("ABC")
        w = "".join(str(d) for d in rng.sample(range(1, 9), 3))
        r = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        g = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        letters = rng.sample(string.ascii_uppercase, 2 * PAIRS)
        pb = " ".join(letters[2 * i] + letters[2 * i + 1] for i in range(PAIRS))
        trials.append((excerpt, (u, w, r, g, pb)))
    return trials


def build_base(ref):
    """Build the BASE binary in a throwaway git worktree; return its path (and the
    worktree dir to clean up)."""
    wt = tempfile.mkdtemp(prefix="enigma-base-")
    cxx = os.environ.get("CXX", "g++")
    try:
        subprocess.run(["git", "worktree", "add", "--detach", wt, ref],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["make", "-C", wt, "CXX=" + cxx],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        sys.exit("error: could not build BASE=%s" % ref)
    return os.path.join(wt, "enigma"), wt


def stats(pcts):
    n = len(pcts)
    mean = sum(pcts) / n
    exact = 100.0 * sum(1 for p in pcts if p >= 99.95) / n
    return mean, exact


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    head = "./enigma"
    if not os.access(head, os.X_OK):
        sys.exit("error: %s not built; run 'make' first" % head)
    DATADIRS[head] = os.path.abspath("ngrams")

    corpus = CORPORA.get(CLANG)
    if corpus is None:
        sys.exit("error: no corpus for CLANG=%s (english/german/danish/french)" % CLANG)

    base, base_wt = (None, None)
    split = SPLIT
    if BASE:
        if SPLIT:
            print("note: SPLIT ignored because BASE (A/B) was requested", file=sys.stderr)
            split = False
        base, base_wt = build_base(BASE)
        wt_ngrams = os.path.join(base_wt, "ngrams")
        DATADIRS[base] = wt_ngrams if os.path.isdir(wt_ngrams) else base_wt

    try:
        if base:
            print("crack quality A/B: BASE=%s vs working tree" % BASE)
        else:
            print("crack quality (working-tree binary)")
        extra = ("  crackopts=%s" % " ".join(CRACKOPTS)) if CRACKOPTS else ""
        print("model=-%s  lang=%s  trials=%d  pairs=%d  seed=%d  corpus=%d chars%s\n"
              % (MODEL, CLANG, TRIALS, PAIRS, SEED, len(corpus), extra))

        if base:
            print("%4s  %18s  %18s" % ("len", "head mean% exact%", "base mean% exact%"))
        elif split:
            print("%4s  %8s  %8s  %12s  %12s"
                  % ("len", "mean%", "exact%", "search-fail%", "scoring-fail%"))
        else:
            print("%4s  %8s  %8s" % ("len", "mean%", "exact%"))

        head_curve = []
        for L in LENGTHS:
            if L > len(corpus):
                print("  skip len=%d (longer than corpus %d)" % (L, len(corpus)), file=sys.stderr)
                continue
            hp, bp, classes = [], [], []
            for excerpt, key in gen_trials(L, corpus):
                ct = encrypt(head, key, excerpt)
                rec, hscore = climb(head, key, ct)
                p = pct_correct(rec, excerpt)
                hp.append(p)
                if base:
                    brec, _ = climb(base, key, ct)
                    bp.append(pct_correct(brec, excerpt))
                elif split:
                    if p >= 99.95:
                        classes.append("exact")
                    else:
                        osc = oracle_score(head, key, ct)
                        h = hscore if hscore is not None else 0.0
                        o = osc if osc is not None else 0.0
                        classes.append("search" if o > h + 0.01 else "scoring")

            hmean, hexact = stats(hp)
            head_curve.append((L, hexact))
            if base:
                bmean, bexact = stats(bp)
                print("%4d  %8.1f %8.1f   %8.1f %8.1f" % (L, hmean, hexact, bmean, bexact))
            elif split:
                n = len(classes)
                sf = 100.0 * classes.count("search") / n
                cf = 100.0 * classes.count("scoring") / n
                print("%4d  %8.1f  %8.1f  %12.1f  %12.1f" % (L, hmean, hexact, sf, cf))
            else:
                print("%4d  %8.1f  %8.1f" % (L, hmean, hexact))

        def lcross(thr):
            for L, exact in head_curve:   # lengths ascending -> shortest reaching thr
                if exact >= thr:
                    return L
            return None

        l50, l90 = lcross(50), lcross(90)
        print("\nheadline (head): L50=%s  L90=%s  (shortest length reaching that "
              "exact-recovery rate; lower is better)"
              % (l50 if l50 else "none", l90 if l90 else "none"))
    finally:
        if base_wt:
            subprocess.run(["git", "worktree", "remove", "--force", base_wt],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
