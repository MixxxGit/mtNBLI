//==================================================================================================
#include <cstdio>
//  fNBLI  -  scalar (non-AVX / non-SSE4) re-implementation of the fNBLI codec
//
//  This file is a from-scratch scalar port of WangXuan95/NBLI 's src/fNBLI .
//  It provides BOTH:
//     * the 1-lane (scalar) coding loop      -- used for small images and for the "banner"
//     * the 16-lane wavefront coding loop    -- bit-exact scalar emulation of the AVX2
//                                               (INT16x16_t) path, so that images which were
//                                               compressed with  is_large=1  can be decoded on
//                                               CPUs without AVX2 (e.g. AMD Phenom II).
//
//  Only plain ISO C++ / 32-64 bit integer arithmetic is used.  No intrinsics, no <immintrin.h>,
//  no floating point in the decoder.  It builds for any x86-64 (or any other) target.
//
//  Original code : https://github.com/WangXuan95/NBLI  (GPLv3)
//  This file     : GPLv3, see ../LICENSE
//==================================================================================================
#ifndef __FNBLI_SCALAR_H__
#define __FNBLI_SCALAR_H__

#include <cstdint>
#include <cstring>
#include <cstdlib>

//------------------------------------------------------------------------------ constants
#define  FNBLI_MAX_Y      255
#define  FNBLI_MID_Y      (FNBLI_MAX_Y >> 1)
#define  FNBLI_MAX_UV     511
#define  FNBLI_MID_UV     (FNBLI_MAX_UV >> 1)

#define  FNBLI_N_QD       12
#define  FNBLI_N_CTX      (FNBLI_N_QD * 2)      // rANS contexts (Y:0..11, U:12..23, V:12..23)

#define  FNBLI_NORM_BIT   14
#define  FNBLI_NORM_SUM   (1 << FNBLI_NORM_BIT)
#define  FNBLI_NORM_MASK  (FNBLI_NORM_SUM - 1)
#define  FNBLI_CODE_BIT   16
#define  FNBLI_CODE_LOW   (1 << FNBLI_CODE_BIT)
#define  FNBLI_CODE_HIGH  ((1 << (2*FNBLI_CODE_BIT-FNBLI_NORM_BIT)) - 1)

#define  FNBLI_N_LANE     16

//------------------------------------------------------------------------------ RGB <-> YUV
template <typename T>
inline static void fnbliRGB2YUV (T &Y, T &U, T &V) {
    U -= Y;
    V -= Y;
    Y += (U + V + 2) >> 2;
    V -= U >> 2;
    V += FNBLI_MAX_Y;
    U += FNBLI_MAX_Y;
}

template <typename T>
inline static void fnbliYUV2RGB (T &Y, T &U, T &V) {
    U -= FNBLI_MAX_Y;
    V -= FNBLI_MAX_Y;
    V += U >> 2;
    Y -= (U + V + 2) >> 2;
    V += Y;
    U += Y;
}

//------------------------------------------------------------------------------ header
// 7 x uint16 :  "fn" , "bC"/"bG" , height , width , crc_hi , crc_lo , is_large
#define  FNBLI_HEADER_WORDS   7

template <bool IS_WRITE>
inline static uint16_t *fnbliRWHeader (uint16_t *p_buf, uint32_t &height, uint32_t &width,
                                       bool &is_rgb, bool &is_large, uint32_t &crc32) {
    const static uint16_t HEADER1      = ('f' | ((uint16_t)'n'<<8));
    const static uint16_t HEADER2_gray = ('b' | ((uint16_t)'G'<<8));
    const static uint16_t HEADER2_RGB  = ('b' | ((uint16_t)'C'<<8));

    if (IS_WRITE) {
        *(p_buf++) = HEADER1;
        *(p_buf++) = is_rgb ? HEADER2_RGB : HEADER2_gray;
        *(p_buf++) = (uint16_t) height;
        *(p_buf++) = (uint16_t) width;
        *(p_buf++) = (uint16_t)(crc32>>16);
        *(p_buf++) = (uint16_t) crc32;
        *(p_buf++) = (uint16_t)(1 & is_large);
    } else {
        height = width = 0;
        if (*(p_buf++) != HEADER1)
            return NULL;
        switch (*(p_buf++)) {
            case HEADER2_gray : is_rgb = false;  break;
            case HEADER2_RGB  : is_rgb = true;   break;
            default           : return NULL;
        }
        height    = *(p_buf++);
        width     = *(p_buf++);
        crc32     = *(p_buf++);
        crc32   <<= 16;
        crc32    |= *(p_buf++);
        is_large  = 1 & *(p_buf++);
    }
    return p_buf;
}

inline static bool fnbliSizeInvalid (uint32_t height, uint32_t width) {
    return !(( 0 < height) && (height <= 32000) && (0 < width) && (width <= 32000));
}

inline static bool fnbliSizeLarge (uint32_t height, uint32_t width) {
    return ((18 <= height) && (32 <= width) && ((64*64) <= (uint64_t)height*width));
}

// note: the image is divided into 2 parts: banner and body.
//       height of banner = height & 15 (at least 2)   -> processed by the 1-lane loop
//       height of body   = height - banner (multiple of 16) -> processed by the 16-lane loop
#define  FNBLI_DIVIDE_BANNER(height, height_banner, height_body) { \
    (height_banner) = (height) & 0xF;                              \
    if ((height_banner) < 2) (height_banner) += 16;                \
    (height_body) = (height) - (height_banner);                    \
}

//==================================================================================================
//  histogram : 512 symbols, 14 bit normalized
//
//  NOTE on FNBLI_HIST_PAD : the last histogram code word of a *valid* stream may legally write
//  up to 3 entries behind hnrm[511] (a "code5" starting at i==510).  In the original code those
//  writes land in hsum[0..2], which is harmless because hsum is recomputed right afterwards.
//  We keep that behaviour, but with an explicit spill area so that no object is ever written
//  out of bounds -- a damaged stream can then be detected (i >= 512+FNBLI_HIST_PAD) instead of
//  silently corrupting memory.
//==================================================================================================
#define  FNBLI_HIST_PAD   8

