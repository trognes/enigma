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
#
# --- Full-crack / scoring-gate tier (WILDCARD, see archived/CRACKQUALITY_TESTS.md §1) --
#
# By default the climb is handed the TRUE rotor key and only the plugboard is
# recovered. WILDCARD wildcards named rotor-key dimensions instead, so the search
# must find the key AND the plugboard -- the setup for the one-time scoring-failure
# gate (does a wrong (key, board) ever out-score the true one?).
#   WILDCARD  subset of "uwrg" to wildcard (reflector/wheels/ring/start); ""
#             (default) keeps the fixed-key tier byte-identical.
#   XMAX      -x value + wheel-sampling cap when wheels are wildcarded (default 3).
#   FILTER    -F key pre-filter budget; "" or "0" => no filter (the gate runs
#             UNFILTERED so -F filter-recall cannot confound the search/scoring split).
#   RESTARTS  -R restart budget.
#   FULLCRACK "1" is sugar: WILDCARD->"wg", and FILTER/RESTARTS default to 200/8
#             only when left unset.
# When WILDCARD is set, trials are generated with true ring AAA (so fixed-ring
# recovery is identifiable), and a key% column reports rotor-key recovery on the
# identifiable columns (reflector+wheels and start). See archived/CRACKQUALITY_TESTS.md §1.
#
# --- -F prefilter validation (§2) and restart diversity (§3) -----------------
#
# Three more report modes, each off by default (the normal flow is unchanged):
#   FILTERRECALL=1  §2 Test A: for a wildcarded keyspace, run the -F search with
#                   --true-key and report the true key's tier-1 recall@{50,100,200,
#                   500} and median rank -- how often the cheap IC pre-filter keeps
#                   the true key. Tier-1 only (-R 0), cheap; needs WILDCARD + -F.
#   SCOREITER=1     §2 Test B: add a score_iter column (mean plugboards scored) to
#                   the normal run, so a filtered vs unfiltered A/B can be balanced
#                   at matched compute.
#   DIVERSITY=1     §3: a fixed-key climb with --dump-all; report per length the
#                   mean number of DISTINCT converged optima across the -R restarts,
#                   the mean count of restarts reaching the best board (best-hits),
#                   and mean %-correct -- a direct basin-collapse signal. Set RESTARTS.

import os
import random
import re
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

# Full-crack / scoring-gate knobs (see the header note and archived/CRACKQUALITY_TESTS.md §1).
WILDCARD = env("WILDCARD", "")
XMAX = env("XMAX", "3")
FILTER = env("FILTER", "")
RESTARTS = env("RESTARTS", "")
if env("FULLCRACK", "0") == "1":
    if not WILDCARD:
        WILDCARD = "wg"
    if not FILTER:
        FILTER = "200"
    if not RESTARTS:
        RESTARTS = "8"
WILD = bool(WILDCARD)

# §2 -F prefilter validation and §3 restart-diversity diagnostics (own report modes;
# each off by default so the normal flow is unchanged). See archived/CRACKQUALITY_TESTS.md §2/§3.
FILTERRECALL = env("FILTERRECALL", "0") == "1"   # §2 Test A: true-key tier-1 recall@N
SCOREITER = env("SCOREITER", "0") == "1"         # §2 Test B: add a score_iter column
DIVERSITY = env("DIVERSITY", "0") == "1"         # §3: restart basin-collapse diagnostics


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
    """The final best score the search printed: the first field of the last
    progress line, e.g. '-4.3598 B241 AAA QEW AB CD ... THEQUICKAN' (older
    binaries, still met as A/B BASE refs, print '-4.3598 W: B241 R: ...').
    Both start with the score, and no other stderr line's first token carries
    a decimal point (the settings echo's bare counts have no '.')."""
    score = None
    for line in stderr.splitlines():
        fields = line.split()
        if fields and "." in fields[0]:
            try:
                score = float(fields[0])
            except ValueError:
                pass
    return score


def last_key(stderr):
    """(W, R, G) = the reflector+wheels / ring / start columns of the last progress
    line (the recovered rotor key). None on the old '-4.36 W: B241 R: ...' format
    (fields[1] == 'W:'), so key% is head-only in an A/B against such a ref."""
    key = None
    for line in stderr.splitlines():
        fields = line.split()
        if len(fields) >= 4 and "." in fields[0] and fields[1] != "W:":
            try:
                float(fields[0])
            except ValueError:
                continue
            key = (fields[1], fields[2], fields[3])
    return key


def key_ok(recovered, true_key):
    """True if the recovered rotor key matches the true key on the IDENTIFIABLE
    columns. With ring pinned AAA (how WILDCARD trials are generated) the start is
    identifiable, so compare the W column (reflector letter + wheel digits, e.g.
    'B241') and the G column (start); ring is not compared."""
    if recovered is None:
        return False
    u, w, _r, g, _pb = true_key
    rw, _rr, rg = recovered
    return rw == (u + w) and rg == g


def score_iter(stderr):
    """Total plugboards scored, from the 'scored M plugboards' diagnostic."""
    n = None
    for line in stderr.splitlines():
        m = re.search(r"scored (\d+) plugboards", line)
        if m:
            n = int(m.group(1))
    return n


