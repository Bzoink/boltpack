/*
 * lzss.h -- LZSS codec for BoltPack (method 1).
 *
 * Distance-based LZSS with a 4096-byte window and 18-byte max match.
 * The encoder uses Okumura's binary-search-tree matcher internally; the
 * on-disk tokens reference a back-distance (not an absolute ring index),
 * so the decoder reads matches straight out of the output buffer and
 * needs no scratch memory or pre-fill at all.
 * C89/C90 only.  No dependencies outside bp_types.h and <string.h>.
 *
 * Working memory:
 *   lzss_v4_compress:   caller provides LZSS_V4_WORK_SIZE bytes of scratch.
 *   lzss_v4_decompress: needs no scratch; the work argument may be NULL.
 *
 * Token encoding (match token, 2 bytes, 12+4 bit split):
 *   Byte 0: [dist_hi_8]                 (distance-1) bits 11..4
 *   Byte 1: [dist_lo_4 | (len-3)_4]     (distance-1) bits 3..0 | length field
 *   distance = ((byte0 << 4) | (byte1 >> 4)) + 1     range 1..4096
 *   length   = (byte1 & 0x0F) + 3                    range 3..18
 *   The matched run is copied from dst[dst_pos - distance] forward.
 *
 * Flag byte covers 8 symbols; bit 0 = first symbol.
 *   bit = 1 => next byte is a literal.
 *   bit = 0 => next 2 bytes are a (distance, length) match token.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_LZSS_V4_METHOD1_V4_H
#define BP_LZSS_V4_METHOD1_V4_H

#include "bp_types.h"

/* ---- algorithm parameters (format-fixed, do not change) ---------------- */

#define LZSS_V4_RING_SIZE  4096    /* N: window length (12-bit back-distance)   */
#define LZSS_V4_MAX_MATCH  18      /* F: maximum match length (4-bit length)    */
#define LZSS_V4_MIN_MATCH  3       /* shorter matches encoded as literals        */
#define LZSS_V4_FILL_BYTE  0x20    /* encoder ring warm-up fill (space char)     */

/*
 * Scratch memory needed by lzss_v4_compress().  The decoder needs none.
 *
 * Layout (unsigned short used for all tree arrays):
 *   ring_buf: LZSS_V4_RING_SIZE bytes             (4 096)
 *   lson:     (LZSS_V4_RING_SIZE + 1) shorts       (8 194)
 *   rson:     (LZSS_V4_RING_SIZE + 257) shorts     (8 706)  -- roots at [N+1..N+256]
 *   dad:      (LZSS_V4_RING_SIZE + 1) shorts       (8 194)
 *   Total:    4096 + 8194 + 8706 + 8194 = 29190 bytes
 * LZSS_V4_WORK_SIZE is rounded up to 29200 to cover alignment padding.
 */
#define LZSS_V4_WORK_SIZE  29200

/* ---- public API -------------------------------------------------------- */

/*
 * Compress src[0..src_len-1] into dst.
 * dst buffer must be at least (src_len + src_len/8 + 16) bytes.
 * work must point to LZSS_V4_WORK_SIZE bytes of scratch (uninitialised is fine).
 * Returns the number of bytes written to dst, or 0 on error.
 */
bp_uint32 lzss_v4_compress(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint8 *work);

/*
 * Decompress src[0..src_len-1] into dst[0..dst_len-1].
 * Needs no scratch memory; the work argument is ignored and may be NULL
 * (kept for API compatibility with callers that pass a shared buffer).
 * Returns BP_OK (0) on success, BP_ERR_CORRUPT on malformed input.
 */
int lzss_v4_decompress(const bp_uint8 *src, bp_uint32 src_len,
                    bp_uint8 *dst, bp_uint32 dst_len, bp_uint8 *work);

#endif /* BP_LZSS_V4_METHOD1_V4_H */
