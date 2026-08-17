#!/usr/bin/env python3
"""Matched-compute A/B: does SIGNATURE SEEDING beat spending the time on -R?

    python3 eval/seeder_vs_restarts.py                 # the A/B
    python3 eval/seeder_vs_restarts.py --reps 4        # a quick look

`eval/selfcrib_probe.py` established that a message ending in a doubled surname
yields ~28 candidate seeds, that exactly one of them is fully correct in 200/200
trials, and that ranking them by the index of coincidence puts that one first
150 times.  None of that says the seeder is worth having.  The lever it competes
with is `-R`, which this repo has measured to dominate every challenger it has
been put against (`--cascade`, `--polish`, `-F`, SA, GA), so the only result
that matters is a paired A/B at matched compute.

THE ARMS, all on the recommended recipe (`-c -f -l wehrmacht -S i4f10 -J
--polish`) with the true rotor key pinned:

  A  baseline     climbing from an empty board, swept over -R
  B  seeded HARD  -R fixed and low, with the IC-top seed's cables pinned via -s
                  and its deduced no-cable letters via --no-plug
  B3 hedged       best by score of the top three seeds, each run as arm B
  S  seeded SOFT  the same IC-top cables via --soft-plug: laid on the board each
                  restart starts from, then left free for the climb to move,
                  merge or drop
  O  oracle       arm B seeded with the CORRECT seed whatever the ranking said
  Bm hard matched arm B at the -R its score_iter needs to match arm S, since
                  nothing is pinned in S and its climbs are ~3x dearer
  SO oracle soft  arm S seeded the same way -- the ceiling for the soft form

B is the honest arm -- it uses what the ranking picks, so it pays for the ~25%
of trials where the ranking is wrong and `-s` then pins plugs the climb cannot
undo.  O exists to split a null result: if O wins and B does not, the seeder's
ranking is the problem; if O does not win either, seeding itself is worthless
and no better ranking would rescue it.

S is what B's failure mode argues for and is why `--soft-plug` was built: a
wrong hard seed measured WORSE than not seeding at all, because a pin cannot be
undone, whereas a wrong soft seed costs only the moves the climb spends walking
back out of it.  The pair (S, SO) answers whether that is true and what it
costs when the seed is right -- a soft seed can also be climbed AWAY from when
it was correct all along, which is the risk running the other way.  Note S
passes no --no-plug: a deduced no-cable letter is an assertion, i.e. a pin, so
the soft arm asserts nothing at all.

WHY A IS SWEPT RATHER THAN PINNED TO B'S -R.  Equal `-R` is NOT equal compute
here: pinning k letters removes them from the 325-toggle scan and from the free
set the climb has to converge, so a seeded restart is far cheaper than a bare
one -- measured ~6x fewer `score_iter` at `-R 8`.  Matching on `-R` would hand
the seeded arm most of the budget and call the result a win.  So A is swept and
the comparison is read at equal `score_iter`.

WALL TIME IS USELESS AT THIS SIZE and is reported only for scale: a run is
~0.1 s of which ~0.05 s is the one-off n-gram load, exactly the startup-inside-
the-benchmark trap `CLAUDE.md` documents.  `score_iter` is the compute axis,
with the standard caveat that it counts only the fused score loop -- the
seeder's own cost is ~28 whole-message decrypts (one per candidate seed) plus
the deduction closure, i.e. ~28 `score_iter`-equivalents and change against
arm B's ~4 400, so ~1%.  Not free, but far below the resolution of anything
below.

SCOPE, and it bounds the conclusion in two directions.  This is the
plugboard-recovery tier -- the rotor key is GIVEN -- which is the tier every
tuning result in this repo is measured on, and it is the seeder's BEST case: at
a wrong key the deduction pins wrong plugs.  So a loss here is decisive and a
win still leaves the full-sweep question open.  It also only applies to messages
that end with a doubled word, which is 10 of the 18 corpus messages carrying a
doubling and ~15% of the corpus overall.
"""
import argparse
import math
import os
import random
import re
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import selfcrib_probe as SC                                  # noqa: E402
from crib_menu import UNSET, core_rows, corpus, random_key    # noqa: E402
from ring_stride_geometry_probe import crypt, num, plugboard  # noqa: E402

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
RECIPE = ["-c", "-f", "-J", "--polish", "-S", "i4f10"]


def seeds_for(ct, rows, plug, tab):
    """Every distinct seed, IC-ranked, with the correct one identified.

    Returns (ranked, correct_index) where ranked is a list of
    (cables, noplug, is_correct) best-IC first.
    """
    c = num(ct)
    n = len(c)
    seen, rec = set(), []
    for menu in SC.terminal_hyps(ct):
        alive, board = SC.deduce(menu, rows)
        for h in np.nonzero(alive)[0]:
            s, known, pairs = SC.seed_from_row(board[h])
            key = s.tobytes()
            if key in seen:
                continue
            seen.add(key)
            rec.append((s, known, pairs,
                        bool(np.all(board[h][known] == plug[known]))))
    if not rec:
        return [], None
    S = np.array([r[0] for r in rec])
    mid = rows[np.arange(n)[None, :], S[:, c]]
    ic = SC.model_scores(S[np.arange(len(S))[:, None], mid], tab)["i"]
    order = np.argsort(-ic, kind="stable")
    ranked = []
    for i in order:
        s, known, pairs, ok = rec[i]
        cables = "".join(ALPHA[a] + ALPHA[b] for a, b in pairs)
        noplug = "".join(ALPHA[a] for a in known if s[a] == a)
        ranked.append((cables, noplug, ok))
    corr = next((r for r, t in enumerate(ranked) if t[2]), None)
    return ranked, corr


