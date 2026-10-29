/*
 * compress.c -- compression layer for BoltPack (BoltLZ wrapper)
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifdef __PLAN9__
#include <u.h>
#include <libc.h>
#endif

#include "compress.h"

#if !defined(__PLAN9__)
#include <stdlib.h>
#include <string.h>
#endif

int bp_compress_init(void)
{
    return BP_OK; /* no initialisation required */
}

int bp_compress_block_work(const bp_uint8 *src, bp_uint32 src_len,
                           bp_uint8 *dst, bp_uint32 *dst_len,
                           void *work, bp_uint8 method_level)
{
    int level = (method_level == 1) ? BOLTLZ_FAST : BOLTLZ_BALANCED;
    if (src_len == 0) {
        if (dst_len != NULL) *dst_len = 0;
        return BP_ERR_INVALID_ARG;
    }
    return boltlz_compress(src, src_len, dst, dst_len, work, level);
}

int bp_compress_block_level(const bp_uint8 *src, bp_uint32 src_len,
                            bp_uint8 *dst, bp_uint32 *dst_len,
                            bp_uint8 method_level)
{
    void *work;
    int rc;
    int level = (method_level == 1) ? BOLTLZ_FAST : BOLTLZ_BALANCED;
    if (src == NULL || dst == NULL || dst_len == NULL || src_len == 0 ||
        src_len > BOLTLZ_MAX_BLOCK) {
        if (dst_len != NULL) *dst_len = 0;
        return BP_ERR_INVALID_ARG;
    }
    work = malloc((size_t)boltlz_work_size(level));
    if (work == NULL) { *dst_len = 0; return BP_ERR_NOMEM; }
    rc = bp_compress_block_work(src, src_len, dst, dst_len, work, method_level);
    free(work);
    return rc;
}

int bp_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                      bp_uint8 *dst, bp_uint32 *dst_len)
{
    return bp_compress_block_level(src, src_len, dst, dst_len, 0);
}

int bp_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint32 *dst_len)
{
    bp_uint32 expected;
    int rc;
    if (dst_len == NULL) return BP_ERR_INVALID_ARG;
    expected = *dst_len;
    *dst_len = 0;
    if (src == NULL || dst == NULL || expected == 0 ||
        expected > BOLTLZ_MAX_BLOCK) return BP_ERR_INVALID_ARG;
    rc = boltlz_decompress(src, src_len, dst, expected);
    if (rc != BP_OK) return BP_ERR_DECOMPRESS;
    *dst_len = expected;
    return BP_OK;
}

#ifdef BP_ENABLE_ADLER32
/* Adler-32 has been retired as an archive checksum in favour of CRC-32 and is
 * no longer compiled into the shipping tools.  It is kept behind this macro so
 * the known-answer unit tests can still exercise it; define BP_ENABLE_ADLER32
 * to build it. */
bp_uint32 bp_checksum(bp_uint32 adler, const bp_uint8 *data, bp_uint32 len)
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

/*
 * CRC-32 (IEEE 802.3, reflected, zlib-compatible).  The 256-entry lookup
 * table is built lazily on first use so we keep a single ~1 KB static cost
 * and no start-up work when CRC is never selected.
 */
static bp_uint32 crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_build_table(void)
{
    bp_uint32 i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            if (c & 1U) {
                c = 0xEDB88320UL ^ (c >> 1);
            } else {
                c = c >> 1;
            }
        }
        crc32_table[i] = c & 0xFFFFFFFFUL;
    }
    crc32_table_ready = 1;
}

bp_uint32 bp_crc32(bp_uint32 crc, const bp_uint8 *data, bp_uint32 len)
{
    bp_uint32 c;
    bp_uint32 i;

    if (len == 0 || data == NULL) {
        return crc;
    }
    if (!crc32_table_ready) {
        crc32_build_table();
    }
    c = crc ^ 0xFFFFFFFFUL;
    for (i = 0; i < len; i++) {
        c = crc32_table[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFUL;
}

/* CRC-32 is the only archive checksum; both helpers ignore ck_type and resolve
 * to it.  The parameter is kept for ABI stability and forward extensibility. */
bp_uint32 bp_checksum_seed(bp_uint8 ck_type)
{
    (void)ck_type;
    return 0U; /* CRC-32 seed */
}

bp_uint32 bp_checksum_typed(bp_uint8 ck_type, bp_uint32 running,
                            const bp_uint8 *data, bp_uint32 len)
{
    (void)ck_type;
    return bp_crc32(running, data, len);
}

bp_uint32 bp_compress_work_size(void)
{
    return boltlz_work_size(BOLTLZ_BALANCED);
}
