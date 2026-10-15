/* Requested-byte accounting, including table-building temporaries.
 * Compile with -I pointing to the selected fluff.c, then -Isrc/core.
 * Caller buffers and this diagnostic's bookkeeping are excluded. C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fluff.h"

typedef struct { void *ptr; size_t bytes; } allocation_t;
static allocation_t allocations[64];
static size_t live_bytes, peak_bytes;

static void *track_alloc(bp_uint32 bytes)
{
    unsigned int i;
    void *ptr = malloc(bytes);
    if (!ptr) exit(2);
    for (i = 0; i < 64; i++) if (!allocations[i].ptr) {
        allocations[i].ptr = ptr;
        allocations[i].bytes = bytes;
        live_bytes += bytes;
        if (live_bytes > peak_bytes) peak_bytes = live_bytes;
        return ptr;
    }
    exit(2);
    return NULL;
}

static void track_free(void *ptr)
{
    unsigned int i;
    if (!ptr) return;
    for (i = 0; i < 64; i++) if (allocations[i].ptr == ptr) {
        live_bytes -= allocations[i].bytes;
        allocations[i].ptr = NULL;
        free(ptr);
        return;
    }
    fputs("untracked free\n", stderr);
    exit(2);
}

/* Older sources bypass the allocator hook for calloc; intercept it as well. */
static void *track_calloc(size_t count, size_t size)
{
    void *ptr = track_alloc((bp_uint32)(count * size));
    memset(ptr, 0, count * size);
    return ptr;
}
#ifdef FLUFF_ACCOUNT_LEGACY_CALLOC
#define calloc track_calloc
#endif
#include "fluff.c"
#undef malloc
#undef calloc
#undef free

int main(void)
{
    bp_uint8 *src = (bp_uint8 *)malloc(262144);
    bp_uint8 *dst = (bp_uint8 *)malloc(fluff_compress_bound(262144));
    fluff_ctx_t *ctx;
    bp_uint32 i, len, compressed_len = 0;
    int mode;
    if (!src || !dst) return 2;
    for (i = 0; i < 262144; i++) src[i] = (bp_uint8)(i % 251);
    fluff_set_allocators(track_alloc, track_free);
    puts("mode,context_bytes,match_table_bytes,scratch_bytes,persistent_bytes,peak_bytes,after_free_bytes");
    for (mode = 0; mode < 3; mode++) {
        size_t persistent, peak, tables;
        peak_bytes = 0;
        ctx = fluff_ctx_alloc();
        if (!ctx) return 2;
        if (mode == 1) {
            len = 262144;
            fluff_ctx_reset_decode(ctx);
            /* The preceding encoder left its compressed stream in dst. */
            if (fluff_decompress_block_ctx(dst, compressed_len, src, &len, ctx)) return 2;
        } else {
            fluff_ctx_reset_level(ctx, 8);
            if (mode == 2) {
                len = fluff_compress_bound(262143);
                if (fluff_compress_block_ctx(src, 262143, dst, &len, ctx)) return 2;
            }
            len = fluff_compress_bound(262144);
            if (fluff_compress_block_ctx(src, 262144, dst, &len, ctx)) return 2;
            compressed_len = len;
        }
        persistent = live_bytes;
        peak = peak_bytes;
        tables = mode == 1 ? 0 : (FLUFF_HASH_SIZE + FLUFF_WINDOW_SIZE + FLUFF_H3_SIZE) * sizeof(fl_u32);
        fluff_ctx_free(ctx);
        printf("%s,%lu,%lu,%lu,%lu,%lu,%lu\n",
            mode == 0 ? "encode" : mode == 1 ? "decode" : "encode_growth",
            (unsigned long)sizeof(*ctx), (unsigned long)tables,
            (unsigned long)(persistent - sizeof(*ctx) - tables),
            (unsigned long)persistent, (unsigned long)peak, (unsigned long)live_bytes);
    }
    fluff_set_allocators(NULL, NULL);
    free(src); free(dst);
    return live_bytes ? 1 : 0;
}
