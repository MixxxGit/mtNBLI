#!/usr/bin/env python3
#===================================================================================================
#  fuzz the decoder with damaged streams
#
#    fuzz.py <file.nbli|.fnbli|.tnbli> [n_runs] [mtnbli]
#
#  Random bytes of the stream are flipped and the decoder is run on the result.  A correct
#  decoder must either reject the file (CRC / sanity check) or, when the file carries no CRC,
#  produce garbage -- but it must NEVER crash.
#===================================================================================================
import sys, os, random, subprocess, collections

def fuzz (path, n=200, mtnbli='./mtnbli'):
    orig    = bytearray (open (path, 'rb').read())
    crashes = collections.Counter()
    accepted = rejected = 0
    for s in range(n):
        rnd = random.Random(s)
        d = bytearray (orig)
        for k in range (rnd.randrange (1, 40)):
            d[rnd.randrange (len(d))] ^= 0xFF
        open ('/tmp/mtnbli_fuzz.bin', 'wb').write (bytes(d))
        r = subprocess.run ([mtnbli, '-f', '--pnm', '/tmp/mtnbli_fuzz.bin', '-o', '/tmp/mtnbli_fuzz.pnm'],
                            stdout = subprocess.DEVNULL, stderr = subprocess.DEVNULL)
        if   r.returncode <  0: crashes[-r.returncode] += 1
        elif r.returncode == 0: accepted += 1
        else:                   rejected += 1
    print ('%-24s %4d runs : %4d rejected, %3d accepted, crashes: %s'
           % (os.path.basename(path), n, rejected, accepted, dict(crashes) or 'NONE'))
    return len(crashes)

def main():
    path  = sys.argv[1]
    n     = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    mt    = sys.argv[3] if len(sys.argv) > 3 else os.path.join (os.path.dirname(os.path.abspath(__file__)), '..', 'mtnbli')
    sys.exit (1 if fuzz(path, n, mt) else 0)

main()
