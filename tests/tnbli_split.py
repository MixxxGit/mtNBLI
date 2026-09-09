#!/usr/bin/env python3
#===================================================================================================
#  split a .tnbli (tiled) container into its tiles
#
#    tnbli_split.py <in.tnbli> [-o <outdir>]
#
#  Every tile of a .tnbli file is a completely ordinary, self contained .fnbli / .nbli stream,
#  so the tiles written by this script can be decoded by the original (single threaded) NBLI /
#  fNBLI tools as well:
#        fNBLI  tile_000.fnbli      ->  tile_000.png      (a horizontal strip of the image)
#===================================================================================================
import sys, os, struct

def main():
    src = sys.argv[1]
    out = '.'
    if len(sys.argv) > 3 and sys.argv[2] == '-o': out = sys.argv[3]
    d = open(src, 'rb').read()
    assert d[:8] == b'MTNBLI\x01\x00', 'not a .tnbli file'
    w, h, n, crc, codec, flags, near_, res = struct.unpack('<IIIIHHHH', d[8:32])
    offs = struct.unpack('<%dQ' % (n+1), d[64:64+8*(n+1)])
    print ('%s : %dx%d  %d tiles  codec=%s  flags=0x%x  near=%d  crc32=%08x' %
           (src, w, h, n, ('fNBLI' if codec==0 else 'NBLI' if codec==1 else '?'), flags, near_, crc))
    suf = '.fnbli' if codec == 0 else '.nbli'
    os.makedirs (out, exist_ok=True)
    base, rh, rr = h // n, h % n, 0
    for i in range(n):
        th = base + (1 if i < rr else 0)
        name = os.path.join (out, '%s_tile%03d%s' % (os.path.basename(src).rsplit('.',1)[0], i, suf))
        open(name, 'wb').write (d[offs[i]:offs[i+1]])
        print ('  rows %5d..%-5d (%4d rows) -> %-40s %8d bytes' % (rr, rr+th-1, th, name, offs[i+1]-offs[i]))
        rr += th

main()
