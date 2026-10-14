/* BoltLZ decoding adapter for memory-limited mini builds.
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License. */
#ifndef BP_MINI_BOLTLZ_DECOMP_H
#define BP_MINI_BOLTLZ_DECOMP_H
#include "../boltlz.h"
#define BOLTLZ_D_OK 0
#define BOLTLZ_D_ERR 1
int bp_boltlz_decompress(const bp_uint8 *src, bp_uint32 src_len,
                         bp_uint8 *dst, bp_uint32 *dst_len);
#endif
