/*
 * boltlz.h -- byte-oriented LZ77 block codec
 *
 * ANSI C (C89/C90) only.
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BP_BOLTLZ_H
#define BP_BOLTLZ_H

#include "bp_types.h"

#define BOLTLZ_FORMAT_VERSION 2
#define BOLTLZ_MAX_BLOCK 262144UL
#define BOLTLZ_FAST 1
#define BOLTLZ_BALANCED 2

/* Experimental block codec; no archive method identifier is assigned.
 * Inputs and outputs must not overlap. Work must be aligned for bp_uint32
 * (malloc provides this) and contain boltlz_work_size(level) bytes.
 * The encoder resets work on every call and performs no allocations.
 * Returns zero for an unsupported level or block length. */
bp_uint32 boltlz_work_size(int level);
bp_uint32 boltlz_compress_bound(bp_uint32 src_len);

/* dst_len initially contains capacity; receives encoded size on success,
 * zero on failure. Partial output on failure must be discarded.
 * Empty input encodes as a version byte; src may be NULL only when empty. */
int boltlz_compress(const bp_uint8 *src, bp_uint32 src_len,
                   bp_uint8 *dst, bp_uint32 *dst_len, void *work, int level);

/* dst_len is the exact expected output length. No workspace is required.
 * Empty output permits NULL dst. Rejects trailing or truncated data,
 * unsupported versions, invalid distances, and length/capacity overflow.
 * On error, partial output must be discarded. */
int boltlz_decompress(const bp_uint8 *src, bp_uint32 src_len,
                     bp_uint8 *dst, bp_uint32 dst_len);

#endif