class FnbliHist {
public:
    uint16_t hnrm [512 + FNBLI_HIST_PAD];   // normalized frequency (+ legal spill area, see above)
    uint16_t hsum [512];                    // prefix sum (cumulative)
    uint32_t cnt  [512];      // raw counter (encoder only)

    inline FnbliHist () {
        for (int i=0; i<512; i++) { cnt[i] = 0; hnrm[i] = 0; hsum[i] = 0; }
        for (int i=512; i<512+FNBLI_HIST_PAD; i++) hnrm[i] = 0;
    }

    inline void add (int i) { cnt[i] ++; }

    //-------------------------------------------------------------- encoder
    inline void normalize () {
        int nz_count = 0, nz_index = 0;
        uint32_t sum = 0;

        for (int i=0; i<512; i++) {
            if (cnt[i] > 0) {
                sum += cnt[i];
                nz_count ++;
                nz_index = i;
            }
        }

        if (nz_count <= 1) {
            cnt[ nz_index] = FNBLI_NORM_SUM - 1;
            cnt[!nz_index] = 1;
        } else {
            double scale = (double)FNBLI_NORM_SUM / sum;

            sum = 0;

            for (int i=0; i<512; i++) {
                if (cnt[i] > 0) {
                    cnt[i] = (uint32_t)(0.49 + scale * cnt[i]);
                    if (cnt[i] == 0) cnt[i] = 1;
                    sum += cnt[i];
                }
            }

            for (int i=0; sum > FNBLI_NORM_SUM; i = (i+1) & 511)
                if (cnt[i] > 1) { cnt[i] --; sum --; }

            for (int i=0; sum < FNBLI_NORM_SUM; i = (i+1) & 511)
                if (cnt[i] > 0) { cnt[i] ++; sum ++; }
        }

        for (int i=0; i<512; i++) hnrm[i] = (uint16_t) cnt[i];
    }

    inline void calcAccumlate () {
        // clamped to FNBLI_NORM_SUM : a damaged (unnormalized) histogram would otherwise make
        // calcDecodeLookupTable() write behind the lookup table.
        hsum[0] = 0;
        uint32_t sum = 0;
        for (int i=1; i<512; i++) {
            sum += hnrm[i-1];
            hsum[i] = (uint16_t) ((sum > (uint32_t)FNBLI_NORM_SUM) ? (uint32_t)FNBLI_NORM_SUM : sum);
        }
    }

    inline uint16_t *encode (uint16_t *p_buf) {
        int16_t hprev = 0;
        for (int i=1; i<512; ) {
            int16_t h0 = (int16_t) hnrm[i];
            int len;
            for (len=1; i+len < 512 && h0 == (int16_t)hnrm[i+len]; len++);
            if (len > 3 && h0 < 4) {
                *(p_buf++) = (uint16_t)((h0<<10) | (len-1) | 0xF000);
                i += len;
            } else {
                int16_t h1 = (i < 511) ? (int16_t)hnrm[i+1] : INT16_MAX;
                int16_t h2 = (i < 510) ? (int16_t)hnrm[i+2] : INT16_MAX;
                int16_t h3 = (i < 509) ? (int16_t)hnrm[i+3] : INT16_MAX;
                int16_t d0 = (int16_t)((hprev - h0) & FNBLI_NORM_MASK);
                int16_t d1 = (i < 511) ? (int16_t)((h0 - h1) & FNBLI_NORM_MASK) : INT16_MAX;

                if               (h0<8 && h1<8 && h2<8 && h3<8) {
                    *(p_buf++) = (uint16_t)((h0<<9) | (h1<<6) | (h2<<3) | h3 | 0xE000);
                    i += 4;
                } else if        (h0<32 && h1<16 && h2<16) {
                    *(p_buf++) = (uint16_t)((h0<<8) | (h1<<4) | h2 | 0xC000);
                    i += 3;
                } else if        (h0<128 && h1<128) {
                    *(p_buf++) = (uint16_t)((h0<<7) | h1 | 0x8000);
                    i += 2;
                } else if        (d0<128 && d1<128) {
                    *(p_buf++) = (uint16_t)((d0<<7) | d1 | 0x4000);
                    i += 2;
                } else {
                    *(p_buf++) = (uint16_t) h0;
                    i += 1;
                }
            }
            hprev = (int16_t) hnrm[i-1];
        }
        return p_buf;
    }

    //-------------------------------------------------------------- decoder
    // returns false when the histogram is malformed (damaged stream).  Every write is bounds
    // checked: a corrupted code word can make the run-length code overrun the 512 entry table,
    // which in the original implementation corrupts the neighbouring member and crashes later.
    inline bool decode (uint16_t *&p_buf, uint16_t *p_lim, bool &fail) {
        #define  FNBLI_HIST_PUT(v)  do { if (i < 512+FNBLI_HIST_PAD) hnrm[i] = (uint16_t)(v); else fail = true; i++; } while (0)
        for (int i=0; i<512+FNBLI_HIST_PAD; i++) hnrm[i] = 0;
        for (int i=1; i<512; ) {
            if (p_lim && p_buf >= p_lim) { fail = true; return false; }
            uint16_t code = *(p_buf++);
            switch (code >> 12) {
                case 0: case 1: case 2: case 3:
                    FNBLI_HIST_PUT(code);
                    break;
                case 4: case 5: case 6: case 7:
                    FNBLI_HIST_PUT((hnrm[i-1] - ((code>>7)&0x7F)) & FNBLI_NORM_MASK);
                    FNBLI_HIST_PUT((hnrm[i-1] - ( code     &0x7F)) & FNBLI_NORM_MASK);
                    break;
                case 8: case 9: case 10: case 11:
                    FNBLI_HIST_PUT((code>>7) & 0x7F);
                    FNBLI_HIST_PUT((code   ) & 0x7F);
                    break;
                case 12: case 13:
                    FNBLI_HIST_PUT((code>>8) & 0x1F);
                    FNBLI_HIST_PUT((code>>4) & 0x0F);
                    FNBLI_HIST_PUT((code   ) & 0x0F);
                    break;
                case 14:
                    FNBLI_HIST_PUT((code>>9) & 0x07);
                    FNBLI_HIST_PUT((code>>6) & 0x07);
                    FNBLI_HIST_PUT((code>>3) & 0x07);
                    FNBLI_HIST_PUT((code   ) & 0x07);
                    break;
                default: {
                    uint16_t hrep = (code>>10) & 0x0003;
                    int      len  = (code    ) & 0x01FF;
                    for (len++; len>0; len--) FNBLI_HIST_PUT(hrep);
                    break;
                }
            }
        }
        #undef FNBLI_HIST_PUT
        hnrm[0] = FNBLI_NORM_SUM;
        for (int i=1; i<512; i++) hnrm[0] -= hnrm[i];
        return !fail;
    }

