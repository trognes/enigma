CXX = g++
# -Wmissing-declarations is here for a specific failure this repo has already
# had: a function with external linkage and no header declaration is invisible
# to every gate. subst_rotors() sat dead for years that way, and three search
# workers with a single same-file caller each were found the same way. Both
# compilers accept it, and a clean tree reports none.
#
# -ffp-contract=off IS LOAD-BEARING, NOT A STYLE CHOICE. The low-order climb
# stages (-S i/m/k) score from a co-occurrence histogram instead of decoding,
# and the whole design rests on the two paths producing the SAME double --
# hist_probe() is compared against score_iter() inside a loop that repeats
# while the score improves, so one ULP of disagreement is not a rounding
# difference, it is an INFINITE LOOP. Both paths assemble the mono score as
# `isum/scale + textlength*bias`, which a compiler is free to contract into an
# FMA at one site and not the other. On x86-64 without -march that never
# happens (no FMA in the baseline ISA); on arm64 it is baseline, g++ contracts
# by default, and `-S m4a10` and `-S m4f10` HANG. IC-only stages are safe by
# construction -- a single division, nothing to contract -- so the exposure is
# exactly the mono and mono+IC models, which includes the recommended k4f10.
# Verified: the x86-64 binary is byte-identical with and without this flag and
# contains zero FMA instructions, so it costs nothing where it is not needed.
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -Wshadow \
           -Wold-style-cast -Wmissing-declarations -O3 -pthread \
           -ffp-contract=off

# Appended after CXXFLAGS; used by CI to add e.g. -Werror or sanitizers
# without dropping the base flags:  make EXTRA_CXXFLAGS=-Werror
EXTRA_CXXFLAGS =

# One object per module under src/ (see CLAUDE.md, "Repository layout").
# Sources are globbed rather than listed so adding a module needs no Makefile
# edit; -MMD -MP writes a .d per object so a header change rebuilds exactly its
# dependents.
SRCS = $(wildcard src/*.cc)
OBJS = $(SRCS:.cc=.o)
DEPS = $(OBJS:.o=.d)

all : enigma

enigma : $(OBJS)
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -o $@ $(OBJS)

src/%.o : src/%.cc
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

test : enigma
	sh tests/run_tests.sh

# Performance benchmark (isolates the search and hill-climb hot paths).
#   make bench                       quick tiers, working-tree binary
#   make bench LONG=1                add the >=15-30s long tiers
#   make bench SCALE=1               also sweep -T to show thread scaling
#   make bench BASE=<git-ref>        same-machine A/B vs <git-ref> (fails on
#                                    >THRESHOLD% slowdown; default 10)
bench : enigma
	sh tests/bench.sh

# Cracking-quality benchmark (recovery rate vs ciphertext length -- the hard,
# short-message regime). Separate from `bench` (speed) and `test` (pass/fail).
#   make crackquality                  working-tree binary
#   make crackquality BASE=<git-ref>   same-machine A/B vs <git-ref>
# Tunables (env): MODEL, CLANG, TRIALS, LENGTHS, PAIRS, SEED, SPLIT, CRACKOPTS.
# Full-crack / scoring-gate knobs (CRACKQUALITY_TESTS.md §1): WILDCARD, XMAX,
# FILTER, RESTARTS, FULLCRACK.
crackquality : enigma
	python3 tests/crack_quality.py

clean :
	rm -f enigma $(OBJS) $(DEPS)

.PHONY : all test bench crackquality clean
