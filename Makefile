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

clean :
	rm -f enigma

.PHONY : all test clean
