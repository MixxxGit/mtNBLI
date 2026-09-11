#===================================================================================================
#  mtnbli : multi-threaded, SIMD-free NBLI / fNBLI codec
#
#  The default target builds a binary that only uses the x86-64 baseline ISA (SSE, SSE2, SSE3):
#  no SSSE3, no SSE4.1/4.2, no AVX, no AVX2.   It therefore runs on an AMD Phenom II X6 1055T
#  (and on anything newer).   `make native` builds an -march=native variant for comparison.
#===================================================================================================
CXX      ?= g++
CC       ?= gcc

UNAME_M  := $(shell uname -m)

# baseline ISA of every x86-64 CPU (incl. AMD K10 / Phenom II)
ifeq ($(UNAME_M),x86_64)
  PORT_ISA := -mno-ssse3 -mno-sse4 -mno-sse4.1 -mno-sse4.2 -mno-avx -mno-avx2 -mno-fma -mno-bmi -mno-bmi2 -mno-lzcnt -mno-popcnt -mno-aes -mno-pclmul
else
  PORT_ISA :=
endif

WARN     := -Wall -Wno-unused-function -Wno-sign-compare
CXXFLAGS ?= -O3 -std=c++11 -pthread -fno-strict-aliasing $(PORT_ISA) $(WARN)
CFLAGS   ?= -O2 -std=gnu99 $(PORT_ISA) -w
LDFLAGS  ?= -pthread

INC      := -Isrc -Isrc/imageio

SRC_CXX  := src/mtnbli.cpp src/nbli/NBLI.cpp
SRC_C    := src/imageio/imageio_pnm.c src/imageio/imageio_png.c src/imageio/deflate.c src/imageio/ioutf8.c src/imageio/uPNG/uPNG.c
OBJ_CXX  := $(SRC_CXX:.cpp=.o)
OBJ_C    := $(SRC_C:.c=.o)
OBJ      := $(OBJ_CXX) $(OBJ_C)

BIN      := mtnbli

#--------------------------------------------------------------------------- Windows cross build
#  needs  apt-get install mingw-w64   (or any x86_64-w64-mingw32 toolchain)
#  Produces a statically linked mtnbli.exe with no DLL dependencies beyond the Windows ones,
#  and with the same "no SSE4 / no AVX" restriction as the portable Linux build.
WINCXX  ?= x86_64-w64-mingw32-g++
WINCC   ?= x86_64-w64-mingw32-gcc
WINBIN  ?= mtnbli.exe
WINOBJ  := $(SRC_CXX:%.cpp=%.w.o) $(SRC_C:%.c=%.w.o)
WINFLAGS = -O3 -std=c++11 -fno-strict-aliasing $(PORT_ISA) $(WARN) $(INC) \
           -static -static-libgcc -static-libstdc++
WINCFLAGS = -O2 -std=gnu99 $(PORT_ISA) -w $(INC)

.PHONY: all native clean test isa defl-test win64 win64-isa win64-check bench-compare help

all: $(BIN)

# run the whole self test suite (needs the upstream NBLI/fNBLI binaries for the cross checks,
# point REFDIR at them if they are somewhere else)
test: $(BIN)
	@./tests/selftest.sh

# prove that the binary contains no instruction a Phenom II does not have
isa: $(BIN)
	@./tests/isa_check.sh

# cross compile a Windows x86-64 .exe with the same portable ISA
win64: $(WINBIN)

$(WINBIN): $(WINOBJ)
	$(WINCXX) $(WINFLAGS) -o $@ $(WINOBJ) -static -static-libgcc -static-libstdc++ -lpthread
	@echo "  built $@  (portable ISA, Windows x86-64)"

%.w.o: %.cpp
	$(WINCXX) $(WINFLAGS) -c -o $@ $<

%.w.o: %.c
	$(WINCC) $(WINCFLAGS) -c -o $@ $<

# verify the Windows build too (isa_check.sh understands PE files via the mingw objdump)
win64-isa: $(WINBIN)
	@./tests/isa_check.sh $(WINBIN)

# run the .exe under wine and compare its output with the native build (skipped if wine is absent)
win64-check: $(WINBIN) $(BIN)
	@./tests/win_check.sh

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	@echo "  built $@  (portable ISA build)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# build for the machine that compiles it (only for benchmarking, NOT portable)
native: CXXFLAGS = -O3 -std=c++11 -pthread -march=native
native: CFLAGS   = -O2 -std=gnu99 -march=native -w
native: clean
native: $(BIN)
	@mv $(BIN) $(BIN)_native

src/mtnbli.o: src/mtnbli.cpp src/fnbli_scalar.h src/fnbli_api.h src/tiles.h src/ThreadPool.h \
              src/FileIO.h src/CRC32.h src/imageio/imageio.h

# the Windows objects have no hand written dependency list, so give them all the headers --
# otherwise editing a header leaves a stale .w.o and the .exe silently keeps the old code
HDRS := $(wildcard src/*.h) $(wildcard src/nbli/*.h) $(wildcard src/imageio/*.h) $(wildcard src/imageio/*.h)
$(WINOBJ): $(HDRS)
src/nbli/NBLI.o: src/nbli/NBLI.cpp src/nbli/rANS.h src/nbli/GolombCodeTree.h src/nbli/NBLIcodec.h \
                 src/nbli/Header.h src/nbli/PlaneModel.h src/nbli/Mapper.h src/nbli/AdvancedPredictor.h

tests/quick_test: tests/quick_test.cpp src/fnbli_scalar.h src/fnbli_api.h
	$(CXX) $(CXXFLAGS) $(INC) -Isrc/imageio -o $@ tests/quick_test.cpp src/imageio/imageio_pnm.c

tests/enc_test: tests/enc_test.cpp src/fnbli_scalar.h src/fnbli_api.h
	$(CXX) $(CXXFLAGS) $(INC) -Isrc/imageio -o $@ tests/enc_test.cpp src/imageio/imageio_pnm.c

# deflate round trip : every level 0..9 must inflate back to the original data
tests/defl_test: tests/defl_test.c src/imageio/deflate.c src/imageio/deflate.h
	$(CC) $(CFLAGS) $(INC) -o $@ tests/defl_test.c src/imageio/deflate.c

defl-test: tests/defl_test
	@./tests/defl_test /tmp/mt_defl >/dev/null && python3 tests/defl_check.py /tmp/mt_defl

# compare mtnbli with the upstream NBLI / fNBLI on a folder of images :
#   make bench-compare BENCH_ARGS="-c /path/to/NBLI -i /path/to/images"   (or -h for the options)
bench-compare: $(BIN)
	@if [ -z "$(BENCH_ARGS)" ]; then python3 tests/bench_compare.py --help; \
	 else python3 tests/bench_compare.py $(BENCH_ARGS); fi

clean:
	rm -f $(OBJ) $(WINOBJ) $(BIN) $(BIN)_native $(WINBIN) tests/quick_test tests/enc_test tests/defl_test

help:
	@echo "targets:  all (default, portable ISA) | native (-march=native) | test | isa | clean"
	@echo "          win64 | win64-isa | win64-check   (cross compile mtnbli.exe, needs mingw-w64)"
	@echo "          bench-compare BENCH_ARGS=\"-c <bin dir> -i <image dir>\" : mtnbli vs upstream"
