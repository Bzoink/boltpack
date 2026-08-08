/*
 * compress.c -- mini compression layer for BoltPack (stored + miniLZO)
 *
 * ANSI C (C89/C90) only.  No fluff dependency.
 *
 * 16-bit systems (DOS, Win16, OS/2 1.x, CP/M-86):
 *   Compression:   stored (0) and LZSS (1) only.
 *     miniLZO compression needs a 64 KB work buffer (LZO1X_1_MEM_COMPRESS)
 *     and is too CPU-intensive for 8088/8086 with segmented memory.
 *     Method 2 returns BP_ERR_UNSUPPORTED_METHOD for compression.
 *   Decompression: stored (0), LZSS (1), and miniLZO (2).
 *     lzo1x_decompress_safe needs no work buffer and works correctly with
 *     block sizes <= 32 KB (well within a 64 KB segment).
 *   Adler-32 is computed inline.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include "compress.h"
#include "../lzss.h"
#include <stdlib.h>
#include <string.h>

#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
/* ---- 16-bit path: stored + LZSS compress; stored + LZSS + LZO decompress - */

/*
 * LZSS work buffer, heap-allocated on the first compress call and cached
 * for the process lifetime (never freed; calls never overlap).  Lazy
 * allocation keeps extract/list/test/info from paying the 29 KB cost;
 * heap placement keeps it off the 4-8 KB 16-bit DOS/Win16 stack.
 */
static bp_uint8 *lzss_work_16 = NULL;

#include "lzo_decomp.h"

int bp_mini_compress_init(void)
{
    return BP_OK; /* no initialisation needed */
}

int bp_mini_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                           bp_uint8 *dst, bp_uint32 *dst_len,
                           bp_uint8 method)
{
    if (src == NULL || dst == NULL || dst_len == NULL)
        return BP_ERR_INVALID_ARG;
    if (src_len > BP_MINI_MAX_BLOCK_SIZE)
        return BP_ERR_INVALID_ARG;
    if (method == BP_METHOD_STORED) {
        memcpy(dst, src, (size_t)src_len);
        *dst_len = src_len;
        return BP_OK;
    }
    if (method == BP_METHOD_LZSS) {
        if (lzss_work_16 == NULL) {
            lzss_work_16 = (bp_uint8 *)malloc((size_t)LZSS_WORK_SIZE);
            if (lzss_work_16 == NULL) {
                return BP_ERR_NOMEM;
            }
        }
        *dst_len = lzss_compress(src, src_len, dst, lzss_work_16);
        return (*dst_len > 0) ? BP_OK : BP_ERR_COMPRESS;
    }
    return BP_ERR_UNSUPPORTED_METHOD;
}

int bp_mini_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                             bp_uint8 *dst, bp_uint32 *dst_len,
                             bp_uint8 method)
{
    if (src == NULL || dst == NULL || dst_len == NULL)
        return BP_ERR_INVALID_ARG;
    if (method == BP_METHOD_STORED) {
        memcpy(dst, src, (size_t)src_len);
        *dst_len = src_len;
        return BP_OK;
    }
    if (method == BP_METHOD_LZSS) {
        int rc;
        /* Distance-based LZSS decode needs no scratch buffer. */
        rc = lzss_decompress(src, src_len, dst, *dst_len, NULL);
        return rc;
    }
    if (method == BP_METHOD_LZO1X) {
        int r;
        r = bp_lzo1x_decompress(src, src_len, dst, dst_len);
        if (r != LZO1X_D_OK) return BP_ERR_DECOMPRESS;
        return BP_OK;
    }
    return BP_ERR_UNSUPPORTED_METHOD;
}

#ifdef BP_ENABLE_ADLER32
/* Adler-32 inline (no miniLZO available on 16-bit).  Retired as an archive
 * checksum in favour of CRC-32; compiled only under BP_ENABLE_ADLER32. */
bp_uint32 bp_mini_checksum(bp_uint32 adler,
                           const bp_uint8 *data, bp_uint32 len)
{
    bp_uint32 s1;
    bp_uint32 s2;
    bp_uint32 i;

    s1 = adler & 0xFFFFUL;
    s2 = (adler >> 16) & 0xFFFFUL;

    for (i = 0; i < len; i++) {
        s1 = (s1 + data[i]) % 65521UL;
        s2 = (s2 + s1)      % 65521UL;
    }
    return (s2 << 16) | s1;
}
#endif /* BP_ENABLE_ADLER32 */

bp_uint32 bp_mini_compress_work_size(void)
{
    return 0; /* no compression work buffer (decompression needs none either) */
}

#else
/* ---- 32/64-bit path: stored + miniLZO ----------------------------------- */

#include "../../../lib/minilzo-2.10/minilzo.h"

/* LZO work memory, heap-allocated on the first LZO compress call and cached
 * for the process lifetime (never freed; calls never overlap).  Lazy
 * allocation keeps extract/list/test/info from paying the 64 KB cost. */
