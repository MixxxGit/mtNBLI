#!/usr/bin/env python3
#===================================================================================================
#  Cross check mtnbli against the upstream (single threaded, unmodified) NBLI / fNBLI binaries
#  on a directory of already existing streams.
#
#  Every stream is decoded twice -- once by mtnbli, once by the upstream tool that produced the
#  format -- and the two images are compared bit-exactly.  That is the strongest possible check :
#  it does not need to know where the stream came from.
#
#  usage :  corpus_check.py <stream_dir> <mtnbli> <upstream_fNBLI> <upstream_NBLI> [--near N]
#===================================================================================================
import sys, os, subprocess, tempfile, glob

sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
from imglib import read_image


def cmpimg (a_path, b_path, tol = 0):
    ma, fa, a = read_image (a_path)
    mb, fb, b = read_image (b_path)
    if fa[:2] != fb[:2]:                       # P5 (gray) vs P6 (rgb)
        return 'magic %s vs %s' % (fa, fb)
    if len (a) != len (b):
        return 'size %d vs %d' % (len (a), len (b))
    if a == b:
        return None
    d = max (abs (a[i] - b[i]) for i in range (len (a)))
    if d <= tol:
        return None
    return 'maxdiff %d (tol %d)' % (d, tol)


def main ():
    if len (sys.argv) < 5:
        raise SystemExit (__doc__)
    sdir, mt = sys.argv[1], sys.argv[2]
    ref_f, ref_n = sys.argv[3], sys.argv[4]
    tol = 0
    if '--near' in sys.argv:
        tol = 2 * int (sys.argv[sys.argv.index ('--near') + 1]) + 1

    tmp = tempfile.mkdtemp (prefix = 'corpus_check.')
    npass = nfail = nskip = 0

    files = sorted (glob.glob (os.path.join (sdir, '*.fnbli')) +
                    glob.glob (os.path.join (sdir, '*.nbli')))
    for src in files:
        name = os.path.basename (src)
        tool = ref_f if name.endswith ('.fnbli') else ref_n
        a = os.path.join (tmp, 'mine.pnm')
        b = os.path.join (tmp, 'up.pnm')
        for p in (a, b):
            if os.path.exists (p): os.remove (p)

        r1 = subprocess.run ([mt, '-f', '--pnm', src, '-o', a],
                             stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL)
        r2 = subprocess.run ([tool, '-f', src, '-o', b],
                             stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL)

        if not os.path.exists (b):
            print ('  skip  %-24s (upstream cannot decode it either)' % name); nskip += 1; continue
        if r1.returncode != 0 or not os.path.exists (a):
            print ('  FAIL  %-24s (mtnbli rejected a stream the upstream tool accepts)' % name)
            nfail += 1; continue
        e = cmpimg (a, b, tol)
        if e: print ('  FAIL  %-24s %s' % (name, e)); nfail += 1
        else: print ('  ok    %-24s identical to upstream' % name); npass += 1

    print ('\n%d passed, %d failed, %d skipped' % (npass, nfail, nskip))
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit (main ())
