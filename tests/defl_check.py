#!/usr/bin/env python3
"""Check the output of tests/defl_test with the zlib module.

    python3 tests/defl_check.py <prefix>

For every  <prefix>_<case>_L<level>.zz  it inflates the stream and compares it with
<prefix>_<case>.bin.  It also reports the sizes so that the levels can be told apart.
"""
import os
import sys
import zlib

prefix = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mt_defl"

bad = 0
for c in range(8):
    raw_name = "%s_%d.bin" % (prefix, c)
    with open(raw_name, "rb") as f:
        raw = f.read()
    out = []
    for lvl in range(10):
        zname = "%s_%d_L%d.zz" % (prefix, c, lvl)
        with open(zname, "rb") as f:
            blob = f.read()
        try:
            got = zlib.decompress(blob)
        except Exception as e:                      # noqa: BLE001
            print("FAIL %s : %s" % (os.path.basename(zname), e))
            bad += 1
            continue
        if got != raw:
            print("FAIL %s : %d bytes decompressed, expected %d"
                  % (os.path.basename(zname), len(got), len(raw)))
            bad += 1
        out.append(len(blob))
    if len(out) == 10:
        # case 0 is pseudo random : for incompressible data a stored block is *smaller* than a
        # huffman one (this is true for zlib as well), so only the overhead is checked there.
        if c == 0:
            if out[0] > len(raw) * 1.01 or out[9] > len(raw) * 1.01:
                print("FAIL case 0 : overhead too big (stored %d, level 9 %d, raw %d)"
                      % (out[0], out[9], len(raw)))
                bad += 1
        elif c <= 4:
            if out[9] > out[1]:
                print("FAIL case %d : level 9 (%d) is bigger than level 1 (%d)" % (c, out[9], out[1]))
                bad += 1
            if out[0] <= out[9]:
                print("FAIL case %d : level 0 (%d) is not bigger than level 9 (%d)"
                      % (c, out[0], out[9]))
                bad += 1

print("deflate : %s" % ("all streams decode to the original data" if bad == 0 else "%d FAILURES" % bad))
sys.exit(1 if bad else 0)
