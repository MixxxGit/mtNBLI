#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ioutf8.h"
#include "deflate.h"



static void write_png_chunk (char *p_name, uint8_t *p_data, uint32_t len, FILE *fp) {
    const static uint32_t crc_table[] = {0, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c };
    uint32_t i, crc=0xFFFFFFFF;
    fputc(((len>>24) & 0xFF), fp);
    fputc(((len>>16) & 0xFF), fp);
    fputc(((len>> 8) & 0xFF), fp);
    fputc(((len    ) & 0xFF), fp);
    fwrite(p_name, sizeof(uint8_t),   4, fp);
    if (len > 0)
        fwrite(p_data, sizeof(uint8_t), len, fp);
    for (i=0; i<4; i++) {
        crc ^= p_name[i];
        crc = (crc >> 4) ^ crc_table[crc & 15];
        crc = (crc >> 4) ^ crc_table[crc & 15];
    }
    for (i=0; i<len; i++) {
        crc ^= p_data[i];
        crc = (crc >> 4) ^ crc_table[crc & 15];
        crc = (crc >> 4) ^ crc_table[crc & 15];
    }
    crc = ~crc;
    fputc(((crc>>24) & 0xFF), fp);
    fputc(((crc>>16) & 0xFF), fp);
    fputc(((crc>> 8) & 0xFF), fp);
    fputc(((crc    ) & 0xFF), fp);
}



//--------------------------------------------------------------------------------------------------
//  PNG row filters (http://www.libpng.org/pub/png/spec/1.2/PNG-Filters.html)
//--------------------------------------------------------------------------------------------------
static unsigned png_paeth (unsigned a, unsigned b, unsigned c) {
    int p  = (int) a + (int) b - (int) c;
    int pa = p - (int) a;  if (pa < 0) pa = -pa;
    int pb = p - (int) b;  if (pb < 0) pb = -pb;
    int pc = p - (int) c;  if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc)            return b;
    return c;
}

// The classic "smallest sum of absolute values" heuristic.  It is evaluated on a *sample* of the
// row only: scoring every byte of a 7680 pixel line five times over is what makes libpng slow on
// 8K frames, and sampling costs almost nothing in compression.
static int png_pick_filter (const uint8_t *p_cur, const uint8_t *p_prev, size_t row, size_t bpp,
                            int fmask, size_t step)
{
    unsigned long score[5] = { 0, 0, 0, 0, 0 };
    size_t        i;
    int           k, best = -1;
    unsigned long bs = 0;

    for (i=0; i<row; i+=step) {
        unsigned a = (i >= bpp)          ? p_cur [i - bpp] : 0;
        unsigned b = (p_prev)            ? p_prev[i]       : 0;
        unsigned c = (p_prev && i>=bpp)  ? p_prev[i - bpp] : 0;
        unsigned x = p_cur[i], f;
        #define  SCORE(k, v)  do { f = (unsigned)((v) & 0xFF); \
                                   score[k] += (f < 128) ? f : 256u - f; } while (0)
        SCORE (0, x);
        SCORE (1, x - a);
        SCORE (2, x - b);
        SCORE (3, x - ((a + b) >> 1));
        SCORE (4, x - png_paeth (a, b, c));
        #undef   SCORE
    }
    for (k=0; k<5; k++)
        if ((fmask & (1 << k)) && (best < 0 || score[k] < bs)) { bs = score[k]; best = k; }
    return (best < 0) ? 0 : best;
}

static void png_apply_filter (uint8_t *p_dst, const uint8_t *p_cur, const uint8_t *p_prev,
                              size_t row, size_t bpp, int f)
{
    size_t i;
    for (i=0; i<row; i++) {
        unsigned a = (i >= bpp)         ? p_cur [i - bpp] : 0;
        unsigned b = (p_prev)           ? p_prev[i]       : 0;
        unsigned c = (p_prev && i>=bpp) ? p_prev[i - bpp] : 0;
        unsigned x = p_cur[i];
        switch (f) {
            case 0:  p_dst[i] = (uint8_t) x;                       break;   // None
            case 1:  p_dst[i] = (uint8_t)(x - a);                  break;   // Sub
            case 2:  p_dst[i] = (uint8_t)(x - b);                  break;   // Up
            case 3:  p_dst[i] = (uint8_t)(x - ((a + b) >> 1));     break;   // Average
            default: p_dst[i] = (uint8_t)(x - png_paeth (a,b,c));  break;   // Paeth
        }
    }
}

