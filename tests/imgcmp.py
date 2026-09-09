#!/usr/bin/env python3
#===================================================================================================
#  compare two images (raw PNM or PNG), ignoring the file header
#     usage :  imgcmp.py <fileA> <fileB> [max_abs_diff] [row0:row1]
#              row0:row1  -> compare only rows [row0,row1) of fileA against rows [0,row1-row0) of B
#     exit  :  0 = identical (or within max_abs_diff), 1 = size, 2 = data, 3 = header
#===================================================================================================
import sys, os
sys.path.insert (0, os.path.dirname(os.path.abspath(__file__)))
from imglib import read_image, rows_crop

def main():
    if len(sys.argv) < 3:
        raise SystemExit (__doc__)
    maxdiff = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    rows = None
    if len(sys.argv) > 4 and ':' in sys.argv[4]:
        rows = tuple (int(v) for v in sys.argv[4].split(':'))
    ma, fa, a = read_image (sys.argv[1])
    mb, fb, b = read_image (sys.argv[2])
    if rows:
        a = rows_crop (ma, fa, a, rows[0], rows[1])
        b = rows_crop (mb, fb, b, 0, rows[1]-rows[0])
    if fa[:3] != fb[:3] and rows is None:
        print ('HEADER DIFFERENT', fa[:3], fb[:3]); sys.exit (3)
    if len(a) != len(b):
        print ('SIZE DIFFERENT', len(a), len(b)); sys.exit (1)
    if maxdiff == 0:
        diff = [i for i in range(len(a)) if a[i] != b[i]]
    else:
        diff = [i for i in range(len(a)) if abs(a[i]-b[i]) > maxdiff]
    if diff:
        print ('ndiff %d / %d  first: %s' % (len(diff), len(a), diff[:5])); sys.exit (2)
    print ('identical (%d bytes, %s %s)' % (len(a), ma, b' '.join(fa[:3]).decode()))
    sys.exit (0)

main()
