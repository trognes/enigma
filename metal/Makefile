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
# One metallib per variant, because the register cap the host's GPU: line
# reports is a property of the COMPILED pipeline: a runtime branch would
# have the compiler allocate for the union of the paths and every variant
# would report the same number.  The default build above is untouched --
# MC_ABLATE unset is variant 0.
#
#   make -C metal ablate        # ten metallibs, seconds each
#   python3 metal/ablate.py     # runs them and prints the tables
#
# TWO FAMILIES, because the first run of this was confounded (climb_body.h,
# MC_FIXED_PASSES).  Variants that change the score change the climb's
# trajectory, so they change how many passes it runs, and climbs/s does not
# normalise for that:
#
#   climb-a<N>   natural climb, MC_PASSES=1 so each run also reports the
#                pass count it took -- which is what turns its climbs/s
#                into passes/s, and what gives the divergence factor
#   climb-f<N>   MC_FIXED_PASSES, so every variant runs the same number of
#                scans by construction and climbs/s is directly comparable
#
# MC_PASSES is on in both: it costs one store per climb, measured at 2 219
# climbs/s against 2 229 for a build without it.
ABLATE_N := 0 1 2 3 4
ABLATE_FIXED ?= 8
ABLATE_LIBS := $(foreach n,$(ABLATE_N),climb-a$(n).metallib) \
               $(foreach n,$(ABLATE_N),climb-f$(n).metallib)

ablate : $(ABLATE_LIBS)

climb-a%.metallib : climb.metal climb_body.h
	xcrun -sdk macosx metal -fno-fast-math -DMC_ABLATE=$* -DMC_PASSES=1 \
	    -c climb.metal -o climb-a$*.air
	xcrun -sdk macosx metallib climb-a$*.air -o $@

climb-f%.metallib : climb.metal climb_body.h
	xcrun -sdk macosx metal -fno-fast-math -DMC_ABLATE=$* -DMC_PASSES=1 \
	    -DMC_FIXED_PASSES=$(ABLATE_FIXED) -c climb.metal -o climb-f$*.air
	xcrun -sdk macosx metallib climb-f$*.air -o $@

.PHONY : ablate
