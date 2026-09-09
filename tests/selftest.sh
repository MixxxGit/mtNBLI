#!/usr/bin/env bash
#===================================================================================================
#  mtnbli self test
#
#   * generates its own test images (no external data needed)
#   * compresses / decompresses them with every codec mode and compares the result bit-exactly
#   * if the upstream (unmodified, single threaded) NBLI / fNBLI binaries are found, they are
#     used for cross checks in both directions:
#          upstream encoder -> mtnbli decoder
#          mtnbli  encoder -> upstream decoder
#   * if the reference binaries are missing, those cross checks are skipped
#
#   usage :  tests/selftest.sh      (REFDIR=/path/to/upstream/NBLI is honoured)
#===================================================================================================
set -u

here=$(cd "$(dirname "$0")"; pwd)
MT="$here/../mtnbli"
GEN="python3 $here/genimg.py"
CMP="python3 $here/imgcmp.py"
SPLIT="python3 $here/tnbli_split.py"
REFDIR=${REFDIR:-$here/../../NBLI}

if [ ! -x "$MT" ]; then echo "*** $MT not built, run 'make' first"; exit 1; fi

TMP=${TMPDIR:-/tmp}/mtnbli_selftest.$$
rm -rf "$TMP"; mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

np=0; nf=0; ns=0
ok   () { np=$((np+1)); printf "  ok    %s\n" "$1"; }
bad  () { nf=$((nf+1)); printf "  FAIL  %s   (%s)\n" "$1" "$2"; }
skip () { ns=$((ns+1)); printf "  skip  %s   (%s)\n" "$1" "$2"; }

# compare two images, optional max difference and row range of the FIRST image
imgcmp () {                      # <A> <B> [maxdiff] [row0:row1]
    if [ $# -ge 3 ]; then python3 "$here/imgcmp.py" "$1" "$2" "$3" ${4:+"$4"} >/dev/null 2>&1
    else                  python3 "$here/imgcmp.py" "$1" "$2" ${4:+"$4"}       >/dev/null 2>&1; fi
}

#--------------------------------------------------------------------------- build the test images
echo "generating test images ..."
IMGS=""
mk () { local f="$TMP/$1"; shift; $GEN "$f" "$@" >/dev/null; IMGS="$IMGS $f"; }
mk rgb_1x1.ppm        1     1     0
mk gray_1x1.pgm       1     1     0
mk rgb_2x3.ppm        2     3     0
mk rgb_17x1.ppm       17    1     0
mk rgb_1x17.ppm       1     17    0
mk rgb_63x63.ppm      63    63    0
mk gray_63x63.pgm     63    63    0
mk rgb_128x128.ppm    128   128   0
mk gray_200x150.pgm   200   150   0
mk rgb_300x200.ppm    300   200   0
mk rgb_noise.ppm      200   150   2
mk gray_flat.pgm      200   150   1
mk rgb_512x512.ppm    512   512   0

#===================================================================================================
echo
echo "1) fNBLI : mtnbli encode -> mtnbli decode  (single stream, lossless)"
for f in $IMGS; do
    b=$(basename $f)
    "$MT" -f -x "$f" -o "$TMP/$b.fnbli" >/dev/null 2>&1 || { bad "$b encode" "exit $?"; continue; }
    "$MT" -f --pnm "$TMP/$b.fnbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b decode" "exit $?"; continue; }
    if imgcmp "$f" "$TMP/$b.out.pnm"; then ok "$b"; else bad "$b" "pixels differ"; fi
done

#===================================================================================================
echo
echo "2) fNBLI tiled : mtnbli -T n encode -> mtnbli decode (1 thread and all threads)"
for f in $IMGS; do
    b=$(basename $f)
    for T in 2 3 7; do
        "$MT" -f -x -T $T "$f" -o "$TMP/$b.t$T.tnbli" >/dev/null 2>&1 || { bad "$b -T $T encode" "exit $?"; continue; }
        for t in 1 0; do
            "$MT" -f -t $t --pnm "$TMP/$b.t$T.tnbli" -o "$TMP/$b.t$T.out.pnm" >/dev/null 2>&1 || { bad "$b -T $T -t $t decode" "exit $?"; continue; }
            if imgcmp "$f" "$TMP/$b.t$T.out.pnm"; then ok "$b -T $T -t $t"; else bad "$b -T $T -t $t" "pixels differ"; fi
        done
    done
    # -T 0 : automatic tile count (2 per thread)
    "$MT" -f -x -T 0 "$f" -o "$TMP/$b.tauto.tnbli" >/dev/null 2>&1 || { bad "$b -T 0 encode" "exit $?"; continue; }
    "$MT" -f --pnm "$TMP/$b.tauto.tnbli" -o "$TMP/$b.tauto.out.pnm" >/dev/null 2>&1 || { bad "$b -T 0 decode" "exit $?"; continue; }
    if imgcmp "$f" "$TMP/$b.tauto.out.pnm"; then ok "$b -T 0 (auto)"; else bad "$b -T 0 (auto)" "pixels differ"; fi