// which filters a level is allowed to look at, and how densely the rows are sampled
static int   png_filter_mask (int level) {
    if (level <= 0) return 1;                                                     // None
    if (level <= 3) return (1<<0) | (1<<2);                                       // None, Up
    if (level <= 6) return (1<<0) | (1<<1) | (1<<2) | (1<<4);                     // + Sub, Paeth
    return 0x1F;                                                                  // all five
}
static size_t png_filter_step (size_t bpp, int level) {
    return bpp * ((level >= 7) ? 1u : 4u);
}

// the deflate output goes straight into IDAT chunks, one per call -- so the encoder never needs
// more than its own ~700 KB, whatever the size of the image
static void png_idat_sink (void *p_ctx, const void *p_data, size_t len) {
    FILE *fp = (FILE*) p_ctx;
    write_png_chunk ((char*) "IDAT", (uint8_t*) p_data, (uint32_t) len, fp);
}

//--------------------------------------------------------------------------------------------------
//  write a PNG.   level 0 = stored (the old behaviour), 1..9 = deflate with the matching effort
//  that zlib/libpng would use for that level.
//
//  return:   0 : success    1 : failed
//--------------------------------------------------------------------------------------------------
int writePNGImageFile (const char *p_filename, const uint8_t *p_buf, int is_rgb,
                       uint32_t height, uint32_t width, int level)
{
    size_t      bpp, row, st_cap, st_len = 0;
    uint8_t     ihdr[13];
    uint8_t    *p_frow = NULL, *p_stage = NULL;
    DeflateCtx *dc     = NULL;
    FILE       *fp     = NULL;
    uint32_t    y;
    int         fmask, rc = 1;

    if (width < 1 || height < 1)
        return 1;
    if (level < 0) level = 0;
    if (level > 9) level = 9;

    bpp    = (is_rgb ? 3 : 1);
    row    = bpp * (size_t) width;
    st_cap = row + 4096;

    p_frow  = (uint8_t*) malloc (row);
    p_stage = (uint8_t*) malloc (st_cap);
    if (p_frow == NULL || p_stage == NULL) goto done;

    fp = mt_fopen (p_filename, "wb");
    if (fp == NULL) goto done;

    dc = mt_deflate_new (level, png_idat_sink, fp);
    if (dc == NULL) goto done;

    fwrite ("\x89PNG\r\n\32\n", sizeof(char), 8, fp);          // 8-bit PNG magic

    ihdr[0] = (uint8_t)( width>>24);  ihdr[1] = (uint8_t)( width>>16);
    ihdr[2] = (uint8_t)( width>> 8);  ihdr[3] = (uint8_t)( width);
    ihdr[4] = (uint8_t)(height>>24);  ihdr[5] = (uint8_t)(height>>16);
    ihdr[6] = (uint8_t)(height>> 8);  ihdr[7] = (uint8_t)(height);
    ihdr[8] = 8;                                                // bit depth
    ihdr[9] = (uint8_t) (is_rgb ? 2 : 0);                       // colour type : truecolour / gray
    ihdr[10] = 0;  ihdr[11] = 0;  ihdr[12] = 0;                 // deflate, filter 0, no interlace
    write_png_chunk ((char*) "IHDR", ihdr, 13, fp);

    fmask = png_filter_mask (level);
    for (y=0; y<height; y++) {
        const uint8_t *p_cur  = p_buf + (size_t) y * row;
        const uint8_t *p_prev = (y == 0) ? NULL : (p_cur - row);
        int            f      = (fmask == 1) ? 0
                              : png_pick_filter (p_cur, p_prev, row, bpp, fmask,
                                                 png_filter_step (bpp, level));
        png_apply_filter (p_frow, p_cur, p_prev, row, bpp, f);

        if (st_len + 1 + row > st_cap) {                        // hand the rows on in pieces
            mt_deflate_write (dc, p_stage, st_len);
            st_len = 0;
        }
        p_stage[st_len++] = (uint8_t) f;
        memcpy (p_stage + st_len, p_frow, row);
        st_len += row;
    }
    if (st_len) mt_deflate_write (dc, p_stage, st_len);
    mt_deflate_end (dc);

    write_png_chunk ((char*) "IEND", ihdr, 0, fp);
    rc = 0;

done:
    if (dc)      mt_deflate_free (dc);
    if (fp)      fclose (fp);
    if (p_frow)  free (p_frow);
    if (p_stage) free (p_stage);
    return rc;
}