def run(binary, args, text):
    t0 = time.perf_counter()
    p = subprocess.run([binary] + args, input=text, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    wall = time.perf_counter() - t0
    iters, score = None, None
    for line in p.stderr.splitlines():
        m = re.search(r"scored (\d+) plugboards", line)
        if m:
            iters = int(m.group(1))
        f = line.split()
        if f and "." in f[0]:
            try:
                score = float(f[0])
            except ValueError:
                pass
    return p.stdout.strip(), iters, wall, score


def pct(rec, truth):
    return 100.0 * sum(a == b for a, b in zip(rec, truth)) / len(truth)


def binom_two(x, y):
    n = x + y
    if n == 0:
        return 1.0
    return min(1.0, sum(math.comb(n, k)
                        for k in range(min(x, y) + 1)) / 2 ** (n - 1))


def paired_ci(d):
    """95% CI on the mean of paired differences."""
    d = np.asarray(d, dtype=float)
    if d.size < 2:
        return 0.0, 0.0, 0.0
    se = d.std(ddof=1) / math.sqrt(d.size)
    return d.mean(), d.mean() - 1.96 * se, d.mean() + 1.96 * se


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default="./enigma")
    ap.add_argument("--lang", default="wehrmacht")
    ap.add_argument("--reps", type=int, default=12,
                    help="fresh keys per corpus message")
    ap.add_argument("--restarts", type=int, default=8,
                    help="-R for the seeded arms")
    ap.add_argument("--hard-matched", type=int, default=24,
                    help="-R for the hard-seeded arm matched to the soft arm")
    ap.add_argument("--sweep", default="8,16,32,64,128,256",
                    help="-R levels for the baseline arm")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seed", type=int, default=20260817)
    ap.add_argument("--out", default="eval/results-seeder-vs-restarts.txt")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    tab = SC.ngram_tables(a.lang)
    texts = [t for t in corpus() if SC.doublings(t)]
    ends = [t for t in texts
            if any(len(t) - (s + 2 * L + 1) <= 1
                   for s, L in SC.doublings(t, minlen=4, maxlen=20))]

    rows_out = []
    log = []

    def say(s=""):
        print(s)
        log.append(s)

    sweep = [int(x) for x in a.sweep.split(",")]
    tags = ["A%d" % r for r in sweep] + ["B", "Bm", "B3", "S", "SO", "O"]

    say(__doc__.split("\n")[0])
    say("\n%d messages x %d keys, %s, -T %d"
        % (len(ends), a.reps, " ".join(RECIPE + ["-l", a.lang]), a.threads))
    say("A (baseline) swept over -R %s; B/O (seeded) at -R %d\n"
        % (a.sweep, a.restarts))

    for pt_text in ends * a.reps:
        wheels, refl, ring, start = random_key(rng)
        plug = plugboard(np.random.default_rng(rng.randrange(1 << 30)), 10)
        ct = crypt(pt_text, wheels, refl, ring, start, plug)
        n = len(ct)
        rows = core_rows(wheels, refl, ring, start, n)
        ranked, corr = seeds_for(ct, rows, plug, tab)
        if not ranked:
            continue
        base = ["-u", refl, "-w", "".join(str(x + 1) for x in wheels),
                "-r", "".join(ALPHA[x] for x in ring),
                "-g", "".join(ALPHA[x] for x in start),
                "-l", a.lang, "-T", str(a.threads),
                "-e", str(rng.randrange(1 << 30))]
        def seeded(sel, soft=False, restarts=None):
            args = list(RECIPE) + base
            args += ["-R", str(restarts or a.restarts)]
            if soft:
                # A soft-seeded board starts GOOD, so the default 10-pair kick
                # is the wrong size for it -- measured 72.7 -> 79.0 mean at
                # --random 3.  The staged IC pre-pass is dropped for the same
                # reason (nothing to pre-seed) and measured near-neutral.
                args += ["--score", "f10", "--random", "3"]
            cables, noplug, _ = ranked[sel]
            if cables:
                # --soft-plug lays the same pairs down and leaves them free; the
                # deduced no-cable letters are a --no-plug assertion, i.e. also
                # a pin, so the soft arm drops them too and asserts nothing.
                args += ["--soft-plug" if soft else "-s", cables]
            if noplug and not soft:
                args += ["--no-plug", noplug]
            return run(a.binary, args, ct)

        out = {}
        for tag in tags:
            if tag[0] == "A":
                rec, it, wl, _ = run(a.binary,
                                     list(RECIPE) + base + ["-R", tag[1:]], ct)
                out[tag] = (pct(rec, pt_text), it, wl)
            elif tag in ("S", "SO"):
                rec, it, wl, _ = seeded(0 if tag == "S" else corr, soft=True)
                out[tag] = (pct(rec, pt_text), it, wl)
            elif tag == "Bm":
                rec, it, wl, _ = seeded(0, restarts=a.hard_matched)
                out[tag] = (pct(rec, pt_text), it, wl)
            elif tag == "B3":
                # No soft seeding exists, so the only way to hedge a wrong
                # ranking with today's options is to run the top few seeds and
                # keep the best by SCORE -- which is a far sharper judge than
                # the IC pre-ranking, since by then each seed's climb has run.
                got = [seeded(s) for s in range(min(3, len(ranked)))]
                best = max(got, key=lambda g: (g[3] is not None, g[3]))
                out[tag] = (pct(best[0], pt_text),
                            sum(g[1] or 0 for g in got),
                            sum(g[2] for g in got))
            else:
                rec, it, wl, _ = seeded(0 if tag == "B" else corr)
                out[tag] = (pct(rec, pt_text), it, wl)
        rows_out.append((n, corr, len(ranked[0][0]) // 2, out))
        print("  n=%-4d rank=%-3s cables=%-2d  %s"
              % (n, "-" if corr is None else corr, len(ranked[0][0]) // 2,
                 "  ".join("%s %5.1f" % (t, out[t][0]) for t in tags)))

    say()
    means = {t: np.array([r[3][t][0] for r in rows_out]) for t in tags}
    exact = {t: (means[t] > 99.999) for t in tags}
    iters = {t: np.array([r[3][t][1] or 0 for r in rows_out]) for t in tags}
    wall = {t: np.array([r[3][t][2] for r in rows_out]) for t in tags}
    say("%-6s %-10s %-12s %-14s %s"
        % ("arm", "mean %", "exact", "score_iter", "wall/trial"))
    for t in tags:
        say("%-6s %-10.1f %-12s %-14.0f %.2f s"
            % (t, means[t].mean(), "%d/%d" % (exact[t].sum(), len(rows_out)),
               iters[t].mean(), wall[t].mean()))

    # the matched-compute read: the baseline level whose score_iter is closest
    # to the seeded arm's, and the level whose QUALITY it matches.
    say()
    ref = iters["B"].mean()
    near = min(("A%d" % r for r in sweep),
               key=lambda t: abs(iters[t].mean() - ref))
    say("matched compute: B costs %.0f score_iter, nearest baseline is %s at"
        " %.0f (%.2fx)" % (ref, near, iters[near].mean(),
                           iters[near].mean() / max(1.0, ref)))
    msg = np.array([r[0] for r in rows_out])
    for t in ("B", "Bm", "B3", "S", "SO", "O"):
        d, lo, hi = paired_ci(means[t] - means[near])
        # The trials repeat over only ~10 distinct messages, so a per-trial
        # interval understates the uncertainty that matters for generalising
        # to a new message.  Cluster on the message and report both.
        per_msg = [np.mean((means[t] - means[near])[msg == u])
                   for u in np.unique(msg)]
        cd, clo, chi = paired_ci(per_msg)
        x = int((exact[t] & ~exact[near]).sum())
        y = int((exact[near] & ~exact[t]).sum())
        say("   %s vs %s: mean %+.2fpp, CI [%+.2f, %+.2f] per trial, "
            "[%+.2f, %+.2f] clustered on %d messages"
            % (t, near, d, lo, hi, clo, chi, len(per_msg)))
        say("      exact %+d (only %s %d, only %s %d), McNemar p = %.4f"
            % (x - y, t, x, near, y, binom_two(x, y)))
    beat = [t for t in ("A%d" % r for r in sweep)
            if means[t].mean() >= means["B"].mean()]
    say("   baseline levels reaching B's mean %%-correct: %s"
        % (", ".join("%s (%.1fx B's cost)"
                     % (t, iters[t].mean() / max(1.0, ref)) for t in beat)
           or "NONE in the sweep"))

    hit = np.array([r[1] == 0 for r in rows_out])
    if hit.any() and (~hit).any():
        say("\nsplit by whether the IC ranking picked the correct seed:")
        say("   %-15s %-7s %-8s %-8s %-8s %-8s %s"
            % ("", "trials", near, "B (-s)", "Bm", "S (soft)", "O"))
        for name, sel in (("ranking right", hit), ("ranking WRONG", ~hit)):
            say("   %-15s %-7d %-8.1f %-8.1f %-8.1f %-8.1f %.1f"
                % (name, sel.sum(), means[near][sel].mean(),
                   means["B"][sel].mean(), means["Bm"][sel].mean(),
                   means["S"][sel].mean(), means["O"][sel].mean()))
        top3 = np.array([r[1] is not None and r[1] < 3 for r in rows_out])
        say("   correct seed in the top 3: %d/%d trials"
            % (top3.sum(), len(rows_out)))

    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(log) + "\n")
    print("\nwritten to %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
