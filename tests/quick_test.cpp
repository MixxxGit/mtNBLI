// quick correctness check of the scalar codecs against the reference binaries
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../src/FileIO.h"
#include "../src/fnbli_api.h"
#include "../src/nbli/NBLI.h"

int main (int argc, char **argv) {
    if (argc < 3) { printf("usage: quick_test <in.nbli|.fnbli> <out.bin>\n"); return 1; }

    size_t len;
    uint8_t *in = loadBytesFromFile(argv[1], len);
    if (!in) { printf("cannot open %s\n", argv[1]); return 1; }

    bool is_rgb = false;
    uint32_t h = 0, w = 0, crc = 0;
    uint8_t *img = NULL;

    if (in[0]=='f' && in[1]=='n') {
        img = fnbliDecompress(in, is_rgb, h, w, crc);
        printf("fNBLI decode: ");
    } else if (in[0]=='n' && in[1]=='b') {
        bool use_golomb=false, use_avp=false;
        int16_t near=0;
        img = NBLIdecompress(in, is_rgb, h, w, use_golomb, use_avp, near, crc);
        printf("NBLI decode (golomb=%d avp=%d near=%d): ", (int)use_golomb, (int)use_avp, (int)near);
    } else {
        printf("unknown format\n"); return 1;
    }

    if (!img) { printf("FAILED\n"); return 1; }
    printf("%ux%u %s crc=%08x\n", w, h, is_rgb?"RGB":"GRAY", crc);

    if (writeBytesToFile(argv[2], img, (size_t)h*w*(is_rgb?3:1))) { printf("write failed\n"); return 1; }
    delete[] img;
    delete[] in;
    printf("OK -> %s\n", argv[2]);
    return 0;
}
