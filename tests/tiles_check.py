#!/usr/bin/env python3
#===================================================================================================
#  verify that every tile of a .tnbli container is a standalone .fnbli / .nbli stream
#
#    tiles_check.py <original image> <file.tnbli> <decoder> [decoder args...]
#
#  Each tile is extracted with tnbli_split.py, handed to <decoder> (for example the *original*
#  single threaded fNBLI / NBLI binary of the upstream project) and the reconstructed strip is
#  compared against the corresponding rows of the original image.
#===================================================================================================
import sys, os, subprocess, struct, tempfile, shutil
sys.path.insert (0, os.path.dirname(os.path.abspath(__file__)))
from imglib import read_image, rows_crop

def main():
    img, tnb, dec = sys.argv[1], sys.argv[2], sys.argv[3]
    extra = sys.argv[4:]
    d = open (tnb, 'rb').read()
    assert d[:8] == b'MTNBLI\x01\x00', 'not a .tnbli file'
    w, h, n, crc, codec, flags = struct.unpack ('<IIIIHH', d[8:28])
    offs = struct.unpack ('<%dQ' % (n+1), d[64:64+8*(n+1)])
    suf = '.fnbli' if codec == 0 else '.nbli'
    tmp = tempfile.mkdtemp (prefix='tiles_check.')
    ma, fa, a = read_image (img)
    base, rest, r0, bad = h // n, h % n, 0, 0
    try:
        for i in range(n):
            th = base + (1 if i < rest else 0)
            tile = os.path.join (tmp, 'tile%03d%s' % (i, suf))
            out  = os.path.join (tmp, 'tile%03d.png' % i)
            open (tile, 'wb').write (d[offs[i]:offs[i+1]])
            r = subprocess.call ([dec, '-f', tile, '-o', out] + extra,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if r != 0:
                print ('  tile %d : decoder failed (%d)' % (i, r)); bad += 1; continue
            mb, fb, b = read_image (out)
            sa = rows_crop (ma, fa, a, r0, r0+th)
            sb = rows_crop (mb, fb, b, 0, th)
            if len(sa) != len(sb) or sa != sb:
                nd = sum (1 for x, y in zip(sa, sb) if x != y)
                print ('  tile %d : %d / %d pixels differ' % (i, nd, len(sa))); bad += 1
            else:
                print ('  tile %d : rows %d..%d ok (%d bytes)' % (i, r0, r0+th-1, len(sa)))
            r0 += th
    finally:
        shutil.rmtree (tmp, ignore_errors=True)
    print ('%s : %d tile(s), %d bad' % (os.path.basename(tnb), n, bad))
    sys.exit (1 if bad else 0)

main()
