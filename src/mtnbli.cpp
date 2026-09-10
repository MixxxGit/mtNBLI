//==================================================================================================
//  mtnbli :  multi-threaded, SIMD-free (no AVX / no SSE4) NBLI + fNBLI codec
//
//  * decodes every flavour of NBLI  (.nbli : rANS or Golomb, with/without advanced predictor,
//    lossless and near-lossless)  and  fNBLI  (.fnbli : 1-lane and 16-lane "is_large" streams)
//  * the 16-lane wave-front coder of fNBLI exists in the original code only as an AVX2
//    implementation;  here it is emulated in plain integer C++, so fNBLI images that were
//    compressed on a modern CPU can be decoded on an old one (AMD Phenom II, Core 2, ...).
//    The original fNBLI.exe refuses those files.
//  * multi-threaded : by default every hardware thread of the machine is used.
//      - a batch of files is decoded/compressed concurrently
//      - a single image is split into independent tiles (.tnbli container) which are
//        processed concurrently
//
//  build :  see ../Makefile
//==================================================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>

#ifndef _WIN32
#  include <glob.h>
#  include <sys/stat.h>
#endif

#ifdef _WIN32
#  define  WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>          // CommandLineToArgvW : the only way to get argv in Unicode
// windef.h still defines the 16-bit era 'near' / 'far' macros, and 'near' is the name of the
// NBLI distortion parameter.  They are empty and useless, drop them.
#  undef   near
#  undef   far
#endif

#include "FileIO.h"
#include "CRC32.h"
#include "safedecode.h"
#include "ThreadPool.h"
#include "tiles.h"
#include "fnbli_api.h"
#include "nbli/NBLI.h"
#include "nbli/rANS.h"
#include "nbli/GolombCodeTree.h"
#include "nbli/NBLIcodec.h"
#include "nbli/Header.h"

extern "C" {
#include "imageio/imageio.h"
}

//-------------------------------------------------------------------------------------------------- usage
static const char *USAGE =
  "|----------------------------------------------------------------------------------|\n"
  "| mtnbli : multi-threaded, SIMD-free (no AVX) NBLI / fNBLI codec                    |\n"
  "|   based on https://github.com/WangXuan95/NBLI  (v0.4, GPLv3)                      |\n"
  "|----------------------------------------------------------------------------------|\n"
  "| this CPU: %-71s|\n"
  "|----------------------------------------------------------------------------------|\n"
  "| Usage:                                                                           |\n"
  "|   mtnbli [-switches]  <in1> [-o <out1>]  [<in2> [-o <out2]]  ...                 |\n"
  "|                                                                                  |\n"
  "| To compress:                                                                     |\n"
  "|   <in>  can be .pgm, .ppm, .pnm or .png                                          |\n"
  "|   <out> can be .fnbli, .nbli or .tnbli (tiled).  Generated if not specified.      |\n"
  "|   <in> may be a wildcard, e.g. \"dir\\*.png\" -- all matching files are processed   |\n"
  "|                                                                                  |\n"
  "| To decompress:                                                                   |\n"
  "|   <in>  can be .fnbli, .nbli or .tnbli                                           |\n"
  "|   <out> can be .pgm, .ppm, .pnm or .png.  Generated if not specified.             |\n"
  "|                                                                                  |\n"
  "| switches:                                                                        |\n"
  "|   -v        : verbose                                                            |\n"
  "|   -f        : force overwrite of output file                                     |\n"
  "|   -x        : put a CRC32 into the stream when compressing                        |\n"
  "|   -t <N>    : number of threads (default: all %-2d hardware threads)              |\n"
  "|   -T <N>    : number of tiles when compressing (default 1 = plain .fnbli/.nbli)   |\n"
  "|               -T 0 = auto (2 tiles per thread).  >1 produces a .tnbli container    |\n"
  "|   -N        : compress with NBLI instead of fNBLI                                 |\n"
  "|   -g        : NBLI: golomb coding tree instead of ANS (slower, smaller)           |\n"
  "|   -a        : NBLI: advanced predictor (extremely slow, smaller)                  |\n"
  "|   -0..-7    : NBLI: distortion level, 0 = lossless (default), 1..7 = lossy        |\n"
  "|   --pnm     : when decompressing, write .pnm/.ppm/.pgm instead of .png            |\n"
  "|----------------------------------------------------------------------------------|\n"
  "\n";

