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
    for mode in "-M N" "-M N -g"; do
        "$MT" -f -x $mode "$f" -o "$TMP/$b.nbli" >/dev/null 2>&1 || { bad "$b $mode encode" "exit $?"; continue; }
        "$MT" -f --pnm "$TMP/$b.nbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b $mode decode" "exit $?"; continue; }
        if imgcmp "$f" "$TMP/$b.out.pnm"; then ok "$b $mode"; else bad "$b $mode" "pixels differ"; fi
    done
    if [ -z "$big" ]; then
        for mode in "-M N -a" "-M N -g -a"; do
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
        "$MT" -f -x -M N -$near "$f" -o "$TMP/$b.nbli" >/dev/null 2>&1 || { bad "$b -M N -$near encode" "exit $?"; continue; }
        "$MT" -f --pnm "$TMP/$b.nbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b -M N -$near decode" "exit $?"; continue; }
        if imgcmp "$f" "$TMP/$b.out.pnm" $tol; then ok "$b -M N -$near (|err|<=$tol)"; else bad "$b -M N -$near" "error > $tol"; fi
    done
done

#===================================================================================================
echo
echo "4) NBLI tiled : mtnbli -M N -T 4 -> mtnbli decode"
for f in $IMGS; do
    b=$(basename $f)
    case "$b" in rgb_512x512*) continue;; esac
    "$MT" -f -x -M N -T 4 "$f" -o "$TMP/$b.tnbli" >/dev/null 2>&1 || { bad "$b -M N -T 4 encode" "exit $?"; continue; }
    "$MT" -f --pnm "$TMP/$b.tnbli" -o "$TMP/$b.out.pnm" >/dev/null 2>&1 || { bad "$b -M N -T 4 decode" "exit $?"; continue; }
    if imgcmp "$f" "$TMP/$b.out.pnm"; then ok "$b -M N -T 4"; else bad "$b -M N -T 4" "pixels differ"; fi
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
            for mode in "-M N" "-M N -g"; do
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
        "$MT" -f -x -M N -T 3 "$f" -o "$TMP/$b.tnbli" >/dev/null 2>&1
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

#===================================================================================================
echo
echo "7) wildcard arguments (\"dir\\*.png\" -- cmd.exe does not expand them for us)"
mkdir -p "$TMP/wild/sub"
for i in 1 2 3; do cp "$TMP/rgb_63x63.ppm" "$TMP/wild/a$i.ppm"; done
cp "$TMP/rgb_63x63.ppm" "$TMP/wild/sub/b1.ppm"

out=$("$MT" -f -v "$TMP/wild/*.ppm" -o "$TMP/wild/x.fnbli" 2>&1 | grep "^summary:")
n=$(echo "$out" | sed -n 's/^summary: \([0-9]*\) compressed.*/\1/p')
if [ "$n" = "3" ]; then ok "'dir/*.ppm' expands to 3 files"; else bad "'dir/*.ppm'" "summary says '${out}'"; fi

out=$("$MT" -f -v "$TMP/wild/*.bmp" 2>&1 | grep "^summary:")
if echo "$out" | grep -q "1 failed"; then ok "a pattern that matches nothing stays one failed input"
else bad "empty pattern" "summary says '${out}'"; fi

out=$("$MT" -f -v "$TMP/wild/*/*.ppm" 2>&1 | grep "^summary:")
n=$(echo "$out" | sed -n 's/^summary: \([0-9]*\) compressed.*/\1/p')
if [ "$n" = "1" ]; then ok "'dir/*/*.ppm' (wildcard in the middle) expands to 1 file"
else bad "two level pattern" "summary says '${out}'"; fi

out=$("$MT" -f -v "$TMP/wild/a?.ppm" 2>&1 | grep "^summary:")
n=$(echo "$out" | sed -n 's/^summary: \([0-9]*\) compressed.*/\1/p')
if [ "$n" = "3" ]; then ok "'a?.ppm' (single character wildcard) expands to 3 files"
else bad "'?' pattern" "summary says '${out}'"; fi

if [ -f "$TMP/wild/a1.fnbli" ] && [ -f "$TMP/wild/a2.fnbli" ] && [ -f "$TMP/wild/a3.fnbli" ]
then ok "each expanded file got its own output name"
else bad "expanded outputs" "missing a1/a2/a3.fnbli"; fi

