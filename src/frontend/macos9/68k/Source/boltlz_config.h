/*
 * boltlz_config.h -- format-independent encoder/decoder experiment controls
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 */
#ifndef BP_BOLTLZ_CONFIG_H
#define BP_BOLTLZ_CONFIG_H

#ifndef BOLTLZ_HASH_BITS
#define BOLTLZ_HASH_BITS 13
#endif
#ifndef BOLTLZ_LAZY_LIMIT
#define BOLTLZ_LAZY_LIMIT 32
#endif
#ifndef BOLTLZ_SECOND_LIMIT
#define BOLTLZ_SECOND_LIMIT 262144UL
#endif
#ifndef BOLTLZ_DUAL_HASH
#define BOLTLZ_DUAL_HASH 0
#endif
#ifndef BOLTLZ_WIDE_MATCH
#define BOLTLZ_WIDE_MATCH 0
#endif
#ifndef BOLTLZ_EXACT_COST
#define BOLTLZ_EXACT_COST 0
#endif
#ifndef BOLTLZ_PRUNE_MATCH
#define BOLTLZ_PRUNE_MATCH 0
#endif
#ifndef BOLTLZ_REUSE_MATCH
#define BOLTLZ_REUSE_MATCH 0
#endif
#ifndef BOLTLZ_BACKWARD_MATCH
#define BOLTLZ_BACKWARD_MATCH 1
#endif
#ifndef BOLTLZ_HASH_CACHE
#define BOLTLZ_HASH_CACHE 0
#endif
#ifndef BOLTLZ_WORD_MATCH
#define BOLTLZ_WORD_MATCH 1
#endif
#ifndef BOLTLZ_SHORT_COPY
#define BOLTLZ_SHORT_COPY 2
#endif
#ifndef BOLTLZ_SMALL_DISTANCE
#define BOLTLZ_SMALL_DISTANCE 1
#endif
#ifndef BOLTLZ_FAST_DECODE
#define BOLTLZ_FAST_DECODE 2
#endif
#ifndef BOLTLZ_CHEAP_COST
#define BOLTLZ_CHEAP_COST 1
#endif
#ifndef BOLTLZ_SPECIALIZE
#define BOLTLZ_SPECIALIZE 1
#endif
#ifndef BOLTLZ_INLINE_FIND
#define BOLTLZ_INLINE_FIND 1
#endif
#ifndef BOLTLZ_ENCODE_SHORT_COPY
#define BOLTLZ_ENCODE_SHORT_COPY 2
#endif
#ifndef BOLTLZ_DECODE_DISTANCE3
#define BOLTLZ_DECODE_DISTANCE3 1
#endif
#ifndef BOLTLZ_DECODE_ZERO_LITERAL
#define BOLTLZ_DECODE_ZERO_LITERAL 0
#endif
#ifndef BOLTLZ_NARROW_TABLE
#define BOLTLZ_NARROW_TABLE 2
#endif
#ifndef BOLTLZ_PIPELINE
#define BOLTLZ_PIPELINE 0
#endif
#ifndef BOLTLZ_FULL_CAPACITY
#define BOLTLZ_FULL_CAPACITY 1
#endif
#ifndef BOLTLZ_EXTENDED_DECODE
#define BOLTLZ_EXTENDED_DECODE 0
#endif

#if BOLTLZ_HASH_BITS < 12 || BOLTLZ_HASH_BITS > 14
#error "BoltLZ hash bits must be 12, 13, or 14"
#endif
#if BOLTLZ_LAZY_LIMIT < 0 || BOLTLZ_LAZY_LIMIT > 64
#error "BoltLZ lazy limit must be between 0 and 64"
#endif
#if BOLTLZ_SECOND_LIMIT < 0 || BOLTLZ_SECOND_LIMIT > 262144UL
#error "BoltLZ second-candidate limit is outside the block range"
#endif
#if BOLTLZ_SHORT_COPY < 0 || BOLTLZ_SHORT_COPY > 2
#error "BoltLZ short-copy mode must be zero, one, or two"
#endif
#if BOLTLZ_FAST_DECODE < 0 || BOLTLZ_FAST_DECODE > 2
#error "BoltLZ fast-decode mode must be zero, one, or two"
#endif
#if BOLTLZ_ENCODE_SHORT_COPY < 0 || BOLTLZ_ENCODE_SHORT_COPY > 2
#error "BoltLZ encoder short-copy mode must be zero, one, or two"
#endif
#if BOLTLZ_EXTENDED_DECODE < 0 || BOLTLZ_EXTENDED_DECODE > 4
#error "BoltLZ extended-decode mode must be between zero and four"
#endif
#if BOLTLZ_NARROW_TABLE < 0 || BOLTLZ_NARROW_TABLE > 2
#error "BoltLZ narrow-table mode must be zero, one, or two"
#endif
#if (BOLTLZ_DUAL_HASH != 0 && BOLTLZ_DUAL_HASH != 1) || \
    (BOLTLZ_WIDE_MATCH != 0 && BOLTLZ_WIDE_MATCH != 1) || \
    (BOLTLZ_EXACT_COST != 0 && BOLTLZ_EXACT_COST != 1) || \
    (BOLTLZ_PRUNE_MATCH != 0 && BOLTLZ_PRUNE_MATCH != 1) || \
    (BOLTLZ_REUSE_MATCH != 0 && BOLTLZ_REUSE_MATCH != 1) || \
    (BOLTLZ_BACKWARD_MATCH != 0 && BOLTLZ_BACKWARD_MATCH != 1) || \
    (BOLTLZ_HASH_CACHE != 0 && BOLTLZ_HASH_CACHE != 1) || \
    (BOLTLZ_WORD_MATCH != 0 && BOLTLZ_WORD_MATCH != 1) || \
    (BOLTLZ_SMALL_DISTANCE != 0 && BOLTLZ_SMALL_DISTANCE != 1) || \
    (BOLTLZ_CHEAP_COST != 0 && BOLTLZ_CHEAP_COST != 1) || \
    (BOLTLZ_SPECIALIZE != 0 && BOLTLZ_SPECIALIZE != 1) || \
    (BOLTLZ_INLINE_FIND != 0 && BOLTLZ_INLINE_FIND != 1) || \
    (BOLTLZ_DECODE_DISTANCE3 != 0 && BOLTLZ_DECODE_DISTANCE3 != 1) || \
    (BOLTLZ_DECODE_ZERO_LITERAL != 0 && BOLTLZ_DECODE_ZERO_LITERAL != 1) || \
    (BOLTLZ_PIPELINE != 0 && BOLTLZ_PIPELINE != 1) || \
    (BOLTLZ_FULL_CAPACITY != 0 && BOLTLZ_FULL_CAPACITY != 1)
#error "BoltLZ experiment switches must be zero or one"
#endif
#endif
