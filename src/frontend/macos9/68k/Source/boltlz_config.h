/*
 * boltlz_config.h -- format-independent encoder/decoder experiment controls
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 */
#ifndef BP_BOLTLZ_CONFIG_H
#define BP_BOLTLZ_CONFIG_H

/* Motorola 68000-family builds. The 68000 has no 32-bit multiply and traps
 * on word or long access at an odd address, so the generic encoder's memcpy
 * loads and 32x32 hash multiply become library calls. BOLTLZ_M68K selects
 * the 68000 load, hash and match-length code in boltlz.c and the defaults
 * below, measured fastest on an 8 MHz 68000 (Hatari). It needs a
 * big-endian CPU. The archive format is unchanged. MC68000 is c68's macro
 * (Sinclair QL). */
#ifndef BOLTLZ_M68K
#if defined(__m68k__) || defined(__mc68000__) || defined(mc68000) || \
    defined(__MC68000__) || defined(MC68000)
#define BOLTLZ_M68K 1
#else
#define BOLTLZ_M68K 0
#endif
#endif
#if BOLTLZ_M68K
#ifndef BOLTLZ_HASH_CACHE
#define BOLTLZ_HASH_CACHE 1      /* hash once per position, not twice */
#endif
#ifndef BOLTLZ_NARROW_TABLE
#define BOLTLZ_NARROW_TABLE 0    /* no data cache to save */
#endif
/* Exact copies beat mode 2's 16-byte over-copy with GCC. c68 (the QL) does
 * not inline the copy helpers, so it keeps mode 2 and its fast memcpy. */
#if !defined(BOLTLZ_FAST_DECODE) && defined(__GNUC__)
#define BOLTLZ_FAST_DECODE 1
#endif
#ifndef BOLTLZ_ENCODE_SHORT_COPY
#define BOLTLZ_ENCODE_SHORT_COPY 0
#endif
#ifndef BOLTLZ_WORD_MATCH
#define BOLTLZ_WORD_MATCH 0      /* boltlz.c has its own m68k match loop */
#endif
#endif

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
/* Hand-written 68000 encoder and decoder loops (boltlz_m68k.S); link that
 * file too. The decoder loop works with any decoder setting below; the
 * encoder implements exactly the default m68k settings checked here. */
#ifndef BOLTLZ_M68K_ASM
#define BOLTLZ_M68K_ASM 0
#endif
#if BOLTLZ_M68K_ASM && (!BOLTLZ_M68K || BOLTLZ_HASH_BITS != 13 || \
    BOLTLZ_LAZY_LIMIT != 32 || BOLTLZ_NARROW_TABLE != 0 || \
    !BOLTLZ_FULL_CAPACITY || !BOLTLZ_BACKWARD_MATCH || BOLTLZ_DUAL_HASH || \
    BOLTLZ_EXACT_COST || BOLTLZ_PRUNE_MATCH || BOLTLZ_REUSE_MATCH || \
    BOLTLZ_SECOND_LIMIT < 262144UL)
#error "BOLTLZ_M68K_ASM needs the default BOLTLZ_M68K encoder settings"
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
    (BOLTLZ_FULL_CAPACITY != 0 && BOLTLZ_FULL_CAPACITY != 1) || \
    (BOLTLZ_M68K != 0 && BOLTLZ_M68K != 1) || \
    (BOLTLZ_M68K_ASM != 0 && BOLTLZ_M68K_ASM != 1)
#error "BoltLZ experiment switches must be zero or one"
#endif
#if BOLTLZ_M68K && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "BOLTLZ_M68K needs a big-endian CPU"
#endif
#endif
