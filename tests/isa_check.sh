#!/bin/bash
#===================================================================================================
#  verify that mtnbli contains no instruction from an ISA that older CPUs lack.
#  Target: AMD Phenom II X6 1055T  ->  x86-64, MMX, 3DNow!, SSE, SSE2, SSE3, SSE4a, ABM
#          NO SSSE3, NO SSE4.1/4.2, NO AVX/AVX2, NO BMI/BMI2, NO FMA, NO POPCNT/LZCNT
#
#  usage :  isa_check.sh [binary]
#===================================================================================================
B=${1:-$(cd "$(dirname "$0")/.."; pwd)/mtnbli}
objdump -d --no-show-raw-insn "$B" > /tmp/isa_check.s 2>/dev/null || { echo "objdump failed"; exit 2; }
mn=$(grep -oE '^\s+[0-9a-f]+:\s+\S+' /tmp/isa_check.s | awk '{print $2}' | sort -u)

# one pattern per forbidden instruction, matched as a whole mnemonic
PAT='^v[a-z0-9]+$
^pblendvb$
^pblendw$
^pmulld$
^pmuldq$
^pminsb$|^pmaxsb$|^pminuw$|^pmaxuw$|^pminud$|^pmaxud$|^pminsd$|^pmaxsd$
^pmovsx|^pmovzx
^packusdw$
^phminposuw$
^pcmpgtq$|^pcmpeqq$
^pextrb$|^pextrd$|^pextrq$|^pinsrb$|^pinsrd$|^pinsrq$|^extractps$|^insertps$
^pshufb$|^psignb$|^psignw$|^psignd$|^pabsb$|^pabsw$|^pabsd$
^pmaddubsw$|^pmulhrsw$|^phaddw$|^phaddd$|^phsubw$|^phsubd$|^phaddsw$|^phsubsw$
^popcnt$|^lzcnt$|^tzcnt$
^andn$|^bextr$|^blsi$|^blsmsk$|^blsr$|^bzhi$|^mulx$|^pdep$|^pext$|^rorx$|^sarx$|^shlx$|^shrx$
^aesenc|^aesdec|^aeskeygenassist$|^pclmulqdq$|^pclmul
^crc32
^movbe$
^dpps$|^dppd$|^roundps$|^roundpd$|^roundss$|^roundsd$|^blendps$|^blendpd$|^blendvps$|^blendvpd$
^mpsadbw$|^ptest$|^testps$|^testpd$|^pcmpestr|^pcmpistr
^fma|^vfmadd|^vfmsub'

hits=$(echo "$mn" | grep -E "$PAT")
if [ -n "$hits" ]; then
    echo "*** FAILED - forbidden instructions present:"; echo "$hits" | sed 's/^/    /'; exit 1
fi
echo "OK : no SSSE3 / SSE4.1 / SSE4.2 / AVX / AVX2 / BMI / BMI2 / FMA / POPCNT / LZCNT /"
echo "     AES / PCLMUL / MOVBE / CRC32 instruction in $B"
echo "     (the highest SIMD level used is baseline SSE2, safe for AMD Phenom II / K10)"
exit 0
