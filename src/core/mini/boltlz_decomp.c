/* Shared bounds-checked BoltLZ decoder; no encoder on 16-bit targets.
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License. */
#include "boltlz_decomp.h"
#include "compress.h"
#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
#define BOLTLZ_DECODE_ONLY
#include "../boltlz.c"
#endif

int bp_boltlz_decompress(const bp_uint8 *src, bp_uint32 src_len,
                         bp_uint8 *dst, bp_uint32 *dst_len)
{
    bp_uint32 expected;
    int rc;
    if (dst_len == NULL) return BOLTLZ_D_ERR;
    expected = *dst_len;
    *dst_len = 0;
    if (src_len > BP_MINI_MAX_BLOCK_SIZE + BP_MINI_MAX_BLOCK_SIZE / 63 + 16 ||
        expected > BP_MINI_MAX_BLOCK_SIZE) return BOLTLZ_D_ERR;
    rc = boltlz_decompress(src, src_len, dst, expected);
    if (rc != BP_OK) return BOLTLZ_D_ERR;
    *dst_len = expected;
    return BOLTLZ_D_OK;
}
