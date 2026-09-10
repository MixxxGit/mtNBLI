#!/usr/bin/env bash
#===================================================================================================
#  build the upstream (single threaded, unmodified) NBLI and fNBLI binaries.
#
#  They are used by selftest.sh / corpus_check.py for the cross checks:
#       upstream encoder -> mtnbli decoder      and      mtnbli encoder -> upstream decoder
#  Without them those checks are simply skipped, they are never a hard requirement.
#
#  usage :  tests/build_ref.sh [destdir]        (default: ../NBLI, i.e. the usual sibling checkout)
#
#  Note that upstream fNBLI is written with AVX2 intrinsics and therefore has to be compiled
#  with -mavx2 -- that is exactly the problem mtnbli solves, so the reference binary can only
#  be built and run on an AVX2 machine.  The build is a best effort: if it fails, this script
#  prints a warning and exits 0 (CI must stay green, the cross checks are optional).
#===================================================================================================
set -u
here=$(cd "$(dirname "$0")"; pwd)
DEST=${1:-$here/../../NBLI}
CXX=${CXX:-g++}

mkdir -p "$DEST" || exit 0
cd "$DEST" || exit 0

if [ ! -d src ]; then
    echo "cloning upstream NBLI ..."
    git clone --depth 1 https://github.com/WangXuan95/NBLI.git . >/dev/null 2>&1 || {
        echo "  warning : could not clone upstream, cross checks will be skipped"; exit 0; }
fi
[ -f src/NBLI/main.cpp ] || { echo "  warning : no upstream sources in $DEST"; exit 0; }

COMMON="src/imageio/*.c src/imageio/uPNG/*.c"
INC="-Isrc -Isrc/imageio -Isrc/imageio/uPNG"

# upstream uPNG.c says  #include "upng.h"  but the file is called  uPNG.h  (case sensitive fs)
[ -f src/imageio/uPNG/uPNG.h ] && ln -sf uPNG.h src/imageio/uPNG/upng.h

rm -f NBLI fNBLI

echo "building upstream NBLI ..."
$CXX -O3 -std=c++11 -static $INC src/NBLI/main.cpp src/NBLI/NBLI.cpp $COMMON -o NBLI -w 2>&1 | tail -3
[ -x NBLI ] && echo "  ok    $DEST/NBLI" || echo "  warning : NBLI build failed"

echo "building upstream fNBLI (-mavx2, upstream needs it) ..."
$CXX -O3 -std=c++11 -mavx2 -static $INC src/fNBLI/main.cpp src/fNBLI/fNBLI.cpp $COMMON -o fNBLI -w 2>&1 | tail -3
[ -x fNBLI ] && echo "  ok    $DEST/fNBLI" || echo "  warning : fNBLI build failed"

exit 0
