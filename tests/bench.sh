#!/usr/bin/env bash
#===================================================================================================
#  measure how mtnbli scales with the number of threads
#
#  usage :  bench.sh <image.ppm|pgm> [tile_count] [ "1 2 4 8 16 32" ]
#===================================================================================================
set -u
here=$(cd "$(dirname "$0")"; pwd)
MT="$here/../mtnbli"
IMG=${1:-}
TILES=${2:-16}
THREADS=${3:-"1 2 4 8 16 32"}

if [ -z "$IMG" ] || [ ! -f "$IMG" ]; then
    echo "usage: bench.sh <image.ppm|pgm> [tile_count] [thread list]"; exit 1
fi
if [ ! -x "$MT" ]; then echo "*** $MT not built"; exit 1; fi

TMP=${TMPDIR:-/tmp}/mtnbli_bench.$$
rm -rf "$TMP"; mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

W=$(head -c 200 "$IMG" | tr -s ' \n' ' ' | cut -d' ' -f2)
H=$(head -c 200 "$IMG" | tr -s ' \n' ' ' | cut -d' ' -f3)
BPP=$(stat -c%s "$IMG")
echo "image : $IMG   ${W}x${H}   $((BPP/1000000)) MB"
echo "tiles : $TILES"
echo

printf "%-8s %12s %8s %14s %12s %8s\n" "threads" "compress" "speedup" "decompress" "speedup" "bytes"
printf -- "-------------------------------------------------------------------------------\n"

base_c=""; base_d=""

for t in $THREADS; do
    rm -f "$TMP/x.tnbli" "$TMP/x.ppm"
    c=$( { TIMEFORMAT=%R; time "$MT" -f -T "$TILES" -t $t --pnm "$IMG" -o "$TMP/x.tnbli" >/dev/null 2>&1; } 2>&1 )
    d=$( { TIMEFORMAT=%R; time "$MT" -f       -t $t --pnm "$TMP/x.tnbli" -o "$TMP/x.ppm"  >/dev/null 2>&1; } 2>&1 )
    [ -z "$base_c" ] && base_c=$c
    [ -z "$base_d" ] && base_d=$d
    sc=$(python3 -c "print('%.2fx'%($base_c/$c))")
    sd=$(python3 -c "print('%.2fx'%($base_d/$d))")
    printf "%-8s %12s %8s %14s %12s %8s\n" "$t" "${c}s" "$sc" "${d}s" "$sd" "$(stat -c%s "$TMP/x.tnbli")"
done
printf -- "-------------------------------------------------------------------------------\n"
