#!/usr/bin/env python3
"""Does the fused model's WEIGHTING have any discrimination headroom left?

    python3 eval/score_weight_probe.py --trials 60 --lengths 40 60 100

THE QUESTION.  -f scores a window as a log-linear mixture
a*logP(ABCD) + b*logP(BCD) + c*logP(CD) + d*logP(D), plus lambda*IC, with
(a,b,c,d) = (1, 0.6, 0.3, 0.15) and lambda = 30 BAKED IN.  Those were tuned
once, on prose, before -f existed in its current form and before k4f10 became
the recommended pre-pass.  On authentic telegraphic German the scoring-failure
share is the dominant residual at short lengths (CLAUDE.md: 56% at L=40 under
-q, 90% under the recommended recipe), so a sharper WEIGHTING -- not a sharper
search -- is the only thing that can move it.

WHY THIS IS OFFLINE, AND WHY THAT IS THE RIGHT SHAPE.  Scoring failure is
"the true plugboard does not score highest".  That is a property of two
DECRYPTS, not of a search: given the true plaintext and a decoy plaintext, any
additive n-gram model can be evaluated on both in Python.  So the expensive
part (producing decrypts with the binary) is done ONCE, and hundreds of weight
vectors are then swept for free.  No binary change is needed -- which matters,
because ENIGMA_LOGLIN does NOT reach these weights: load_table() passes the
baked vector as force_ll and that branch ignores the environment.

THE DECOYS MUST BE HARD, AND MUST NOT BE THE ARGMAX OF THE MODEL UNDER TEST.
Those pull in opposite directions and getting either wrong wastes the run.
Perturbations of the true board and random boards are model-independent but
TRIVIAL -- measured, the truth outscores them in 100% of trials, so the probe
reports a tie and ranks on tie-break noise.  Conversely, decoys taken from a
climb under the shipped model are adversarially selected AGAINST it: that
board is the shipped model's own argmax, so every alternative weighting beats
it for free.

Weak generators do not work either: climbs under -i/-m/-t at -R 8 leave the
truth winning 100% of trials, so the probe ranks on tie-break noise.  The
decoys are therefore the winners of STRONG searches (-K --polish, high -R)
under FIVE different targets -- i, t, q, a, f.  Each grid weighting then faces
the argmax of whichever generator is nearest it, so the bias is roughly
SYMMETRIC across the grid rather than aimed at the shipped point.  It is not
eliminated: a weighting far from every generator still faces a weaker pool.
That is why this screens rather than decides.

WHAT IT CAN AND CANNOT SAY.  It RANKS weightings; it does not size an
end-to-end gain -- CLAUDE.md records that an offline probe predicted the wrong
SIGN for the kick-ranking question, and that the AUC-to-recovery conversion
spans 6x.  A weighting that wins here has earned a paired recovery run, nothing
more.  A weighting that does not win here is dead without one.
"""

import argparse
import math
import os
import random
import re
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ENIGMA = os.path.join(HERE, os.pardir, "enigma")
NGRAMS = os.path.join(HERE, os.pardir, "ngrams")
LET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def decrypts(path):
    out = []
    for blk in open(path, encoding="utf-8").read().split("### Message ")[1:]:
        m = re.search(r"^DECRYPT:(.*?)(?=^[A-Z][A-Z ]*:|\Z)", blk, re.S | re.M)
        if m:
            out.append(re.sub(r"[^A-Z]", "", m.group(1)))
    return out


def load_order(lang, name):
    """log10 probabilities, unseen floored at log10(1/total) -- the hapax floor
    the C++ loader uses, so an unseen gram is scored as a single occurrence."""
    path = os.path.join(NGRAMS, f"{lang}_{name}.txt")
    counts = {}
    total = 0
    for line in open(path, encoding="utf-8"):
        p = line.split()
        if len(p) == 2 and p[0].isalpha():
            c = int(p[1])
            counts[p[0].upper()] = counts.get(p[0].upper(), 0) + c
            total += c
    if total == 0:
        sys.exit(f"empty table: {path}")
    lg = {g: math.log10(c / total) for g, c in counts.items()}
    return lg, math.log10(1.0 / total)