    inline void calcDecodeLookupTable (uint16_t dlut [FNBLI_NORM_SUM]) {
        for (int i=0; i<511; i++)
            for (int j=hsum[i]; j<hsum[i+1]; j++)
                dlut[j] = (uint16_t) i;
        for (int j=hsum[511]; j<FNBLI_NORM_SUM; j++)
            dlut[j] = 511;
    }
};

//==================================================================================================
//  rANS : 16 interleaved states.  lane 0 is used by the 1-lane loop, lanes 0..15 by the
//         16-lane loop.  Exactly the same stream layout as the AVX2 implementation.
//==================================================================================================
class FnbliRANSe {
public:
    uint32_t   ans [FNBLI_N_LANE];
    uint16_t  *p_buf;
    FnbliHist *hists;                 // [FNBLI_N_CTX]

    // context/value pair stack, grows downwards from p_top
    uint16_t  *p_top;
    uint32_t   count1, count16;

    inline FnbliRANSe (uint16_t *_p_buf, uint16_t *_p_buf_end) {
        p_buf   = _p_buf;
        hists   = new FnbliHist [FNBLI_N_CTX];
        p_top   = _p_buf_end;
        count1  = 0;
        count16 = 0;
        for (int i=0; i<FNBLI_N_LANE; i++) ans[i] = FNBLI_CODE_LOW;
    }

    inline ~FnbliRANSe () { delete[] hists; }

    inline void push    (int16_t ctx, int16_t val) { count1 ++; p_top --; p_top[0] = (uint16_t)((ctx<<10) | val); }
    inline void push_x16(const int16_t *ctx, const int16_t *val) {
        count16 ++;
        p_top -= 16;
        for (int k=0; k<16; k++) p_top[k] = (uint16_t)((ctx[k]<<10) | val[k]);
    }

    // ---------------- 1 lane
    inline void codec (int16_t ctx, int16_t val) {
        hists[ctx].add(val);
        push(ctx, val);
    }

    // ---------------- 16 lanes
    inline void codec_x16 (const int16_t *ctx, const int16_t *val) {
        push_x16(ctx, val);
        for (int k=0; k<16; k++) hists[ctx[k]].add(val[k]);
    }

    inline void encode (int16_t ctx, int16_t val) {
        uint16_t hnrm = hists[ctx].hnrm[val], hsum = hists[ctx].hsum[val];
        uint32_t nans = ans[0] / hnrm;
        if (nans > FNBLI_CODE_HIGH) {
            *(p_buf++) = (uint16_t) ans[0];
            ans[0] >>= FNBLI_CODE_BIT;
            nans = ans[0] / hnrm;
        }
        ans[0] %= hnrm;
        ans[0] += (nans << FNBLI_NORM_BIT);
        ans[0] += hsum;
    }

    inline void encode_x16 (const int16_t *ctx, const int16_t *val) {
        uint32_t hnrm[16], hsum[16], nans[16];
        bool     flag [16];

        for (int k=0; k<16; k++) {
            hnrm[k] = hists[ctx[k]].hnrm[val[k]];
            hsum[k] = hists[ctx[k]].hsum[val[k]];
            nans[k] = ans[k] / hnrm[k];
            flag[k] = (nans[k] > FNBLI_CODE_HIGH);
        }
        for (int k=15; k>=0; k--)
            if (flag[k]) *(p_buf++) = (uint16_t) ans[k];
        for (int k=0; k<16; k++)
            if (flag[k]) { ans[k] >>= FNBLI_CODE_BIT; nans[k] = ans[k] / hnrm[k]; }
        for (int k=0; k<16; k++) {
            ans[k] %= hnrm[k];
            ans[k] += (nans[k] << FNBLI_NORM_BIT);
            ans[k] += hsum[k];
        }
    }

    inline uint16_t *encode_all () {
        for (int c=0; c<FNBLI_N_CTX; c++) {
            hists[c].normalize();
            hists[c].calcAccumlate();
            p_buf = hists[c].encode(p_buf);
        }

        uint16_t *p_buf_tmp = p_buf;

        while (count16) {
            int16_t ctx[16], val[16];
            count16 --;
            for (int k=0; k<16; k++) { ctx[k] = (int16_t)(0x003F & (p_top[k]>>10)); val[k] = (int16_t)(0x03FF & p_top[k]); }
            p_top += 16;
            encode_x16(ctx, val);
        }

        while (count1) {
            int16_t ctx, val;
            count1 --;
            ctx = (int16_t)(0x003F & (p_top[0]>>10)); val = (int16_t)(0x03FF & p_top[0]);
            p_top ++;
            encode(ctx, val);
        }

        for (int i=15; i>=0; i--) {
            *(p_buf++) = (uint16_t) ans[i];
            *(p_buf++) = (uint16_t)(ans[i] >> FNBLI_CODE_BIT);
        }

        // reverseStream
        for (uint16_t *pa = p_buf_tmp, *pb = p_buf-1; pa < pb; pa++, pb--) {
            uint16_t t = *pa; *pa = *pb; *pb = t;
        }

        return p_buf;
    }
};


