/*
 * compress.c -- compression layer for BoltPack (miniLZO wrapper)
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include "compress.h"
#include "minilzo.h"
#include <string.h>

/* Work memory for LZO compression */
static bp_uint8 lzo_workmem[LZO1X_1_MEM_COMPRESS];

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

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    out_len = 0;
    r = lzo1x_1_compress(src, (lzo_uint)src_len,
                         dst, &out_len,
                         lzo_workmem);
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
/* Adler-32 via miniLZO.  Retired as an archive checksum in favour of CRC-32;
 * compiled only when BP_ENABLE_ADLER32 is defined. */
bp_uint32 bp_checksum(bp_uint32 adler, const bp_uint8 *data, bp_uint32 len)
{
    return (bp_uint32)lzo_adler32((lzo_uint32_t)adler, data, (lzo_uint)len);
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
    return (bp_uint32)LZO1X_1_MEM_COMPRESS;
}
