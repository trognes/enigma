#!/usr/bin/env python3
"""How many null samples does --confidence need?  (CLAUDE.md, --confidence.)

    python3 eval/confidence_sample_size.py --seeds 12

WHAT IS BEING SIZED.  --confidence N estimates the mean and sd of the score a
key gets with no signal, and reports the winner's distance above that minus
sqrt(2 ln K), the distance the best of K keys reaches by chance.  K is exact
arithmetic; N buys precision in mu-hat and sd-hat and NOTHING else.  So the
question is not "is the margin bigger with more samples" but "how far does the
REPORTED margin move between runs that differ only in the sampling seed".

WHY THE SIGNAL-FREE ARM IS THE ONE THAT DECIDES IT.  The flag exists to say
"nothing was found".  A noisy null can push a signal-free run's margin ABOVE
zero, which is a false positive on the one question the flag answers -- and no
amount of reading the number carefully recovers from that.  Scatter on the
signal arm is harmless by comparison: +16 +/- 4 and +16 +/- 0.2 lead to the
same decision.  Both arms are measured; the recommendation follows the noise
one.

The two arms are paired -- same key, same keyspace, same length -- because a
one-sided measurement would rate a build that always printed "significant" as
having excellent precision.

CORPUS IS PINNED, NOT READ FROM THE REPO.  An earlier harness in this repo drew
its plaintext from README.md and CLAUDE.md, so writing up the result silently
changed the corpus underneath the follow-up runs.  eval/corpus-tune-phase-ab.txt
is a frozen snapshot; each plaintext is hashed into the output so a rerun can be
checked for drift rather than assumed identical.

WHAT THE SPREAD SHOULD LOOK LIKE.  Propagating the error in mu-hat and sd-hat
through (s - mu)/sd gives

    SE(margin) ~ sqrt((1 + z^2/2) / N)

with z the winner's z-score: the 1/N term from the mean, the z^2/2N term from
the sd's own relative error.  The harness prints that prediction beside the
observed spread, because the two agreeing is what licenses reading a
recommendation off a handful of seeds instead of hundreds.  Near the decision
boundary z is sqrt(2 ln K) whatever the keyspace, which is why the answer does
not depend on K or on message length.
"""
import argparse
import hashlib
import math
import os
import random
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ENIGMA = os.path.join(ROOT, "enigma")
CORPUS = os.path.join(HERE, "corpus-tune-phase-ab.txt")

MARGIN_RE = re.compile(r"margin ([+-][0-9.]+) sd")

# One wheel order, ring pinned, start wildcarded: K = 26^3 = 17576 keys.  Small
# enough to sweep in a moment and large enough that sqrt(2 ln K) = 4.4 is a real
# bar rather than a rounding error.
KEYSPACE = ["-u", "B", "-w", "123", "-r", "AAA", "-g", "..."]
TRUE_START = "AQD"


def run(args, stdin_text):
  """Return (stdout, stderr) for one invocation."""
  p = subprocess.run([ENIGMA] + args, input=stdin_text, capture_output=True,
                     text=True, check=False)
  return p.stdout, p.stderr


def encrypt(plain, extra):
  """Enigma is reciprocal, so a fixed-key 'decrypt' of plaintext is the
  ciphertext.  Using the tool itself keeps the machine model in one place."""
  out, _ = run(["-u", "B", "-w", "123", "-r", "AAA", "-g", TRUE_START,
                "-q", "-l", "english"] + extra, plain)
  return out.strip()


def margin_of(cipher, n, seed):
  """The margin --confidence reports for a scan of the whole keyspace, or None
  if it printed none."""
  _, err = run(KEYSPACE + ["-q", "-l", "english", "--confidence", str(n),
                           "-e", str(seed)], cipher)
  m = MARGIN_RE.search(err)
  return float(m.group(1)) if m else None


def timed(cipher, n, climb, reps=3):
  """Min of `reps` wall times.  The COST arms pin the rotor key: the search
  itself must be near-free, or a difference of a few hundred milliseconds of
  calibration is invisible under a sweep of thousands of keys -- which is
  exactly what the first attempt at this measurement showed (N=512 cost nothing
  detectable under a 31 s -c run)."""
  args = ["-u", "B", "-w", "123", "-r", "AAA", "-g", TRUE_START,
          "-q", "-l", "english", "--confidence", str(n), "-e", "1"]
  if climb:
    args += ["-c", "-R", "1"]
  best = float("inf")
  for _ in range(reps):
    t0 = time.time()
    run(args, cipher)
    best = min(best, time.time() - t0)
  return best


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--seeds", type=int, default=12,
                  help="sampling seeds per cell (the spread is over these)")
  ap.add_argument("--length", type=int, default=200)
  ap.add_argument("--ns", type=int, nargs="+",
                  default=[16, 32, 64, 128, 256, 512, 1024])
  ap.add_argument("--corpus-offset", type=int, default=500)
  args = ap.parse_args()

  if not os.path.exists(ENIGMA):
    sys.exit("build the binary first (make)")

  text = open(CORPUS, encoding="utf-8").read()
  text = re.sub("[^A-Z]", "", text.upper())
  plain = text[args.corpus_offset:args.corpus_offset + args.length]
  if len(plain) < args.length:
    sys.exit("corpus too short for that offset/length")
  rng = random.Random(7)
  noise_plain = "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
                        for _ in range(args.length))

  signal = encrypt(plain, [])
  # The noise arm must be ciphertext with no plaintext behind it, not an
  # encryption of random letters -- those are the same thing, but generating it
  # directly keeps the arms symmetric in how they reach the tool.
  noise = noise_plain

  print(f"# corpus {os.path.basename(CORPUS)} "
        f"sha1(plain)={hashlib.sha1(plain.encode()).hexdigest()[:12]} "
        f"L={args.length} K=17576 seeds={args.seeds}")
  print(f"# {'N':>5} {'noise sd':>9} {'noise max':>10} {'signal sd':>10} "
        f"{'pred sd':>8} {'scan s':>7} {'climb s':>8}")

  zk = math.sqrt(2 * math.log(17576))

  for n in args.ns:
    noi = [margin_of(noise, n, e) for e in range(1, args.seeds + 1)]
    sig = [margin_of(signal, n, e) for e in range(1, args.seeds + 1)]
    noi = [v for v in noi if v is not None]
    sig = [v for v in sig if v is not None]
    if len(noi) < 2 or len(sig) < 2:
      print(f"  {n:>5} -- too few margins reported")
      continue
    pred = math.sqrt((1 + zk * zk / 2) / n)
    print(f"  {n:>5} {statistics.stdev(noi):9.2f} {max(noi):+10.2f} "
          f"{statistics.stdev(sig):10.2f} {pred:8.2f} "
          f"{timed(signal, n, False):7.3f} {timed(signal, n, True):8.3f}")

  print("# noise max above zero at any N = a false 'significant' at that N.")
  print("# pred sd is sqrt((1 + zk^2/2)/N) at the decision boundary z = zk.")


if __name__ == "__main__":
  main()