class FnbliRANSd {
public:
    uint32_t   ans [FNBLI_N_LANE];
    uint16_t  *p_buf;
    uint16_t  *p_lim;                 // first word that must NOT be read (NULL = unchecked)
    bool       fail;                  // set when the stream is damaged / truncated
    FnbliHist *hists;                 // [FNBLI_N_CTX]
    uint16_t  *dlut;                  // [FNBLI_N_CTX][FNBLI_NORM_SUM]

    // read the next stream word, refuse to read outside the input buffer
    inline uint16_t rd () {
        if (p_lim && p_buf >= p_lim) { fail = true; return 0; }
        return *(p_buf++);
    }

    inline FnbliRANSd (uint16_t *_p_buf, uint16_t *_p_lim = NULL) {
        p_buf = _p_buf;
        p_lim = _p_lim;
        fail  = false;
        hists = new FnbliHist [FNBLI_N_CTX];
        dlut  = new uint16_t [(size_t)FNBLI_N_CTX * FNBLI_NORM_SUM];

        for (int c=0; c<FNBLI_N_CTX; c++) {
            if (!hists[c].decode (p_buf, p_lim, fail) || fail) return;
            hists[c].calcAccumlate();
            hists[c].calcDecodeLookupTable(dlut + (size_t)c * FNBLI_NORM_SUM);
        }

        for (int i=0; i<FNBLI_N_LANE; i++) {
            if (fail) { ans[i] = FNBLI_CODE_LOW; continue; }
            ans[i]  = ((uint32_t) rd()) << FNBLI_CODE_BIT;
            ans[i] |=  rd();
        }
    }

    inline ~FnbliRANSd () { delete[] hists; delete[] dlut; }

    inline void codec (int16_t ctx, int16_t &val) {
        uint32_t  a  = ans[0];
        uint32_t  lb = a & FNBLI_NORM_MASK;
        int       v  = dlut[(size_t)ctx * FNBLI_NORM_SUM + lb];
        val = (int16_t) v;
#ifdef FNBLI_SYMDBG
        fprintf(stderr, "S %d %d\n", (int)ctx, (int)v);
#endif
        a >>= FNBLI_NORM_BIT;
        a  *= hists[ctx].hnrm[v];
        a  += lb;
        a  -= hists[ctx].hsum[v];
        if (a < FNBLI_CODE_LOW) {
            a <<= FNBLI_CODE_BIT;
            a  |= rd();
        }
        ans[0] = a;
    }

    // 16 lanes: renormalization words are consumed in lane order (exactly as the
    //           AVX2 'align_by_mask_eq0' based code does)
    inline void codec_x16 (const int16_t *ctx, int16_t *val) {
        for (int k=0; k<16; k++) {
            uint32_t a  = ans[k];
            uint32_t lb = a & FNBLI_NORM_MASK;
            int      v  = dlut[(size_t)ctx[k] * FNBLI_NORM_SUM + lb];
            val[k] = (int16_t) v;
#ifdef FNBLI_SYMDBG
            fprintf(stderr, "S %d %d\n", (int)ctx[k], (int)v);
#endif
            a >>= FNBLI_NORM_BIT;
            a  *= hists[ctx[k]].hnrm[v];
            a  += lb;
            a  -= hists[ctx[k]].hsum[v];
            if (a < FNBLI_CODE_LOW) {
                a <<= FNBLI_CODE_BIT;
                a  |= rd();
            }
            ans[k] = a;
        }
    }
};

//==================================================================================================
//  pixel corrector (adaptive context model)
//==================================================================================================
template <int16_t MAXVAL>
class FnbliPxCorrector {
private:
    static const int CTX_COEF  = 6;
    static const int CTX_SCALE = 7;

    inline static int16_t clip (int v, int a, int b) { return (int16_t)((v<a) ? a : ((v>b) ? b : v)); }

    // arithmetic right shift without relying on implementation defined behaviour :
    //   v >= 0 :  v >> n
    //   v <  0 :  ~((~v) >> n)      (== floor (v / 2^n))
    inline static int32_t ashr (int32_t v, int n) { return (v >= 0) ? (v >> n) : ~((~v) >> n); }

    // ctx32 = (63*ctx + 128*err + 31) >> 6      (multiplication instead of a shift : shifting a
    //                                            negative value would be UB in C++11)
    inline static int16_t updateContext (int16_t ctx, int16_t err) {
        int32_t c = (int32_t) ctx;
        int32_t e = (int32_t) err;
        c  = (c * ((1 << CTX_COEF) - 1)) + (e * (1 << CTX_SCALE)) + ((1 << (CTX_COEF-1)) - 1);
        return (int16_t) ashr (c, CTX_COEF);
    }

    int16_t *array_ctx;

public:
    inline FnbliPxCorrector (int n_context) {
        array_ctx = new int16_t [n_context];
        for (int i=0; i<n_context; i++) array_ctx[i] = 0;
    }
    inline ~FnbliPxCorrector () { delete[] array_ctx; }

    template <bool IS_ENC>
    inline int16_t act (int16_t adr, int16_t px, int16_t &x, int16_t &w) {
        int16_t ctx  = array_ctx[adr];
        int16_t sign = (int16_t)((ctx >> (CTX_SCALE-1)) & 1);
        int16_t pxc  = clip((int)px + sign + ((int)ctx >> CTX_SCALE), 0, MAXVAL);
        if (IS_ENC) w = (int16_t)(((sign ? ((int)x-pxc) : (pxc-(int)x)) & MAXVAL));
        else        x = (int16_t)(((sign ? (pxc+(int)w) : (pxc-(int)w)) & MAXVAL));
        int16_t err  = clip((int)x - px, -(MAXVAL/2), (MAXVAL/2));
        array_ctx[adr] = updateContext (ctx, err);
        return err;
    }

