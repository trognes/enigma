# metal/Makefile -- the GPU host and its backends. Nothing here is reached
# by the top-level make, make test or CI (metal/DESIGN.md 13).
#
#   make -C metal          enigma-ref: the CPU reference backend, any platform
#   make -C metal metal    enigma-metal + climb.metallib, macOS only
#
# Both link every src/*.o except main.o, so build the tool first (`make` at
# the top level); those objects carry -ffp-contract=off, and so does this.

CXX ?= c++
TOP := ..
OBJS := $(filter-out $(TOP)/src/main.o,$(wildcard $(TOP)/src/*.o))
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -O3 -pthread \
            -ffp-contract=off -I$(TOP)/src

all : enigma-ref

$(OBJS) :
	$(MAKE) -C $(TOP)

enigma-ref : host_common.o backend_cpu.o $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ host_common.o backend_cpu.o $(OBJS)

metal : enigma-metal climb.metallib

enigma-metal : host_common.o backend_metal.o $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ host_common.o backend_metal.o $(OBJS) \
	    -framework Metal -framework Foundation

host_common.o : host_common.cc host_common.h climb_body.h
	$(CXX) $(CXXFLAGS) -c -o $@ host_common.cc

backend_cpu.o : backend_cpu.cc host_common.h climb_body.h
	$(CXX) $(CXXFLAGS) -c -o $@ backend_cpu.cc

backend_metal.o : backend_metal.mm host_common.h climb_body.h
	$(CXX) $(CXXFLAGS) -ObjC++ -fobjc-arc -c -o $@ backend_metal.mm

# -fno-fast-math: the body holds no floating point, so this is defence in
# depth against any FP that reaches a wrapper later (DESIGN.md 11).
climb.metallib : climb.metal climb_body.h
	xcrun -sdk macosx metal -fno-fast-math -c climb.metal -o climb.air
	xcrun -sdk macosx metallib climb.air -o $@

clean :
	rm -f enigma-ref enigma-metal *.o climb.air climb.metallib

.PHONY : all metal clean
