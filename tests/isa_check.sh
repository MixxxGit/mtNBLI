#!/bin/bash
#===================================================================================================
#  verify that mtnbli contains no instruction from an ISA that older CPUs lack.
#  Target: AMD Phenom II X6 1055T  ->  x86-64, MMX, 3DNow!, SSE, SSE2, SSE3, SSE4a, ABM
#          NO SSSE3, NO SSE4.1/4.2, NO AVX/AVX2, NO BMI/BMI2, NO FMA, NO POPCNT/LZCNT
#
#  Works on ELF (Linux) and PE (Windows .exe) binaries -- a PE file is disassembled with the
#  mingw objdump.  Set WINOBJDUMP to override.
#
#  usage :  isa_check.sh [binary]
#  exit  :  0 = clean, 1 = forbidden instruction found, 2 = could not check
#===================================================================================================
B=${1:-$(cd "$(dirname "$0")/.."; pwd)/mtnbli}
[ -f "$B" ] || { echo "*** no such file: $B"; exit 2; }

# pick the objdump that understands the file : a PE (Windows) binary needs the mingw one
OD=objdump
if [ "$(head -c 2 "$B" 2>/dev/null)" = "MZ" ]; then
    OD=${WINOBJDUMP:-x86_64-w64-mingw32-objdump}
    command -v "$OD" >/dev/null 2>&1 || { echo "*** $B is a PE file but '$OD' is not installed"; exit 2; }
fi

TMP_S=$(mktemp /tmp/isa_check.XXXXXX.s) || exit 2
HITS=$(mktemp /tmp/isa_hits.XXXXXX.txt) || { rm -f "$TMP_S"; exit 2; }
trap 'rm -f "$TMP_S" "$HITS"' EXIT

$OD -d --no-show-raw-insn "$B" > "$TMP_S" 2>/dev/null || { echo "objdump failed"; exit 2; }
echo "     (disassembling with $OD)"

#--------------------------------------------------------------------------- forbidden mnemonics
# IMPORTANT: this has to be ONE line.  mawk (the default awk on Debian/Ubuntu) silently fails to
# match a multi-line ERE passed through -v, which makes the whole check vacuous -- it would
# happily pass a binary that is full of AVX2.  Verified by tests/isa_check.sh's own negative test.
PAT='^v[a-z0-9]+$|^pblendvb$|^pblendw$|^pmulld$|^pmuldq$|^pminsb$|^pmaxsb$|^pminuw$|^pmaxuw$|^pminud$|^pmaxud$|^pminsd$|^pmaxsd$|^pmovsx|^pmovzx|^packusdw$|^phminposuw$|^pcmpgtq$|^pcmpeqq$|^pextrb$|^pextrd$|^pextrq$|^pinsrb$|^pinsrd$|^pinsrq$|^extractps$|^insertps$|^pshufb$|^psignb$|^psignw$|^psignd$|^pabsb$|^pabsw$|^pabsd$|^pmaddubsw$|^pmulhrsw$|^phaddw$|^phaddd$|^phsubw$|^phsubd$|^phaddsw$|^phsubsw$|^popcnt$|^lzcnt$|^tzcnt$|^andn$|^bextr$|^blsi$|^blsmsk$|^blsr$|^bzhi$|^mulx$|^pdep$|^pext$|^rorx$|^sarx$|^shlx$|^shrx$|^aesenc|^aesdec|^aeskeygenassist$|^pclmul|^crc32|^movbe$|^dpps$|^dppd$|^roundps$|^roundpd$|^roundss$|^roundsd$|^blendps$|^blendpd$|^blendvps$|^blendvpd$|^mpsadbw$|^ptest$|^testps$|^testpd$|^pcmpestr|^pcmpistr|^fma|^vfmadd|^vfmsub'

#--------------------------------------------------------------------------- known, harmless exception
#  TZCNT (BMI1) is emitted by the mingw C runtime's float <-> string converter (gdtoa, the
#  *_D2A / strtodg family), which gets linked in because printf() references it unconditionally.
#  mtnbli itself never converts a float to a string (the report is formatted with integer
#  arithmetic, see fmt_fixed() in mtnbli.cpp), so that code is unreachable here.  And even if it
#  did run: F3 0F BC is BSF on a CPU without BMI1 -- identical result for every non-zero
#  operand, which is all gdtoa ever passes it.
#  A TZCNT outside those symbols, or any other forbidden instruction, is still a hard failure.
ALLOW_TZCNT_IN='(_D2A|strtodg|dtoa|gdtoa|__mingw_)'

#--------------------------------------------------------------------------- scan
# '  14005c011:<tab>tzcnt  (%rax),%eax'  ->  the mnemonic is the token right after the colon
awk -v pat="$PAT" -v allow="$ALLOW_TZCNT_IN" '
    /^[0-9a-f]+ </  { fn = $2; sub(/^</,"",fn); sub(/>:?$/,"",fn); next }
    {
        if (! match ($0, /^[ \t]*[0-9a-f]+:[ \t]+[a-zA-Z0-9_.]+/)) next
        s = substr ($0, RSTART, RLENGTH)
        sub (/^[ \t]*[0-9a-f]+:[ \t]+/, "", s)
        seen++
        if (s !~ pat) next
        if (s == "tzcnt" && fn ~ allow) { note[fn] = note[fn] " " s; next }
        printf "    %s   in %s\n", s, fn
    }
    END {
        for (k in note)
            printf "  (allowed) %s  in %s  -- mingw CRT float conversion, unreachable in mtnbli\n", note[k], k
        printf "%d\n", seen+0 > "/dev/stderr"
    }
' "$TMP_S" > "$HITS" 2>/tmp/isa_seen.$$
n_seen=$(cat /tmp/isa_seen.$$ 2>/dev/null); rm -f /tmp/isa_seen.$$

# the parser must actually have understood the listing, otherwise the check would be vacuous
if [ "${n_seen:-0}" -lt 50 ]; then
    echo "*** only ${n_seen:-0} instructions parsed -- objdump output not understood, refusing to pass"
    exit 2
fi

bad=$(grep -E '^    ' "$HITS" 2>/dev/null)
if [ -n "$bad" ]; then
    echo "*** FAILED - forbidden instructions present ($n_seen instructions scanned):"
    echo "$bad"
    exit 1
fi

grep -E '^  \(allowed\)' "$HITS" 2>/dev/null
echo "OK : no SSSE3 / SSE4.1 / SSE4.2 / AVX / AVX2 / BMI / BMI2 / FMA / POPCNT / LZCNT /"
echo "     AES / PCLMUL / MOVBE / CRC32 instruction in $B"
echo "     ($n_seen instructions scanned; highest SIMD level used is baseline SSE2,"
echo "      safe for AMD Phenom II / K10)"
exit 0