    // 16 lanes : all contexts are read first, then all are written (lane order), which is
    //            exactly what the AVX2 gather / scatter sequence does.
    template <bool IS_ENC>
    inline void act_x16 (const int16_t *adr, const int16_t *px, int16_t *x, int16_t *w, int16_t *err) {
        int16_t ctx  [FNBLI_N_LANE];
        int16_t pxc  [FNBLI_N_LANE];
        int16_t sign [FNBLI_N_LANE];

        for (int k=0; k<FNBLI_N_LANE; k++) ctx[k] = array_ctx[adr[k]];

        for (int k=0; k<FNBLI_N_LANE; k++) {
            sign[k] = (int16_t)((ctx[k] >> (CTX_SCALE-1)) & 1);
            pxc [k] = clip((int)px[k] + sign[k] + ((int)ctx[k] >> CTX_SCALE), 0, MAXVAL);
        }

        for (int k=0; k<FNBLI_N_LANE; k++) {
            if (IS_ENC) w[k] = (int16_t)(((sign[k] ? ((int)x[k]-pxc[k]) : (pxc[k]-(int)x[k])) & MAXVAL));
            else        x[k] = (int16_t)(((sign[k] ? (pxc[k]+(int)w[k]) : (pxc[k]-(int)w[k])) & MAXVAL));
        }

        for (int k=0; k<FNBLI_N_LANE; k++)
            err[k] = clip((int)x[k] - px[k], -(MAXVAL/2), (MAXVAL/2));

        for (int k=0; k<FNBLI_N_LANE; k++)
            array_ctx[adr[k]] = updateContext (ctx[k], err[k]);
    }
};

//==================================================================================================
//  modelling functions
//==================================================================================================
template <int16_t MAXVAL>
inline static int16_t fnbliPredict (int16_t &dvdh, int16_t a, int16_t b, int16_t c, int16_t d,
                                    int16_t e, int16_t f, int16_t g) {
    int dh = std::abs((int)a-e) + std::abs((int)b-c) + std::abs((int)b-d);
    int dv = std::abs((int)a-c) + std::abs((int)b-f) + std::abs((int)d-g);
    dvdh = (int16_t)(dv + dh);
    int dvh = dv - dh;
    int ab  = (dvh > 0) ? a : b;
    if (dvh < 0) dvh = -dvh;
    int px  = (a+b)*9 + (d<<1) - (c<<1) - e - f;
    px = (px < 0) ? 0 : ((px > 16*MAXVAL) ? 16*MAXVAL : px);
    if      (dvh > 80) return (int16_t) ab;
    else if (dvh > 32) return (int16_t)((px + (ab<<4) + 16) >> 5);
    else if (dvh >  8) return (int16_t)((3*px + (ab<<4) + 32) >> 6);
    else               return (int16_t)((px + 8) >> 4);
}

inline static int16_t fnbliGetQD (int16_t dvdh, int16_t err) {
    static const uint8_t lut_qd [] = {
        0, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9,
        9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
        9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
        9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
        10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
        10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
        10, 10, 10, 10, 10, 10, 10, 11};
    int qd = (int)dvdh + (((int)err < 0 ? -(int)err : (int)err) << 1);
    if (qd > (int)(sizeof(lut_qd)/sizeof(*lut_qd)) - 1) qd = (int)(sizeof(lut_qd)/sizeof(*lut_qd)) - 1;
    return lut_qd[qd];
}

inline static int16_t fnbliGetContextAddr (int16_t a, int16_t b, int16_t c, int16_t d, int16_t e,
                                           int16_t f, int16_t px, int16_t qd) {
    int adr = qd;
    adr <<= 1;  adr |= (px > a);
    adr <<= 1;  adr |= (px > b);
    adr <<= 1;  adr |= (px > c);
    adr <<= 1;  adr |= (px > d);
    adr <<= 1;  adr |= (px > e);
    adr <<= 1;  adr |= (px > f);
    adr <<= 1;  adr |= (px > (2*(int)a-e));
    adr <<= 1;  adr |= (px > (2*(int)b-f));
    return (int16_t) adr;
}

//==================================================================================================
//  image sampler : 1 lane (row major)
//==================================================================================================
template <bool IS_RGB>
class FnbliSampler1 {
private:
    inline void loadAt (int16_t &Y, int16_t &U, int16_t &V, int i, int j) const {
        if (IS_RGB) {
            const uint8_t *p = p_img + 3 * ((size_t)width*i + j);
            U = p[0]; Y = p[1]; V = p[2];
            fnbliRGB2YUV(Y, U, V);
        } else {
            Y = p_img[(size_t)width*i + j];
        }
    }

public:
    uint8_t *p_img;
    int      height, width;
    int      i, j;

public:
    int16_t aY, bY, cY, dY, eY, fY, gY;
    int16_t aU, bU, cU, dU, eU, fU, gU;
    int16_t aV, bV, cV, dV, eV, fV, gV;

    inline FnbliSampler1 (uint8_t *_p_img, int _height, int _width) {
        p_img = _p_img; height = _height; width = _width; i = j = 0;
        aY = bY = cY = dY = eY = fY = gY = FNBLI_MID_Y;
        aU = bU = cU = dU = eU = fU = gU = FNBLI_MID_UV;
        aV = bV = cV = dV = eV = fV = gV = FNBLI_MID_UV;
    }

    inline bool next () {
        j ++;
        if (j >= width) { j = 0; i ++; }
        return (i < height);
    }

    inline void clearAtStartOfLine (int16_t &vY, int16_t &vU, int16_t &vV) {
        if (j == 0) { vY = 0; vU = 0; vV = 0; }
    }

    inline void loadPixels (int16_t &Y, int16_t &U, int16_t &V) { loadAt(Y, U, V, i, j); }

