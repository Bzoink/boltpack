/*
 * compress.c -- compression layer for BoltPack (fastlz wrapper)
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "compress.h"

#include "../../lib/fastlz/fastlz.h"
#include <stdlib.h>
#include <string.h>

/* fastlz takes signed int lengths.  Full builds have at least 32-bit int;
 * keep the explicit limit here so the public bp_uint32 API cannot silently
 * turn a large length into a negative value. */
#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
#define BP_FASTLZ_MAX_INT 32767UL
#else
#define BP_FASTLZ_MAX_INT 2147483647UL
#endif

int bp_compress_init(void)
{
    return BP_OK; /* fastlz needs no init */
}

int bp_compress_block_level(const bp_uint8 *src, bp_uint32 src_len,
                            bp_uint8 *dst, bp_uint32 *dst_len,
                            bp_uint8 method_level)
{
    int r;
    int flz_level;

    if (src == NULL || dst == NULL || dst_len == NULL || src_len == 0u ||
        src_len > BP_FASTLZ_MAX_INT) {
        return BP_ERR_INVALID_ARG;
    }

    /* Map the BoltPack method_level (0, 1-9) to a fastlz level (1 or 2).
     * 0 = auto-select (fastlz_compress), 1 = L1, 2 = L2, 3-9 clamp to L2. */
    switch (method_level) {
        case 1:
            flz_level = 1;
            r = fastlz_compress_level(flz_level, src, (int)src_len, dst);
            break;
        case 2:
        default:
            flz_level = 2;
            r = fastlz_compress_level(flz_level, src, (int)src_len, dst);
            break;
        case 0:
            r = fastlz_compress(src, (int)src_len, dst);
            break;
    }

    if (r <= 0) {
        return BP_ERR_COMPRESS;
    }
    *dst_len = (bp_uint32)r;
    return BP_OK;
}

int bp_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                      bp_uint8 *dst, bp_uint32 *dst_len)
{
    return bp_compress_block_level(src, src_len, dst, dst_len, 0);
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
    return 0; /* fastlz needs no work buffer */
}
