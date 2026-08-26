/*
 * zstd_wrap.c -- wrapper around the zstd single-file amalgamation.
 *
 * NOTE: This file is compiled with -std=c99 (NOT -ansi / -pedantic).
 *       lib/zstd/zstd.h requires C99.  This file is explicitly exempt
 *       from BoltPack's C89 portability rule (see docs/portability.md).
 *
 * Only compiled when BOLTPACK_ZSTD is defined.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifdef BOLTPACK_ZSTD

#include "zstd_wrap.h"
#include "bp_types.h"

/* Pull in the zstd single-file amalgamation.  The amalgamation defines
 * ZSTD_STATIC_LINKING_ONLY internally. */
#include "../../lib/zstd/zstd.h"

#include <stddef.h>
#include <stdlib.h>

/*
 * zstd level table: BoltPack level (1-9) -> zstd level.
 *
 * Chosen so that:
 *   - level 3 is zstd's own default (3) — good speed/ratio balance
 *   - level 9 reaches zstd level 19 — maximum practical compression
 */
static const int zstd_level_map[10] = {
    3,   /* unused [0] */
    1,   /* BP level 1 -> zstd 1  (fastest) */
    2,   /* BP level 2 -> zstd 2  */
    3,   /* BP level 3 -> zstd 3  (default) */
    5,   /* BP level 4 -> zstd 5  */
    7,   /* BP level 5 -> zstd 7  */
    9,   /* BP level 6 -> zstd 9  */
    12,  /* BP level 7 -> zstd 12 */
    15,  /* BP level 8 -> zstd 15 */
    19   /* BP level 9 -> zstd 19 (best ratio) */
};

/* ---- public API -------------------------------------------------------- */

/* No-op: contexts are now per-call; kept for backward compatibility. */
int bp_zstd_init(void)
{
    return BP_OK;
}

/* No-op: level is now passed per-call via bp_zstd_compress_block.
 * Kept for backward compatibility; callers should use method_level instead. */
void bp_zstd_set_level(int level)
{
    (void)level;
}

int bp_zstd_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                            bp_uint8 *dst, bp_uint32 *dst_len,
                            int bp_level)
{
    ZSTD_CCtx *cctx;
    size_t out;
    size_t dst_cap;
    int zstd_level;

    if (src == NULL || dst == NULL || dst_len == NULL) return BP_ERR_INVALID_ARG;

    if (bp_level < 1 || bp_level > 9)
        bp_level = 3; /* default */
    zstd_level = zstd_level_map[bp_level];

    cctx = ZSTD_createCCtx();
    if (cctx == NULL) return BP_ERR_COMPRESS;

    dst_cap = (size_t)*dst_len;
    out = ZSTD_compressCCtx(cctx, dst, dst_cap, src, (size_t)src_len,
                             zstd_level);
    ZSTD_freeCCtx(cctx);

    if (ZSTD_isError(out)) return BP_ERR_COMPRESS;
    *dst_len = (bp_uint32)out;
    return BP_OK;
}

int bp_zstd_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                              bp_uint8 *dst, bp_uint32 *dst_len)
{
    ZSTD_DCtx *dctx;
    size_t out;

    if (src == NULL || dst == NULL || dst_len == NULL) return BP_ERR_INVALID_ARG;

    dctx = ZSTD_createDCtx();
    if (dctx == NULL) return BP_ERR_CORRUPT;

    out = ZSTD_decompressDCtx(dctx, dst, (size_t)*dst_len,
                               src, (size_t)src_len);
    ZSTD_freeDCtx(dctx);

    if (ZSTD_isError(out)) return BP_ERR_CORRUPT;
    *dst_len = (bp_uint32)out;
    return BP_OK;
}

bp_uint32 bp_zstd_compress_bound(bp_uint32 src_len)
{
    size_t bound = ZSTD_compressBound((size_t)src_len);
    /* zstd's own bound is already sufficient; just cap to bp_uint32. */
    if (bound > 0xFFFFFFFFUL) return 0xFFFFFFFFUL;
    return (bp_uint32)bound;
}

/* No-op: contexts are now per-call; kept for backward compatibility. */
void bp_zstd_cleanup(void)
{
}

#endif /* BOLTPACK_ZSTD */
