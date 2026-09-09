#!/usr/bin/env python3
#===================================================================================================
#  generate test images (raw PPM / PGM, no dependencies beyond the stdlib)
#
#    genimg.py <out.ppm|out.pgm> <width> <height> <pattern>
#    pattern : 0 = smooth photo-like   1 = flat + text-like noise   2 = pure random (incompressible)
#===================================================================================================
import sys, math, random

def smooth (w, h, rgb):
    rnd = random.Random(12345)
    px = bytearray()
    for y in range(h):
        for x in range(w):
            u = x / max(1, w-1); v = y / max(1, h-1)
            r = int(127 + 120*math.sin(9*u) * math.cos(7*v))
            g = int(127 + 120*math.sin(5*v + 1.3) * math.cos(3*u))
            b = int(127 + 120*(0.5 + 0.5*math.sin(13*(u+v))))
            # a few hard edges / circles so the predictor has something to do
            if ((x-3*w//8)**2 + (y-h//3)**2) < (min(w,h)//7)**2:
                r, g, b = 240, 30, 30
            if ((x-5*w//7)**2 + (y-2*h//3)**2) < (min(w,h)//11)**2:
                r, g, b = 20, 220, 90
            n = rnd.randint(-3, 3)
            r = max(0, min(255, r+n)); g = max(0, min(255, g+n)); b = max(0, min(255, b+n))
            if rgb: px += bytes((r, g, b))
            else:   px.append ((r*77 + g*150 + b*29) >> 8)
    return bytes(px)

def noisy (w, h, rgb):
    rnd = random.Random(999)
    px = bytearray()
    for y in range(h):
        for x in range(w):
            base = 200 if ((x >> 4) + (y >> 4)) & 1 else 40
            v = base + rnd.randint(-24, 24)
            v = max(0, min(255, v))
            if rgb: px += bytes((v, (v*3) & 255, 255-v))
            else:   px.append (v)
    return bytes(px)

def rndimg (w, h, rgb):
    rnd = random.Random(7)
    n = w*h*(3 if rgb else 1)
    return bytes(rnd.randrange(256) for _ in range(n))

def main():
    path, w, h, pat = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    rgb = path.lower().endswith('.ppm')
    data = (smooth, noisy, rndimg)[pat] (w, h, rgb)
    with open(path, 'wb') as f:
        f.write (('P%d\n%d %d\n255\n' % (6 if rgb else 5, w, h)).encode())
        f.write (data)
    print ('%-28s %dx%d %s  %d bytes' % (path, w, h, 'RGB' if rgb else 'gray', len(data)))

main()
