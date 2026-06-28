all : enigma

enigma : enigma.cc
	g++ -Wall -O3 -o enigma enigma.cc

test : enigma
	sh tests/run_tests.sh

clean :
	rm -f enigma

.PHONY : all test clean
