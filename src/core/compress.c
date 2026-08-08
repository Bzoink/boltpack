/*
 * compress.c -- compression layer for BoltPack (miniLZO wrapper)
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include "compress.h"
#include "../../lib/minilzo-2.10/minilzo.h"
#include <stdlib.h>
#include <string.h>

int bp_compress_init(void)
{
    if (lzo_init() != LZO_E_OK) {
        return BP_ERR_COMPRESS;
    }
    return BP_OK;
}

int bp_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                      bp_uint8 *dst, bp_uint32 *dst_len)
{
    int r;
    lzo_uint out_len;
    bp_uint8 *workmem;

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    workmem = (bp_uint8 *)malloc(LZO1X_1_MEM_COMPRESS);
    if (workmem == NULL) {
        return BP_ERR_NOMEM;
    }

    out_len = 0;
    r = lzo1x_1_compress(src, (lzo_uint)src_len,
                         dst, &out_len,
                         workmem);
    free(workmem);

    if (r != LZO_E_OK) {
        return BP_ERR_COMPRESS;
    }
    *dst_len = (bp_uint32)out_len;
    return BP_OK;
}

int bp_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint32 *dst_len)
{
    int r;
    lzo_uint out_len;

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    out_len = (lzo_uint)*dst_len;
    r = lzo1x_decompress_safe(src, (lzo_uint)src_len,
                              dst, &out_len,
                              NULL);
    if (r != LZO_E_OK) {
        return BP_ERR_DECOMPRESS;
    }
    *dst_len = (bp_uint32)out_len;
    return BP_OK;
}

#ifdef BP_ENABLE_ADLER32
/* Adler-32 has been retired as an archive checksum in favour of CRC-32 and is
 * no longer compiled into the shipping tools.  It is kept behind this macro so
 * the known-answer unit tests can still exercise it; define BP_ENABLE_ADLER32
 * to build it. */
bp_uint32 bp_checksum(bp_uint32 adler, const bp_uint8 *data, bp_uint32 len)
{
    /* lzo_adler32 returns the constant 1 when data is NULL, discarding the
     * running value; an empty chunk must leave the checksum unchanged. */
    if (len == 0) {
        return adler;
    }
    return (bp_uint32)lzo_adler32((lzo_uint32_t)adler, data, (lzo_uint)len);
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
    return (bp_uint32)LZO1X_1_MEM_COMPRESS;
}