def last_filter_rank(stderr):
    """(rank, total) from the 'true-key tier1 rank R of N' line (--true-key), or
    None if the true key was not in the searched keyspace / the line is absent."""
    out = None
    for line in stderr.splitlines():
        m = re.search(r"true-key tier1 rank (\d+) of (\d+)", line)
        if m:
            out = (int(m.group(1)), int(m.group(2)))
    return out


def parse_restarts(stderr):
    """List of (score, board) from the 'dumpall <key> <ring> <start> <score> <board>'
    dump lines (--dump-all). Board is the canonical plug-pair string ('' = empty).
    This is a fixed-key diagnostic, so the key columns are skipped."""
    out = []
    for line in stderr.splitlines():
        if line.startswith("dumpall "):
            parts = line.split(None, 5)
            try:
                sc = float(parts[4])
            except (ValueError, IndexError):
                continue
            out.append((sc, parts[5] if len(parts) > 5 else ""))
    return out


def encrypt(binary, key, plain):
    u, w, r, g, pb = key
    out, _ = run(binary, ["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", pb], plain)
    return out.strip()


def wild_key(key):
    """Substitute '.' wildcards for the WILDCARD-named rotor-key dimensions;
    returns (u, w, r, g). With WILDCARD unset the true key is returned unchanged."""
    u, w, r, g, _ = key
    if "u" in WILDCARD:
        u = "."
    if "w" in WILDCARD:
        w = "..."
    if "r" in WILDCARD:
        r = "..."
    if "g" in WILDCARD:
        g = "..."
    return u, w, r, g


