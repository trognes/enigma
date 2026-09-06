# metal/Makefile -- the GPU host and its backends. Nothing here is reached
# by the top-level make, make test or CI (metal/DESIGN.md 13).
#
#   make -C metal          enigma-ref: the CPU reference backend, any platform
#   make -C metal metal    enigma-metal + climb.metallib, macOS only
#
# Both link every src/*.o except main.o, so build the tool first (`make` at
# the top level); those objects carry -ffp-contract=off, and so does this.
#
# The list is derived from the SOURCES, not from the objects on disk. A
# wildcard over src/*.o once linked a stale src/enigma.o -- the single
# translation unit from before the module split, which no source produces
# any more but which a long-lived checkout still had lying around -- and
# the link died on 23 duplicate symbols. The top-level Makefile builds from
# src/*.cc and never sees such a file, so only this one could.

CXX ?= c++
TOP := ..
SRCS := $(filter-out $(TOP)/src/main.cc,$(wildcard $(TOP)/src/*.cc))
OBJS := $(SRCS:.cc=.o)
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
#
# Since Xcode 16 the Metal compiler is a separately downloaded component
# ("requires Xcode ... command line tools instance" usually means Xcode is
# installed but not selected: sudo xcode-select -s /Applications/Xcode.app/
# Contents/Developer; DESIGN.md 13 has the no-Xcode route too):
# if xcrun reports a missing Metal Toolchain, run (once per machine)
#     xcodebuild -downloadComponent MetalToolchain
climb.metallib : climb.metal climb_body.h
	xcrun -sdk macosx metal -fno-fast-math -c climb.metal -o climb.air
	xcrun -sdk macosx metallib climb.air -o $@

clean :
	rm -f enigma-ref enigma-metal *.o *.air *.metallib

.PHONY : all metal clean

# --- DESIGN.md 17.6 step 1: the cost probes -------------------------------
# One metallib per MC_ABLATE variant, because the register cap the host's
# GPU: line reports is a property of the COMPILED pipeline: a runtime
# branch would have the compiler allocate for the union of the paths and
# every variant would report the same number.  The default build above is
# untouched -- MC_ABLATE unset is variant 0.
#
#   make -C metal ablate        # climb-a1.metallib .. climb-a5.metallib
#   python3 metal/ablate.py     # runs them and prints the table
ABLATE_LIBS := $(foreach n,1 2 3 4 5,climb-a$(n).metallib)

ablate : $(ABLATE_LIBS)

climb-a%.metallib : climb.metal climb_body.h
	xcrun -sdk macosx metal -fno-fast-math -DMC_ABLATE=$* -c climb.metal \
	    -o climb-a$*.air
	xcrun -sdk macosx metallib climb-a$*.air -o $@

.PHONY : ablate
