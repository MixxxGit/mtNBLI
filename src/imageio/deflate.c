//==================================================================================================
//  deflate.c : a small, portable deflate (RFC 1951) encoder with a zlib wrapper (RFC 1950)
//
//  Why not simply link zlib?
//    * mtnbli is built for old CPUs (AMD Phenom II X6 1055T and friends).  The binary must not
//      contain SSSE3 / SSE4 / AVX / BMI instructions (see tests/isa_check.sh).  A distribution
//      libz is compiled for a much newer baseline, and the mingw copy that ships with the cross
//      toolchain is built with BMI enabled -- its TZCNT alone breaks the promise.
//    * the codec has to stay a handful of source files with no external dependency.
//
//  What the levels mean (the same idea as zlib / libpng, the numbers are our own):
//      0     : stored blocks, no compression at all
//    1 .. 3  : greedy matching, no lazy matching, short hash chains
//    4 .. 9  : lazy matching with an ever deeper hash chain
//
//  Memory : ~700 KB per encoder, independent of the image size.  The encoder only keeps a
//  64 KB sliding window, so an 8K RGB frame (100 MB) does not need a second 100 MB buffer.
//==================================================================================================
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "deflate.h"

#define  DEFL_WBITS       15
#define  DEFL_WSIZE       (1u << DEFL_WBITS)          // 32768 : deflate's largest distance
#define  DEFL_RINGBITS    16
#define  DEFL_RINGSIZE    (1u << DEFL_RINGBITS)       // 65536 : > WSIZE + MAX_MATCH, no wrap
#define  DEFL_RINGMASK    (DEFL_RINGSIZE - 1u)
#define  DEFL_MIN_MATCH   3
#define  DEFL_MAX_MATCH   258
#define  DEFL_MAX_DIST    DEFL_WSIZE
#define  DEFL_HBITS       15
#define  DEFL_HSIZE       (1u << DEFL_HBITS)
#define  DEFL_NIL         0xFFFFFFFFu
#define  DEFL_TOKMAX      16384                       // tokens per deflate block
#define  DEFL_OUTCAP      (128u * 1024u)
#define  DEFL_OUTFLUSH    (64u  * 1024u)              // hand the sink a chunk of this size
#define  DEFL_CHUNK       (16u  * 1024u)              // bytes pulled in per round
#define  DEFL_LCODES      286                         // literal / length alphabet
#define  DEFL_DCODES      30                          // distance alphabet
#define  DEFL_BLCODES     19                          // code length alphabet

// level -> hash chain depth / "good enough" length / lazy matching threshold
static const uint32_t CFG_CHAIN[10] = {   0,   4,   8,  32,  16,   32,  128,  256,  512, 1024 };
static const uint32_t CFG_NICE [10] = {   0,   8,  16,  32,  16,   32,  128,  256,  258,  258 };
static const uint32_t CFG_LAZY [10] = {   0,   0,   0,   0,   4,    8,    8,    8,   32,   32 };

