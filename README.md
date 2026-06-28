# Enigma cipher tool

This is a tool to encrypt or decrypt messages using an Enigma cipher machine simulator.

If you do not know the correct settings for decryption, it can also be used to try out
a large number of settings and look for plaintext messages that look similar to
text written in a selected language, i.e. crack the code.

The settings include the reflector (umkehrwalze) and wheels (walzen) used,
the ring positions (ringstellung) and start positions (grundstellung),
as well as the position of the plugs in the plugboard (steckerbrett).

Both the common three-wheel Enigma as well as the special Norway Enigma (Norenigma) is supported.

If specified (with the -c option), a hill-climbing algorithm will be used to identify the optimal plugboard configuration.

All possible combinations of the other unspecified settings will be tried.

```
Enigma cipher tool version 1.1.0
Copyright (C) 2017-2026 Torbjørn Rognes

Usage: enigma [OPTIONS]
  -h           Show help information
  -v           Show version information
  -u X         Reflector (umkehrwalze) X (A-C, N or .) [.]
  -w XYZ       Wheels (walzen) XYZ (1-8 or .) [...]
  -x integer   Highest wheel number to use (3-8) [5]
  -n           Use the Norway Enigma reflector (N) and wheels (1-5)
  -r XYZ       Ring positions (ringstellung) XYZ (A-Z or .) [AA.]
  -g XYZ       Start positions (grundstellung) XYZ (A-Z or .) [...]
  -s AB...     Plugboard (steckerbrett) letter pairs (A-Z pairs) [none]
  -c           Perform hill climbing to determine plugboard settings
  -l language  Scoring language (english, german, danish, french); required
               for -m/-b/-t/-q (no default), not used by -i
  -i           Use index of coincidence (IC) to determine plaintext score
  -m           Use monogram statistics to determine plaintext score
  -b           Use bigram statistics to determine plaintext score
  -t           Use trigram statistics to determine plaintext score
  -q           Use quadgram statistics to determine plaintext score [default]
  -p filename  Name of file containing plaintext to compare result with
  -d directory Directory holding the n-gram files (or $ENIGMA_DATA) [.]
  -T integer   Number of worker threads for the search (1-256) [1]

Defaults are indicated in [square brackets].

The ciphertext is read from standard input. The final plaintext is written
to standard output.

For the reflector, wheels, ring position and start position, a dot (.)
works as a wild card, leaving it unspecified. When these settings are not
specified, the program will try all combinations to find the settings
resulting in the highest plaintext score. If asked for, a hill climbing
algorithm will be used to try to determine the plugboard settings.
```

The search is parallelised over the whole key space — reflectors, wheel orders,
ring settings and start positions — so `-T N` uses N worker threads even when the
wheels are fixed and only the rings/starts are being searched. The default is a
single thread; on a 4-core machine a search runs about 3× faster with `-T 4`, and
scaling can be measured with `make bench SCALE=1`. When a run finishes, a short
diagnostic (wall-clock time, thread count, precomputed-table memory, peak memory)
is printed to standard error.

The files with the ngram frequencies for various languages have been obtained from the
[Practical cryptograhy](http://practicalcryptography.com/cryptanalysis/letter-frequencies-various-languages/)
website. Additional languages are available there. By default they are read from
the current directory; use `-d <directory>` (or set `ENIGMA_DATA`) to read them
from elsewhere, so the tool can be run from any working directory.

The hill climbing strategy is based on the algorithms described in the
[publications by Frode Weierud et al.](http://cryptocellar.org/Enigma/)

The software is available under the GNU GPL version 3 license.
