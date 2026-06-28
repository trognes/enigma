CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O3

all : enigma

enigma : enigma.cc
	$(CXX) $(CXXFLAGS) -o enigma enigma.cc

test : enigma
	sh tests/run_tests.sh

clean :
	rm -f enigma

.PHONY : all test clean