def climb(binary, key, ct):
    """Run the search + plugboard hill-climb; return
    (recovered_plaintext, best_score, recovered_key, score_iter). WILDCARD replaces
    the named rotor-key dimensions with '.' wildcards so the key is searched, not
    fixed; FILTER/RESTARTS wire -F/-R. With WILDCARD unset the enigma invocation is
    byte-identical to the fixed-key climb (no extra args)."""
    u, w, r, g = wild_key(key)
    args = ["-" + MODEL, "-l", CLANG, "-u", u, "-w", w, "-r", r, "-g", g, "-c"]
    if "w" in WILDCARD:
        args += ["-x", XMAX]
    if FILTER and FILTER != "0":
        args += ["-F", FILTER]
    if RESTARTS:
        args += ["-R", RESTARTS]
    args += CRACKOPTS
    out, err = run(binary, args, ct)
    return out.strip(), last_score(err), last_key(err), score_iter(err)


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
        # cap the wheel range to XMAX when wheels are wildcarded, so the true
        # order lies inside the searched space (-x XMAX); else the usual 1..8.
        wheel_hi = int(XMAX) + 1 if "w" in WILDCARD else 9
        w = "".join(str(d) for d in rng.sample(range(1, wheel_hi), 3))
        r = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        g = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
        # With a wildcarded key but ring not itself wildcarded, pin the true ring
        # to AAA so fixed-ring recovery is identifiable (archived/CRACKQUALITY_TESTS.md §1);
        # r was still drawn above, so the RNG stream is unchanged.
        if WILD and "r" not in WILDCARD:
            r = "AAA"
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


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    return float(s[n // 2]) if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


# --- §2 Test A: -F tier-1 recall (--true-key) --------------------------------
def filter_rank(binary, key, ct):
    """Run the -F search with --true-key; return (rank, total) or None (true key
    not in the searched keyspace). Rank is over ALL keys, so it is independent of
    the -F budget -- any filter that triggers tier-1 will do (default 200)."""
    u, w, r, g = wild_key(key)
    truekey = key[0] + key[1] + key[2] + key[3]   # the TRUE u+w+r+g (10 chars)
    filt = FILTER if (FILTER and FILTER != "0") else "200"
    args = ["-" + MODEL, "-l", CLANG, "-u", u, "-w", w, "-r", r, "-g", g, "-c",
            "-F", filt, "-R", "0", "--true-key", truekey]
    if "w" in WILDCARD:
        args += ["-x", XMAX]
    args += CRACKOPTS
    _, err = run(binary, args, ct)
    return last_filter_rank(err)


def run_filter_recall(head, corpus):
    ns = [50, 100, 200, 500]
    print("filter recall (-F tier-1, --true-key) -- working-tree binary")
    print("model=-%s  lang=%s  trials=%d  pairs=%d  seed=%d  wildcard=%s\n"
          % (MODEL, CLANG, TRIALS, PAIRS, SEED, WILDCARD or "(none)"))
    print("%4s %s %10s %8s"
          % ("len", " ".join("%8s" % ("rec@%d" % n) for n in ns), "median-rk", "not-in%"))
    for L in LENGTHS:
        if L > len(corpus):
            print("  skip len=%d (longer than corpus %d)" % (L, len(corpus)), file=sys.stderr)
            continue
        ranks, notin = [], 0
        for excerpt, key in gen_trials(L, corpus):
            rt = filter_rank(head, key, encrypt(head, key, excerpt))
            if rt is None:
                notin += 1
            else:
                ranks.append(rt[0])
        n = len(ranks) + notin
        recs = " ".join("%8.1f" % (100.0 * sum(1 for r in ranks if r <= nn) / n) for nn in ns)
        print("%4d %s %10.1f %8.1f" % (L, recs, median(ranks), 100.0 * notin / n))


# --- §3: restart-diversity diagnostics (--dump-all) ---------------------
def diversity_run(binary, key, ct):
    """Fixed-key climb with --dump-all; return (plaintext, [(score, board)])."""
    u, w, r, g, _ = key   # always the fixed true key (diversity is a fixed-key test)
    args = ["-" + MODEL, "-l", CLANG, "-u", u, "-w", w, "-r", r, "-g", g,
            "-c", "--dump-all"]
    if RESTARTS:
        args += ["-R", RESTARTS]
    args += CRACKOPTS
    out, err = run(binary, args, ct)
    return out.strip(), parse_restarts(err)


def run_diversity(head, corpus):
    print("restart diversity (fixed key, --dump-all) -- working-tree binary")
    print("model=-%s  lang=%s  trials=%d  pairs=%d  seed=%d  restarts=%s\n"
          % (MODEL, CLANG, TRIALS, PAIRS, SEED, RESTARTS or "0"))
    print("%4s %9s %10s %8s" % ("len", "distinct", "best-hits", "mean%"))
    for L in LENGTHS:
        if L > len(corpus):
            print("  skip len=%d (longer than corpus %d)" % (L, len(corpus)), file=sys.stderr)
            continue
        dist, hits, pct = [], [], []
        for excerpt, key in gen_trials(L, corpus):
            rec, rs = diversity_run(head, key, encrypt(head, key, excerpt))
            pct.append(pct_correct(rec, excerpt))
            if rs:
                dist.append(len(set(b for _, b in rs)))
                best = max(rs, key=lambda sb: sb[0])
                hits.append(sum(1 for sb in rs if sb[1] == best[1]))
            else:
                dist.append(0)
                hits.append(0)
        n = len(pct)
        print("%4d %9.1f %10.1f %8.1f"
              % (L, sum(dist) / n, sum(hits) / n, sum(pct) / n))


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

    # §2 recall / §3 diversity are self-contained report modes (no BASE / SPLIT).
    if FILTERRECALL:
        run_filter_recall(head, corpus)
        return
    if DIVERSITY:
        run_diversity(head, corpus)
        return

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
        if WILD:
            extra += "  wildcard=%s" % WILDCARD
            if "w" in WILDCARD:
                extra += " xmax=%s" % XMAX
            extra += " -F%s" % (FILTER if (FILTER and FILTER != "0") else "off")
            if RESTARTS:
                extra += " -R%s" % RESTARTS
        print("model=-%s  lang=%s  trials=%d  pairs=%d  seed=%d  corpus=%d chars%s\n"
              % (MODEL, CLANG, TRIALS, PAIRS, SEED, len(corpus), extra))

        keyhdr = "  %6s" % "key%" if WILD else ""
        sihdr = "  %10s" % "score_iter" if SCOREITER else ""
        if base:
            print("%4s  %18s%s  %18s" % ("len", "head mean% exact%", keyhdr, "base mean% exact%"))
        elif split:
            print("%4s  %8s  %8s%s%s  %12s  %12s"
                  % ("len", "mean%", "exact%", keyhdr, sihdr, "search-fail%", "scoring-fail%"))
        else:
            print("%4s  %8s  %8s%s%s" % ("len", "mean%", "exact%", keyhdr, sihdr))

        head_curve = []
        for L in LENGTHS:
            if L > len(corpus):
                print("  skip len=%d (longer than corpus %d)" % (L, len(corpus)), file=sys.stderr)
                continue
            hp, bp, kp, si, classes = [], [], [], [], []
            for excerpt, key in gen_trials(L, corpus):
                ct = encrypt(head, key, excerpt)
                rec, hscore, hkey, hsi = climb(head, key, ct)
                p = pct_correct(rec, excerpt)
                hp.append(p)
                if WILD:
                    kp.append(key_ok(hkey, key))
                if SCOREITER and hsi is not None:
                    si.append(hsi)
                if base:
                    brec, _, _, _ = climb(base, key, ct)
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
            keycol = ("  %6.1f" % (100.0 * sum(kp) / len(kp))) if WILD else ""
            sicol = ("  %10d" % round(sum(si) / len(si))) if (SCOREITER and si) else \
                    ("  %10s" % "-" if SCOREITER else "")
            if base:
                bmean, bexact = stats(bp)
                print("%4d  %8.1f %8.1f%s   %8.1f %8.1f" % (L, hmean, hexact, keycol, bmean, bexact))
            elif split:
                n = len(classes)
                sf = 100.0 * classes.count("search") / n
                cf = 100.0 * classes.count("scoring") / n
                print("%4d  %8.1f  %8.1f%s%s  %12.1f  %12.1f" % (L, hmean, hexact, keycol, sicol, sf, cf))
            else:
                print("%4d  %8.1f  %8.1f%s%s" % (L, hmean, hexact, keycol, sicol))

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
