/*
 * compress.c -- compression layer for BoltPack 68K (fastlz wrapper)
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "compress.h"
#include "fastlz.h"
#include <string.h>

#define BP_FASTLZ_MAX_INT 2147483647UL

int bp_compress_init(void)
{
    return BP_OK; /* fastlz needs no init */
}

int bp_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                      bp_uint8 *dst, bp_uint32 *dst_len)
{
    int r;

    if (src == NULL || dst == NULL || dst_len == NULL || src_len == 0u ||
        src_len > BP_FASTLZ_MAX_INT) {
        return BP_ERR_INVALID_ARG;
    }

    r = fastlz_compress(src, (int)src_len, dst);
    if (r <= 0) {
        return BP_ERR_COMPRESS;
    }
    *dst_len = (bp_uint32)r;
    return BP_OK;
}

int bp_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint32 *dst_len)
{
    int r;

    if (src == NULL || dst == NULL || dst_len == NULL || src_len == 0u ||
        src_len > BP_FASTLZ_MAX_INT || *dst_len == 0u ||
        *dst_len > BP_FASTLZ_MAX_INT) {
        return BP_ERR_INVALID_ARG;
    }

    r = fastlz_decompress(src, (int)src_len, dst, (int)*dst_len);
    if (r <= 0) {
        return BP_ERR_DECOMPRESS;
    }
    *dst_len = (bp_uint32)r;
    return BP_OK;
}

#ifdef BP_ENABLE_ADLER32
/* Adler-32 inline.  Retired as an archive checksum in favour of CRC-32;
 * compiled only when BP_ENABLE_ADLER32 is defined. */
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

/* CRC-32 (IEEE 802.3, zlib-compatible), table-free (68k has little RAM). */
bp_uint32 bp_crc32(bp_uint32 crc, const bp_uint8 *data, bp_uint32 len)
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
 * ck_type is retained for the checksum_type field and future methods. */
bp_uint32 bp_checksum_seed(bp_uint8 ck_type)
{
    (void)ck_type;
    return 0UL; /* CRC-32 seed */
}

bp_uint32 bp_checksum_typed(bp_uint8 ck_type, bp_uint32 running,
                            const bp_uint8 *data, bp_uint32 len)
{
    (void)ck_type;
    return bp_crc32(running, data, len);
}

bp_uint32 bp_compress_work_size(void)
{
    return 0; /* fastlz needs no work buffer */
}