static const uint16_t LEN_BASE [29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t  LEN_EXTRA[29] = {
    0,0,0,0,0,0,0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,  4,  5,  5,  5,  5,  0
};
static const uint16_t DIST_BASE [30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t  DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};
// the order in which the code lengths of the code-length alphabet are transmitted
static const uint8_t  BL_ORDER[DEFL_BLCODES] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

typedef struct {
    uint16_t sym;          // literal byte            (len == 0)
    uint16_t len;          // match length 3..258     (len != 0)
    uint16_t dist;         // match distance 1..32768
} DeflTok;

struct DeflateCtx {
    int        level;
    defl_sink  sink;
    void      *p_sink_ctx;

    uint8_t   *p_out;      size_t   out_len;
    uint32_t   bitbuf;     int      bitcnt;
    uint32_t   adler_a, adler_b;

    uint8_t   *p_ring;                       // sliding window, DEFL_RINGSIZE bytes
    uint32_t  *p_head;                       // DEFL_HSIZE : newest position of a hash
    uint32_t  *p_prev;                       // DEFL_RINGSIZE : previous position, same hash
    uint64_t   pos, end;                     // consumed / fed, absolute stream positions

    uint32_t   chain, nice, lazy;
    int        pre_ins;                          // the next position is already in the hash

    DeflTok   *p_tok;      int      ntok;
    uint32_t   lfreq[DEFL_LCODES];
    uint32_t   dfreq[DEFL_DCODES];
};

//======================================================================================= bit output

static void out_byte (DeflateCtx *c, unsigned b) {
    if (c->out_len >= DEFL_OUTCAP)
        c->sink (c->p_sink_ctx, c->p_out, c->out_len), c->out_len = 0;
    c->p_out[c->out_len++] = (uint8_t) b;
}

static void flush_out (DeflateCtx *c) {
    if (c->out_len) { c->sink (c->p_sink_ctx, c->p_out, c->out_len); c->out_len = 0; }
}

// deflate packs bits into the stream least significant bit first
static void put_bits (DeflateCtx *c, uint32_t v, int n) {
    c->bitbuf |= (v & ((1u << n) - 1u)) << c->bitcnt;
    c->bitcnt += n;
    while (c->bitcnt >= 8) {
        out_byte (c, c->bitbuf & 0xFF);
        c->bitbuf >>= 8;
        c->bitcnt  -= 8;
    }
}

// pad with zeros up to the next byte boundary (needed before a stored block and at the end)
static void flush_bits (DeflateCtx *c) {
    while (c->bitcnt >= 8) {
        out_byte (c, c->bitbuf & 0xFF);
        c->bitbuf >>= 8;
        c->bitcnt  -= 8;
    }
    if (c->bitcnt) { out_byte (c, c->bitbuf & 0xFF); c->bitbuf = 0; c->bitcnt = 0; }
}

// huffman codes are stored most significant bit first, so they are reversed once, here
static unsigned rev_bits (unsigned code, int n) {
    unsigned r = 0;
    while (n--) { r = (r << 1) | (code & 1); code >>= 1; }
    return r;
}

//========================================================================================== adler32

static void adler_add (DeflateCtx *c, const uint8_t *p, size_t n) {
    uint32_t a = c->adler_a, b = c->adler_b;
    while (n) {
        size_t k = (n < 5552) ? n : 5552;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u;  b %= 65521u;
    }
    c->adler_a = a;  c->adler_b = b;
}

//=================================================================================== huffman coding
//  Builds a length limited canonical Huffman code (RFC 1951 section 3.2.2).
//  If the plain Huffman tree would be deeper than max_len the frequencies are flattened and the
//  tree is rebuilt; every round halves the spread, so the loop always terminates (with all
//  frequencies equal to 1 the tree is balanced and roughly log2(n) deep).
typedef struct { uint32_t w; int s; } DeflWS;

static int cmp_ws (const void *A, const void *B) {
    const DeflWS *a = (const DeflWS*) A, *b = (const DeflWS*) B;
    if (a->w != b->w) return (a->w < b->w) ? -1 : 1;
    return a->s - b->s;
}

static void huff_build (const uint32_t *p_freq, int n, int max_len, uint8_t *p_lens, uint16_t *p_codes)
{
    uint32_t  f[DEFL_LCODES];
    uint32_t  w[600];  int lc[600], rc[600];
    uint32_t  bl_count[16], next_code[16];
    uint8_t   depth[DEFL_LCODES];
    DeflWS    arr[DEFL_LCODES];
    int       i, m, iter, mx;

    memset (depth, 0, n);
    for (i=0; i<n; i++) f[i] = p_freq[i];

    for (iter=0; iter<40 && n > 0; iter++) {
        m = 0;
        for (i=0; i<n; i++) if (f[i]) { arr[m].w = f[i]; arr[m].s = i; m++; }
        if (m == 0) break;

        if (m == 1) {                                     // a single symbol still needs 1 bit
            depth[arr[0].s] = 1;
            break;
        }

        qsort (arr, (size_t) m, sizeof (DeflWS), cmp_ws);

        {   // two queue Huffman : the leaves are sorted, the merges come out sorted as well
            int qa = 0, qb = m, nn = m, root, x, y;
            for (i=0; i<m; i++) { w[i] = arr[i].w; lc[i] = -1; rc[i] = arr[i].s; }
            while ((m - qa) + (nn - qb) > 1) {
                if (qa < m && (qb >= nn || w[qa] <= w[qb])) x = qa++; else x = qb++;
                if (qa < m && (qb >= nn || w[qa] <= w[qb])) y = qa++; else y = qb++;
                w[nn] = w[x] + w[y];  lc[nn] = x;  rc[nn] = y;  nn++;
            }
            root = (qa < m) ? qa : qb;

            {   int sn[1200], sd[1200], sp = 0;
                sn[sp] = root; sd[sp] = 0; sp++;
                while (sp) {
                    sp--;
                    int nd = sn[sp], d = sd[sp];
                    if (lc[nd] < 0) depth[rc[nd]] = (uint8_t) (d ? d : 1);
                    else {
                        sn[sp] = lc[nd]; sd[sp] = d+1; sp++;
                        sn[sp] = rc[nd]; sd[sp] = d+1; sp++;
                    }
                }
            }
        }

        mx = 0;
        for (i=0; i<n; i++) if (f[i] && depth[i] > mx) mx = depth[i];
        if (mx <= max_len) break;

        for (i=0; i<n; i++) if (f[i] > 1) f[i] >>= 1;      // flatten and try again
    }

    // canonical codes
    for (i=0; i<16; i++) bl_count[i] = 0;
    for (i=0; i<n; i++) { if (depth[i] > max_len) depth[i] = (uint8_t) max_len;
                          if (depth[i]) bl_count[depth[i]]++; }
    {   uint32_t code = 0;
        for (i=1; i<=max_len; i++) { code = (code + bl_count[i-1]) << 1; next_code[i] = code; }
    }
    for (i=0; i<n; i++) {
        int l = depth[i];
        if (l) { p_codes[i] = (uint16_t) rev_bits (next_code[l]++, l); p_lens[i] = (uint8_t) l; }
        else   { p_codes[i] = 0; p_lens[i] = 0; }
    }
}

//===================================================================================== match finder

static void defl_insert (DeflateCtx *c, unsigned ci) {
    uint32_t x = ((uint32_t) c->p_ring[ci] << 16) |
                 ((uint32_t) c->p_ring[(ci + 1) & DEFL_RINGMASK] << 8) |
                 ((uint32_t) c->p_ring[(ci + 2) & DEFL_RINGMASK]);
    unsigned h = (unsigned) ((x * 2654435761u) >> (32 - DEFL_HBITS));
    c->p_prev[ci] = c->p_head[h];
    c->p_head[h]  = ci;
}

// longest match at ring index ci.  returns 0 when there is none, else the length (3..258)
static unsigned longest_match (DeflateCtx *c, unsigned ci, unsigned max_len, unsigned *p_dist)
{
    const uint8_t *r = c->p_ring;
    unsigned cur      = c->p_prev[ci];
    unsigned best_len = 0, best_dist = 0;
    unsigned chain    = c->chain;

    while (cur != DEFL_NIL && chain--) {
        unsigned d = (unsigned) ((ci - cur) & DEFL_RINGMASK);
        if (d == 0 || d > DEFL_MAX_DIST) break;              // too far back, the chain is over

        if (r[(cur + best_len) & DEFL_RINGMASK] == r[(ci + best_len) & DEFL_RINGMASK] &&
            r[ cur                            ] == r[ ci                            ] &&
            r[(cur + 1       ) & DEFL_RINGMASK] == r[(ci + 1       ) & DEFL_RINGMASK]) {
            unsigned l = 0;
            while (l < max_len && r[(cur + l) & DEFL_RINGMASK] == r[(ci + l) & DEFL_RINGMASK]) l++;
            if (l > best_len) {
                best_len = l;  best_dist = d;
                if (l >= c->nice || l >= max_len) break;
            }
        }
        cur = c->p_prev[cur];
    }
    if (best_len >= DEFL_MIN_MATCH) { *p_dist = best_dist; return best_len; }
    return 0;
}

//========================================================================================= tokenizer

static void emit_tok (DeflateCtx *c, unsigned sym, unsigned len, unsigned dist) {
    DeflTok *t = &c->p_tok[c->ntok++];
    t->sym  = (uint16_t) sym;
    t->len  = (uint16_t) len;
    t->dist = (uint16_t) dist;
}

// skip over a match and hash every position it covers, starting at offset ins_from
static void defl_skip (DeflateCtx *c, unsigned len, unsigned dist, unsigned ins_from) {
    unsigned k;
    for (k=ins_from; k<len; k++) {
        uint64_t p = c->pos + k;
        if (p + 3 <= c->end) defl_insert (c, (unsigned) (p & DEFL_RINGMASK));
    }
    emit_tok (c, 0, len, dist);
    c->pos += len;
}

static void defl_step (DeflateCtx *c) {
    uint64_t pos     = c->pos;
    unsigned ci      = (unsigned) (pos & DEFL_RINGMASK);
    unsigned max_len = (unsigned) (c->end - pos);
    unsigned len = 0, dist = 0;
    int      hashed = c->pre_ins;

    c->pre_ins = 0;
    if (max_len > DEFL_MAX_MATCH) max_len = DEFL_MAX_MATCH;

    if (c->level > 0 && max_len >= DEFL_MIN_MATCH) {
        if (!hashed) defl_insert (c, ci);
        len = longest_match (c, ci, max_len, &dist);

        if (c->lazy && len >= DEFL_MIN_MATCH && len < c->lazy && max_len > DEFL_MIN_MATCH) {
            // "lazy" : a longer match may start one byte later
            unsigned ci1  = (unsigned) ((pos + 1) & DEFL_RINGMASK);
            unsigned m2   = max_len - 1;
            unsigned l2   = 0, d2 = 0;
            defl_insert (c, ci1);
            if (m2 >= DEFL_MIN_MATCH) l2 = longest_match (c, ci1, m2, &d2);
            if (l2 > len) {
                emit_tok (c, c->p_ring[ci], 0, 0);          // the byte at pos stays a literal
                c->pos = pos + 1;
                defl_skip (c, l2, d2, 1);                   // pos+1 is hashed already
                return;
            }
            if (len >= DEFL_MIN_MATCH) { defl_skip (c, len, dist, 2); return; }
            c->pre_ins = 1;                                 // pos+1 is hashed and comes next
        }
    }

    if (len >= DEFL_MIN_MATCH) { defl_skip (c, len, dist, 1); return; }

    emit_tok (c, c->p_ring[ci], 0, 0);
    c->pos = pos + 1;
}

//=================================================================================== block emission

static void defl_block (DeflateCtx *c, int last)
{
    uint8_t   ll_lens[DEFL_LCODES], d_lens[DEFL_DCODES];
    uint16_t  ll_code[DEFL_LCODES], d_code[DEFL_DCODES];
    uint8_t   all_lens[DEFL_LCODES + DEFL_DCODES];
    uint8_t   bl_sym[640], bl_extra[640];
    uint8_t   bl_lens[DEFL_BLCODES];
    uint16_t  bl_code[DEFL_BLCODES];
    uint32_t  bl_freq[DEFL_BLCODES];
    int       i, n_all, n_seq, num_lit, num_dst, num_bl;

    memset (c->lfreq, 0, sizeof (c->lfreq));
    memset (c->dfreq, 0, sizeof (c->dfreq));
    for (i=0; i<c->ntok; i++) {
        const DeflTok *t = &c->p_tok[i];
        if (t->len) {
            int li = 0, di = 0;
            while (li < 28 && t->len >= LEN_BASE[li+1]) li++;
            while (di < 29 && t->dist >= DIST_BASE[di+1]) di++;
            c->lfreq[257 + li]++;
            c->dfreq[di]++;
        } else {
            c->lfreq[t->sym]++;
        }
    }
    c->lfreq[256] = 1;                       // end of block
    c->dfreq[0]  |= 1u;                      // a stream always carries at least one distance code

    huff_build (c->lfreq, DEFL_LCODES, 15, ll_lens, ll_code);
    huff_build (c->dfreq, DEFL_DCODES, 15, d_lens,  d_code);

    num_lit = DEFL_LCODES;  while (num_lit > 257 && ll_lens[num_lit-1] == 0) num_lit--;
    num_dst = DEFL_DCODES;  while (num_dst > 1   && d_lens [num_dst-1] == 0) num_dst--;

    n_all = 0;
    memcpy (all_lens + n_all, ll_lens, (size_t) num_lit); n_all += num_lit;
    memcpy (all_lens + n_all, d_lens,  (size_t) num_dst); n_all += num_dst;

    // run length encode the code lengths
    n_seq = 0;
    for (i=0; i<n_all; ) {
        unsigned l   = all_lens[i];
        unsigned cnt = 1;
        while (i + cnt < n_all && all_lens[i + cnt] == l) cnt++;
        if (l == 0) {
            while (cnt >= 11) { unsigned r = (cnt > 138) ? 138 : cnt;
                                bl_sym[n_seq] = 18; bl_extra[n_seq++] = (uint8_t)(r - 11); cnt -= r; }
            while (cnt >=  3) { unsigned r = (cnt >  10) ?  10 : cnt;
                                bl_sym[n_seq] = 17; bl_extra[n_seq++] = (uint8_t)(r -  3); cnt -= r; }
            while (cnt--)     { bl_sym[n_seq] =  0; bl_extra[n_seq++] = 0; }
        } else {
            bl_sym[n_seq] = (uint8_t) l; bl_extra[n_seq++] = 0; cnt--;
            while (cnt >= 3)  { unsigned r = (cnt > 6) ? 6 : cnt;
                                bl_sym[n_seq] = 16; bl_extra[n_seq++] = (uint8_t)(r - 3); cnt -= r; }
            while (cnt--)     { bl_sym[n_seq] = (uint8_t) l; bl_extra[n_seq++] = 0; }
        }
        i += 1;
        while (i < n_all && all_lens[i] == l) i++;       // the run we just encoded
    }

    memset (bl_freq, 0, sizeof (bl_freq));
    for (i=0; i<n_seq; i++) bl_freq[bl_sym[i]]++;
    huff_build (bl_freq, DEFL_BLCODES, 7, bl_lens, bl_code);

    num_bl = DEFL_BLCODES;
    while (num_bl > 4 && bl_lens[BL_ORDER[num_bl-1]] == 0) num_bl--;

    // ---- block header
    put_bits (c, last ? 1u : 0u, 1);
    put_bits (c, 2u, 2);                                  // BTYPE = 10 : dynamic huffman
    put_bits (c, (uint32_t) (num_lit - 257), 5);
    put_bits (c, (uint32_t) (num_dst -   1), 5);
    put_bits (c, (uint32_t) (num_bl  -   4), 4);
    for (i=0; i<num_bl; i++) put_bits (c, bl_lens[BL_ORDER[i]], 3);

    // ---- the code lengths
    for (i=0; i<n_seq; i++) {
        unsigned s = bl_sym[i];
        put_bits (c, bl_code[s], bl_lens[s]);
        if      (s == 16) put_bits (c, bl_extra[i], 2);
        else if (s == 17) put_bits (c, bl_extra[i], 3);
        else if (s == 18) put_bits (c, bl_extra[i], 7);
    }

    // ---- the tokens
    for (i=0; i<c->ntok; i++) {
        const DeflTok *t = &c->p_tok[i];
        if (t->len == 0) {
            put_bits (c, ll_code[t->sym], ll_lens[t->sym]);
        } else {
            int li = 0, di = 0;
            while (li < 28 && t->len  >= LEN_BASE [li+1]) li++;
            while (di < 29 && t->dist >= DIST_BASE[di+1]) di++;
            put_bits (c, ll_code[257+li], ll_lens[257+li]);
            put_bits (c, (uint32_t)(t->len  - LEN_BASE [li]), LEN_EXTRA [li]);
            put_bits (c, d_code[di], d_lens[di]);
            put_bits (c, (uint32_t)(t->dist - DIST_BASE[di]), DIST_EXTRA[di]);
        }
        if (c->out_len >= DEFL_OUTFLUSH) flush_out (c);
    }
    put_bits (c, ll_code[256], ll_lens[256]);             // end of block
    c->ntok = 0;
}

// a stored (uncompressed) deflate block
static void defl_stored (DeflateCtx *c, const uint8_t *p_data, size_t len, int last)
{
    do {
        size_t n = (len > 65535) ? 65535 : len;
        flush_bits (c);
        put_bits (c, (last && n == len) ? 1u : 0u, 1);
        put_bits (c, 0u, 2);                              // BTYPE = 00 : stored
        flush_bits (c);
        put_bits (c, (uint32_t) n, 16);
        put_bits (c, (uint32_t) ((~n) & 0xFFFF), 16);
        flush_bits (c);
        while (n) {
            size_t room = DEFL_OUTCAP - c->out_len;
            size_t k    = (n < room) ? n : room;
            memcpy (c->p_out + c->out_len, p_data, k);
            c->out_len += k;  p_data += k;  n -= k;
            if (c->out_len >= DEFL_OUTFLUSH) flush_out (c);
        }
        if (len > 65535) len -= 65535; else len = 0;
    } while (len);
}

//======================================================================================== public API

DeflateCtx *mt_deflate_new (int level, defl_sink sink, void *p_sink_ctx)
{
    DeflateCtx *c;
    unsigned    cmf, flg, flevel;

    if (level < 0) level = 0;
    if (level > 9) level = 9;

    c = (DeflateCtx*) calloc (1, sizeof (DeflateCtx));
    if (c == NULL) return NULL;

    c->p_out  = (uint8_t*)  malloc (DEFL_OUTCAP);
    c->p_ring = (uint8_t*)  malloc (DEFL_RINGSIZE);
    c->p_head = (uint32_t*) malloc (DEFL_HSIZE * sizeof (uint32_t));
    c->p_prev = (uint32_t*) malloc (DEFL_RINGSIZE * sizeof (uint32_t));
    c->p_tok  = (DeflTok*)  malloc (DEFL_TOKMAX * sizeof (DeflTok));

    if (!c->p_out || !c->p_ring || !c->p_head || !c->p_prev || !c->p_tok) {
        mt_deflate_free (c);
        return NULL;
    }

    c->level      = level;
    c->sink       = sink;
    c->p_sink_ctx = p_sink_ctx;
    c->adler_a    = 1;
    c->adler_b    = 0;
    c->chain      = CFG_CHAIN[level];
    c->nice       = CFG_NICE [level];
    c->lazy       = CFG_LAZY [level];

    for (size_t i=0; i<DEFL_HSIZE;    i++) c->p_head[i] = DEFL_NIL;
    for (size_t i=0; i<DEFL_RINGSIZE; i++) c->p_prev[i] = DEFL_NIL;

    // zlib header : CM = 8 (deflate), CINFO = 7 (32 KB window), FCHECK makes it a multiple of 31
    flevel = (level == 0) ? 0u : ((level < 6) ? ((level < 2) ? 0u : 1u) : ((level == 6) ? 2u : 3u));
    cmf    = 0x78u;
    flg    = (flevel << 6) | ((31u - (((cmf << 8) | (flevel << 6)) % 31u)) % 31u);
    out_byte (c, cmf);
    out_byte (c, flg);
    return c;
}

void mt_deflate_free (DeflateCtx *c) {
    if (c == NULL) return;
    free (c->p_out);  free (c->p_ring);  free (c->p_head);
    free (c->p_prev); free (c->p_tok);   free (c);
}

int mt_deflate_write (DeflateCtx *c, const uint8_t *p_data, size_t len)
{
    if (c == NULL || (len && p_data == NULL)) return 1;
    if (len == 0) return 0;

    adler_add (c, p_data, len);

    if (c->level == 0) {
        defl_stored (c, p_data, len, 0);
        return 0;
    }

    while (len) {
        size_t   k  = (len > DEFL_CHUNK) ? DEFL_CHUNK : len;
        size_t   i0 = (size_t) (c->end & DEFL_RINGMASK);
        size_t   kk = k;
        if (i0 + kk > DEFL_RINGSIZE) kk = DEFL_RINGSIZE - i0;
        memcpy (c->p_ring + i0, p_data, kk);
        if (kk < k) memcpy (c->p_ring, p_data + kk, k - kk);

        c->end += k;
        p_data += k;
        len    -= k;

        // keep DEFL_MAX_MATCH bytes of lookahead : every emitted match may then reach full length
        while (c->pos + DEFL_MAX_MATCH <= c->end) {
            defl_step (c);
            if (c->ntok >= DEFL_TOKMAX) defl_block (c, 0);
        }
    }
    return 0;
}

int mt_deflate_end (DeflateCtx *c)
{
    uint32_t s;
    if (c == NULL) return 1;

    if (c->level == 0) {
        defl_stored (c, NULL, 0, 1);                 // an empty final block is legal
    } else {
        while (c->pos < c->end) {
            defl_step (c);
            if (c->ntok >= DEFL_TOKMAX) defl_block (c, 0);
        }
        defl_block (c, 1);
        flush_bits (c);
    }

    s = (c->adler_b << 16) | c->adler_a;             // big endian, as RFC 1950 wants it
    out_byte (c, (s >> 24) & 0xFF);
    out_byte (c, (s >> 16) & 0xFF);
    out_byte (c, (s >>  8) & 0xFF);
    out_byte (c, (s      ) & 0xFF);
    flush_out (c);
    return 0;
}
