/*
 * compress.h -- mini compression layer for BoltPack
 *
 * Methods: 0 (stored), 1 (LZSS), 2 (fastlz, 32/64-bit only).
 * Methods 3 (fluff) and any method > 3 return BP_ERR_UNSUPPORTED_METHOD.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_MINI_COMPRESS_H
#define BP_MINI_COMPRESS_H

#include "../bp_types.h"

/*
 * Initialize the mini compression subsystem. fastlz needs no init.
 * Must be called once before any compress or decompress operation.
 * Returns BP_OK on success.
 */
int bp_mini_compress_init(void);

/*
 * Compress one block.
 *   method  BP_METHOD_STORED (0), BP_METHOD_LZSS (1), or BP_METHOD_FASTLZ (2).
 *   src_len must be <= BP_MINI_MAX_BLOCK_SIZE.
 *
 * For stored: dst must be >= src_len bytes.
 * For LZSS:   dst must be >= src_len + src_len/8 + 16 bytes.
 * For fastlz: dst must be >= src_len + src_len/20 + 66 bytes.
 *
 * LZSS is available on all platforms (16-bit and 32/64-bit).
 * fastlz is only available on 32/64-bit (returns
 * BP_ERR_UNSUPPORTED_METHOD on 16-bit).
 *
 * Returns BP_ERR_UNSUPPORTED_METHOD for methods 3+ (or fastlz on 16-bit).
 * Returns BP_ERR_INVALID_ARG      if src_len > BP_MINI_MAX_BLOCK_SIZE.
 */
int bp_mini_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                           bp_uint8 *dst, bp_uint32 *dst_len,
                           bp_uint8 method);

/*
 * Decompress one block.
 *   method   BP_METHOD_STORED (0), BP_METHOD_LZSS (1), or BP_METHOD_FASTLZ (2).
 *   *dst_len must be set to the original (uncompressed) size on entry.
 *
 * Returns BP_ERR_UNSUPPORTED_METHOD for methods other than 0, 1, or 2.
 */
int bp_mini_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                             bp_uint8 *dst, bp_uint32 *dst_len,
                             bp_uint8 method);

#ifdef BP_ENABLE_ADLER32
/*
 * Compute Adler-32 checksum.  Retired as an archive checksum (CRC-32 is now
 * the only compiled method); compiled only when BP_ENABLE_ADLER32 is defined.
 * Pass adler=1 for the first call; chain the return value for streaming.
 */
bp_uint32 bp_mini_checksum(bp_uint32 adler,
                           const bp_uint8 *data, bp_uint32 len);
#endif

/*
 * Compute CRC-32 (IEEE 802.3, zlib-compatible), table-free so no ~1 KB
 * static table is needed on 16-bit targets.  Pass crc=0 for the first call.
 */
bp_uint32 bp_mini_crc32(bp_uint32 crc, const bp_uint8 *data, bp_uint32 len);

/* Fold data into a running checksum of the given type (BP_CKSUM_*).
 * Seed a fresh checksum with bp_mini_checksum_seed(). */
bp_uint32 bp_mini_checksum_typed(bp_uint8 ck_type, bp_uint32 running,
                                 const bp_uint8 *data, bp_uint32 len);

/* Initial value for a fresh checksum: 1 for Adler-32, 0 for CRC-32. */
bp_uint32 bp_mini_checksum_seed(bp_uint8 ck_type);

/* Return the size of the fastlz compression work memory buffer in bytes. */
bp_uint32 bp_mini_compress_work_size(void);

#endif /* BP_MINI_COMPRESS_H */