//-------------------------------------------------------------------------------------------------- helpers
inline static bool matchSuffixIgnoringCase (const char *string, const char *suffix) {
    #define  TO_LOWER(c)   ((((c) >= 'A') && ((c) <= 'Z')) ? ((c)+32) : (c))
    const char *p1, *p2;
    for (p1=string; *p1; p1++);
    for (p2=suffix; *p2; p2++);
    while (TO_LOWER(*p1) == TO_LOWER(*p2)) {
        if (p2 <= suffix) return true;
        if (p1 <= string) return false;
        p1 --; p2 --;
    }
    return false;
}

inline static std::string replaceFileSuffix (const char *p_src, const char *p_suffix) {
    std::string s (p_src);
    size_t pos = s.find_last_of ('.');
    size_t sl  = s.find_last_of ("/\\");
    if (pos == std::string::npos || (sl != std::string::npos && sl > pos))
        s += '.';
    else
        s = s.substr (0, pos+1);
    s += p_suffix;
    return s;
}

// name of the CPU we are running on (only for the banner)
static std::string cpuName () {
    std::string r;

#ifdef _WIN32
    // HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\ProcessorNameString
    {
        HKEY hk = NULL;
        if (RegOpenKeyExA (HKEY_LOCAL_MACHINE,
                           "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                           0, KEY_READ, &hk) == ERROR_SUCCESS) {
            char  buf[256] = {0};
            DWORD sz = sizeof(buf), ty = 0;
            if (RegQueryValueExA (hk, "ProcessorNameString", NULL, &ty, (LPBYTE) buf, &sz) == ERROR_SUCCESS)
                r = buf;
            RegCloseKey (hk);
        }
    }
#else
    FILE *fp = fopen ("/proc/cpuinfo", "r");
    if (fp) {
        char line[512];
        while (fgets (line, sizeof(line), fp)) {
            if (!strncmp (line, "model name", 10)) {
                char *p = strchr (line, ':');
                if (p) {
                    r = p+1;
                    while (!r.empty() && (r[0]==' ' || r[0]=='\t')) r.erase(r.begin());
                    while (!r.empty() && (r.back()=='\n' || r.back()=='\r' || r.back()==' ')) r.pop_back();
                }
                break;
            }
        }
        fclose (fp);
    }
#endif

#if defined(__x86_64__) && defined(__GNUC__)
    __builtin_cpu_init ();
    r += __builtin_cpu_supports ("avx2") ? "   (AVX2: yes)" : "   (AVX2: NO)";
#endif
    if (r.empty()) r = "unknown";
    if (r.size() > 71) r = r.substr (0, 71);
    return r;
}

//-------------------------------------------------------------------------------------------------- integer fixed point formatting
//  v is expected to be finite and non-negative; anything else prints as "0".
static const unsigned long long *g_pow10 = NULL;

static void fmt_init () {
    static unsigned long long p [10];
    p[0] = 1;
    for (int i=1; i<10; i++) p[i] = p[i-1] * 10ull;
    g_pow10 = p;
}

// appends "int.frac" with exactly 'dec' fraction digits (no locale, no floats in the binary)
static void fmt_fixed (std::string &s, double v, int dec) {
    if (!(v > 0) || v > 1e15) { s += (dec > 0) ? "0." : "0"; for (int i=0; i<dec; i++) s += '0'; return; }
    unsigned long long ip = (unsigned long long) v;
    double rest = v - (double) ip;
    unsigned long long fr = (unsigned long long) (rest * (double) g_pow10[dec] + 0.5);
    if (fr >= g_pow10[dec]) { fr -= g_pow10[dec]; ip++; }        // rounding carried
    char buf [64];
    snprintf (buf, sizeof(buf), "%llu", ip);
    s += buf;
    if (dec > 0) {
        s += '.';
        snprintf (buf, sizeof(buf), "%0*llu", dec, fr);
        s += buf;
    }
}

static void fmt_rate (std::string &s, double kBps) {             // "12345 kB/s"
    char buf [64];
    snprintf (buf, sizeof(buf), "%llu", (!(kBps > 0) || kBps > 1e15) ? 0ull : (unsigned long long)(kBps + 0.5));
    s += buf;
}

static double nowSec () {
    return std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

//-------------------------------------------------------------------------------------------------- globals from switches
static bool     g_verbose   = false;
static bool     g_force     = false;
static bool     g_crc       = false;
static bool     g_useNBLI   = false;
static bool     g_golomb    = false;
static bool     g_avp       = false;
static bool     g_pnm       = false;
static int      g_near      = 0;
static unsigned g_threads   = 0;
static int      g_tiles     = 1;

//-------------------------------------------------------------------------------------------------- one file job
struct Job {
    std::string src, dst;
    bool        ok;
    int         kind;          // 0 = compressed, 1 = decompressed
    uint32_t    w, h;
    uint32_t    crc;
    size_t      bytes;         // size of the compressed stream
    double      secs;
    std::string note;
    Job () : ok(false), kind(-1), w(0), h(0), crc(0), bytes(0), secs(0) {}
};

//-------------------------------------------------------------------------------------------------- NBLI : decode into an existing buffer
// (the original NBLI.cpp always allocates; this variant lets us decode a tile in place)
static bool nbliDecompressTo (uint8_t *p_buf, uint8_t *p_out, uint32_t &height, uint32_t &width,
                              bool &is_rgb, uint32_t &crc32) {
    if (1 & (size_t)p_buf)
        return false;

    uint16_t *p_u16 = (uint16_t*) p_buf;

    bool use_golomb=false, use_avp=false;
    int16_t near = 0;

    // re-implementation of loadHeader + dispatch (identical to src/nbli/NBLI.cpp)
    p_u16 = loadHeader (p_u16, height, width, is_rgb, use_golomb, use_avp, near, crc32);
    if (p_u16 == NULL) return false;
    if (!((0<height) && (height<=32000) && (0<width) && (width<=32000))) return false;

    #define  DISPATCH(RGB, GOL, AVP)                                                            \
        if (RGB) {                                                                              \
            if (GOL) NBLIcodec<false,true ,AVP, GolombCodeTree<false,3,N_QD>> (p_u16, NULL, p_out, height, width, near); \
            else     NBLIcodec<false,true ,AVP, rANSwithGlobalHistogram<false,3*N_QD,9>> (p_u16, NULL, p_out, height, width, near); \
        } else {                                                                                \
            if (GOL) NBLIcodec<false,false,AVP, GolombCodeTree<false,1,N_QD>> (p_u16, NULL, p_out, height, width, near); \
            else     NBLIcodec<false,false,AVP, rANSwithGlobalHistogram<false,N_QD,8>> (p_u16, NULL, p_out, height, width, near); \
        }

    if (is_rgb) {
        if (use_golomb) { if (use_avp) DISPATCH(true ,true ,true ) else DISPATCH(true ,true ,false) }
        else            { if (use_avp) DISPATCH(true ,false,true ) else DISPATCH(true ,false,false) }
    } else {
        if (use_golomb) { if (use_avp) DISPATCH(false,true ,true ) else DISPATCH(false,true ,false) }
        else            { if (use_avp) DISPATCH(false,false,true ) else DISPATCH(false,false,false) }
    }
    #undef DISPATCH

    if (crc32) {
        size_t img_size = (size_t)(is_rgb?3:1) * height * width;
        if (crc32 != calculateCRC32 (p_out, img_size))
            return false;
    }
    return true;
}

//-------------------------------------------------------------------------------------------------- decompress one buffer
// returns: 0 = ok, else error.  *pp_img receives a new[] buffer unless p_out was given
static int decompressStream (const uint8_t *p_src, size_t src_len, uint8_t *p_out,
                             uint8_t *&p_img, bool &is_rgb, uint32_t &h, uint32_t &w,
                             uint32_t &crc, std::string &err)
{
    // the codecs want a 2-byte aligned, writable buffer.  loadBytesFromFile() gives us that.
    uint8_t *p_buf = const_cast<uint8_t*>(p_src);
    (void) src_len;

    if (tnbliIsTiled (p_src, src_len)) {
        const TnbliHeader *ph = (const TnbliHeader*) p_src;
        if (ph->codec != TNBLI_CODEC_FNBLI && ph->codec != TNBLI_CODEC_NBLI) { err = "unknown tiled codec"; return 1; }
        h = ph->height; w = ph->width;
        is_rgb = (ph->flags & TNBLI_FLAG_RGB) != 0;
        crc    = ph->crc32;
        if (h == 0 || w == 0 || ph->n_tiles == 0) { err = "bad tiled header"; return 1; }
        if (p_out == NULL) p_img = new uint8_t [(size_t)h*w*(is_rgb?3:1)];
        else               p_img = p_out;

        const uint64_t *offs = (const uint64_t*)(p_src + TNBLI_HEADER_SIZE);

        bool  avp    = (ph->flags & TNBLI_FLAG_AVP) != 0;
        bool  golomb = (ph->flags & TNBLI_FLAG_GOLOMB) != 0;

        // memory guard : the advanced predictor allocates ~ 3 * 688 * width bytes per tile
        unsigned n_thr = g_threads ? g_threads : ParallelFor::defaultThreadCount();
        if (avp) {
            size_t per_tile = (size_t)3 * 688 * w + (1<<20);
            size_t limit    = (size_t)512 << 20;
            unsigned max_t  = (unsigned)(limit / (per_tile ? per_tile : 1));
            if (max_t < 1) max_t = 1;
            if (n_thr > max_t) n_thr = max_t;
        }

        std::atomic<int> nfail (0);
        size_t bpp = is_rgb ? 3 : 1;

        ParallelFor::run (ph->n_tiles, n_thr, [&] (size_t i) {
            // every worker needs its own guard : a damaged tile must not kill the process
            FAULT_TRY
                uint32_t row0 = tnbliTileRow (h, ph->n_tiles, (uint32_t)i);
                uint32_t th_  = tnbliTileHeight (h, ph->n_tiles, (uint32_t)i);
                uint8_t *p_dst = p_img + (size_t)row0 * w * bpp;
                size_t   o0 = (size_t) offs[i], o1 = (size_t) offs[i+1];
                if (o1 <= o0 || o1 > src_len) { nfail++; }
                else if (ph->codec == TNBLI_CODEC_FNBLI) {
                    bool     tirgb = false; uint32_t th=0, tw=0, tcrc=0;
                    uint8_t *r = fnbliDecompress (p_buf + o0, tirgb, th, tw, tcrc, p_dst, o1-o0);
                    if (r == NULL || th != th_ || tw != w) nfail++;
                } else {
                    bool tig=false, tgol=false, tia=false; int16_t tn=0; uint32_t th=0, tw=0, tcrc=0;
                    // the rANS decoder of NBLI refuses to read outside of the tile
                    nbliRansLimit()   = (uint16_t*)(p_buf + o0) + ((o1-o0)>>1);
                    nbliRansDamaged() = false;
                    uint8_t *tmp = NBLIdecompress (p_buf + o0, tig, th, tw, tgol, tia, tn, tcrc);
                    nbliRansLimit() = NULL;
                    if (tmp == NULL || nbliRansDamaged()) { nfail++; delete[] tmp; }
                    else { memcpy (p_dst, tmp, (size_t)th*tw*bpp); delete[] tmp; }
                    (void) golomb;
                }
            FAULT_CATCH
                nfail++;
            FAULT_END
        });

        if (nfail) { err = "tile decode failed"; return 1; }

        if (crc) {
            if (crc != calculateCRC32 (p_img, (size_t)h*w*bpp)) { err = "CRC32 mismatch"; return 1; }
        }
        return 0;
    }

    if (src_len >= 2 && p_src[0]=='f' && p_src[1]=='n') {
        p_img = fnbliDecompress (p_buf, is_rgb, h, w, crc, p_out, src_len);
        if (p_img == NULL) { err = "fNBLI decode failed"; return 1; }
        return 0;
    }

    if (src_len >= 2 && p_src[0]=='n' && p_src[1]=='b') {
        // the NBLI rANS decoder refuses to read outside of this stream
        nbliRansLimit()   = (uint16_t*) p_buf + (src_len>>1);
        nbliRansDamaged() = false;
        if (p_out == NULL) {
            bool ug=false, ua=false; int16_t nr=0;
            p_img = NBLIdecompress (p_buf, is_rgb, h, w, ug, ua, nr, crc);
            if (p_img == NULL) { err = "NBLI decode failed"; return 1; }
        } else {
            if (!nbliDecompressTo (p_buf, p_out, h, w, is_rgb, crc)) { err = "NBLI decode failed"; return 1; }
            p_img = p_out;
        }
        nbliRansLimit() = NULL;
        if (nbliRansDamaged()) {
            if (p_out == NULL) delete[] p_img;
            err = "NBLI decode failed (truncated / damaged stream)";
            return 1;
        }
        return 0;
    }

    err = "not a .nbli / .fnbli / .tnbli file";
    return 1;
}

//-------------------------------------------------------------------------------------------------- process one file
static void processFile (Job &job)
{
    double t0 = nowSec();

    int is_rgb_i = 0;
    uint32_t h = 0, w = 0;
    uint8_t *in_img = loadPNMImageFile (job.src.c_str(), &is_rgb_i, &h, &w);
    if (in_img == NULL) in_img = loadPNGImageFile (job.src.c_str(), &is_rgb_i, &h, &w);

    // ---------------------------------------------------------------- compress
    if (in_img != NULL) {
        bool   is_rgb = (bool) is_rgb_i;
        size_t img_size = (size_t)h * w * (is_rgb?3:1);
        uint32_t crc = g_crc ? 1u : 0u;

        job.w = w; job.h = h; job.kind = 0;

        uint32_t n_tiles = (g_tiles > 0) ? (uint32_t)g_tiles : tnbliAutoTiles (h, g_threads);
        if (n_tiles < 1) n_tiles = 1;
        if ((uint32_t)n_tiles > h) n_tiles = h;
        bool tiled = (n_tiles > 1);

        if (!tiled) {                                        // ---- plain single stream
            uint8_t *out = NULL;
            size_t   cs  = 0;
            if (g_useNBLI) {
                bool ug = g_golomb, ua = g_avp; int16_t nr = (int16_t)g_near;
                out = NBLIcompress (cs, in_img, is_rgb, h, w, ug, ua, nr, crc);
            } else {
                out = fnbliCompress (cs, in_img, is_rgb, h, w, crc, false);
            }
            free (in_img);
            if (out == NULL) { job.note = "compress failed"; return; }
            job.crc = crc; job.bytes = cs;

            std::string dst = job.dst.empty() ? replaceFileSuffix (job.src.c_str(), g_useNBLI?"nbli":"fnbli") : job.dst;
            if (!g_force && fileExist (dst.c_str())) { job.note = "output exists"; delete[] out; return; }
            if (writeBytesToFile (dst.c_str(), out, cs)) { job.note = "write failed"; delete[] out; return; }
            delete[] out;
            job.ok = true; job.secs = nowSec()-t0;
            return;
        }

        // ------------------------------------------------------------ tiled container
        size_t bpp = is_rgb ? 3 : 1;
        std::vector<uint8_t*> blobs  (n_tiles, NULL);
        std::vector<size_t>   blens  (n_tiles, 0);
        std::atomic<int>      nfail  (0);

        ParallelFor::run (n_tiles, g_threads, [&] (size_t i) {
            uint32_t row0 = tnbliTileRow (h, n_tiles, (uint32_t)i);
            uint32_t th_  = tnbliTileHeight (h, n_tiles, (uint32_t)i);
            uint8_t *p    = in_img + (size_t)row0 * w * bpp;
            uint32_t tcrc = g_crc ? 1u : 0u;
            size_t   cs   = 0;
            uint8_t *out  = NULL;
            if (g_useNBLI) {
                bool ug = g_golomb, ua = g_avp; int16_t nr = (int16_t)g_near;
                out = NBLIcompress (cs, p, is_rgb, th_, w, ug, ua, nr, tcrc);
            } else {
                out = fnbliCompress (cs, p, is_rgb, th_, w, tcrc, false);
            }
            if (out == NULL) { nfail++; return; }
            blobs[i] = out; blens[i] = cs;
        });

        if (nfail) {
            for (size_t i=0; i<n_tiles; i++) if (blobs[i]) delete[] blobs[i];
            free (in_img);
            job.note = "tile compress failed";
            return;
        }

        uint32_t full_crc = g_crc ? calculateCRC32 (in_img, img_size) : 0u;
        free (in_img);

        size_t hdr = TNBLI_HEADER_SIZE + 8*((size_t)n_tiles+1);
        size_t total = hdr;
        for (uint32_t i=0; i<n_tiles; i++) {
            total = (total + 7) & ~(size_t)7;                 // keep every tile 8-byte aligned
            total += blens[i];
        }

        uint8_t *outbuf = new uint8_t [total];
        memset (outbuf, 0, hdr);

        TnbliHeader *ph = (TnbliHeader*) outbuf;
        memcpy (ph->magic, TNBLI_MAGIC, 8);
        ph->width   = w;
        ph->height  = h;
        ph->n_tiles = n_tiles;
        ph->crc32   = full_crc;
        ph->codec   = g_useNBLI ? TNBLI_CODEC_NBLI : TNBLI_CODEC_FNBLI;
        ph->flags   = (is_rgb?TNBLI_FLAG_RGB:0) | (g_avp?TNBLI_FLAG_AVP:0) | (g_golomb?TNBLI_FLAG_GOLOMB:0);
        ph->near_   = (uint16_t) g_near;
        ph->reserved= 0;
        for (int i=0; i<4; i++) ph->reserved2[i] = 0;

        uint64_t *offs = (uint64_t*)(outbuf + TNBLI_HEADER_SIZE);
        size_t pos = hdr;
        for (uint32_t i=0; i<n_tiles; i++) {
            pos = (pos + 7) & ~(size_t)7;
            offs[i] = pos;
            memcpy (outbuf + pos, blobs[i], blens[i]);
            pos += blens[i];
            delete[] blobs[i];
        }
        offs[n_tiles] = pos;

        job.crc   = full_crc;
        job.bytes = total;

        std::string dst = job.dst.empty() ? replaceFileSuffix (job.src.c_str(), "tnbli") : job.dst;
        if (!g_force && fileExist (dst.c_str())) { job.note = "output exists"; delete[] outbuf; return; }
        if (writeBytesToFile (dst.c_str(), outbuf, total)) { job.note = "write failed"; delete[] outbuf; return; }
        delete[] outbuf;
        job.ok = true; job.secs = nowSec()-t0;
        return;
    }

    // ---------------------------------------------------------------- decompress
    GuardedBuffer gb;
    if (!loadFileGuarded (job.src.c_str(), gb)) { job.note = "cannot open"; return; }
    uint8_t *in_buf = gb.p;
    size_t   len    = gb.len;

    bool     is_rgb = false;
    uint32_t crc = 0;
    uint8_t *p_img = NULL;
    std::string err;

    int r;
    FAULT_TRY
        r = decompressStream (in_buf, len, NULL, p_img, is_rgb, h, w, crc, err);
    FAULT_CATCH
        freeGuarded (gb);
        job.note = "damaged stream (illegal memory access)";
        return;
    FAULT_END

    freeGuarded (gb);
    (void) is_rgb_i;

    if (r) { job.note = err; return; }

    job.w = w; job.h = h; job.crc = crc; job.kind = 1; job.bytes = len;

    std::string dst = job.dst.empty()
        ? replaceFileSuffix (job.src.c_str(), g_pnm ? (is_rgb?"ppm":"pgm") : "png")
        : job.dst;

    if (!g_force && fileExist (dst.c_str())) { job.note = "output exists"; delete[] p_img; return; }

    bool failed;
    if        (matchSuffixIgnoringCase (dst.c_str(), "pnm") || matchSuffixIgnoringCase (dst.c_str(), "ppm") ||
               matchSuffixIgnoringCase (dst.c_str(), "pgm")) {
        failed = writePNMImageFile (dst.c_str(), p_img, is_rgb?1:0, h, w);
    } else if (matchSuffixIgnoringCase (dst.c_str(), "png")) {
        failed = writePNGImageFile (dst.c_str(), p_img, is_rgb?1:0, h, w);
    } else {
        delete[] p_img;
        job.note = "unsupported output suffix";
        return;
    }
    delete[] p_img;

    if (failed) { job.note = "write failed"; return; }

    job.ok = true;
    job.secs = nowSec() - t0;
}

//----------------------------------------------------------------------------------- wildcard expansion
//  cmd.exe (and the Windows CRT, unlike the Unix shells) does NOT expand  dir\*.png  before the
//  program sees it -- so we have to do it ourselves, otherwise the argument stays the literal
//  name of a file called "*.png" and every input fails with "cannot open".
//  On Linux the shell normally expands the pattern already; a *quoted* pattern reaches us and is
//  expanded here as well, which makes the two platforms behave the same.
//
//  A pattern that matches nothing is passed through unchanged, so the error message still names
//  what the user actually typed.  Only source arguments are expanded, never a -o destination.

static bool hasWildcard (const char *s) {
    for (; *s; s++)
        if (*s == '*' || *s == '?') return true;
    return false;
}

#ifdef _WIN32

//------ UTF-8 <-> UTF-16, so that non-ASCII directories work (CreateFileW / FindFirstFileW)
static std::wstring utf8ToWide (const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w ((size_t) n, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), &w[0], n);
    return w;
}
static std::string wideToUtf8 (const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s ((size_t) n, '\0');
    WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(), &s[0], n, NULL, NULL);
    return s;
}

