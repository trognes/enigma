#!/usr/bin/env python3
#
# Per-run evaluation harness -- appends ONE ROW PER RUN to eval/results.tsv, a
# persistent, committed benchmark of short-message plugboard recovery.
#
# Scope (the regime we care about): short messages (default L=50), 10 plugs,
# quad as the final/ranking score, English and German. Each run is one
# independent random problem (excerpt + rotor key + 10-pair plugboard); the
# TRUE rotor key is handed to the search and only the plugboard is hill-climbed
# (the cheap "plugboard-recovery" tier -- the same tier crack_quality.py uses).
#
# Unlike crack_quality.py (which prints per-length AGGREGATES over a fixed,
# SEED-deterministic trial set), this driver records every individual run with
# enough detail to reproduce and to diagnose it. See eval/README.md for the
# column schema and the exact reproduction recipe.
#
# Reproducibility: a row is self-contained. plaintext + true_* settings fully
# specify the instance (no problem seed / corpus needed); git_sha + cli_options
# + solver_seed specify the solve. solver_seed is pinned to 0 and recorded.
#
# Usage:
#   make            # build ./enigma first
#   python3 tests/eval.py                 # 40 runs/lang, recommended recipe
#   EVAL_RUNS=60 python3 tests/eval.py    # more runs
#   EVAL_OPTS='-S i4q10 -R 10' EVAL_LABEL='i4q10.R10' python3 tests/eval.py
#
# Env knobs:
#   EVAL_LANGS    space-separated languages (default "english german")
#   EVAL_RUNS     runs per language (default 40)
#   EVAL_LENGTH   message length (default 50)
#   EVAL_PAIRS    plug pairs (default 10)
#   EVAL_OPTS     climb-strategy options, the part that varies per experiment
#                 (default "-J -S i4q10 -R 10"); the rotor key is supplied
#                 separately from the true settings, so DON'T put -u/-w/-r/-g/-s
#                 here. -q (quad) is fixed by design and prepended automatically.
#   EVAL_LABEL    short config label for grouping (default: EVAL_OPTS collapsed)
#   EVAL_THREADS  -T threads (default 1, for clean comparable wall-time)
#   EVAL_SOLVER_SEED  -e restart seed (default 0)
#   EVAL_OUT      output TSV (default eval/results.tsv)

import datetime
import os
import random
import re
import shlex
import socket
import string
import subprocess
import sys
import time

