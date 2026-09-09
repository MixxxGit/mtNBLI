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

# how many CPUs can this process REALLY use?  nproc lies inside a container with a CFS quota,
# and a table that saturates at the quota says nothing about the codec.
n_cpu=$($MT 2>&1 | sed -n 's/.*default: all \([0-9]*\) hardware.*/\1/p')
n_cpu=${n_cpu:-$(nproc)}
quota=""
for f in /sys/fs/cgroup/cpu.max /sys/fs/cgroup/cpu/cpu.cfs_quota_us; do [ -r "$f" ] && quota=$(cat "$f"); done
eff=""
if [ -n "$quota" ]; then
    set -- $quota
    q=$1; p=${2:-100000}
    if [ "$q" != "-1" ] && [ "$q" != "max" ] && [ "$p" -gt 0 ]; then
        eff=$(python3 -c "print('%.2f'%($q/$p))")
    fi
fi
echo "cpus  : $n_cpu visible${eff:+, but the cgroup quota is $eff CPU -> speed-ups above that are pointless}"
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
