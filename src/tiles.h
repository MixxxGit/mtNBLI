//==================================================================================================
//  .tnbli  :  a *tiled* container for NBLI / fNBLI streams
//
//  A legacy .nbli / .fnbli file is ONE adaptive arithmetic/ANS stream.  Such a stream is a
//  strictly sequential object: symbol N can only be decoded when the complete coder state
//  (rANS state + all adaptive context-model states) produced by symbols 0..N-1 is known.
//  No amount of threads can change that, so a single legacy file can only use one core.
//
//  The .tnbli container simply stores K *independent* NBLI/fNBLI streams, one per horizontal
//  strip of the image.  Each strip is a completely ordinary .nbli/.fnbli bit-stream (with its
//  own header and its own CRC), so:
//       * every strip can be decoded by any NBLI/fNBLI decoder, and
//       * the strips can be decoded concurrently  ->  a single image scales with the core count.
//
//  Layout (all integers little endian) :
//      TnbliHeader           64 bytes
//      uint64 offset[n+1]    8*(n+1) bytes, offsets are 8-byte aligned
//      tile 0 blob ...  (each blob is a self contained .nbli / .fnbli stream)
//      tile n-1 blob
//==================================================================================================
#ifndef __TILES_H__
#define __TILES_H__

#include <cstdint>
#include <cstring>
#include <cstdio>

#define  TNBLI_MAGIC       "MTNBLI\x01\x00"
#define  TNBLI_HEADER_SIZE 64
#define  TNBLI_CODEC_FNBLI 0
#define  TNBLI_CODEC_NBLI  1
#define  TNBLI_FLAG_RGB     0x1
#define  TNBLI_FLAG_AVP     0x2
#define  TNBLI_FLAG_GOLOMB  0x4

struct TnbliHeader {
    char     magic   [8];     // "MTNBLI\1\0"
    uint32_t width;
    uint32_t height;
    uint32_t n_tiles;
    uint32_t crc32;           // CRC32 of the whole reconstructed image (0 = not stored)
    uint16_t codec;           // TNBLI_CODEC_FNBLI / TNBLI_CODEC_NBLI
    uint16_t flags;           // TNBLI_FLAG_*
    uint16_t near_;           // NBLI distortion level 0..7
    uint16_t reserved;
    uint64_t reserved2[4];
};                            // = 64 bytes

inline static bool tnbliIsTiled (const uint8_t *p_buf, size_t len) {
    return (len >= TNBLI_HEADER_SIZE) && (memcmp(p_buf, TNBLI_MAGIC, 8) == 0);
}

// height of tile i  (tiles are horizontal strips; the last one may be shorter)
inline static uint32_t tnbliTileHeight (uint32_t height, uint32_t n_tiles, uint32_t i) {
    if (n_tiles == 0) return 0;
    uint32_t base = height / n_tiles;
    uint32_t rest = height % n_tiles;
    return base + ((i < rest) ? 1u : 0u);
}

inline static uint32_t tnbliTileRow (uint32_t height, uint32_t n_tiles, uint32_t i) {
    if (n_tiles == 0) return 0;
    uint32_t base = height / n_tiles;
    uint32_t rest = height % n_tiles;
    return base*i + ((i < rest) ? i : rest);
}

// how many tiles should be used for an image of this size / this many threads
inline static uint32_t tnbliAutoTiles (uint32_t height, uint32_t n_thread) {
    uint32_t t = n_thread ? n_thread : 1;
    t *= 2;                                            // 2 tiles per thread -> better load balance
    uint32_t max_t = height / 96;                      // keep strips tall enough to stay efficient
    if (max_t < 1) max_t = 1;
    if (t > max_t) t = max_t;
    if (t < 1) t = 1;
    return t;
}

#endif // __TILES_H__