    inline void storePixels (int16_t Y, int16_t U, int16_t V) {
        if (IS_RGB) {
            uint8_t *p = p_img + 3 * ((size_t)width*i + j);
            int16_t R=U, G=Y, B=V;
            fnbliYUV2RGB(G, R, B);
            p[0] = (uint8_t)R; p[1] = (uint8_t)G; p[2] = (uint8_t)B;
        } else {
            p_img[(size_t)width*i + j] = (uint8_t) Y;
        }
    }

    inline void getTemplates (int16_t xY, int16_t xU, int16_t xV) {
        if (j == 0) {
            if (i > 0) {
                loadAt(bY, bU, bV, i-1, 0);
                fY = bY; fU = bU; fV = bV;
                if (i > 1) loadAt(fY, fU, fV, i-2, 0);
            }
            eY = aY = cY = bY;  eU = aU = cU = bU;  eV = aV = cV = bV;
        } else {
            eY = aY;  eU = aU;  eV = aV;
            aY = xY;  aU = xU;  aV = xV;
            cY = bY;  cU = bU;  cV = bV;
            bY = dY;  bU = dU;  bV = dV;
            fY = gY;  fU = gU;  fV = gV;
        }

        bool flag = (j+1 < width);

        if      (i < 1)   { dY = aY;  dU = aU;  dV = aV; }
        else if (flag)    loadAt(dY, dU, dV, i-1, j+1);

        if      (i < 2)   { gY = dY;  gU = dU;  gV = dV; }
        else if (flag)    loadAt(gY, gU, gV, i-2, j+1);
    }
};

//==================================================================================================
//  image sampler : 16 lanes (diagonal wave-front)  --  scalar emulation of ImageSampler_x16_AVX
//==================================================================================================
template <bool IS_RGB>
class FnbliSampler16 {
private:
    inline void loadAt (int16_t &Y, int16_t &U, int16_t &V, int i, int j) const {
        if (IS_RGB) {
            const uint8_t *p = p_img + 3 * ((size_t)width*i + j);
            U = p[0]; Y = p[1]; V = p[2];
            fnbliRGB2YUV(Y, U, V);
        } else {
            Y = p_img[(size_t)width*i + j];
        }
    }

    uint8_t *p_img;
    int16_t  height, width;
    int16_t  i [FNBLI_N_LANE];
    int16_t  j [FNBLI_N_LANE];

    inline void initIJ () {
        i[15] = -16;
        j[15] = (int16_t) width;
        for (int k=14; k>=0; k--) { i[k] = (int16_t)(i[k+1] + 1); j[k] = (int16_t)(j[k+1] - 2); }
        i[15] = 0;
        j[15] = 0;
    }

    inline void getTemplatesStartOfLine () {
        int k0 = -1;
        for (int k=0; k<16; k++) if (j[k] == 0) { k0 = k; break; }

        if (k0 < 0) return;

        int ik = i[k0];

        int16_t bYk = FNBLI_MID_Y , dYk = FNBLI_MID_Y , fYk = FNBLI_MID_Y , gYk = FNBLI_MID_Y ;
        int16_t bUk = FNBLI_MID_UV, dUk = FNBLI_MID_UV, fUk = FNBLI_MID_UV, gUk = FNBLI_MID_UV;
        int16_t bVk = FNBLI_MID_UV, dVk = FNBLI_MID_UV, fVk = FNBLI_MID_UV, gVk = FNBLI_MID_UV;

        if (ik < height) {     // only called for the body, no need to handle i<=1
            loadAt(bYk, bUk, bVk, ik-1, 0);
            loadAt(dYk, dUk, dVk, ik-1, 1);
            loadAt(fYk, fUk, fVk, ik-2, 0);
            loadAt(gYk, gUk, gVk, ik-2, 1);
        }

        for (int k=0; k<16; k++) {
            if (j[k] == 0) {
                aY[k] = bY[k] = cY[k] = eY[k] = bYk;  dY[k] = dYk;  fY[k] = fYk;  gY[k] = gYk;
                if (IS_RGB) {
                    aU[k] = bU[k] = cU[k] = eU[k] = bUk;  dU[k] = dUk;  fU[k] = fUk;  gU[k] = gUk;
                    aV[k] = bV[k] = cV[k] = eV[k] = bVk;  dV[k] = dVk;  fV[k] = fVk;  gV[k] = gVk;
                }
            }
        }
    }

public:
    int16_t aY[16], bY[16], cY[16], dY[16], eY[16], fY[16], gY[16];
    int16_t aU[16], bU[16], cU[16], dU[16], eU[16], fU[16], gU[16];
    int16_t aV[16], bV[16], cV[16], dV[16], eV[16], fV[16], gV[16];

    inline FnbliSampler16 (uint8_t *_p_img, int _height, int _width) {
        p_img = _p_img; height = (int16_t)_height; width = (int16_t)_width;
        initIJ();
        for (int k=0; k<16; k++) {
            aY[k]=bY[k]=cY[k]=dY[k]=eY[k]=fY[k]=gY[k]= FNBLI_MID_Y;
            aU[k]=bU[k]=cU[k]=dU[k]=eU[k]=fU[k]=gU[k]= FNBLI_MID_UV;
            aV[k]=bV[k]=cV[k]=dV[k]=eV[k]=fV[k]=gV[k]= FNBLI_MID_UV;
        }
    }

    inline bool next () {
        for (int k=0; k<16; k++) {
            j[k] = (int16_t)(j[k] + 1);
            if (j[k] == width) { j[k] = 0; i[k] = (int16_t)(i[k] + 16); }
        }
        return (i[0] < height);
    }

    inline void clearAtStartOfLine (int16_t *vY, int16_t *vU, int16_t *vV) {
        for (int k=0; k<16; k++) {
            if (j[k] == 0) {
                vY[k] = 0;
                if (IS_RGB) { vU[k] = 0; vV[k] = 0; }
            }
        }
    }