#include "uPNG/uPNG.h"


// return:  NULL     : failed
//          non-NULL : pointer to image pixels, allocated by malloc(), need to be free() later
uint8_t* loadPNGImageFile (const char *p_filename, int *p_is_rgb, uint32_t *p_height, uint32_t *p_width) {
    upng_t     *p_upng;
    upng_error  err;
    upng_format png_format;
    static const char *upng_format_names[] = {
        (const char*)"BADFORMAT",
        (const char*)"RGB8",
        (const char*)"RGB16",
        (const char*)"RGBA8",
        (const char*)"RGBA16",
        (const char*)"LUMA1",
        (const char*)"LUMA2",
        (const char*)"LUMA4",
        (const char*)"LUMA8",
        (const char*)"LUMA_ALPHA1",
        (const char*)"LUMA_ALPHA2",
        (const char*)"LUMA_ALPHA4",
        (const char*)"LUMA_ALPHA8"
    };
    size_t img_size;
    uint8_t *p_dst_base, *p_dst;
    const uint8_t *p_src;
    
    p_upng = upng_new_from_file(p_filename);
    
    if (p_upng == NULL)
        return NULL;
    
    err = upng_decode(p_upng);
    
    if (err != UPNG_EOK) {
        if (err==UPNG_EUNSUPPORTED || err==UPNG_EUNINTERLACED || err==UPNG_EUNFORMAT)
            printf("   ***ERROR: this PNG format is not-yet supported, error code = %d\n", err);
        upng_free(p_upng);
        return NULL;
    }
    
    png_format = upng_get_format(p_upng);
    
    if (png_format != UPNG_RGBA8 && png_format != UPNG_RGB8 && png_format != UPNG_LUMINANCE8) {
        printf("   ***ERROR: only support LUMA8, RGB8, and RGBA8. But this PNG is %s\n", upng_format_names[png_format]);
        upng_free(p_upng);
        return NULL;
    }
    
    *p_is_rgb = (png_format != UPNG_LUMINANCE8);
    *p_height = upng_get_height(p_upng);
    *p_width  = upng_get_width(p_upng);
    
    img_size = (size_t)((*p_is_rgb)?3:1) * (*p_height) * (*p_width);
    
    p_dst_base = p_dst = (uint8_t*)malloc(img_size);
    
    if (p_dst_base) {
        size_t i;
        p_src = upng_get_buffer(p_upng);
        if (png_format == UPNG_RGBA8) {
            printf("   *warning: disard alpha channel of this PNG\n");
            for (i=(size_t)(*p_height)*(*p_width); i>0; i--) {
                p_dst[0] = p_src[0];
                p_dst[1] = p_src[1];
                p_dst[2] = p_src[2];
                p_dst += 3;
                p_src += 4;
            }
        } else {
            for (i=img_size; i>0; i--) {
                *(p_dst++) = *(p_src++);
            }
        }
    }
    
    upng_free(p_upng);
    
    return p_dst_base;
}