//  match one path component at a time, so  C:\a\*\b\*.png  works too
static void globRec (const std::string &dir, const std::string &rest, std::vector<std::string> &out) {
    size_t p    = rest.find_first_of ("\\/");
    std::string comp = (p == std::string::npos) ? rest : rest.substr (0, p);
    std::string tail = (p == std::string::npos) ? std::string() : rest.substr (p + 1);
    char        sep  = (p == std::string::npos) ? '\\' : rest[p];

    if (!hasWildcard (comp.c_str())) {                   // plain component : just walk into it
        std::string next = dir + comp;
        if (tail.empty()) out.push_back (next);
        else              globRec (next + sep, tail, out);
        return;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW (utf8ToWide (dir + comp).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;               // nothing matches : keep the pattern
    do {
        // a directory is a perfectly good match for everything but the last component
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && tail.empty()) continue;
        std::string nm = wideToUtf8 (fd.cFileName);
        if (nm == "." || nm == "..") continue;
        if (tail.empty()) out.push_back (dir + nm);
        else              globRec (dir + nm + sep, tail, out);
    } while (FindNextFileW (h, &fd));
    FindClose (h);
}

static void expandWildcards (const std::string &arg, std::vector<std::string> &out) {
    if (!hasWildcard (arg.c_str())) { out.push_back (arg); return; }
    std::vector<std::string> hits;
    globRec (std::string(), arg, hits);
    if (hits.empty()) { out.push_back (arg); return; }   // report the pattern itself
    std::sort (hits.begin(), hits.end());                // deterministic, shell-like order
    out.insert (out.end(), hits.begin(), hits.end());
}

#else                                                    // POSIX : the C library has glob()

static void expandWildcards (const std::string &arg, std::vector<std::string> &out) {
    if (!hasWildcard (arg.c_str())) { out.push_back (arg); return; }
    glob_t g;
    memset (&g, 0, sizeof (g));
    if (glob (arg.c_str(), 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree (&g);
        out.push_back (arg);
        return;
    }
    std::vector<std::string> hits;
    for (size_t i=0; i<g.gl_pathc; i++) {
        struct stat st;
        if (stat (g.gl_pathv[i], &st) == 0 && S_ISDIR (st.st_mode)) continue;   // no directories
        hits.push_back (g.gl_pathv[i]);
    }
    globfree (&g);
    if (hits.empty()) { out.push_back (arg); return; }
    std::sort (hits.begin(), hits.end());
    out.insert (out.end(), hits.begin(), hits.end());
}

#endif

//-------------------------------------------------------------------------------------------------- main
#define  MAX_N_FILE  4096

int main (int argc, char **argv)
{
#ifdef _WIN32
    // The mingw startup code converts the command line to the *ANSI* code page before it builds
    // argv, so every non-ASCII path arrives already mangled ('?' or mojibake) -- a Russian or
    // Chinese directory simply does not open.  Take the arguments from the Unicode command line
    // instead and convert them to UTF-8 here; that is what the rest of the program (and
    // CreateFileW) expects.
    {
        int     wargc = 0;
        LPWSTR *wargv = CommandLineToArgvW (GetCommandLineW(), &wargc);
        if (wargv && wargc > 0) {
            static std::vector<std::string>  uargv;
            static std::vector<char *>       pargv;
            for (int i=0; i<wargc; i++) uargv.push_back (wideToUtf8 (wargv[i]));
            LocalFree (wargv);
            for (size_t i=0; i<uargv.size(); i++) pargv.push_back (&uargv[i][0]);
            argc = (int) uargv.size();
            argv = &pargv[0];
        }
    }
#endif

    std::vector<std::string> srcs, dsts;

    // ---------------- parse command line
    bool next_is_dst = false;
    bool have_dst    = false;

    for (int i=1; i<argc; i++) {
        char *arg = argv[i];

        if (arg[0] == '-' && arg[1] != '\0' && !(arg[1]=='0')) {
            if (!strcmp (arg, "-pnm") || !strcmp (arg, "--pnm")) { g_pnm = true; continue; }
            for (arg++ ; *arg ; arg++) {
                switch (*arg) {
                    case 'v': case 'V': g_verbose = true; break;
                    case 'f': case 'F': g_force   = true; break;
                    case 'x': case 'X': g_crc     = true; break;
                    case 'N':           g_useNBLI = true; break;
                    case 'g': case 'G': g_golomb  = true; g_useNBLI = true; break;
                    case 'a': case 'A': g_avp     = true; g_useNBLI = true; break;
                    case 'o': case 'O': next_is_dst = true; break;
                    case 't':
                        if (i+1 < argc) g_threads = (unsigned) atoi (argv[++i]);
                        break;
                    case 'T':
                        if (i+1 < argc) g_tiles = atoi (argv[++i]);
                        break;
                    default:
                        if (*arg >= '0' && *arg <= '7') { g_near = *arg - '0'; g_useNBLI = true; }
                        break;
                }
            }
        } else if (arg[0]=='-' && arg[1]=='0') {
            g_near = 0; g_useNBLI = true;
        } else {
            if (next_is_dst) {
                next_is_dst = false;
                // pad with empty strings up to the index of the source this name belongs to
                while (dsts.size() + 1 < srcs.size()) dsts.push_back (std::string());
                dsts.push_back (arg);
                have_dst = true;
            } else {
                expandWildcards (arg, srcs);      // "dir\*.png" : cmd.exe does not do it for us
            }
        }
    }
    (void) have_dst;

    FaultGuard::install();
    unsigned hw = ParallelFor::defaultThreadCount();

    if (srcs.empty()) {
        printf (USAGE, cpuName().c_str(), hw);
        return 1;
    }

    while (dsts.size() < srcs.size()) dsts.push_back (std::string());

    if (g_verbose) {
        printf ("mtnbli : %u hardware thread%s, %s\n",
                hw, hw==1?"":"s", g_threads ? "" : "(all are used by default)");
        fflush (stdout);
    }

    // ---------------- run
    std::vector<Job> jobs (srcs.size());
    for (size_t i=0; i<srcs.size(); i++) {
        jobs[i].src = srcs[i];
        jobs[i].dst = dsts[i];
    }

    double t_all0 = nowSec();

    ParallelFor::run (jobs.size(), g_threads, [&] (size_t i) {
        processFile (jobs[i]);
    });

    double t_all = nowSec() - t_all0;

    // ---------------- report
    // The numbers are formatted by hand with integer arithmetic and NOT with printf("%f"):
    // %f drags the C library's float -> string converter (gdtoa) into the binary, and mingw's
    // copy of it is built with BMI enabled, so the .exe would contain a TZCNT -- an instruction
    // a Phenom II does not have.  See tests/isa_check.sh.
    fmt_init ();
    int n_comp=0, n_dec=0, n_fail=0;
    for (size_t i=0; i<jobs.size(); i++) {
        Job &j = jobs[i];
        if (g_verbose) {
            if (jobs.size() > 1) printf ("(%zu/%zu) ", i+1, jobs.size());
            printf ("%s ", j.src.c_str());
            if (!j.ok) {
                printf ("*** FAILED : %s\n", j.note.c_str());
            } else if (j.kind == 0) {
                std::string n;
                fmt_fixed (n, (8.0*(double)j.bytes) / ((double)j.w*j.h), 5);  n += "  ";
                fmt_fixed (n, j.secs + 1e-9, 4);                              n += " s  ";
                fmt_rate  (n, (0.001*(double)((size_t)j.w*j.h*3)) / (j.secs + 1e-9)); n += " kB/s";
                printf ("(%ux%u) -> %llu bytes  BPP=%s%s%s\n",
                        j.w, j.h, (unsigned long long)j.bytes, n.c_str(),
                        j.crc ? "  CRC" : "", "");
            } else {
                std::string n;
                fmt_fixed (n, j.secs + 1e-9, 4);                              n += " s  ";
                fmt_rate  (n, (0.001*(double)((size_t)j.w*j.h*3)) / (j.secs + 1e-9)); n += " kB/s";
                printf ("(%llu bytes) -> %ux%u  %s%s\n",
                        (unsigned long long)j.bytes, j.w, j.h, n.c_str(),
                        j.crc ? "  CRC OK" : "");
            }
            fflush (stdout);
        }
        if (!j.ok) n_fail++;
        else if (j.kind == 0) n_comp++;
        else n_dec++;
    }

    if (g_verbose || jobs.size() > 1) {
        std::string n; fmt_fixed (n, t_all, 4);
        printf ("summary: %d compressed, %d decompressed, %d failed, %s s wall (%u thread%s)\n",
                n_comp, n_dec, n_fail, n.c_str(),
                g_threads ? g_threads : hw, ((g_threads?g_threads:hw)==1)?"":"s");
    }

    return n_fail;
}
