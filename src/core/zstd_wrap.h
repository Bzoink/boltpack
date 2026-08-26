/*
 * zstd_wrap.h -- C89-compatible interface to the zstd compression library
 *
 * This header is safe to include from strict ANSI C89 code.  The actual
 * implementation (zstd_wrap.c) is compiled with -std=c99 because it
 * includes lib/zstd/zstd.h which requires C99.
 *
 * Only compiled and linked when BOLTPACK_ZSTD is defined.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_ZSTD_WRAP_H
#define BP_ZSTD_WRAP_H

#ifdef BOLTPACK_ZSTD

#include "bp_types.h"

/*
 * Level mapping: BoltPack levels 1-9 map to zstd levels as follows.
 *
 *   BP level | zstd level | notes
 *   ---------|------------|----------------------------------
 *      1     |     1      | fastest
 *      2     |     2      |
 *      3     |     3      | default (zstd's own default)
 *      4     |     5      |
 *      5     |     7      |
 *      6     |     9      |
 *      7     |    12      |
 *      8     |    15      |
 *      9     |    19      | best ratio
 *
 * bp_zstd_set_level() accepts BoltPack levels 1-9 and maps them to the
 * appropriate zstd level.  The default is BP level 3 (zstd level 3).
 */

/* No-op; kept for backward compatibility.  Contexts are now per-call. */
int bp_zstd_init(void);

/* Deprecated no-op.  Set bp_archive_t.method_level instead. */
void bp_zstd_set_level(int level);

/* Compress a single block.
 * dst must be at least bp_zstd_compress_bound(src_len) bytes.
 * bp_level is the BoltPack compression level (1-9); 0 or out-of-range uses 3.
 * A fresh ZSTD_CCtx is created and freed per call (reentrant).
 * *dst_len is set to the actual compressed size on success.
 * Returns BP_OK on success. */
int bp_zstd_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                            bp_uint8 *dst, bp_uint32 *dst_len,
                            int bp_level);

/* Decompress a single block.
 * dst must be at least orig_len bytes.
 * *dst_len should be set to orig_len on input; updated on output.
 * A fresh ZSTD_DCtx is created and freed per call (reentrant).
 * Returns BP_OK on success. */
int bp_zstd_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                              bp_uint8 *dst, bp_uint32 *dst_len);

/* Upper bound on compressed output for a given source size.
 * Safe for use as the dst allocation size passed to bp_zstd_compress_block. */
bp_uint32 bp_zstd_compress_bound(bp_uint32 src_len);

/* No-op; kept for backward compatibility.  Contexts are now per-call. */
void bp_zstd_cleanup(void);

#endif /* BOLTPACK_ZSTD */

#endif /* BP_ZSTD_WRAP_H */
