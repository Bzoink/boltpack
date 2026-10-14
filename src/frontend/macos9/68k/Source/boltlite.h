/*
 * boltlite.h -- low-memory BoltLite codec for BoltPack method 1.
 *
 * The format uses a 4 KiB history window, one flag bit per symbol, compact
 * one-byte local/repeated-distance matches, and an explicit-distance form.
 * The decoder needs no workspace. The encoder uses a fixed-size hash-chain
 * workspace and works with strict ANSI C89 compilers, including 16-bit ones.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_BOLTLITE_H
#define BP_BOLTLITE_H

#include "bp_types.h"

#define BOLTLITE_WINDOW_SIZE       4096U
#define BOLTLITE_MIN_MATCH            3U
#define BOLTLITE_MAX_MATCH           66U
#define BOLTLITE_HASH_BUCKETS      1024U
#define BOLTLITE_SEARCH_LIMIT        64U
#define BOLTLITE_WORK_SIZE        18432U
#define BOLTLITE_COMPRESS_BOUND(n) ((n) + (n) / 8U + 16U)

/* Compress src_len bytes into a buffer of at least BOLTLITE_COMPRESS_BOUND(src_len).
 * work must point to BOLTLITE_WORK_SIZE writable bytes. Returns encoded length,
 * or zero on invalid input. */
bp_uint32 boltlite_compress(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint8 *work);

/* Decode exactly dst_len bytes. The work argument is ignored and may be NULL.
 * Returns BP_OK on success or BP_ERR_CORRUPT for malformed input. */
int boltlite_decompress(const bp_uint8 *src, bp_uint32 src_len,
                    bp_uint8 *dst, bp_uint32 dst_len, bp_uint8 *work);

#endif /* BP_BOLTLITE_H */
