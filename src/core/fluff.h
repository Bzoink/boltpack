/*
 * fluff.h -- fluff compression wrapper for BoltPack
 *
 * Fluff is BoltPack's native higher-ratio codec. It combines LZ77 match
 * finding (256 KB sliding window, 3-entry repeat-distance ring) with
 * FSE/tANS entropy coding (compact stream version 0x08). The decoder also
 * accepts the legacy FSE format (0x07/0x87) and the original adaptive range
 * coder format (stream version 0x06).
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_FLUFF_H
#define BP_FLUFF_H

#include "bp_types.h"

/*
 * Stream version byte values (byte 2 of every fluff block header).
 * These are public so that fluff_fast.c can patch the version byte after
 * calling fluff_compress_block_ctx().
 */
#define FLUFF_STREAM_VERSION_FSE       0x07 /* FSE/tANS encoding (legacy)           */
#define FLUFF_STREAM_VERSION_FSE_RESET 0x87 /* FSE + per-block context reset signal */
#define FLUFF_STREAM_VERSION_FSE2      0x08 /* compact FSE encoding (default)       */
#define FLUFF_STREAM_VERSION_FSE2_RESET 0x88/* compact FSE + context reset signal   */

/* Forward declare trace context (opaque to this header) */
typedef struct fluff_trace_s fluff_trace_t;

/* Initialize the fluff codec. Returns BP_OK on success. */
int fluff_init(void);

/*
 * Set the default compression level for all subsequent fluff_ctx_reset()
 * calls.  Level 1 is fastest (chain depth 8); level 8 is the historical
 * default (chain depth 256 = FLUFF_MAX_CHAIN); level 9 is slowest/best
 * (chain depth 512).  Invalid values are clamped to [1, 9].
 */
void fluff_set_level(int level);

/* Optional allocator hook for platforms that want Fluff to use a different
 * heap than the C runtime default (for example classic Mac temporary memory).
 * Pass NULL, NULL to restore the default malloc/free behavior. */
typedef void *(*fluff_alloc_fn)(bp_uint32 size);
typedef void (*fluff_free_fn)(void *ptr);
void fluff_set_allocators(fluff_alloc_fn alloc_fn, fluff_free_fn free_fn);

/* Compress a single block using fluff.
 * dst must be at least fluff_compress_bound(src_len) bytes.
 * *dst_len is set to the actual compressed size on output.
 * Returns BP_OK on success. */
int fluff_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                         bp_uint8 *dst, bp_uint32 *dst_len);

/* Decompress a single fluff block.
 * *dst_len must be set to the output buffer size on input.
 * Returns BP_OK on success. */
int fluff_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                           bp_uint8 *dst, bp_uint32 *dst_len);

/* Conservative upper bound for fluff_compress_block output. */
bp_uint32 fluff_compress_bound(bp_uint32 src_len);

/* Required external work memory size (0 -- allocated internally). */
bp_uint32 fluff_compress_work_size(void);

/*
 * Persistent compression context.
 *
 * Holds hash tables and a 256 KB ring buffer that persist across blocks
 * belonging to the same file, enabling cross-block back-references and
 * avoiding per-block malloc overhead.
 *
 * Usage:
 *   ctx = fluff_ctx_alloc();
 *   fluff_ctx_reset(ctx);           -- once per file
 *   for each block:
 *     fluff_compress_block_ctx(src, len, dst, dst_len, ctx);
 *   fluff_ctx_free(ctx);
 */
typedef struct fluff_ctx_s fluff_ctx_t;  /* opaque */

fluff_ctx_t *fluff_ctx_alloc(void);
void         fluff_ctx_free(fluff_ctx_t *ctx);
void         fluff_ctx_reset(fluff_ctx_t *ctx);
void         fluff_ctx_reset_decode(fluff_ctx_t *ctx);
/* Per-context reset with explicit level (1-9); bypasses the global default.
 * Use instead of fluff_ctx_reset() when the level is stored per-archive. */