class Model:
    """The -f score, reimplemented so weights are a parameter rather than baked.

    Matches the shipped shape: per-window log-linear mixture with the symmetric
    fold (each order's sub-grams averaged over their window multiplicity, so
    leading-edge grams are included), normalised per symbol, plus lambda*IC."""

    def __init__(self, lang):
        self.q, self.qf = load_order(lang, "quadgrams")
        self.t, self.tf = load_order(lang, "trigrams")
        self.b, self.bf = load_order(lang, "bigrams")
        self.m, self.mf = load_order(lang, "monograms")

    def parts(self, text):
        """The four order-sums and the IC, once per text: sweeping weights then
        costs a dot product instead of a re-scan."""
        n = len(text)
        s4 = s3 = s2 = s1 = 0.0
        for i in range(3, n):
            w = text[i - 3:i + 1]
            s4 += self.q.get(w, self.qf)
            s3 += (self.t.get(w[0:3], self.tf)
                   + self.t.get(w[1:4], self.tf)) / 2.0
            s2 += (self.b.get(w[0:2], self.bf) + self.b.get(w[1:3], self.bf)
                   + self.b.get(w[2:4], self.bf)) / 3.0
            s1 += (self.m.get(w[0], self.mf) + self.m.get(w[1], self.mf)
                   + self.m.get(w[2], self.mf)
                   + self.m.get(w[3], self.mf)) / 4.0
        f = Counter(text)
        coin = sum(v * (v - 1) for v in f.values())
        ic = coin / (n * (n - 1)) if n > 1 else 0.0
        nt = max(n - 3, 1)
        return (s4 / nt, s3 / nt, s2 / nt, s1 / nt, ic)

    @staticmethod
    def score(parts, w, lam):
        return (w[0] * parts[0] + w[1] * parts[1] + w[2] * parts[2]
                + w[3] * parts[3] + lam * parts[4])


def run(argv, text):
    env = dict(os.environ)
    env["ENIGMA_SEED"] = "0"
    env["ENIGMA_DATA"] = NGRAMS
    p = subprocess.run([ENIGMA] + [str(a) for a in argv], input=text,
                       capture_output=True, text=True, check=False, env=env)
    return p.stdout.strip()


def board_str(pairs):
    return " ".join(a + b for a, b in pairs)


def perturb(pairs, k, rng):
    """Rewire k plugs: break k pairs and re-pair their letters differently."""
    pairs = [list(p) for p in pairs]
    idx = rng.sample(range(len(pairs)), min(k, len(pairs)))
    loose = [c for i in idx for c in pairs[i]]
    rng.shuffle(loose)
    for j, i in enumerate(idx):
        pairs[i] = [loose[2 * j], loose[2 * j + 1]]
    return [tuple(p) for p in pairs]