static bp_uint8 *lzo_workmem = NULL;

int bp_mini_compress_init(void)
{
    if (lzo_init() != LZO_E_OK) {
        return BP_ERR_COMPRESS;
    }
    return BP_OK;
}

int bp_mini_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                           bp_uint8 *dst, bp_uint32 *dst_len,
                           bp_uint8 method)
{
    lzo_uint out_len;
    int r;

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (method != BP_METHOD_STORED && method != BP_METHOD_LZO1X &&
        method != BP_METHOD_LZSS) {
        return BP_ERR_UNSUPPORTED_METHOD;
    }
    if (src_len > BP_MINI_MAX_BLOCK_SIZE) {
        return BP_ERR_INVALID_ARG;
    }

    if (method == BP_METHOD_STORED) {
        memcpy(dst, src, (size_t)src_len);
        *dst_len = src_len;
        return BP_OK;
    }

    if (method == BP_METHOD_LZSS) {
        bp_uint8 lzss_work[LZSS_WORK_SIZE]; /* 32/64-bit: stack is fine */
        *dst_len = lzss_compress(src, src_len, dst, lzss_work);
        return (*dst_len > 0) ? BP_OK : BP_ERR_COMPRESS;
    }

    /* method == BP_METHOD_LZO1X */
    if (lzo_workmem == NULL) {
        lzo_workmem = (bp_uint8 *)malloc((size_t)LZO1X_1_MEM_COMPRESS);
        if (lzo_workmem == NULL) {
            return BP_ERR_NOMEM;
        }
    }
    out_len = 0;
    r = lzo1x_1_compress(src, (lzo_uint)src_len, dst, &out_len, lzo_workmem);
    if (r != LZO_E_OK) {
        return BP_ERR_COMPRESS;
    }
    *dst_len = (bp_uint32)out_len;
    return BP_OK;
}

int bp_mini_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                             bp_uint8 *dst, bp_uint32 *dst_len,
                             bp_uint8 method)
{
    lzo_uint out_len;
    int r;

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (method != BP_METHOD_STORED && method != BP_METHOD_LZO1X &&
        method != BP_METHOD_LZSS) {
        return BP_ERR_UNSUPPORTED_METHOD;
    }

    if (method == BP_METHOD_STORED) {
        memcpy(dst, src, (size_t)src_len);
        *dst_len = src_len;
        return BP_OK;
    }

    if (method == BP_METHOD_LZSS) {
        int rc;
        /* Distance-based LZSS decode needs no scratch buffer. */
        rc = lzss_decompress(src, src_len, dst, *dst_len, NULL);
        return rc;
    }

    /* method == BP_METHOD_LZO1X */
    out_len = (lzo_uint)*dst_len;
    r = lzo1x_decompress_safe(src, (lzo_uint)src_len, dst, &out_len, NULL);
    if (r != LZO_E_OK) {
        return BP_ERR_DECOMPRESS;
    }
    *dst_len = (bp_uint32)out_len;
    return BP_OK;
}

#ifdef BP_ENABLE_ADLER32
/* Adler-32 via miniLZO.  Retired as an archive checksum; see 16-bit variant. */
bp_uint32 bp_mini_checksum(bp_uint32 adler,
                           const bp_uint8 *data, bp_uint32 len)
{
    return (bp_uint32)lzo_adler32((lzo_uint32_t)adler, data, (lzo_uint)len);
}
#endif /* BP_ENABLE_ADLER32 */

bp_uint32 bp_mini_compress_work_size(void)
{
    return (bp_uint32)LZO1X_1_MEM_COMPRESS;
}

#endif /* SIZEOF_INT < 4 */

/* ---- CRC-32 and typed-checksum helpers (shared by both paths) ----------- */

bp_uint32 bp_mini_crc32(bp_uint32 crc, const bp_uint8 *data, bp_uint32 len)
{
    bp_uint32 c;
    bp_uint32 i;
    int k;

    if (len == 0 || data == NULL) {
        return crc;
    }
    c = crc ^ 0xFFFFFFFFUL;
    for (i = 0; i < len; i++) {
        c ^= data[i];
        for (k = 0; k < 8; k++) {
            c = (c & 1U) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFUL;
}

/* CRC-32 is the only compiled archive checksum; both helpers resolve to it.
 * ck_type is retained for the on-disk checksum_type field and future methods.
 * Adler-32 lives behind BP_ENABLE_ADLER32. */
bp_uint32 bp_mini_checksum_seed(bp_uint8 ck_type)
{
    (void)ck_type;
    return 0UL; /* CRC-32 seed */
}

bp_uint32 bp_mini_checksum_typed(bp_uint8 ck_type, bp_uint32 running,
                                 const bp_uint8 *data, bp_uint32 len)
{
    (void)ck_type;
    return bp_mini_crc32(running, data, len);
}