void         fluff_ctx_reset_level(fluff_ctx_t *ctx, int level);
void         fluff_ctx_feed_raw(fluff_ctx_t *ctx, const bp_uint8 *data,
                                bp_uint32 len);
int          fluff_ctx_preload(fluff_ctx_t *ctx,
                               const bp_uint8 *history1,
                               bp_uint32 history1_len,
                               const bp_uint8 *history2,
                               bp_uint32 history2_len,
                               bp_uint32 file_pos);
bp_uint32    fluff_window_size(void);

int fluff_compress_block_ctx(const bp_uint8 *src, bp_uint32 src_len,
                              bp_uint8 *dst, bp_uint32 *dst_len,
                              fluff_ctx_t *ctx);

int fluff_decompress_block_ctx(const bp_uint8 *src, bp_uint32 src_len,
                                bp_uint8 *dst, bp_uint32 *dst_len,
                                fluff_ctx_t *ctx);

/* Get compression statistics from context. */
void fluff_ctx_get_literal_count(fluff_ctx_t *ctx, bp_uint32 *count);
void fluff_ctx_get_match_count(fluff_ctx_t *ctx, bp_uint32 *count);
void fluff_ctx_get_total_match_bytes(fluff_ctx_t *ctx, bp_uint32 *bytes);
void fluff_ctx_get_chain_stats(fluff_ctx_t *ctx, bp_uint32 *total_probes,
                                bp_uint32 *max_depth);

typedef struct {
    bp_uint32 searches;
    bp_uint32 probes;
    bp_uint32 cmp_bytes;
} fluff_matchfinder_bucket_t;

typedef struct {
    fluff_matchfinder_bucket_t no_match;
    fluff_matchfinder_bucket_t short_filtered;
    fluff_matchfinder_bucket_t accepted_match;
    fluff_matchfinder_bucket_t rep_selected;
    fluff_matchfinder_bucket_t lookahead_discarded;
} fluff_matchfinder_outcome_stats_t;

typedef struct {
    bp_uint32 probes;
    bp_uint32 matches;
    bp_uint32 wins;
    bp_uint32 cmp_bytes;
} fluff_rep_slot_stats_t;

typedef struct {
    bp_uint32 searches;
    fluff_rep_slot_stats_t rep0;
    fluff_rep_slot_stats_t rep1;
    fluff_rep_slot_stats_t rep2;
} fluff_rep_stats_t;

void fluff_ctx_get_matchfinder_stats(fluff_ctx_t *ctx,
                                      bp_uint32 *searches,
                                      bp_uint32 *no_match_saturations,
                                      bp_uint32 *subhash_wins,
                                      bp_uint32 *chain_cmp_bytes);
void fluff_ctx_get_matchfinder_outcome_stats(
    fluff_ctx_t *ctx,
    fluff_matchfinder_outcome_stats_t *stats);
void fluff_ctx_get_rep_stats(fluff_ctx_t *ctx, fluff_rep_stats_t *stats);

/*
 * Trace-enabled versions (for debugging/analysis).
 * Pass NULL for trace_ctx to disable tracing.
 */

int fluff_compress_block_ctx_trace(const bp_uint8 *src, bp_uint32 src_len,
                                    bp_uint8 *dst, bp_uint32 *dst_len,
                                    fluff_ctx_t *ctx,
                                    fluff_trace_t *trace_ctx);

int fluff_decompress_block_ctx_trace(const bp_uint8 *src, bp_uint32 src_len,
                                      bp_uint8 *dst, bp_uint32 *dst_len,
                                      fluff_ctx_t *ctx,
                                      fluff_trace_t *trace_ctx);

/*
 * Parse-trace debug hook (only available when compiled with -DFLUFF_DEBUG_PARSE).
 * Call with a FILE * (e.g. stdout or an open file) before compressing to
 * receive a CSV line per token: abs_pos,action,length,distance,literal_byte,is_lazy
 * Call with NULL to disable.
 */
#ifdef FLUFF_DEBUG_PARSE
#include <stdio.h>
void fluff_set_debug_fp(FILE *fp);
#endif

#endif /* BP_FLUFF_H */
