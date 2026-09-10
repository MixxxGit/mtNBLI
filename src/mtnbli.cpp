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
#include <atomic>
#include <mutex>

#ifndef _WIN32
#  include <glob.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#ifdef _WIN32
#  define  WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>          // CommandLineToArgvW : the only way to get argv in Unicode
#  include <io.h>                // _isatty : is stdout a console (for the status line)?
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
//------ UTF-8 <-> UTF-16 (Windows only : every path that leaves the program has to go through it)
#ifdef _WIN32
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
#endif

//------ size of a file, 0 if it does not exist (UTF-8 safe, >4 GB safe)
static uint64_t fileSize (const char *p_name) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW (utf8ToWide (p_name).c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t) d.nFileSizeHigh << 32) | (uint64_t) d.nFileSizeLow;
#else
    struct stat st;
    if (stat (p_name, &st) != 0) return 0;
    return (uint64_t) st.st_size;
#endif
}

//---------------------------------------------------------------------------- console output + progress
//  With -v and a queue of files the old code printed everything only after the last file was
//  done, which looks exactly like a hang.  Now every result is printed the moment it is ready
//  and a status line in front of it says what the workers are doing right now.
static std::mutex  g_out_mtx;
static bool        g_tty      = false;      // is stdout a console? -> the status line can be redrawn
static bool        g_prog_on  = false;      // a status line is currently on screen
static const int   PROG_WIDTH = 100;

static bool stdoutIsConsole () {
#ifdef _WIN32
    return _isatty (_fileno (stdout)) != 0;
#else
    return isatty (fileno (stdout)) != 0;
#endif
}

// wipe the status line so that a result line does not get appended to it
static void clearProgressLine () {
    if (g_tty && g_prog_on) {
        fputs ("\r", stdout);
        for (int i=0; i<PROG_WIDTH; i++) fputc (' ', stdout);
        fputs ("\r", stdout);
        g_prog_on = false;
    }
}

// print one finished line (thread safe, and never mixed into the status line)
static void emitLine (const std::string &s) {
    std::lock_guard<std::mutex> g (g_out_mtx);
    clearProgressLine ();
    fputs (s.c_str(), stdout);
    fputc ('\n', stdout);
    fflush (stdout);
}

// redraw the status line in place; on a redirected stdout it becomes a normal line, and then
// only every few seconds so that a log file does not fill up with it
static void emitProgress (const std::string &s) {
    std::lock_guard<std::mutex> g (g_out_mtx);
    if (g_tty) {
        std::string t = s;
        if ((int) t.size() > PROG_WIDTH) t.resize (PROG_WIDTH);
        fputs ("\r", stdout);
        fputs (t.c_str(), stdout);
        for (int i=(int) t.size(); i<PROG_WIDTH; i++) fputc (' ', stdout);
        fputs ("\r", stdout);
        g_prog_on = true;
    } else {
        fputs (s.c_str(), stdout);
        fputc ('\n', stdout);
    }
    fflush (stdout);
}

//--------------------------------------------------------------------------- number formatting
//  Still no printf("%f") : see the comment at the summary in main().
static void fmt_size (std::string &s, uint64_t bytes) {          // "1853.4 MB"
    static const uint64_t U[5] = { 1, 1024, 1024*1024, 1024*1024*1024, 1024ULL*1024*1024*1024 };
    static const char    *N[5] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    while (u < 4 && bytes >= 10*U[u+1]) u++;                     // keep two integer digits
    uint64_t ip = bytes / U[u];
    uint64_t fp = ((bytes - ip*U[u]) * 10 + U[u]/2) / U[u];      // tenths, rounded
    if (fp >= 10) { ip++; fp = 0; }
    char buf[64];
    if (u == 0) snprintf (buf, sizeof(buf), "%llu B",  (unsigned long long) ip);
    else        snprintf (buf, sizeof(buf), "%llu.%llu %s", (unsigned long long) ip,
                                                (unsigned long long) fp, N[u]);
    s += buf;
}