#===================================================================================================
echo
echo "8) live progress and the summary block"
out=$("$MT" -f -v "$TMP/wild/a1.ppm" "$TMP/wild/a2.ppm" "$TMP/wild/a3.ppm" -o "$TMP/wild/y.fnbli" 2>&1)
if echo "$out" | grep -q "^compressed   : 3 files"; then ok "summary block reports the totals"
else bad "summary block" "no 'compressed   : 3 files' in: $(echo "$out" | tail -3)"; fi
if echo "$out" | grep -q "average .* BPP   best"; then ok "summary block reports average / best / worst BPP"
else bad "summary BPP" "no average/best/worst line"; fi
if echo "$out" | grep -qE "^\(1/3\) .* -> "; then ok "results are printed with (i/n) as they finish"
else bad "per file lines" "no '(1/3)' result line"; fi

# without -v the program stays quiet until the summary (no status line, no per file line)
out=$("$MT" -f "$TMP/wild/a1.ppm" -o "$TMP/wild/q.fnbli" 2>&1)
if [ -z "$out" ]; then ok "without -v : no output at all for a single file"
else bad "quiet mode" "unexpected output: $out"; fi

# ... but a failure is always reported, even without -v
out=$("$MT" "$TMP/wild/a1.ppm" -o "$TMP/wild/q.fnbli" 2>&1)          # output exists, no -f
if echo "$out" | grep -q "FAILED : output exists"; then ok "a failure is reported even without -v"
else bad "quiet failure" "the failed file was not reported: $out"; fi

# a status line must appear in a log too (every few seconds, not on every tick)
python3 "$here/genimg.py" "$TMP/wild/log1.ppm" 1600 1200 0 >/dev/null
cp "$TMP/wild/log1.ppm" "$TMP/wild/log2.ppm"; cp "$TMP/wild/log1.ppm" "$TMP/wild/log3.ppm"
out=$("$MT" -f -v -t 1 "$TMP/wild/log1.ppm" "$TMP/wild/log2.ppm" "$TMP/wild/log3.ppm" 2>&1 | grep -c "^\[00:")
if [ "$out" -ge 1 ]; then ok "status line in a redirected log ($out line(s))"
else bad "status line" "no '[mm:ss] i/n' line in the log"; fi

#===================================================================================================
echo
echo "9) -M <N|F|MT> picks the output format"
f="$TMP/rgb_300x200.ppm"; b=$(basename $f .ppm)        # -M replaces the suffix : rgb_300x200.nbli

rm -f "$TMP/$b.nbli" "$TMP/$b.fnbli" "$TMP/$b.tnbli"
"$MT" -f -M N  "$f" >/dev/null 2>&1
"$MT" -f -M F  "$f" >/dev/null 2>&1
"$MT" -f -M MT "$f" >/dev/null 2>&1
if   [ -f "$TMP/$b.nbli" ];  then ok "-M N  writes a .nbli";  else bad "-M N"  "no $b.nbli";  fi
if   [ -f "$TMP/$b.fnbli" ]; then ok "-M F  writes a .fnbli"; else bad "-M F"  "no $b.fnbli"; fi
if   [ -f "$TMP/$b.tnbli" ]; then ok "-M MT writes a .tnbli"; else bad "-M MT" "no $b.tnbli"; fi

# all three must decode back to the original pixels
for m in N F MT; do
    case $m in N) s=nbli;; F) s=fnbli;; MT) s=tnbli;; esac
    "$MT" -f --pnm "$TMP/$b.$s" -o "$TMP/$b.$s.pnm" >/dev/null 2>&1
    if imgcmp "$f" "$TMP/$b.$s.pnm"; then ok "-M $m round trip is lossless"
    else bad "-M $m round trip" "pixels differ"; fi
done

# -M MT really is multi-threaded : K independent strips, K = 2 per thread by default
nt=$(python3 "$here/tnbli_split.py" "$TMP/$b.tnbli" -o "$TMP/tilesz" 2>/dev/null | sed -n 's/.* \([0-9]*\) tiles.*/\1/p')
if [ -n "$nt" ] && [ "$nt" -ge 2 ]
then ok "-M MT without -T uses the automatic tile count"
else bad "-M MT tiles" "not split into several strips"; fi

# -M MT -T 4 : four strips
"$MT" -f -M MT -T 4 "$f" -o "$TMP/mt4.tnbli" >/dev/null 2>&1
nt=$(python3 "$here/tnbli_split.py" "$TMP/mt4.tnbli" -o "$TMP/tiles4" 2>/dev/null | sed -n 's/.* \([0-9]*\) tiles.*/\1/p')
if [ "$nt" = "4" ]
then ok "-M MT -T 4 makes 4 strips"
else bad "-M MT -T 4" "not 4 strips"; fi

# -M MT with an NBLI option : the strips must be NBLI
"$MT" -f -M MT -g "$f" -o "$TMP/mtn.tnbli" >/dev/null 2>&1
"$MT" -f --pnm "$TMP/mtn.tnbli" -o "$TMP/mtn.pnm" >/dev/null 2>&1
if imgcmp "$f" "$TMP/mtn.pnm"; then ok "-M MT -g : tiled container with NBLI strips"
else bad "-M MT -g" "pixels differ"; fi

