CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Wcast-qual -Wshadow -O3

# Appended after CXXFLAGS; used by CI to add e.g. -Werror or sanitizers
# without dropping the base flags:  make EXTRA_CXXFLAGS=-Werror
EXTRA_CXXFLAGS =

all : enigma

enigma : enigma.cc
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -o enigma enigma.cc

test : enigma
	sh tests/run_tests.sh

# Performance benchmark (isolates the search and hill-climb hot paths).
#   make bench                       quick tiers, working-tree binary
#   make bench LONG=1                add the >=15-30s long tiers
#   make bench BASE=<git-ref>        same-machine A/B vs <git-ref> (fails on
#                                    >THRESHOLD% slowdown; default 10)
bench : enigma
	sh tests/bench.sh

clean :
	rm -f enigma

.PHONY : all test bench clean
