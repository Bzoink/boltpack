/* Frozen BoltLZ v2 encoder benchmark API. BSD 2-Clause License. */
#ifndef BP_BOLTLZ_REFERENCE_V2_H
#define BP_BOLTLZ_REFERENCE_V2_H
#include "bp_types.h"
bp_uint32 boltlz_reference_work_size(int level);
bp_uint32 boltlz_reference_compress_bound(bp_uint32 src_len);
int boltlz_reference_compress(const bp_uint8 *src, bp_uint32 src_len,
                             bp_uint8 *dst, bp_uint32 *dst_len,
                             void *work, int level);
#endif