# the -N of the previous versions is gone, and it says so
if "$MT" -N "$f" >/dev/null 2>&1; then bad "-N is gone" "it was accepted"
else ok "-N is rejected with a message pointing at -M"; fi

# an NBLI only option contradicts -M F
if "$MT" -M F -a "$f" >/dev/null 2>&1; then bad "-M F -a" "it was accepted"
else ok "-M F with -a is rejected"; fi
if "$MT" -M Q "$f" >/dev/null 2>&1; then bad "-M Q" "it was accepted"
else ok "-M Q is rejected"; fi

# the value may be attached : -MT, -M=MT
rm -f "$TMP/att.tnbli"
"$MT" -f -MT "$f" -o "$TMP/att.tnbli" >/dev/null 2>&1
if [ -f "$TMP/att.tnbli" ]; then ok "-MT (value attached to the switch) works"
else bad "-MT" "no output"; fi

#===================================================================================================
echo
echo "10) -d (decode to PNG) and -pc <0..9> (PNG compression level)"

rm -f "$TMP/d_*.png"
for s in fnbli nbli tnbli; do
    "$MT" -f -d "$TMP/$b.$s" -o "$TMP/d_$s.png" >/dev/null 2>&1
    if imgcmp "$f" "$TMP/d_$s.png"; then ok "-d $s -> PNG, pixels identical"
    else bad "-d $s" "no PNG, or pixels differ"; fi
done

# -d wins over --pnm
rm -f "$TMP/d_pnm.png" "$TMP/d_pnm.ppm"
"$MT" -f -d --pnm "$TMP/$b.fnbli" -o "$TMP/d_pnm.png" >/dev/null 2>&1
if [ -f "$TMP/d_pnm.png" ]; then ok "-d overrides --pnm"
else bad "-d vs --pnm" "no PNG was written"; fi

# -d and -M are mutually exclusive
if "$MT" -d -M N "$TMP/$b.fnbli" >/dev/null 2>&1; then bad "-d -M N" "it was accepted"
else ok "-d together with -M is rejected"; fi

# -pc : every level must produce a readable PNG with the right pixels, and the levels must differ
prev=0; prev_ok=1
for L in 0 1 3 6 9; do
    rm -f "$TMP/pc$L.png"
    "$MT" -f -d -pc $L "$TMP/$b.fnbli" -o "$TMP/pc$L.png" >/dev/null 2>&1
    if ! imgcmp "$f" "$TMP/pc$L.png"; then bad "-pc $L" "pixels differ"; prev_ok=0; continue; fi
    sz=$(stat -c %s "$TMP/pc$L.png")
    if [ $L -gt 0 ] && [ $sz -gt $prev ]; then bad "-pc $L" "bigger ($sz) than -pc of the level below ($prev)"; prev_ok=0; fi
    prev=$sz
done
if [ $prev_ok = 1 ]; then ok "-pc 0..9 : every level decodes to the original pixels, size never grows"; fi

# the default must be level 6
rm -f "$TMP/pcdef.png"
"$MT" -f -d "$TMP/$b.fnbli" -o "$TMP/pcdef.png" >/dev/null 2>&1
if [ "$(stat -c %s "$TMP/pcdef.png")" = "$(stat -c %s "$TMP/pc6.png")" ]
then ok "-pc is 6 by default"
else bad "default -pc" "differs from -pc 6"; fi

# a level that is not a single digit is rejected
if "$MT" -pc 10 "$TMP/$b.fnbli" >/dev/null 2>&1; then bad "-pc 10" "it was accepted"
else ok "-pc 10 is rejected"; fi
if "$MT" -pc x "$TMP/$b.fnbli" >/dev/null 2>&1; then bad "-pc x" "it was accepted"
else ok "-pc x is rejected"; fi

#===================================================================================================
echo
echo "11) deflate : every level 0..9 must inflate back to the original data"
if make -C "$here/.." tests/defl_test >"$TMP/defl_build.txt" 2>&1 && command -v python3 >/dev/null 2>&1; then
    if "$here/defl_test" "$TMP/defl" >"$TMP/defl.txt" 2>&1 && python3 "$here/defl_check.py" "$TMP/defl" >>"$TMP/defl.txt" 2>&1
    then ok "$(tail -1 "$TMP/defl.txt")"
    else bad "deflate levels" "$(tail -2 "$TMP/defl.txt" | tr '\n' ' ')"; fi
else
    skip "deflate levels" "tests/defl_test could not be built"
fi

echo
echo "=================================================================================================="
echo "  $np passed, $nf failed, $ns skipped"
echo "=================================================================================================="
[ $nf = 0 ]