# Same passages as crack_quality.py, so the two harnesses sample the same text.
CORPORA = {
    "english": "THEQUICKANALYSISOFLANGUAGESTATISTICSSHOWSTHATENGLISHTEXTHASAMUCHHIGHERINDEXOFCOINCIDENCETHANRANDOMLYCHOSENLETTERSBECAUSESOMELETTERSLIKEEANDTOCCURFARMOREOFTENTHANOTHERSWHENWEEXAMINEALONGPASSAGEOFORDINARYPROSEWEFINDTHATCERTAINCOMMONWORDSANDLETTERPATTERNSREPEATSOOFTENTHATTHEYBETRAYTHEUNDERLYINGSTRUCTUREOFTHEMESSAGEEVENAFTERITHASBEENENCRYPTEDWITHAROTORMACHINELIKETHEENIGMAUSEDINTHEWARHISTORIANSBELIEVETHATBREAKINGTHISCIPHERSHORTENEDTHECONFLICTBYSEVERALYEARSANDSAVEDCOUNTLESSLIVES",
    "german": "DIEENIGMAMASCHINEWURDEIMZWEITENWELTKRIEGVONDERDEUTSCHENWEHRMACHTVERWENDETUMGEHEIMENACHRICHTENZUVERSCHLUESSELNABERDIEALLIIERTENKONNTENDENGEHEIMENCODETROTZDEMBRECHENWEILDIEDEUTSCHENOFTDIEGLEICHENFLOSKELNVERWENDETENUNDWEILVIELEBEDIENERIMMERWIEDERDIESELBENFEHLERMACHTENDIEPOLNISCHENUNDBRITISCHENMATHEMATIKERBAUTENMASCHINENUMDIETAEGLICHENSCHLUESSELZUFINDENUNDLASENSODIEGEHEIMENFUNKSPRUECHEDESFEINDESMITUNDVERKUERZTENDADURCHDENKRIEGUMMEHREREJAHREUNDRETTETENVIELETAUSENDMENSCHENLEBEN",
    "danish": "DETVARENGANGENLILLEHAVFRUESOMBOEDELANGTUDEPAAHAVETSBUNDSAMMENMEDSINFADEROGSINEFEMSOESTREHUNVARDENYNGSTEOGSMUKKESTEAFDEMALLEMENHUNLAENGTESEFTERATKOMMEOPTILMENNESKENESVERDENOGSEDENSTORESKIBEOGBYERNEOGSKOVENEHVERTAARBLEVHUNAELDREOGFIKLOVTILATSTIGEOPGENNEMDETKLAREVANDFORATSIDDEPAAKLIPPERNEISKINNETFRAMAANENOGSEUDOVERDENSTOREVIDEVERDENOGNAARSOLENGIKNEDDYKKEDEHUNNEDIGENMENHUNGLEMTEALDRIGDENDEJLIGEVERDENOVENOVERVANDETOGENDAGDAHUNREDDEDEENUNGPRINSFRADRUKNINGFORELSKEDEHUNSIGHAABLOEST",
    "french": "LESSANGLOTSLONGSDESVIOLONSDELAUTOMNEBLESSENTMONCOEURDUNELANGUEURMONOTONETOUTSUFFOCANTETBLEMEQUANDSONNELHEUREJEMESOUVIENSDESJOURSANCIENSETALORSJEPLEUREETJEMENVAISAUVENTMAUVAISQUIMEMPORTEDECADELABCOMMELAFEUILLEMORTEPENDANTLONGTEMPSJEMESUISCOUCHEDEBONNEHEUREETJAIREVEDESPAYSLOINTAINSOULESHOMMESSONTLIBRESETOULAVIEESTDOUCEETBELLECHAQUEMATINJEMEPROMENAISLELONGDELARIVIEREENECOUTANTLECHANTDESOISEAUXETLEMURMUREDELEAUQUICOULAITDOUCEMENTVERSLAMER",
}

HEADER = [
    "git_sha", "timestamp_utc", "host", "language", "corpus", "length", "num_plugs",
    "true_reflector", "true_rotors", "true_ring", "true_grund", "true_plugs",
    "plaintext", "cli_options", "config_label", "solver_seed", "threads",
    "letters_matched_count", "letters_matched_pct", "exact_match", "score_iter",
    "wall_time_ms", "recovered_plugs", "recovered_plaintext", "recovered_score",
    "true_score",
]


def env(name, default):
    v = os.environ.get(name)
    return v if v not in (None, "") else default


def run(binary, args, text):
    """Feed `text` on stdin; return (stdout, stderr, wall_ms)."""
    t0 = time.perf_counter()
    p = subprocess.run([binary] + args, input=text,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)
    return p.stdout, p.stderr, (time.perf_counter() - t0) * 1000.0


def encrypt(binary, key, plain):
    u, w, r, g, pb = key
    out, _, _ = run(binary, ["-i", "-u", u, "-w", w, "-r", r, "-g", g, "-s", pb], plain)
    return out.strip()


def last_progress(stderr):
    """The last progress line's fields, or None. A progress line's first token is
    the score (has a '.'); the new format is 'score W R G <plugs...> <preview>'."""
    fields = None
    for line in stderr.splitlines():
        f = line.split()
        if f and "." in f[0]:
            try:
                float(f[0])
            except ValueError:
                continue
            fields = f
    return fields