    inline void loadPixels (int16_t *Y, int16_t *U, int16_t *V) {
        for (int k=0; k<16; k++) {
            bool flag = (i[k] > -1) && (i[k] < height);
            if (!flag) {
                Y[k] = FNBLI_MID_Y;
                if (IS_RGB) { U[k] = FNBLI_MID_UV; V[k] = FNBLI_MID_UV; }
            } else {
                loadAt(Y[k], U[k], V[k], i[k], j[k]);
            }
        }
    }

    inline void storePixels (int16_t *Y, int16_t *U, int16_t *V) {
        for (int k=0; k<16; k++) {
            if ((i[k] > -1) && (i[k] < height)) {
                size_t addr = (size_t)((int)i[k] * width + (int)j[k]);
                if (IS_RGB) {
                    uint8_t *p = p_img + 3*addr;
                    int16_t R=U[k], G=Y[k], B=V[k];
                    fnbliYUV2RGB(G, R, B);
                    p[0] = (uint8_t)R; p[1] = (uint8_t)G; p[2] = (uint8_t)B;
                } else {
                    p_img[addr] = (uint8_t) Y[k];
                }
            }
        }
    }

    inline void getTemplates (const int16_t *xY, const int16_t *xU, const int16_t *xV) {
        for (int k=0; k<16; k++) {
            eY[k] = aY[k];
            aY[k] = xY[k];
            cY[k] = bY[k];
            bY[k] = dY[k];
            fY[k] = gY[k];
            if (IS_RGB) {
                eU[k] = aU[k];  eV[k] = aV[k];
                aU[k] = xU[k];  aV[k] = xV[k];
                cU[k] = bU[k];  cV[k] = bV[k];
                bU[k] = dU[k];  bV[k] = dV[k];
                fU[k] = gU[k];  fV[k] = gV[k];
            }
        }

        // ring_shift_right :  res[k] = src[(k+1) % 16]
        for (int k=0; k<16; k++) {
            bool flag = (k < 15) && (j[k] < width-1);
            if (flag) {
                gY[k] = cY[(k+1) & 15];
                dY[k] = aY[(k+1) & 15];
                if (IS_RGB) {
                    gU[k] = cU[(k+1) & 15];  dU[k] = aU[(k+1) & 15];
                    gV[k] = cV[(k+1) & 15];  dV[k] = aV[(k+1) & 15];
                }
            }
        }

        getTemplatesStartOfLine();

        if (i[15] < height && j[15] < (int16_t)(width-1)) {
            int16_t Y, U, V;
            loadAt(Y, U, V, i[15]-2, j[15]+1);
            gY[15] = Y;  if (IS_RGB) { gU[15] = U; gV[15] = V; }
            loadAt(Y, U, V, i[15]-1, j[15]+1);
            dY[15] = Y;  if (IS_RGB) { dU[15] = U; dV[15] = V; }
        }
    }
};

//==================================================================================================
//  fNBLI coding loops
//==================================================================================================
template <bool IS_RGB, bool IS_ENC, typename CODEC_T>
static void fnbliCodec1 (
    CODEC_T                       &codec,
    FnbliPxCorrector<FNBLI_MAX_Y> &pcY,
    FnbliPxCorrector<FNBLI_MAX_UV>&pcU,
    FnbliPxCorrector<FNBLI_MAX_UV>&pcV,
    uint8_t *p_img, int height, int width
) {
    FnbliSampler1<IS_RGB> sp(p_img, height, width);

    int16_t errY = 0, xY = FNBLI_MID_Y;
    int16_t errU = 0, xU = FNBLI_MID_UV;
    int16_t errV = 0, xV = FNBLI_MID_UV;

    do {
        sp.getTemplates(xY, xU, xV);
        sp.clearAtStartOfLine(errY, errU, errV);

        if (IS_ENC) sp.loadPixels(xY, xU, xV);

        if (IS_RGB) {
            int16_t dvdh;
            int16_t px  = fnbliPredict<FNBLI_MAX_UV>(dvdh, sp.aU, sp.bU, sp.cU, sp.dU, sp.eU, sp.fU, sp.gU);
            int16_t qd  = fnbliGetQD(dvdh, errU);
#ifdef FNBLI_SYMDBG
            fprintf(stderr, "P %d %d | %d %d %d %d %d %d %d | dvdh %d errU %d px %d qd %d\n", sp.i, sp.j, (int)sp.aU,(int)sp.bU,(int)sp.cU,(int)sp.dU,(int)sp.eU,(int)sp.fU,(int)sp.gU, (int)dvdh,(int)errU,(int)px,(int)qd);
#endif
            int16_t adr = fnbliGetContextAddr(sp.aU, sp.bU, sp.cU, sp.dU, sp.eU, sp.fU, px, qd);
            qd = (int16_t)(qd + FNBLI_N_QD);
            int16_t w = 0;
            if (IS_ENC) { errU = pcU.act<IS_ENC>(adr, px, xU, w); codec.codec(qd, w); }
            else        { codec.codec(qd, w); errU = pcU.act<IS_ENC>(adr, px, xU, w); }
            errV = (int16_t)(((std::abs((int)errV) + std::abs((int)errU) + 1) >> 1));
        }

        if (IS_RGB) {
            int16_t dvdh;
            int16_t px  = fnbliPredict<FNBLI_MAX_UV>(dvdh, sp.aV, sp.bV, sp.cV, sp.dV, sp.eV, sp.fV, sp.gV);
            int16_t qd  = fnbliGetQD(dvdh, errV);
            int16_t adr = fnbliGetContextAddr(sp.aV, sp.bV, sp.cV, sp.dV, sp.eV, sp.fV, px, qd);
            adr = (int16_t)((adr<<1) | (errU>0));
            qd  = (int16_t)(qd + FNBLI_N_QD);
            int16_t w = 0;
            if (IS_ENC) { errV = pcV.act<IS_ENC>(adr, px, xV, w); codec.codec(qd, w); }
            else        { codec.codec(qd, w); errV = pcV.act<IS_ENC>(adr, px, xV, w); }
            errY = (int16_t)(((std::abs((int)errY) + std::abs((int)errU) + std::abs((int)errV) + 1) >> 1));
        }

        {
            int16_t dvdh;
            int16_t px  = fnbliPredict<FNBLI_MAX_Y>(dvdh, sp.aY, sp.bY, sp.cY, sp.dY, sp.eY, sp.fY, sp.gY);
            int16_t qd  = fnbliGetQD(dvdh, errY);
            int16_t adr = fnbliGetContextAddr(sp.aY, sp.bY, sp.cY, sp.dY, sp.eY, sp.fY, px, qd);
            if (IS_RGB) adr = (int16_t)((adr<<2) | (errU>0) | ((errV>0)<<1));
            int16_t w = 0;
            if (IS_ENC) { errY = pcY.act<IS_ENC>(adr, px, xY, w); codec.codec(qd, w); }
            else        { codec.codec(qd, w); errY = pcY.act<IS_ENC>(adr, px, xY, w); }
        }

        if (!IS_ENC) sp.storePixels(xY, xU, xV);

    } while (sp.next());
}


