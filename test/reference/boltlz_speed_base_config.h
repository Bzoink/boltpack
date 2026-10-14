/* Frozen speed-tuning baseline; do not change. BSD 2-Clause License. */
/*
 * boltlz_config.h -- encoder-only experiment controls (format independent)
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 */
#ifndef BP_SPEED_BASE_BOLTLZ_CONFIG_H
#define BP_SPEED_BASE_BOLTLZ_CONFIG_H

#ifndef SB_BOLTLZ_HASH_BITS
#define SB_BOLTLZ_HASH_BITS 13
#endif
#ifndef SB_BOLTLZ_LAZY_LIMIT
#define SB_BOLTLZ_LAZY_LIMIT 32
#endif
#ifndef SB_BOLTLZ_SECOND_LIMIT
#define SB_BOLTLZ_SECOND_LIMIT 262144UL
#endif
#ifndef SB_BOLTLZ_DUAL_HASH
#define SB_BOLTLZ_DUAL_HASH 0
#endif
#ifndef SB_BOLTLZ_WIDE_MATCH
#define SB_BOLTLZ_WIDE_MATCH 0
#endif
#ifndef SB_BOLTLZ_EXACT_COST
#define SB_BOLTLZ_EXACT_COST 0
#endif
#ifndef SB_BOLTLZ_PRUNE_MATCH
#define SB_BOLTLZ_PRUNE_MATCH 0
#endif
#ifndef SB_BOLTLZ_REUSE_MATCH
#define SB_BOLTLZ_REUSE_MATCH 0
#endif
#ifndef SB_BOLTLZ_BACKWARD_MATCH
#define SB_BOLTLZ_BACKWARD_MATCH 1
#endif
#ifndef SB_BOLTLZ_HASH_CACHE
#define SB_BOLTLZ_HASH_CACHE 0
#endif
#ifndef SB_BOLTLZ_WORD_MATCH
#define SB_BOLTLZ_WORD_MATCH 1
#endif

#if SB_BOLTLZ_HASH_BITS < 12 || SB_BOLTLZ_HASH_BITS > 14
#error "BoltLZ hash bits must be 12, 13, or 14"
#endif
#if SB_BOLTLZ_LAZY_LIMIT < 0 || SB_BOLTLZ_LAZY_LIMIT > 64
#error "BoltLZ lazy limit must be between 0 and 64"
#endif
#if SB_BOLTLZ_SECOND_LIMIT < 0 || SB_BOLTLZ_SECOND_LIMIT > 262144UL
#error "BoltLZ second-candidate limit is outside the block range"
#endif
#if (SB_BOLTLZ_DUAL_HASH != 0 && SB_BOLTLZ_DUAL_HASH != 1) || \
    (SB_BOLTLZ_WIDE_MATCH != 0 && SB_BOLTLZ_WIDE_MATCH != 1) || \
    (SB_BOLTLZ_EXACT_COST != 0 && SB_BOLTLZ_EXACT_COST != 1) || \
    (SB_BOLTLZ_PRUNE_MATCH != 0 && SB_BOLTLZ_PRUNE_MATCH != 1) || \
    (SB_BOLTLZ_REUSE_MATCH != 0 && SB_BOLTLZ_REUSE_MATCH != 1) || \
    (SB_BOLTLZ_BACKWARD_MATCH != 0 && SB_BOLTLZ_BACKWARD_MATCH != 1) || \
    (SB_BOLTLZ_HASH_CACHE != 0 && SB_BOLTLZ_HASH_CACHE != 1) || \
    (SB_BOLTLZ_WORD_MATCH != 0 && SB_BOLTLZ_WORD_MATCH != 1)
#error "BoltLZ experiment switches must be zero or one"
#endif
#endif