// signed percentage with one decimal, e.g. "-29.7"
static void fmt_percent (std::string &s, int64_t num, int64_t den) {
    if (den == 0) { s += "-"; return; }
    bool     neg = num < 0;
    uint64_t a   = neg ? (uint64_t)(-num) : (uint64_t) num;
    uint64_t t   = (a * 1000 + (uint64_t) den/2) / (uint64_t) den;   // tenths of a percent
    if (neg) s += '-';
    char buf[32];
    snprintf (buf, sizeof(buf), "%llu.%llu", (unsigned long long)(t/10), (unsigned long long)(t%10));
    s += buf;
}

// throughput in MB/s (decimal MB, like everybody else quotes it)
static void fmt_mbs (std::string &s, double bytes_per_sec) {
    uint64_t b = (uint64_t)(bytes_per_sec + 0.5);
    uint64_t t = b * 10 / 1000000ULL;                 // tenths of a MB/s
    char buf[64];
    snprintf (buf, sizeof(buf), "%llu.%llu MB/s", (unsigned long long)(t/10), (unsigned long long)(t%10));
    s += buf;
}

// file name without the directories (for the status line)
static std::string baseName (const std::string &p) {
    size_t s = p.find_last_of ("/\\");
    return (s == std::string::npos) ? p : p.substr (s+1);
}

// seconds as "mm:ss"
static void fmt_clock (std::string &s, double sec) {
    long t = (long)(sec + 0.5);
    long m = t / 60, ss = t % 60;
    char buf[32];
    snprintf (buf, sizeof(buf), "%02ld:%02ld", m, ss);
    s += buf;
}

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
    uint64_t    in_bytes;      // size of the source file (for the summary)
    int         rgb;           // 1 = RGB, 0 = gray, -1 = unknown
    double      secs;
    std::string note;
    Job () : ok(false), kind(-1), w(0), h(0), crc(0), bytes(0), in_bytes(0), rgb(-1), secs(0) {}
};

//--------------------------------------------------------------------------- one result line
//  printed the moment a file is finished, not after the whole batch (which looked like a hang)
static std::string formatJobLine (const Job &j, size_t idx, size_t n) {
    fmt_init ();
    std::string s;
    if (n > 1) { char b[64]; snprintf (b, sizeof(b), "(%zu/%zu) ", idx+1, n); s += b; }
    s += j.src;
    s += ' ';
    if (!j.ok) { s += "*** FAILED : "; s += j.note; return s; }

    std::string num;
    if (j.kind == 0) {
        char b[64];
        snprintf (b, sizeof(b), "(%ux%u) -> %llu bytes  BPP=", j.w, j.h, (unsigned long long) j.bytes);
        s += b;
        fmt_fixed (num, (j.w && j.h) ? (8.0*(double) j.bytes) / ((double) j.w * j.h) : 0.0, 5);
        num += "  ";
        fmt_fixed (num, j.secs + 1e-9, 4);  num += " s  ";
        fmt_rate  (num, (0.001 * (double)((size_t) j.w * j.h * 3)) / (j.secs + 1e-9)); num += " kB/s";
        s += num;
        if (j.crc) s += "  CRC";
    } else {
        char b[64];
        snprintf (b, sizeof(b), "(%llu bytes) -> %ux%u  ", (unsigned long long) j.bytes, j.w, j.h);
        s += b;
        fmt_fixed (num, j.secs + 1e-9, 4);  num += " s  ";
        fmt_rate  (num, (0.001 * (double)((size_t) j.w * j.h * 3)) / (j.secs + 1e-9)); num += " kB/s";
        s += num;
        if (j.crc) s += "  CRC OK";
    }
    return s;
}

