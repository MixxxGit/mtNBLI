#!/usr/bin/env bash
#===================================================================================================
#  verify the Windows cross build (mtnbli.exe) by running it under wine and comparing its output
#  with the native Linux build, byte for byte.
#
#  Skipped (exit 0) when mtnbli.exe or wine is missing -- this is only possible on a machine
#  with the mingw toolchain and wine installed.
#
#  usage :  tests/win_check.sh [stream_dir]
#===================================================================================================
set -u
here=$(cd "$(dirname "$0")"; pwd)
MT="$here/../mtnbli"
WX="$here/../mtnbli.exe"
DIR=${1:-$here/../../corpus}

if [ ! -x "$WX" ]; then echo "skip : $WX not built (make win64)"; exit 0; fi
if ! command -v wine >/dev/null 2>&1; then echo "skip : wine not installed"; exit 0; fi
if [ ! -d "$DIR" ]; then echo "skip : no stream directory $DIR"; exit 0; fi
if [ ! -x "$MT" ]; then echo "*** $MT not built"; exit 1; fi

export WINEDEBUG=-all
TMP=$(mktemp -d /tmp/mtnbli_wincheck.XXXXXX) || exit 1
trap 'rm -rf "$TMP"' EXIT

np=0; nf=0; ns=0

for f in "$DIR"/*.fnbli "$DIR"/*.nbli; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    rm -f "$TMP/lin.pnm" "$TMP/win.pnm"
    "$MT" -f --pnm "$f" -o "$TMP/lin.pnm" >/dev/null 2>&1
    if [ ! -s "$TMP/lin.pnm" ]; then echo "  skip  $b (native build cannot decode it)"; ns=$((ns+1)); continue; fi
    wine "$WX" -f --pnm "$f" -o "$TMP/win.pnm" >/dev/null 2>&1
    if [ ! -s "$TMP/win.pnm" ]; then echo "  FAIL  $b : the Windows build could not decode it"; nf=$((nf+1)); continue; fi
    if cmp -s "$TMP/lin.pnm" "$TMP/win.pnm"; then echo "  ok    $b"; np=$((np+1));
    else echo "  FAIL  $b : output differs from the native build"; nf=$((nf+1)); fi
done

# damaged streams must be rejected by the Windows SEH guard, not crash
python3 - "$WX" "$TMP" <<'PY'
import sys, random, subprocess, os
exe, tmp = sys.argv[1], sys.argv[2]
orig = bytearray (open ('%s/RGB.nbli' % os.environ.get('WCHECK_DIR','/workspace/corpus'), 'rb').read())
crash = rej = acc = 0
for s in range (40):
    rnd = random.Random (s); d = bytearray (orig)
    for k in range (rnd.randrange (1, 40)): d [rnd.randrange (len (d))] ^= 0xFF
    p = os.path.join (tmp, 'fz.bin'); open (p, 'wb').write (bytes (d))
    try:
        r = subprocess.run (['wine', exe, '-f', '--pnm', p, '-o', os.path.join(tmp,'fz.pnm')],
                            capture_output = True, timeout = 300)
        if r.returncode < 0: crash += 1
        elif r.returncode == 0: acc += 1
        else: rej += 1
    except subprocess.TimeoutExpired: crash += 1
print ("  %s 40 damaged streams : %d rejected, %d accepted, %d crashed"
       % ("ok   " if crash == 0 else "FAIL ", rej, acc, crash))
sys.exit (1 if crash else 0)
PY
rc=$?
[ $rc -eq 0 ] && np=$((np+1)) || nf=$((nf+1))

#--------------------------------------------------------------------------- wildcards and Unicode
#  cmd.exe does not expand  dir\*.png , and the Windows CRT hands argv to us in the ANSI code
#  page, so both need their own code path -- and both are Windows only, so they can only be
#  checked here.
WTMP="$TMP/wild"; mkdir -p "$WTMP/суб"
python3 "$here/genimg.py" "$WTMP/a1.ppm" 40 30 0
cp "$WTMP/a1.ppm" "$WTMP/a2.ppm"; cp "$WTMP/a1.ppm" "$WTMP/a3.ppm"
cp "$WTMP/a1.ppm" "$WTMP/суб/b1.ppm"
W="Z:$(printf '%s' "$WTMP" | tr '/' '\\')"          # /tmp/mtnbli_wincheck.x -> Z:\tmp\...

wrun () { wine "$WX" -f -v "$1" -o "$TMP/wildout.fnbli" 2>&1 | grep "^summary:"; }
wcount () { printf '%s' "$1" | sed -n 's/^summary: \([0-9]*\) compressed.*/\1/p'; }

s=$(wrun "$W\*.ppm")
if [ "$(wcount "$s")" = "3" ]; then np=$((np+1)); echo "  ok    windows wildcard 'dir\\*.ppm' -> 3 files"
else nf=$((nf+1)); echo "  FAIL  windows wildcard 'dir\\*.ppm' : $s"; fi

s=$(wrun "$W\a?.ppm")
if [ "$(wcount "$s")" = "3" ]; then np=$((np+1)); echo "  ok    windows wildcard 'dir\\a?.ppm' -> 3 files"
else nf=$((nf+1)); echo "  FAIL  windows wildcard 'dir\\a?.ppm' : $s"; fi

s=$(wrun "$W\*\*.ppm")
if [ "$(wcount "$s")" = "1" ]; then np=$((np+1)); echo "  ok    windows wildcard 'dir\\*\\*.ppm' -> 1 file"
else nf=$((nf+1)); echo "  FAIL  windows wildcard 'dir\\*\\*.ppm' : $s"; fi

s=$(wrun "$W\суб\*.ppm")
if [ "$(wcount "$s")" = "1" ]; then np=$((np+1)); echo "  ok    non-ASCII directory (UTF-8 argv + _wfopen)"
else nf=$((nf+1)); echo "  FAIL  non-ASCII directory : $s"; fi

s=$(wrun "$W\*.bmp")
if printf '%s' "$s" | grep -q "1 failed"; then np=$((np+1)); echo "  ok    a pattern matching nothing is reported as one failed input"
else nf=$((nf+1)); echo "  FAIL  empty pattern : $s"; fi

echo
echo "  $np passed, $nf failed, $ns skipped"
[ $nf -eq 0 ] || exit 1
exit 0
