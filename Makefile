CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -Wshadow -Wold-style-cast -O3 -pthread

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
