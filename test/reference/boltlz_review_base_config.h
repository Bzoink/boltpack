/* Frozen code-review baseline; do not change. BSD 2-Clause License. */
/*
 * boltlz_config.h -- format-independent encoder/decoder experiment controls
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 */
#ifndef BP_REVIEW_BASE_BOLTLZ_CONFIG_H
#define BP_REVIEW_BASE_BOLTLZ_CONFIG_H

#ifndef RB_BOLTLZ_HASH_BITS
#define RB_BOLTLZ_HASH_BITS 13
#endif
#ifndef RB_BOLTLZ_LAZY_LIMIT
#define RB_BOLTLZ_LAZY_LIMIT 32
#endif
#ifndef RB_BOLTLZ_SECOND_LIMIT
#define RB_BOLTLZ_SECOND_LIMIT 262144UL
#endif
#ifndef RB_BOLTLZ_DUAL_HASH
#define RB_BOLTLZ_DUAL_HASH 0
#endif
#ifndef RB_BOLTLZ_WIDE_MATCH
#define RB_BOLTLZ_WIDE_MATCH 0
#endif
#ifndef RB_BOLTLZ_EXACT_COST
#define RB_BOLTLZ_EXACT_COST 0
#endif
#ifndef RB_BOLTLZ_PRUNE_MATCH
#define RB_BOLTLZ_PRUNE_MATCH 0
#endif
#ifndef RB_BOLTLZ_REUSE_MATCH
#define RB_BOLTLZ_REUSE_MATCH 0
#endif
#ifndef RB_BOLTLZ_BACKWARD_MATCH
#define RB_BOLTLZ_BACKWARD_MATCH 1
#endif
#ifndef RB_BOLTLZ_HASH_CACHE
#define RB_BOLTLZ_HASH_CACHE 0
#endif
#ifndef RB_BOLTLZ_WORD_MATCH
#define RB_BOLTLZ_WORD_MATCH 1
#endif
#ifndef RB_BOLTLZ_SHORT_COPY
#define RB_BOLTLZ_SHORT_COPY 2
#endif
#ifndef RB_BOLTLZ_SMALL_DISTANCE
#define RB_BOLTLZ_SMALL_DISTANCE 1
#endif
#ifndef RB_BOLTLZ_FAST_DECODE
#define RB_BOLTLZ_FAST_DECODE 2
#endif
#ifndef RB_BOLTLZ_CHEAP_COST
#define RB_BOLTLZ_CHEAP_COST 1
#endif
#ifndef RB_BOLTLZ_SPECIALIZE
#define RB_BOLTLZ_SPECIALIZE 1
#endif
#ifndef RB_BOLTLZ_INLINE_FIND
#define RB_BOLTLZ_INLINE_FIND 1
#endif

#if RB_BOLTLZ_HASH_BITS < 12 || RB_BOLTLZ_HASH_BITS > 14
#error "BoltLZ hash bits must be 12, 13, or 14"
#endif
#if RB_BOLTLZ_LAZY_LIMIT < 0 || RB_BOLTLZ_LAZY_LIMIT > 64
#error "BoltLZ lazy limit must be between 0 and 64"
#endif
#if RB_BOLTLZ_SECOND_LIMIT < 0 || RB_BOLTLZ_SECOND_LIMIT > 262144UL
#error "BoltLZ second-candidate limit is outside the block range"
#endif
#if RB_BOLTLZ_SHORT_COPY < 0 || RB_BOLTLZ_SHORT_COPY > 2
#error "BoltLZ short-copy mode must be zero, one, or two"
#endif
#if RB_BOLTLZ_FAST_DECODE < 0 || RB_BOLTLZ_FAST_DECODE > 2
#error "BoltLZ fast-decode mode must be zero, one, or two"
#endif
#if (RB_BOLTLZ_DUAL_HASH != 0 && RB_BOLTLZ_DUAL_HASH != 1) || \
    (RB_BOLTLZ_WIDE_MATCH != 0 && RB_BOLTLZ_WIDE_MATCH != 1) || \
    (RB_BOLTLZ_EXACT_COST != 0 && RB_BOLTLZ_EXACT_COST != 1) || \
    (RB_BOLTLZ_PRUNE_MATCH != 0 && RB_BOLTLZ_PRUNE_MATCH != 1) || \
    (RB_BOLTLZ_REUSE_MATCH != 0 && RB_BOLTLZ_REUSE_MATCH != 1) || \
    (RB_BOLTLZ_BACKWARD_MATCH != 0 && RB_BOLTLZ_BACKWARD_MATCH != 1) || \
    (RB_BOLTLZ_HASH_CACHE != 0 && RB_BOLTLZ_HASH_CACHE != 1) || \
    (RB_BOLTLZ_WORD_MATCH != 0 && RB_BOLTLZ_WORD_MATCH != 1) || \
    (RB_BOLTLZ_SMALL_DISTANCE != 0 && RB_BOLTLZ_SMALL_DISTANCE != 1) || \
    (RB_BOLTLZ_CHEAP_COST != 0 && RB_BOLTLZ_CHEAP_COST != 1) || \
    (RB_BOLTLZ_SPECIALIZE != 0 && RB_BOLTLZ_SPECIALIZE != 1) || \
    (RB_BOLTLZ_INLINE_FIND != 0 && RB_BOLTLZ_INLINE_FIND != 1)
#error "BoltLZ experiment switches must be zero or one"
#endif
#endif
