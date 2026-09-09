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
SRC_C    := src/imageio/imageio_pnm.c src/imageio/imageio_png.c src/imageio/uPNG/uPNG.c
OBJ_CXX  := $(SRC_CXX:.cpp=.o)
OBJ_C    := $(SRC_C:.c=.o)
OBJ      := $(OBJ_CXX) $(OBJ_C)

BIN      := mtnbli

.PHONY: all native clean test isa help

all: $(BIN)

# run the whole self test suite (needs the upstream NBLI/fNBLI binaries for the cross checks,
# point REFDIR at them if they are somewhere else)
test: $(BIN)
	@./tests/selftest.sh

# prove that the binary contains no instruction a Phenom II does not have
isa: $(BIN)
	@./tests/isa_check.sh

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
src/nbli/NBLI.o: src/nbli/NBLI.cpp src/nbli/rANS.h src/nbli/GolombCodeTree.h src/nbli/NBLIcodec.h \
                 src/nbli/Header.h src/nbli/PlaneModel.h src/nbli/Mapper.h src/nbli/AdvancedPredictor.h

tests/quick_test: tests/quick_test.cpp src/fnbli_scalar.h src/fnbli_api.h
	$(CXX) $(CXXFLAGS) $(INC) -Isrc/imageio -o $@ tests/quick_test.cpp src/imageio/imageio_pnm.c

tests/enc_test: tests/enc_test.cpp src/fnbli_scalar.h src/fnbli_api.h
	$(CXX) $(CXXFLAGS) $(INC) -Isrc/imageio -o $@ tests/enc_test.cpp src/imageio/imageio_pnm.c

clean:
	rm -f $(OBJ) $(BIN) $(BIN)_native tests/quick_test tests/enc_test

help:
	@echo "targets:  all (default, portable ISA) | native (-march=native) | test | isa | clean"