//--------------------------------------------------------------------------- progress counters
//  tiles : a single 8K frame is one job but dozens of tiles, so "3/115 files" would sit still
//          for a minute.  Both counters are global because several files run at the same time.
static std::atomic<uint64_t> g_tile_done  (0);
static std::atomic<uint64_t> g_tile_total (0);
static std::atomic<uint64_t> g_pix_done   (0);   // raw pixels finished, for the throughput figure
static std::atomic<size_t>   g_jobs_done  (0);

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

        g_tile_total += ph->n_tiles;
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
            g_tile_done++;
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

    job.in_bytes = fileSize (job.src.c_str());

    int is_rgb_i = 0;
    uint32_t h = 0, w = 0;
    uint8_t *in_img = loadPNMImageFile (job.src.c_str(), &is_rgb_i, &h, &w);
    if (in_img == NULL) in_img = loadPNGImageFile (job.src.c_str(), &is_rgb_i, &h, &w);

    // ---------------------------------------------------------------- compress
    if (in_img != NULL) {
        bool   is_rgb = (bool) is_rgb_i;
        job.rgb = is_rgb ? 1 : 0;
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

        g_tile_total += n_tiles;
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
            g_tile_done++;
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

    job.w = w; job.h = h; job.crc = crc; job.kind = 1; job.bytes = len; job.rgb = is_rgb ? 1 : 0;

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

//  match one path component at a time, so  C:\a\*\b\*.png  works too
//  (utf8ToWide / wideToUtf8 live at the top of this file)
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

    g_tty = stdoutIsConsole ();

    // what every worker is doing right now : 0 = waiting, 1 = running, 2 = finished
    std::mutex       state_mtx;
    std::vector<int> state (jobs.size(), 0);
    std::atomic<size_t> n_done (0);
    std::atomic<bool>   halt (false);

    double t_all0 = nowSec();

    //----------------------------------------------------------------------- live progress
    //  The old code printed every result only after the last file was finished, which with
    //  115 images looked exactly like a hang.  Now the results are printed as they come in
    //  and a status line in front of them names the files that are being worked on.
    std::thread reporter ([&] () {
        double last = -1e9;
        if (!g_verbose) return;             // no -v : nothing is printed until the summary
        for (;;) {
            for (int k=0; k<10 && !halt; k++)                  // 10 x 20 ms, then refresh
                std::this_thread::sleep_for (std::chrono::milliseconds (20));
            if (halt) break;

            double el = nowSec() - t_all0;
            size_t d  = n_done;
            size_t n  = jobs.size();

            std::string s;
            s += "["; fmt_clock (s, el); s += "] ";
            { char b[64]; snprintf (b, sizeof(b), "%zu/%zu (%zu%%)", d, n, n ? (100*d)/n : 0); s += b; }

            if (d > 0 && d < n) {                              // eta from the average so far
                s += "  eta ";
                fmt_clock (s, el * (double)(n-d) / (double) d);
            }
            uint64_t td = g_tile_done, tt = g_tile_total;
            if (tt > 0) { char b[64]; snprintf (b, sizeof(b), "  tiles %llu/%llu",
                                                (unsigned long long) td, (unsigned long long) tt); s += b; }
            if (d > 0) {
                s += "  ";
                fmt_rate (s, (0.001 * (double) g_pix_done) / (el + 1e-9));
                s += " kB/s";
            }

            std::vector<size_t> run;
            {   std::lock_guard<std::mutex> g (state_mtx);
                for (size_t i=0; i<n; i++) if (state[i] == 1) run.push_back (i);
            }
            if (!run.empty()) {
                s += "  | now: ";
                for (size_t k=0; k<run.size() && k<2; k++) {
                    if (k) s += ", ";
                    s += baseName (jobs[run[k]].src);
                }
                if (run.size() > 2) { char b[32]; snprintf (b, sizeof(b), " +%zu more", run.size()-2); s += b; }
            }

            if (g_tty || el - last >= 5.0) { emitProgress (s); last = el; }   // do not flood a log
        }
    });

    ParallelFor::run (jobs.size(), g_threads, [&] (size_t i) {
        { std::lock_guard<std::mutex> g (state_mtx); state[i] = 1; }
        processFile (jobs[i]);
        { std::lock_guard<std::mutex> g (state_mtx); state[i] = 2; }
        n_done++;
        if (jobs[i].ok)
            g_pix_done += (uint64_t) jobs[i].w * jobs[i].h * (jobs[i].rgb == 0 ? 1 : 3);
        if (g_verbose || !jobs[i].ok)                     // a failure is always reported
            emitLine (formatJobLine (jobs[i], i, jobs.size()));
    });

    halt = true;
    reporter.join();
    { std::lock_guard<std::mutex> g (g_out_mtx); clearProgressLine (); }

    double t_all = nowSec() - t_all0;

    // ---------------- report
    // The numbers are formatted by hand with integer arithmetic and NOT with printf("%f"):
    // %f drags the C library's float -> string converter (gdtoa) into the binary, and mingw's
    // copy of it is built with BMI enabled, so the .exe would contain a TZCNT -- an instruction
    // a Phenom II does not have.  See tests/isa_check.sh.
    fmt_init ();
    int      n_comp=0, n_dec=0, n_fail=0;
    uint64_t c_in=0, c_out=0, d_in=0, d_out=0;
    double   cpu_c=0, cpu_d=0, cpu_all=0;
    uint64_t c_pix=0;
    double   bpp_min=1e30, bpp_max=-1e30;
    std::string bpp_min_name, bpp_max_name;

    for (size_t i=0; i<jobs.size(); i++) {
        Job &j = jobs[i];
        cpu_all += j.secs;
        if (!j.ok) { n_fail++; continue; }
        uint64_t pix = (uint64_t) j.w * j.h;
        if (j.kind == 0) {
            n_comp++; c_in += j.in_bytes; c_out += j.bytes; c_pix += pix; cpu_c += j.secs;
            if (pix) {
                double bpp = 8.0 * (double) j.bytes / (double) pix;
                if (bpp < bpp_min) { bpp_min = bpp; bpp_min_name = j.src; }
                if (bpp > bpp_max) { bpp_max = bpp; bpp_max_name = j.src; }
            }
        } else {
            n_dec++; d_in += j.bytes; d_out += pix * (j.rgb == 0 ? 1 : 3); cpu_d += j.secs;
        }
    }

    unsigned nthr = g_threads ? g_threads : hw;

    if (g_verbose || jobs.size() > 1) {
        std::string n; fmt_fixed (n, t_all, 4);
        printf ("summary: %d compressed, %d decompressed, %d failed, %s s wall (%u thread%s)\n",
                n_comp, n_dec, n_fail, n.c_str(), nthr, nthr==1?"":"s");

        if (n_comp + n_dec > 0) {
            printf ("----------------------------------------------------------------------------------\n");

            if (n_comp) {
                std::string a, b, p, mb, sp;
                fmt_size (a, c_in); fmt_size (b, c_out);
                fmt_percent (p, (int64_t) c_out - (int64_t) c_in, (int64_t) c_in);
                fmt_mbs (mb, (double) c_pix / (t_all + 1e-9));
                fmt_fixed (sp, cpu_c / (t_all + 1e-9), 2);
                printf ("compressed   : %d file%s, %s -> %s   (%s %%, saved ",
                        n_comp, n_comp==1?"":"s", a.c_str(), b.c_str(), p.c_str());
                std::string sv; fmt_size (sv, c_in > c_out ? c_in - c_out : 0);
                printf ("%s)\n", sv.c_str());
                if (c_pix) {
                    std::string bp; fmt_fixed (bp, 8.0 * (double) c_out / (double) c_pix, 4);
                    printf ("               average %s BPP", bp.c_str());
                    if (!bpp_min_name.empty()) {
                        std::string lo, hi; fmt_fixed (lo, bpp_min, 4); fmt_fixed (hi, bpp_max, 4);
                        printf ("   best %s (%s)   worst %s (%s)\n",
                                lo.c_str(), baseName(bpp_min_name).c_str(),
                                hi.c_str(), baseName(bpp_max_name).c_str());
                    } else printf ("\n");
                }
                printf ("               %s, %sx on %u thread%s (%.1f s of work in ",
                        mb.c_str(), sp.c_str(), nthr, nthr==1?"":"s", cpu_c);
                std::string tl; fmt_fixed (tl, t_all, 2);
                printf ("%s s)\n", tl.c_str());
            }

            if (n_dec) {
                std::string a, b, p, mb;
                fmt_size (a, d_in); fmt_size (b, d_out);
                fmt_percent (p, (int64_t) d_in, (int64_t) d_out);          // stream / raw pixels
                fmt_mbs (mb, (double) d_out / (t_all + 1e-9));
                printf ("decompressed : %d file%s, %s of streams -> %s of pixels   (streams are %s %% of the raw image)\n",
                        n_dec, n_dec==1?"":"s", a.c_str(), b.c_str(), p.c_str());
                printf ("               %s\n", mb.c_str());
            }

            if (n_fail) {
                printf ("failed       : %d\n", n_fail);
                for (size_t i=0; i<jobs.size(); i++)
                    if (!jobs[i].ok) printf ("               %s : %s\n",
                                             baseName (jobs[i].src).c_str(), jobs[i].note.c_str());
            }
        }
    }

    return n_fail;
}