done

#===================================================================================================
echo
echo "3) NBLI : every coder / predictor / near-lossless combination"
for f in $IMGS; do
    b=$(basename $f)
    # the advanced predictor is O(huge) -- only run it on the small images
    big=; case "$b" in rgb_512x512*|rgb_300x200*|rgb_noise*) big=1;; esac
    for mode in "-N" "-N -g"; do
        "$MT" -f -x $mode "$f" -o "$TMP/$b.nbli" >/dev/null 2>&1 || { bad "$b $mode encode" "exit $?"; continue; }
        "$MT" -f --pnm "$TMP/$b.nbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b $mode decode" "exit $?"; continue; }
        if imgcmp "$f" "$TMP/$b.out.pnm"; then ok "$b $mode"; else bad "$b $mode" "pixels differ"; fi
    done
    if [ -z "$big" ]; then
        for mode in "-N -a" "-N -g -a"; do
            "$MT" -f -x $mode "$f" -o "$TMP/$b.nbli" >/dev/null 2>&1 || { bad "$b $mode encode" "exit $?"; continue; }
            "$MT" -f --pnm "$TMP/$b.nbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b $mode decode" "exit $?"; continue; }
            if imgcmp "$f" "$TMP/$b.out.pnm"; then ok "$b $mode"; else bad "$b $mode" "pixels differ"; fi
        done
    fi
    # note : for RGB the near-lossless bound of NBLI holds in the internal (mapped) colour space,
    #        so the RGB error can reach ~2*near.  The upstream codec produces exactly the same
    #        numbers (verified), hence the tolerance 2*near+1 here.
    for near in 1 2 3; do
        tol=$((2*near+1))
        "$MT" -f -x -N -$near "$f" -o "$TMP/$b.nbli" >/dev/null 2>&1 || { bad "$b -N -$near encode" "exit $?"; continue; }
        "$MT" -f --pnm "$TMP/$b.nbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b -N -$near decode" "exit $?"; continue; }
        if imgcmp "$f" "$TMP/$b.out.pnm" $tol; then ok "$b -N -$near (|err|<=$tol)"; else bad "$b -N -$near" "error > $tol"; fi
    done
done

#===================================================================================================
echo
echo "4) NBLI tiled : mtnbli -N -T 4 -> mtnbli decode"
for f in $IMGS; do
    b=$(basename $f)
    case "$b" in rgb_512x512*) continue;; esac
    "$MT" -f -x -N -T 4 "$f" -o "$TMP/$b.tnbli" >/dev/null 2>&1 || { bad "$b -N -T 4 encode" "exit $?"; continue; }
    "$MT" -f --pnm "$TMP/$b.tnbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b -N -T 4 decode" "exit $?"; continue; }
    if imgcmp "$f" "$TMP/$b.out.pnm"; then ok "$b -N -T 4"; else bad "$b -N -T 4" "pixels differ"; fi
done

#===================================================================================================
echo
echo "5) cross checks with the upstream codecs"
REF_FNBLI=""; REF_NBLI=""
for c in "$REFDIR/fNBLI" "$REFDIR/fNBLI.exe" "$REFDIR/build/fNBLI"; do [ -x "$c" ] && REF_FNBLI="$c"; done
for c in "$REFDIR/NBLI"  "$REFDIR/NBLI.exe"  "$REFDIR/build/NBLI";  do [ -x "$c" ] && REF_NBLI="$c";  done

if [ -z "$REF_FNBLI" ] && [ -z "$REF_NBLI" ]; then
    skip "upstream binaries" "not found in $REFDIR (set REFDIR=...), cross checks skipped"
