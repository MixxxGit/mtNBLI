#ifndef   __MT_DEFLATE_H__
#define   __MT_DEFLATE_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A tiny, dependency free deflate (RFC 1951) encoder wrapped in a zlib stream (RFC 1950).
//
// The data is fed in with mt_deflate_write() -- the encoder keeps a 64 KB sliding window, so it
// never needs the whole image in one block of memory.  Every time its output buffer is full it
// calls the sink; for a PNG that sink writes one IDAT chunk, which is why the sink exists at all.
//
// level 0 = stored (no compression), 1..9 = LZ77 + dynamic Huffman with an ever deeper
// hash chain, exactly like the levels of zlib / libpng.
typedef void (*defl_sink) (void *p_ctx, const void *p_data, size_t len);

typedef struct DeflateCtx DeflateCtx;

DeflateCtx *mt_deflate_new   (int level, defl_sink sink, void *p_sink_ctx);
int         mt_deflate_write (DeflateCtx *p_ctx, const uint8_t *p_data, size_t len);
int         mt_deflate_end   (DeflateCtx *p_ctx);
void        mt_deflate_free  (DeflateCtx *p_ctx);

#ifdef __cplusplus
}
#endif

#endif // __MT_DEFLATE_H__
