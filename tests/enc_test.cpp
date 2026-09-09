// round-trip test of the scalar fNBLI encoder :  encode with our scalar code,
// decode with the reference (AVX2) fNBLI binary and compare.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/FileIO.h"
#include "../src/imageio/imageio.h"
#include "../src/fnbli_api.h"

int main (int argc, char **argv) {
    // mode 0 : encode (image -> .fnbli)     argv: enc <in.pnm/.png> <out.fnbli> [0|1 = force 1 lane]
    // mode 1 : decode (.fnbli -> raw)       argv: dec <in.fnbli> <out.raw>
    if (argc < 3) { printf("usage: enc_test enc|dec <in> <out> [flag]\n"); return 1; }

    if (!strcmp(argv[1], "enc")) {
        int is_rgb = 0; uint32_t h=0, w=0;
        uint8_t *img = loadPNMImageFile(argv[2], &is_rgb, &h, &w);
        if (!img) img = loadPNGImageFile(argv[2], &is_rgb, &h, &w);
        if (!img) { printf("cannot load %s\n", argv[2]); return 1; }
        size_t cs; uint32_t crc = 1;
        bool one_lane = (argc > 4) && (atoi(argv[4]) != 0);
        uint8_t *out = fnbliCompress(cs, img, (bool)is_rgb, h, w, crc, one_lane);
        free(img);
        if (!out) { printf("compress failed\n"); return 1; }
        printf("encoded %ux%u %s -> %zu bytes (crc %08x, %s)\n", w, h, is_rgb?"RGB":"GRAY", cs, crc, one_lane?"1-lane":"16-lane");
        return writeBytesToFile(argv[3], out, cs);
    } else {
        size_t len;
        uint8_t *in = loadBytesFromFile(argv[2], len);
        if (!in) { printf("cannot open %s\n", argv[2]); return 1; }
        bool is_rgb=false; uint32_t h=0,w=0,crc=0;
        uint8_t *img = fnbliDecompress(in, is_rgb, h, w, crc);
        delete[] in;
        if (!img) { printf("decode failed\n"); return 1; }
        printf("%ux%u %s\n", w, h, is_rgb?"RGB":"GRAY");
        size_t sz = (size_t)h*w*(is_rgb?3:1);
        int r = writeBytesToFile(argv[3], img, sz);
        delete[] img;
        return r;
    }
}