else
    [ -n "$REF_FNBLI" ] && echo "     upstream fNBLI : $REF_FNBLI"
    [ -n "$REF_NBLI"  ] && echo "     upstream NBLI  : $REF_NBLI"

    # ---- 5a : upstream fNBLI encoder  ->  mtnbli decoder
    if [ -n "$REF_FNBLI" ]; then
        for f in $IMGS; do
            b=$(basename $f)
            "$REF_FNBLI" -f -x "$f" -o "$TMP/$b.ref.fnbli" >/dev/null 2>&1 || { bad "$b ref-encode" "exit $?"; continue; }
            "$MT" -f --pnm "$TMP/$b.ref.fnbli" -o "$TMP/$b.ref.out.pnm" >/dev/null 2>&1 || { bad "$b decode(upstream stream)" "exit $?"; continue; }
            if imgcmp "$f" "$TMP/$b.ref.out.pnm"; then ok "$b : upstream fNBLI -> mtnbli"; else bad "$b : upstream fNBLI -> mtnbli" "pixels differ"; fi
        done
    fi

    # ---- 5b : mtnbli encoder  ->  upstream fNBLI decoder
    if [ -n "$REF_FNBLI" ]; then
        for f in $IMGS; do
            b=$(basename $f)
            "$MT" -f -x "$f" -o "$TMP/$b.mine.fnbli" >/dev/null 2>&1
            "$REF_FNBLI" -f "$TMP/$b.mine.fnbli" -o "$TMP/$b.mine.ref.png" >/dev/null 2>&1 || { bad "$b upstream-decode" "exit $?"; continue; }
            if imgcmp "$f" "$TMP/$b.mine.ref.png"; then ok "$b : mtnbli -> upstream fNBLI"; else bad "$b : mtnbli -> upstream fNBLI" "pixels differ"; fi
        done
    fi

    # ---- 5c : upstream NBLI encoder (all modes) -> mtnbli decoder
    if [ -n "$REF_NBLI" ]; then
        for f in $IMGS; do
            b=$(basename $f)
            case "$b" in rgb_512x512*) continue;; esac
            for mode in "" "-g" "-a" "-g -a"; do
                "$REF_NBLI" -f -x $mode "$f" -o "$TMP/$b.ref.nbli" >/dev/null 2>&1 || { bad "$b ref NBLI $mode" "exit $?"; continue; }
                "$MT" -f --pnm "$TMP/$b.ref.nbli" -o "$TMP/$b.refn.out.pnm" >/dev/null 2>&1 || { bad "$b decode NBLI $mode" "exit $?"; continue; }
                if imgcmp "$f" "$TMP/$b.refn.out.pnm"; then ok "$b : upstream NBLI '$mode' -> mtnbli"; else bad "$b : upstream NBLI '$mode' -> mtnbli" "pixels differ"; fi
            done
        done
    fi

    # ---- 5d : mtnbli NBLI encoder -> upstream NBLI decoder
    if [ -n "$REF_NBLI" ]; then
        for f in $IMGS; do
            b=$(basename $f)
            case "$b" in rgb_512x512*) continue;; esac
            for mode in "-N" "-N -g"; do
                "$MT" -f -x $mode "$f" -o "$TMP/$b.mine.nbli" >/dev/null 2>&1
                "$REF_NBLI" -f "$TMP/$b.mine.nbli" -o "$TMP/$b.mine.refn.png" >/dev/null 2>&1 || { bad "$b upstream NBLI decode" "exit $?"; continue; }
                if imgcmp "$f" "$TMP/$b.mine.refn.png"; then ok "$b : mtnbli '$mode' -> upstream NBLI"; else bad "$b : mtnbli '$mode' -> upstream NBLI" "pixels differ"; fi
            done
        done
    fi

    # ---- 5e : a .tnbli tile is an ordinary stream that the upstream tool can decode
    if [ -n "$REF_FNBLI" ]; then
        f="$TMP/rgb_300x200.ppm"; b=$(basename $f)
        "$MT" -f -x -T 3 "$f" -o "$TMP/$b.tnbli" >/dev/null 2>&1
        if python3 "$here/tiles_check.py" "$f" "$TMP/$b.tnbli" "$REF_FNBLI" >"$TMP/tiles.txt" 2>&1
        then ok "tiles of a .tnbli decode standalone (upstream fNBLI)"
        else bad "tiles of a .tnbli" "$(tail -2 "$TMP/tiles.txt" | tr '\n' ' ')"; fi
    fi
    if [ -n "$REF_NBLI" ]; then
        f="$TMP/gray_200x150.pgm"; b=$(basename $f)
        "$MT" -f -x -N -T 3 "$f" -o "$TMP/$b.tnbli" >/dev/null 2>&1
        if python3 "$here/tiles_check.py" "$f" "$TMP/$b.tnbli" "$REF_NBLI" >"$TMP/tilesn.txt" 2>&1
        then ok "tiles of a .tnbli decode standalone (upstream NBLI)"
        else bad "tiles of a .tnbli (NBLI)" "$(tail -2 "$TMP/tilesn.txt" | tr '\n' ' ')"; fi
    fi
fi

#===================================================================================================
echo
echo "6) negative tests (damaged input must be rejected, not crash)"
f="$TMP/rgb_128x128.ppm.fnbli"
"$MT" -f -x "$TMP/rgb_128x128.ppm" -o "$f" >/dev/null 2>&1
cp "$f" "$TMP/damaged.fnbli"
python3 - <<EOF
d = bytearray(open("$TMP/damaged.fnbli","rb").read())
for i in range(20, 60): d[i] ^= 0xFF
open("$TMP/damaged.fnbli","wb").write(bytes(d))
EOF
"$MT" -f --pnm "$TMP/damaged.fnbli" -o "$TMP/damaged.ppm" >/dev/null 2>&1
if [ $? -ne 0 ]; then ok "damaged fNBLI stream rejected"; else bad "damaged fNBLI stream" "decoder accepted garbage"; fi
if [ ! -f "$TMP/damaged.ppm" ]; then ok "no output file written for damaged stream"; else bad "damaged stream" "output written anyway"; fi

echo
echo "=================================================================================================="
echo "  $np passed, $nf failed, $ns skipped"
echo "=================================================================================================="
[ $nf = 0 ]