template <bool IS_RGB, bool IS_ENC, typename CODEC_T>
static void fnbliCodec16 (
    CODEC_T                       &codec,
    FnbliPxCorrector<FNBLI_MAX_Y> &pcY,
    FnbliPxCorrector<FNBLI_MAX_UV>&pcU,
    FnbliPxCorrector<FNBLI_MAX_UV>&pcV,
    uint8_t *p_img, int height, int width
) {
    int16_t errY[16], xY[16];
    int16_t errU[16], xU[16];
    int16_t errV[16], xV[16];
    int16_t px[16], qd[16], adr[16], w[16], dvdh[16];

    for (int k=0; k<16; k++) {
        errY[k] = 0; xY[k] = FNBLI_MID_Y;
        errU[k] = 0; xU[k] = FNBLI_MID_UV;
        errV[k] = 0; xV[k] = FNBLI_MID_UV;
    }

    FnbliSampler16<IS_RGB> sp(p_img, height, width);

    do {
        sp.getTemplates(xY, xU, xV);
        sp.clearAtStartOfLine(errY, errU, errV);

        if (IS_ENC) sp.loadPixels(xY, xU, xV);

        if (IS_RGB) {
            for (int k=0; k<16; k++)
                px[k] = fnbliPredict<FNBLI_MAX_UV>(dvdh[k], sp.aU[k], sp.bU[k], sp.cU[k], sp.dU[k], sp.eU[k], sp.fU[k], sp.gU[k]);
            for (int k=0; k<16; k++) qd[k]  = fnbliGetQD(dvdh[k], errU[k]);
            for (int k=0; k<16; k++) adr[k] = fnbliGetContextAddr(sp.aU[k], sp.bU[k], sp.cU[k], sp.dU[k], sp.eU[k], sp.fU[k], px[k], qd[k]);
            for (int k=0; k<16; k++) qd[k]  = (int16_t)(qd[k] + FNBLI_N_QD);
            if (IS_ENC) {
                pcU.act_x16<IS_ENC>(adr, px, xU, w, errU);
                codec.codec_x16(qd, w);
            } else {
                codec.codec_x16(qd, w);
                pcU.act_x16<IS_ENC>(adr, px, xU, w, errU);
            }
            for (int k=0; k<16; k++)
                errV[k] = (int16_t)(((std::abs((int)errV[k]) + std::abs((int)errU[k]) + 1) >> 1));
        }

        if (IS_RGB) {
            for (int k=0; k<16; k++)
                px[k] = fnbliPredict<FNBLI_MAX_UV>(dvdh[k], sp.aV[k], sp.bV[k], sp.cV[k], sp.dV[k], sp.eV[k], sp.fV[k], sp.gV[k]);
            for (int k=0; k<16; k++) qd[k]  = fnbliGetQD(dvdh[k], errV[k]);
            for (int k=0; k<16; k++) adr[k] = fnbliGetContextAddr(sp.aV[k], sp.bV[k], sp.cV[k], sp.dV[k], sp.eV[k], sp.fV[k], px[k], qd[k]);
            for (int k=0; k<16; k++) { adr[k] = (int16_t)((adr[k]<<1) | (errU[k]>0)); qd[k] = (int16_t)(qd[k] + FNBLI_N_QD); }
            if (IS_ENC) {
                pcV.act_x16<IS_ENC>(adr, px, xV, w, errV);
                codec.codec_x16(qd, w);
            } else {
                codec.codec_x16(qd, w);
                pcV.act_x16<IS_ENC>(adr, px, xV, w, errV);
            }
            for (int k=0; k<16; k++)
                errY[k] = (int16_t)(((std::abs((int)errY[k]) + std::abs((int)errU[k]) + std::abs((int)errV[k]) + 1) >> 1));
        }

        {
            for (int k=0; k<16; k++)
                px[k] = fnbliPredict<FNBLI_MAX_Y>(dvdh[k], sp.aY[k], sp.bY[k], sp.cY[k], sp.dY[k], sp.eY[k], sp.fY[k], sp.gY[k]);
            for (int k=0; k<16; k++) qd[k]  = fnbliGetQD(dvdh[k], errY[k]);
            for (int k=0; k<16; k++) adr[k] = fnbliGetContextAddr(sp.aY[k], sp.bY[k], sp.cY[k], sp.dY[k], sp.eY[k], sp.fY[k], px[k], qd[k]);
            if (IS_RGB)
                for (int k=0; k<16; k++) adr[k] = (int16_t)((adr[k]<<2) | (errU[k]>0) | ((errV[k]>0)<<1));
            if (IS_ENC) {
                pcY.act_x16<IS_ENC>(adr, px, xY, w, errY);
                codec.codec_x16(qd, w);
            } else {
                codec.codec_x16(qd, w);
                pcY.act_x16<IS_ENC>(adr, px, xY, w, errY);
            }
        }

        if (!IS_ENC) sp.storePixels(xY, xU, xV);

    } while (sp.next());
}

#endif // __FNBLI_SCALAR_H__