def parse_recovered(stderr):
    """(score, plugs_str) from the last progress line. plugs = the 2-letter tokens
    between the start column (index 3) and the trailing 15-char text preview."""
    f = last_progress(stderr)
    if not f:
        return None, ""
    score = float(f[0])
    plugs = " ".join(f[4:-1])   # drop score/W/R/G and the trailing preview token
    return score, plugs


def score_iter(stderr):
    n = None
    for line in stderr.splitlines():
        m = re.search(r"scored (\d+) plugboards", line)
        if m:
            n = int(m.group(1))
    return n


def oracle_score(binary, key, ct, lang):
    """Quad score of the TRUE (key+plugboard) decrypt -- true_score."""
    u, w, r, g, pb = key
    _, err, _ = run(binary, ["-q", "-l", lang, "-u", u, "-w", w,
                             "-r", r, "-g", g, "-s", pb], ct)
    s, _ = parse_recovered(err)
    return s


def load_corpora(lang):
    """[(name, text), ...] for a language from eval/corpora/<lang>_*.txt (A-Z
    only, uppercased); falls back to the built-in passage if the dir has none.
    Drop more <lang>_<name>.txt files in eval/corpora/ to add corpora -- no code
    change needed (any non-A-Z is stripped, so raw text files are fine).
    EVAL_CORPORA (comma-separated corpus names) restricts to a subset, e.g.
    EVAL_CORPORA=doenitz1945 to test one corpus in isolation."""
    only = env("EVAL_CORPORA", "")
    only_set = set(only.split(",")) if only else None
    out = []
    cdir = os.path.join("eval", "corpora")
    if os.path.isdir(cdir):
        for fn in sorted(os.listdir(cdir)):
            if fn.startswith(lang + "_") and fn.endswith(".txt"):
                name = fn[len(lang) + 1:-4]
                if only_set is not None and name not in only_set:
                    continue
                with open(os.path.join(cdir, fn)) as fh:
                    text = re.sub(r"[^A-Z]", "", fh.read().upper())
                if text:
                    out.append((name, text))
    if not out and lang in CORPORA and (only_set is None or "builtin" in only_set):
        out = [("builtin", CORPORA[lang])]
    return out


def gen_problem(rng, corpora, length, pairs):
    """One random problem from a randomly chosen corpus, weighted so every valid
    excerpt across all corpora is equally likely. Returns
    (corpus_name, excerpt, (u, w, r, g, pb))."""
    usable = [(nm, t) for nm, t in corpora if len(t) >= length]
    weights = [len(t) - length + 1 for _, t in usable]
    nm, text = rng.choices(usable, weights=weights, k=1)[0]
    off = rng.randrange(len(text) - length + 1)
    excerpt = text[off:off + length]
    u = rng.choice("ABC")
    w = "".join(str(d) for d in rng.sample(range(1, 9), 3))
    r = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
    g = "".join(rng.choice(string.ascii_uppercase) for _ in range(3))
    letters = rng.sample(string.ascii_uppercase, 2 * pairs)
    pb = " ".join(letters[2 * i] + letters[2 * i + 1] for i in range(pairs))
    return nm, excerpt, (u, w, r, g, pb)


