/*
 * compress.c -- mini compression layer for BoltPack (stored + LZSS + boltlz)
 *
 * ANSI C (C89/C90) only.  No fluff dependency.
 *
 * 16-bit systems create Stored or LZSS data and decode BoltLZ with the
 * shared decoder-only implementation. Blocks are limited to 32 KiB.
 * Other mini builds also compress with BoltLZ Balanced, using heap workspace.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "compress.h"
#include "../lzss.h"
#include <stdlib.h>
#include <string.h>

#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
/* ---- 16-bit path: stored + LZSS compress; stored + LZSS + boltlz decompress - */

/*
 * LZSS work buffer, heap-allocated on the first compress call and cached
 * for the process lifetime (never freed; calls never overlap).  Lazy
 * allocation keeps extract/list/test/info from paying the 29 KB cost;
 * heap placement keeps it off the 4-8 KB 16-bit DOS/Win16 stack.
 */
static bp_uint8 *lzss_work_16 = NULL;

#include "boltlz_decomp.h"

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
    if (method == BP_METHOD_BOLTLZ) {
        int r;
        r = bp_boltlz_decompress(src, src_len, dst, dst_len);
        if (r != BOLTLZ_D_OK) return BP_ERR_DECOMPRESS;
        return BP_OK;
    }
    return BP_ERR_UNSUPPORTED_METHOD;
}

#ifdef BP_ENABLE_ADLER32
/* Adler-32 inline.  Retired as an archive checksum in favour of CRC-32;
 * compiled only under BP_ENABLE_ADLER32. */
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
/* ---- 32/64-bit path: stored + LZSS + boltlz ------------------------------ */

#include "../boltlz.h"

int bp_mini_compress_init(void)
{
    return BP_OK; /* no initialisation required */
}

int bp_mini_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                           bp_uint8 *dst, bp_uint32 *dst_len,
                           bp_uint8 method)
{
    int r;
    void *work;

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (method != BP_METHOD_STORED && method != BP_METHOD_BOLTLZ &&
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

    /* method == BP_METHOD_BOLTLZ */
    work = malloc((size_t)boltlz_work_size(BOLTLZ_BALANCED));
    if (work == NULL) return BP_ERR_NOMEM;
    r = boltlz_compress(src, src_len, dst, dst_len, work, BOLTLZ_BALANCED);
    free(work);
    return r;
}

int bp_mini_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                             bp_uint8 *dst, bp_uint32 *dst_len,
                             bp_uint8 method)
{
    int r;

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (method != BP_METHOD_STORED && method != BP_METHOD_BOLTLZ &&
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

    /* method == BP_METHOD_BOLTLZ */
    r = boltlz_decompress(src, src_len, dst, *dst_len);
    if (r != BP_OK) { *dst_len = 0; return BP_ERR_DECOMPRESS; }
    return BP_OK;
}

#ifdef BP_ENABLE_ADLER32
/* Adler-32 inline.  Retired as an archive checksum in favour of CRC-32;
 * compiled only under BP_ENABLE_ADLER32. */
bp_uint32 bp_mini_checksum(bp_uint32 adler,
                           const bp_uint8 *data, bp_uint32 len)
{
    bp_uint32 s1;
    bp_uint32 s2;
    bp_uint32 i;

    if (len == 0) {
        return adler;
    }

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
    return boltlz_work_size(BOLTLZ_BALANCED);
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
