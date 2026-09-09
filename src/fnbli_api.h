//==================================================================================================
//  fNBLI top level API (scalar implementation)  --  see fnbli_scalar.h
//==================================================================================================
#ifndef __FNBLI_API_H__
#define __FNBLI_API_H__

#include <cstdint>
#include <cstddef>
#include "fnbli_scalar.h"
#include "CRC32.h"

//--------------------------------------------------------------------------------------------------
//  compress
//  return: NULL = failed, otherwise a new[] uint8_t buffer of *comp_size bytes (caller deletes)
//  force_large : when true the 16-lane wave-front coder is used whenever the image is big
//                enough (the original fNBLI only did that when the CPU had AVX2).  The stream
//                format is identical, only the flag in the header differs.
//--------------------------------------------------------------------------------------------------
inline static uint8_t *fnbliCompress (size_t &comp_size, uint8_t *p_img, bool is_rgb,
                                      uint32_t height, uint32_t width, uint32_t &crc32,
                                      bool force_1lane = false) {
    if (fnbliSizeInvalid(height, width))
        return NULL;

    // is_large : use the 16-lane wave-front coder for the body of the image.  The original
    //            fNBLI only did that when the CPU had AVX2, but the stream layout is identical
    //            and our scalar implementation handles it as well.
    bool is_large = fnbliSizeLarge(height, width) && !force_1lane;

    size_t img_size = (size_t)height * width * (is_rgb?3:1);
    size_t n_pairs  = img_size;                       // 1 (gray) or 3 (RGB) symbols per pixel
    // worst case: stack (n_pairs words) + rANS output (<= n_pairs words) + histograms + header
    size_t buf_size = 2*n_pairs + (1u<<21);

    uint16_t *p_buf_base = new uint16_t [buf_size];
    uint16_t *p_buf      = p_buf_base;
    uint16_t *p_buf_end  = p_buf_base + buf_size;

    if (crc32) crc32 = calculateCRC32(p_img, img_size);

    p_buf = fnbliRWHeader<true>(p_buf, height, width, is_rgb, is_large, crc32);

    FnbliPxCorrector<FNBLI_MAX_UV> pcU(FNBLI_N_QD*256), pcV(FNBLI_N_QD*256*2);
    FnbliPxCorrector<FNBLI_MAX_Y>  pcY(FNBLI_N_QD*256*4);

    FnbliRANSe encoder (p_buf, p_buf_end);

    if (!is_large) {
        if (is_rgb) fnbliCodec1 <true , true, FnbliRANSe> (encoder, pcY, pcU, pcV, p_img, height, width);
        else        fnbliCodec1 <false, true, FnbliRANSe> (encoder, pcY, pcU, pcV, p_img, height, width);
    } else {
        uint32_t height_banner, height_body;
        FNBLI_DIVIDE_BANNER(height, height_banner, height_body);

        uint8_t *p_img_body = p_img + ((size_t)height_banner * width * (is_rgb?3:1));

        if (is_rgb) {
            fnbliCodec1  <true , true, FnbliRANSe> (encoder, pcY, pcU, pcV, p_img,      height_banner, width);
            fnbliCodec16 <true , true, FnbliRANSe> (encoder, pcY, pcU, pcV, p_img_body, height_body  , width);
        } else {
            fnbliCodec1  <false, true, FnbliRANSe> (encoder, pcY, pcU, pcV, p_img,      height_banner, width);
            fnbliCodec16 <false, true, FnbliRANSe> (encoder, pcY, pcU, pcV, p_img_body, height_body  , width);
        }
    }

    p_buf = encoder.encode_all();

    comp_size = 2 * (size_t)(p_buf - p_buf_base);

    return (uint8_t*) p_buf_base;
}


//--------------------------------------------------------------------------------------------------
//  decompress
//  return: NULL = failed, otherwise a new[] uint8_t pixel buffer (caller deletes)
//  Handles both is_large=0 (1-lane) and is_large=1 (16-lane wave-front) streams in pure scalar
//  code, i.e. it works on CPUs without AVX2 -- which the original fNBLI.exe refuses to do.
//--------------------------------------------------------------------------------------------------
inline static uint8_t *fnbliDecompress (uint8_t *p_buf, bool &is_rgb, uint32_t &height,
                                        uint32_t &width, uint32_t &crc32,
                                        uint8_t *p_out = NULL, size_t src_len = 0) {
    if (1 & (size_t)p_buf)          // buffer must be 2-byte aligned
        return NULL;

    // src_len, when known, is used as a read limit : a damaged stream then makes the decoder
    // report an error instead of reading (or writing) outside of the buffer.
    uint16_t *p_lim = src_len ? ((uint16_t*) p_buf) + (src_len >> 1) : NULL;

    uint16_t *p_u16 = (uint16_t*) p_buf;

    bool is_large;

    p_u16 = fnbliRWHeader<false>(p_u16, height, width, is_rgb, is_large, crc32);

    if (p_u16 == NULL)
        return NULL;

    if (fnbliSizeInvalid(height, width))
        return NULL;

    size_t img_size = (size_t)height * width * (is_rgb?3:1);
    uint8_t *p_img  = p_out ? p_out : new uint8_t [img_size];

    FnbliPxCorrector<FNBLI_MAX_UV> pcU(FNBLI_N_QD*256), pcV(FNBLI_N_QD*256*2);
    FnbliPxCorrector<FNBLI_MAX_Y>  pcY(FNBLI_N_QD*256*4);

    FnbliRANSd decoder (p_u16, p_lim);

    if (decoder.fail) {
        if (!p_out) delete[] p_img;
        return NULL;
    }

    if (!is_large) {
        if (is_rgb) fnbliCodec1 <true , false, FnbliRANSd> (decoder, pcY, pcU, pcV, p_img, height, width);
        else        fnbliCodec1 <false, false, FnbliRANSd> (decoder, pcY, pcU, pcV, p_img, height, width);
    } else {
        uint32_t height_banner, height_body;
        FNBLI_DIVIDE_BANNER(height, height_banner, height_body);

        uint8_t *p_img_body = p_img + ((size_t)height_banner * width * (is_rgb?3:1));

        if (is_rgb) {
            fnbliCodec1  <true , false, FnbliRANSd> (decoder, pcY, pcU, pcV, p_img,      height_banner, width);
            fnbliCodec16 <true , false, FnbliRANSd> (decoder, pcY, pcU, pcV, p_img_body, height_body  , width);
        } else {
            fnbliCodec1  <false, false, FnbliRANSd> (decoder, pcY, pcU, pcV, p_img,      height_banner, width);
            fnbliCodec16 <false, false, FnbliRANSd> (decoder, pcY, pcU, pcV, p_img_body, height_body  , width);
        }
    }

    if (decoder.fail) {                 // truncated or damaged stream
        if (!p_out) delete[] p_img;
        return NULL;
    }

    if (crc32) {
        if (crc32 != calculateCRC32(p_img, img_size)) {
            if (!p_out) delete[] p_img;
            return NULL;
        }
    }

    return p_img;
}

#endif // __FNBLI_API_H__