def random_board(rng, n):
    ls = list(LET)
    rng.shuffle(ls)
    return [(ls[2 * i], ls[2 * i + 1]) for i in range(n)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=60)
    ap.add_argument("--lengths", type=int, nargs="+", default=[40, 60, 100])
    ap.add_argument("--plugs", type=int, default=10)
    ap.add_argument("--language", default="wehrmacht")
    ap.add_argument("--restarts", type=int, default=8)
    ap.add_argument("--generators", nargs="+",
                    default=["i", "t", "q", "a", "f"],
                    help="model LETTERS (no dash -- argparse would read one as "
                         "an option) whose strong climb winners become decoys")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    if not os.path.exists(ENIGMA):
        sys.exit("build the binary first (make)")
    model = Model(args.language)
    corpus = "".join(decrypts(os.path.join(HERE, "enigma-messages.txt"))
                     + decrypts(os.path.join(HERE,
                                             "enigma-army-messages-1941.txt")))
    rng = random.Random(args.seed)

    # (a,b,c,d) mixture weights x lambda. The shipped vector is first so it is
    # never advantaged by tie-break ordering.
    SHIPPED = ((1.0, 0.6, 0.3, 0.15), 30.0)
    grid = [SHIPPED]
    for b in (0.0, 0.3, 0.6, 1.0, 1.5):
        for c in (0.0, 0.15, 0.3, 0.6):
            for d in (0.0, 0.15, 0.3):
                for lam in (0.0, 10.0, 30.0, 60.0, 120.0):
                    v = ((1.0, b, c, d), lam)
                    if v != SHIPPED:
                        grid.append(v)

    # wins[k] = trials where the truth outscores EVERY decoy, per weight vector
    wins = [0] * len(grid)
    margin = [0.0] * len(grid)
    ntr = 0
    per_len = {L: [0] * len(grid) for L in args.lengths}
    per_len_n = dict.fromkeys(args.lengths, 0)

    for L in args.lengths:
        for _ in range(args.trials):
            pt = corpus[rng.randrange(0, len(corpus) - L):][:L]
            w = "".join(str(x) for x in rng.sample([1, 2, 3, 4, 5], 3))
            r = "".join(rng.choice(LET) for _ in range(3))
            g = "".join(rng.choice(LET) for _ in range(3))
            true_pairs = random_board(rng, args.plugs)
            key = ["-u", "B", "-w", w, "-r", r, "-g", g]
            ct = run(key + ["-s", board_str(true_pairs)], pt)
            if len(ct) != L:
                continue

            # Hard decoys: what a climb under a DIFFERENT model converges to.
            # Its own argmax, so competitive; not the grid's, so not rigged.
            texts = []
            for gletter in args.generators:
                gmodel = "-" + gletter.lstrip("-")
                a = key + ["-c", "-K", "--polish", gmodel,
                           "-R", args.restarts, "-T", 1]
                if gmodel != "-i":
                    a += ["-l", args.language]
                t = run(a, ct)
                if len(t) == L and t != pt:
                    texts.append(t)
            # Two easy ones, so a trial is never decided by a lucky generator.
            for d in (perturb(true_pairs, 1, rng),
                      random_board(rng, args.plugs)):
                t = run(key + ["-s", board_str(d)], ct)
                if len(t) == L and t != pt:
                    texts.append(t)
            if not texts:
                continue

            tp = model.parts(pt)
            dp = [model.parts(t) for t in texts]
            ntr += 1
            per_len_n[L] += 1
            for k, (wv, lam) in enumerate(grid):
                s_true = Model.score(tp, wv, lam)
                s_dec = [Model.score(p, wv, lam) for p in dp]
                best = max(s_dec)
                if s_true > best:
                    wins[k] += 1
                    per_len[L][k] += 1
                spread = (max(s_dec) - min(s_dec)) or 1.0
                margin[k] += (s_true - best) / spread

    if ntr == 0:
        sys.exit("no usable trials")
    order = sorted(range(len(grid)), key=lambda k: (-wins[k], -margin[k]))
    print(f"# {ntr} trials, L={args.lengths}, {args.plugs} plugs, "
          f"decoys: climbs under {' '.join(args.generators)} "
          f"+2 easy, "
          f"-l {args.language}, seed {args.seed}")
    print("# READ THE GENERATOR LIST: a weighting is disadvantaged against "
          "decoys")
    print("# produced by a model resembling it. Run two generator sets before "
          "believing")
    print("# any result -- measured, that alone moves lambda's apparent value "
          "by 24pp.")
    print(f"\n{'rank':>4} {'a':>4} {'b':>4} {'c':>4} {'d':>5} {'lambda':>7} "
          f"{'truth wins':>11} {'margin':>8}")
    shipped_k = 0
    for rank, k in enumerate(order[:12], 1):
        (a, b, c, d), lam = grid[k]
        tag = "  <-- SHIPPED" if k == shipped_k else ""
        print(f"{rank:>4} {a:>4.1f} {b:>4.1f} {c:>4.1f} {d:>5.2f} {lam:>7.1f} "
              f"{100 * wins[k] / ntr:>10.1f}% {margin[k] / ntr:>8.3f}{tag}")
    k = shipped_k
    (a, b, c, d), lam = grid[k]
    print(f"\nSHIPPED  ({a}, {b}, {c}, {d}) lambda={lam}: "
          f"{100 * wins[k] / ntr:.1f}% truth wins, rank "
          f"{order.index(k) + 1} of {len(grid)}")
    # Named vectors, printed whatever their rank: the question is not only
    # "what wins" but "what does one specific documented change do".
    NAMED = [("shipped", (1.0, 0.6, 0.3, 0.15), 30.0),
             ("no bigram", (1.0, 0.6, 0.0, 0.15), 30.0),
             ("no bigram, no IC", (1.0, 0.6, 0.0, 0.15), 0.0),
             ("shipped, no IC", (1.0, 0.6, 0.3, 0.15), 0.0),
             ("pure quad", (1.0, 0.0, 0.0, 0.0), 0.0),
             ("quad+IC", (1.0, 0.0, 0.0, 0.0), 30.0)]
    print("\nnamed vectors (truth wins %, and per length):")
    for nm, wv, lam in NAMED:
        try:
            k = grid.index((wv, lam))
        except ValueError:
            continue
        cells = "  ".join(
            f"L={L}:{100 * per_len[L][k] / (per_len_n[L] or 1):5.1f}%"
            for L in args.lengths)
        print(f"  {nm:<18} {100 * wins[k] / ntr:5.1f}%   {cells}")

    print("\nby length (truth-wins %, shipped vs best in grid):")
    for L in args.lengths:
        n = per_len_n[L] or 1
        best = max(range(len(grid)), key=lambda j: per_len[L][j])
        (a, b, c, d), lam = grid[best]
        print(f"  L={L:<4} shipped {100 * per_len[L][shipped_k] / n:5.1f}%"
              f"   best {100 * per_len[L][best] / n:5.1f}%"
              f"  = ({a}, {b}, {c}, {d}) lambda={lam}")


if __name__ == "__main__":
    main()
