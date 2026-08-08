/*
 * compress.h -- compression layer interface for BoltPack
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_COMPRESS_H
#define BP_COMPRESS_H

#include "bp_types.h"

/* Initialize the compression subsystem. Call once at startup.
 * Returns BP_OK on success. */
int bp_compress_init(void);

/* Compress a single block using LZO1X-1.
 * dst must be at least (src_len + src_len/16 + 64 + 3) bytes.
 * *dst_len is set to the actual compressed size on output.
 * Returns BP_OK on success. */
int bp_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                      bp_uint8 *dst, bp_uint32 *dst_len);

/* Decompress a single block using LZO1X (safe variant).
 * dst must be at least orig_len bytes.
 * *dst_len should be set to orig_len on input; updated on output.
 * Returns BP_OK on success. */
int bp_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint32 *dst_len);

#ifdef BP_ENABLE_ADLER32
/* Compute Adler-32 checksum.  Retired as an archive checksum (CRC-32 is now
 * the only compiled method); available only when BP_ENABLE_ADLER32 is defined,
 * which the unit tests do to keep its known-answer vectors covered.
 * Pass adler=1 for the first call; chain with previous result for streaming. */
bp_uint32 bp_checksum(bp_uint32 adler, const bp_uint8 *data, bp_uint32 len);
#endif

/* Compute CRC-32 (IEEE 802.3 polynomial, zlib-compatible).
 * Pass crc=0 for the first call; chain with the previous result for
 * streaming.  The value is inverted internally on entry and exit, so the
 * chaining contract matches zlib's crc32(). */
bp_uint32 bp_crc32(bp_uint32 crc, const bp_uint8 *data, bp_uint32 len);

/* Fold data into a running checksum of the given type (BP_CKSUM_*).
 * Use bp_checksum_seed() for the initial value of a fresh checksum. */
bp_uint32 bp_checksum_typed(bp_uint8 ck_type, bp_uint32 running,
                            const bp_uint8 *data, bp_uint32 len);

/* Initial (seed) value for a fresh checksum of the given type:
 * 1 for Adler-32, 0 for CRC-32. */
bp_uint32 bp_checksum_seed(bp_uint8 ck_type);

/* Required size for compression work memory */
bp_uint32 bp_compress_work_size(void);

#endif /* BP_COMPRESS_H */
