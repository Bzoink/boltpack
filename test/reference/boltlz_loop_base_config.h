/* Frozen loop-tuning baseline; do not change. BSD 2-Clause License. */
/*
 * boltlz_config.h -- format-independent encoder/decoder experiment controls
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 */
#ifndef BP_LOOP_BASE_BOLTLZ_CONFIG_H
#define BP_LOOP_BASE_BOLTLZ_CONFIG_H

#ifndef LB_BOLTLZ_HASH_BITS
#define LB_BOLTLZ_HASH_BITS 13
#endif
#ifndef LB_BOLTLZ_LAZY_LIMIT
#define LB_BOLTLZ_LAZY_LIMIT 32
#endif
#ifndef LB_BOLTLZ_SECOND_LIMIT
#define LB_BOLTLZ_SECOND_LIMIT 262144UL
#endif
#ifndef LB_BOLTLZ_DUAL_HASH
#define LB_BOLTLZ_DUAL_HASH 0
#endif
#ifndef LB_BOLTLZ_WIDE_MATCH
#define LB_BOLTLZ_WIDE_MATCH 0
#endif
#ifndef LB_BOLTLZ_EXACT_COST
#define LB_BOLTLZ_EXACT_COST 0
#endif
#ifndef LB_BOLTLZ_PRUNE_MATCH
#define LB_BOLTLZ_PRUNE_MATCH 0
#endif
#ifndef LB_BOLTLZ_REUSE_MATCH
#define LB_BOLTLZ_REUSE_MATCH 0
#endif
#ifndef LB_BOLTLZ_BACKWARD_MATCH
#define LB_BOLTLZ_BACKWARD_MATCH 1
#endif
#ifndef LB_BOLTLZ_HASH_CACHE
#define LB_BOLTLZ_HASH_CACHE 0
#endif
#ifndef LB_BOLTLZ_WORD_MATCH
#define LB_BOLTLZ_WORD_MATCH 1
#endif
#ifndef LB_BOLTLZ_SHORT_COPY
#define LB_BOLTLZ_SHORT_COPY 2
#endif
#ifndef LB_BOLTLZ_SMALL_DISTANCE
#define LB_BOLTLZ_SMALL_DISTANCE 1
#endif
#ifndef LB_BOLTLZ_FAST_DECODE
#define LB_BOLTLZ_FAST_DECODE 2
#endif
#ifndef LB_BOLTLZ_CHEAP_COST
#define LB_BOLTLZ_CHEAP_COST 1
#endif
#ifndef LB_BOLTLZ_SPECIALIZE
#define LB_BOLTLZ_SPECIALIZE 1
#endif
#ifndef LB_BOLTLZ_INLINE_FIND
#define LB_BOLTLZ_INLINE_FIND 1
#endif
#ifndef LB_BOLTLZ_ENCODE_SHORT_COPY
#define LB_BOLTLZ_ENCODE_SHORT_COPY 2
#endif
#ifndef LB_BOLTLZ_DECODE_DISTANCE3
#define LB_BOLTLZ_DECODE_DISTANCE3 1
#endif
#ifndef LB_BOLTLZ_DECODE_ZERO_LITERAL
#define LB_BOLTLZ_DECODE_ZERO_LITERAL 0
#endif

#if LB_BOLTLZ_HASH_BITS < 12 || LB_BOLTLZ_HASH_BITS > 14
#error "BoltLZ hash bits must be 12, 13, or 14"
#endif
#if LB_BOLTLZ_LAZY_LIMIT < 0 || LB_BOLTLZ_LAZY_LIMIT > 64
#error "BoltLZ lazy limit must be between 0 and 64"
#endif
#if LB_BOLTLZ_SECOND_LIMIT < 0 || LB_BOLTLZ_SECOND_LIMIT > 262144UL
#error "BoltLZ second-candidate limit is outside the block range"
#endif
#if LB_BOLTLZ_SHORT_COPY < 0 || LB_BOLTLZ_SHORT_COPY > 2
#error "BoltLZ short-copy mode must be zero, one, or two"
#endif
#if LB_BOLTLZ_FAST_DECODE < 0 || LB_BOLTLZ_FAST_DECODE > 2
#error "BoltLZ fast-decode mode must be zero, one, or two"
#endif
#if LB_BOLTLZ_ENCODE_SHORT_COPY < 0 || LB_BOLTLZ_ENCODE_SHORT_COPY > 2
#error "BoltLZ encoder short-copy mode must be zero, one, or two"
#endif
#if (LB_BOLTLZ_DUAL_HASH != 0 && LB_BOLTLZ_DUAL_HASH != 1) || \
    (LB_BOLTLZ_WIDE_MATCH != 0 && LB_BOLTLZ_WIDE_MATCH != 1) || \
    (LB_BOLTLZ_EXACT_COST != 0 && LB_BOLTLZ_EXACT_COST != 1) || \
    (LB_BOLTLZ_PRUNE_MATCH != 0 && LB_BOLTLZ_PRUNE_MATCH != 1) || \
    (LB_BOLTLZ_REUSE_MATCH != 0 && LB_BOLTLZ_REUSE_MATCH != 1) || \
    (LB_BOLTLZ_BACKWARD_MATCH != 0 && LB_BOLTLZ_BACKWARD_MATCH != 1) || \
    (LB_BOLTLZ_HASH_CACHE != 0 && LB_BOLTLZ_HASH_CACHE != 1) || \
    (LB_BOLTLZ_WORD_MATCH != 0 && LB_BOLTLZ_WORD_MATCH != 1) || \
    (LB_BOLTLZ_SMALL_DISTANCE != 0 && LB_BOLTLZ_SMALL_DISTANCE != 1) || \
    (LB_BOLTLZ_CHEAP_COST != 0 && LB_BOLTLZ_CHEAP_COST != 1) || \
    (LB_BOLTLZ_SPECIALIZE != 0 && LB_BOLTLZ_SPECIALIZE != 1) || \
    (LB_BOLTLZ_INLINE_FIND != 0 && LB_BOLTLZ_INLINE_FIND != 1) || \
    (LB_BOLTLZ_DECODE_DISTANCE3 != 0 && LB_BOLTLZ_DECODE_DISTANCE3 != 1) || \
    (LB_BOLTLZ_DECODE_ZERO_LITERAL != 0 && LB_BOLTLZ_DECODE_ZERO_LITERAL != 1)
#error "BoltLZ experiment switches must be zero or one"
#endif
#endif