def git_sha(root, ignore=()):
    """Short HEAD sha, '-dirty' appended if the tree differs from HEAD -- but
    changes to `ignore` paths (e.g. the results file we are appending to) don't
    count as dirty, so a clean-code run records a clean sha."""
    def g(args):
        return subprocess.run(["git", "-C", root] + args,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              universal_newlines=True).stdout.strip()
    sha = g(["rev-parse", "--short", "HEAD"]) or "unknown"
    ignore_set = {os.path.normpath(p) for p in ignore}
    # Parse the path as the token after the 2-char status code. Don't rely on a
    # fixed offset: g() strips, which drops porcelain's leading status space.
    status = subprocess.run(["git", "-C", root, "status", "--porcelain"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            universal_newlines=True).stdout
    dirty = [ln for ln in status.splitlines()
             if ln.strip() and os.path.normpath(ln.split(maxsplit=1)[-1]) not in ignore_set]
    if dirty:
        sha += "-dirty"
    return sha


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    binary = "./enigma"
    if not os.access(binary, os.X_OK):
        sys.exit("error: %s not built; run 'make' first" % binary)

    langs = env("EVAL_LANGS", "english german").split()
    runs = int(env("EVAL_RUNS", "40"))
    length = int(env("EVAL_LENGTH", "50"))
    pairs = int(env("EVAL_PAIRS", "10"))
    opts = env("EVAL_OPTS", "-J -S i4q10 -R 10")
    label = env("EVAL_LABEL", "") or re.sub(r"\s+", "", opts)
    threads = env("EVAL_THREADS", "1")
    solver_seed = env("EVAL_SOLVER_SEED", "0")
    out_path = env("EVAL_OUT", os.path.join("eval", "results.tsv"))

    sha = git_sha(root, ignore=[out_path])
    host = socket.gethostname()
    opt_list = shlex.split(opts)
    rng = random.Random()   # fresh OS entropy -> different problems each invocation

    # cli_options records the strategy portion; the rotor key comes from true_*.
    cli_options = " ".join(["-q", "-l", "<lang>", "-c"] + opt_list +
                           ["-e", solver_seed, "-T", threads])

    if not os.path.exists(out_path) or os.path.getsize(out_path) == 0:
        with open(out_path, "w") as fh:
            fh.write("\t".join(HEADER) + "\n")

    total = len(langs) * runs
    done = 0
    print("eval: %d runs x %d langs=%d rows -> %s  [sha=%s opts='%s' label=%s]"
          % (runs, len(langs), total, out_path, sha, opts, label), file=sys.stderr)

    for lang in langs:
        corpora = load_corpora(lang)
        if not corpora:
            print("skip unknown language %s" % lang, file=sys.stderr)
            continue
        cli = cli_options.replace("<lang>", lang)
        exact_ct = 0
        pct_sum = 0.0
        for _ in range(runs):
            corpus_name, excerpt, key = gen_problem(rng, corpora, length, pairs)
            u, w, r, g, pb = key
            ct = encrypt(binary, key, excerpt)

            args = ["-q", "-l", lang, "-u", u, "-w", w, "-r", r, "-g", g, "-c"] \
                + opt_list + ["-e", solver_seed, "-T", threads]
            rec_out, rec_err, wall_ms = run(binary, args, ct)
            recovered = rec_out.strip()

            matched = sum(1 for a, b in zip(recovered, excerpt) if a == b)
            pct = 100.0 * matched / length
            exact = 1 if matched == length else 0
            si = score_iter(rec_err)
            rec_score, rec_plugs = parse_recovered(rec_err)
            true_score = oracle_score(binary, key, ct, lang)

            ts = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"
            row = [
                sha, ts, host, lang, corpus_name, str(length), str(pairs),
                u, w, r, g, pb, excerpt, cli, label, solver_seed, threads,
                str(matched), "%.1f" % pct, str(exact),
                str(si) if si is not None else "",
                "%.1f" % wall_ms, rec_plugs, recovered,
                "%.4f" % rec_score if rec_score is not None else "",
                "%.4f" % true_score if true_score is not None else "",
            ]
            with open(out_path, "a") as fh:
                fh.write("\t".join(row) + "\n")

            exact_ct += exact
            pct_sum += pct
            done += 1

        print("  %-8s: %d runs  mean%%=%.1f  exact=%d/%d"
              % (lang, runs, pct_sum / runs, exact_ct, runs), file=sys.stderr)

    print("eval: wrote %d rows to %s" % (done, out_path), file=sys.stderr)


if __name__ == "__main__":
    main()
