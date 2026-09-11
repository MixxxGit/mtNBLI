//==================================================================================================
//  defl_test : exercise src/imageio/deflate.c
//
//  1. builds a few buffers with very different statistics (random, text, long runs, a ramp)
//  2. compresses each of them at every level 0..9 with mt_deflate_*()
//  3. writes  <prefix>_<case>.bin  (the raw data)  and  <prefix>_<case>_L<n>.zz  (the zlib stream)
//
//  tests/defl_check.py then inflates every .zz with the zlib module and compares.
//  Usage :  defl_test <prefix>
//==================================================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "imageio/deflate.h"

typedef struct { FILE *fp; } Sink;

static void sink_fn (void *p_ctx, const void *p_data, size_t len) {
    FILE *fp = ((Sink*) p_ctx)->fp;
    fwrite (p_data, 1, len, fp);
}

static uint32_t rnd_state = 12345;
static unsigned rnd (void) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 8) & 0xFFFFFF;
}

int main (int argc, char **argv)
{
    const char *prefix = (argc > 1) ? argv[1] : "/tmp/mt_defl";
    size_t      sizes[8][10];
    int         c, l;
    char        name[512];

    //---------------------------------------------------------------- build the test buffers
    uint8_t  *buf[8];
    size_t    blen[8];
    size_t    i;
    size_t    N = 300000;

    buf[0] = (uint8_t*) malloc (N);                        // pseudo random : incompressible
    for (i=0; i<N; i++) buf[0][i] = (uint8_t) (rnd() >> 8);
    blen[0] = N;

    buf[1] = (uint8_t*) malloc (N);                        // long runs
    { size_t p = 0;
      while (p < N) { unsigned v = rnd() & 0xFF, r = 1 + (rnd() % 900);
                      while (r-- && p < N) buf[1][p++] = (uint8_t) v; } }
    blen[1] = N;

    buf[2] = (uint8_t*) malloc (N);                        // english-ish text
    { static const char *w[] = { "the ", "quick ", "brown ", "fox ", "jumps ", "over ",
                                 "lazy ", "dog ", "and ", "then ", "nothing ", "happens " };
      size_t p = 0;
      while (p < N) { const char *s = w[rnd() % 12];
                      while (*s && p < N) buf[2][p++] = (uint8_t) *s++; } }
    blen[2] = N;

    buf[3] = (uint8_t*) malloc (N);                        // a slow ramp : good for filters
    for (i=0; i<N; i++) buf[3][i] = (uint8_t) ((i / 97) & 0xFF);
    blen[3] = N;

    buf[4] = (uint8_t*) malloc (N);                        // one repeated 64 byte block
    for (i=0; i<N; i++) buf[4][i] = (uint8_t) ((i % 64) * 4);
    blen[4] = N;

    buf[5] = (uint8_t*) malloc (1);                        // the tiniest possible input
    buf[5][0] = 0xAB;
    blen[5] = 1;

    // a repetition just *outside* the 32768 byte window : must not be matched
    buf[6] = (uint8_t*) malloc (2 * 33000);
    for (i=0; i<33000; i++) buf[6][i] = (uint8_t) (rnd() >> 8);
    memcpy (buf[6] + 33000, buf[6], 33000);
    blen[6] = 2 * 33000;

    // ... and one just inside it : must be matched
    buf[7] = (uint8_t*) malloc (2 * 32000);
    for (i=0; i<32000; i++) buf[7][i] = (uint8_t) (rnd() >> 8);
    memcpy (buf[7] + 32000, buf[7], 32000);
    blen[7] = 2 * 32000;

    //---------------------------------------------------------------- run
    for (c=0; c<8; c++) {
        snprintf (name, sizeof(name), "%s_%d.bin", prefix, c);
        FILE *fraw = fopen (name, "wb");
        if (!fraw) { printf ("cannot write %s\n", name); return 1; }
        fwrite (buf[c], 1, blen[c], fraw);
        fclose (fraw);

        for (l=0; l<=9; l++) {
            Sink        s;
            DeflateCtx *dc;
            snprintf (name, sizeof(name), "%s_%d_L%d.zz", prefix, c, l);
            s.fp = fopen (name, "wb");
            if (!s.fp) { printf ("cannot write %s\n", name); return 1; }
            dc = mt_deflate_new (l, sink_fn, &s);
            if (!dc) { printf ("mt_deflate_new failed\n"); return 1; }
            // feed it in odd sized pieces : the encoder must cope with any chunking
            { size_t p = 0;
              while (p < blen[c]) {
                  size_t k = 1 + ((p * 7 + 13) % 9973);
                  if (k > blen[c] - p) k = blen[c] - p;
                  mt_deflate_write (dc, buf[c] + p, k);
                  p += k;
              } }
            mt_deflate_end (dc);
            mt_deflate_free (dc);
            fclose (s.fp);
            sizes[c][l] = 0;
            { FILE *f = fopen (name, "rb");
              if (f) { fseek (f, 0, SEEK_END); sizes[c][l] = (size_t) ftell (f); fclose (f); } }
        }
        printf ("case %d : %lu raw -> ", c, (unsigned long) blen[c]);
        for (l=0; l<=9; l++) printf ("%lu ", (unsigned long) sizes[c][l]);
        printf ("\n");
    }

    for (c=0; c<8; c++) free (buf[c]);
    return 0;
}
