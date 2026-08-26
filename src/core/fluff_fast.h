/*
 * fluff_fast.h -- parallel fluff compression for modern systems
 *
 * Provides block-level parallel compression using OpenMP. Consecutive blocks
 * are compressed in small groups: the first block in each group resets the
 * history, and later blocks in the same group can reference earlier blocks in
 * that group.
 *
 * Trade-off vs sequential fluff:
 *   - Compression speed: ~N× faster on N cores
 *   - Compression ratio: somewhat worse than sequential fluff, but better
 *     than the fully independent per-block mode
 *
 * Wire format: same archive method ID (BP_METHOD_FLUFF = 3). The first
 * compressed block in each fast-fluff group uses
 * FLUFF_STREAM_VERSION_FSE_RESET (0x87), signalling to the decoder that it
 * must reset its ring buffer before decoding that block. Later compressed
 * blocks in the same group keep the normal 0x07 stream version.
 *
 * This header is intentionally C89-compatible so it can be included from
 * archiver.c (which must remain strict C89).  The IMPLEMENTATION (fluff_fast.c)
 * requires C11 and OpenMP.
 *
 * Do NOT include this header on vintage or resource-constrained platforms.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_FLUFF_FAST_H
#define BP_FLUFF_FAST_H

#include "bp_types.h"
#include "fluff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Context ─────────────────────────────────────────────────────────────── */

/* Opaque context: holds one fluff_ctx_t per OpenMP thread. */
typedef struct fluff_fast_ctx_s fluff_fast_ctx_t;

/*
 * fluff_fast_ctx_alloc: allocate a fast-fluff context for up to nthreads
 * simultaneous blocks.
 *
 * nthreads <= 0: use the OpenMP default thread count (omp_get_max_threads()).
 * Returns NULL on allocation failure.
 */
fluff_fast_ctx_t *fluff_fast_ctx_alloc(int nthreads);

/* Free a fast-fluff context and all per-thread state. */
void fluff_fast_ctx_free(fluff_fast_ctx_t *fctx);

/*
 * Initialize the SIMD matcher dispatch used by the modern fast-fluff path.
 * Safe to call multiple times.
 */
void fluff_fast_init_matcher(void);

/*
 * Compare two in-memory byte ranges and return the number of equal leading
 * bytes, up to max bytes.  This is a C89-callable wrapper around the SIMD
 * matcher used by modern fluff builds.
 */
bp_uint32 fluff_fast_match_len(const bp_uint8 *a, const bp_uint8 *b,
                               bp_uint32 max);

/*
 * Same as fluff_fast_match_len, but assumes fluff_fast_init_matcher() has
 * already run.  This is used by fluff.c's match-finder hot path to avoid
 * repeating the initialization check for every candidate comparison.
 */
bp_uint32 fluff_fast_match_len_ready(const bp_uint8 *a, const bp_uint8 *b,
                                     bp_uint32 max);

/* ── Compression ─────────────────────────────────────────────────────────── */

/*
 * fluff_fast_compress_blocks: compress num_blocks independent blocks in
 * parallel using OpenMP.
 *
 *   srcs[i]     pointer to input data for block i
 *   src_lens[i] byte length of input block i
 *   dsts[i]     output buffer for block i
 *               MUST be at least fluff_compress_bound(src_lens[i]) bytes
 *   dst_lens[i] set on output to the actual compressed (or stored) size
 *
 * Blocks are encoded in small consecutive groups. The first block in each
 * group is context-independent; later blocks in the same group may use that
 * group's history. Blocks that do not compress to < 99 % of their original
 * size are stored verbatim (raw copy) and dst_lens[i] is set to src_lens[i].
 *
 * The stream-header version byte of the first compressed block in each group
 * is set to FLUFF_STREAM_VERSION_FSE_RESET (0x87) so the decoder resets its
 * context before decoding that group.
 *
 * Returns BP_OK if all blocks succeeded.  On error, the global return code
 * is set to the first non-OK result; remaining blocks still get a stored
 * fallback so dst_lens[i] is always valid on return.
 *
 * Thread-safety: concurrent calls are safe if they use distinct ctx objects.
 */
int fluff_fast_compress_blocks(
    const bp_uint8  **srcs,
    const bp_uint32  *src_lens,
    bp_uint8        **dsts,
    bp_uint32        *dst_lens,
    int               num_blocks,
    fluff_fast_ctx_t *fctx);

/*
 * Variant used by the archiver when it has the previous batch's decoded tail.
 * history/history_len are the bytes immediately preceding srcs[0] in the same
 * file, and start_file_pos is the absolute uncompressed offset of srcs[0]
 * modulo 2^32 (matching the core Fluff context counter).
 */
int fluff_fast_compress_blocks_with_history(
    const bp_uint8  **srcs,
    const bp_uint32  *src_lens,
    bp_uint8        **dsts,
    bp_uint32        *dst_lens,
    int               num_blocks,
    fluff_fast_ctx_t *fctx,
    const bp_uint8   *history,
    bp_uint32         history_len,
    bp_uint32         start_file_pos);

/* ── Configuration ───────────────────────────────────────────────────────── */

/*
 * fluff_fast_set_threads: override the default thread count used when
 * fluff_fast_ctx_alloc is called with nthreads <= 0.
 * n <= 0 restores the omp_get_max_threads() default.
 */
void fluff_fast_set_threads(int n);

/*
 * fluff_fast_get_threads: return the effective thread count that would be
 * used by a new fluff_fast_ctx_alloc(0) call.
 */
int fluff_fast_get_threads(void);

/*
 * fluff_fast_set_group_blocks: override the number of consecutive blocks in
 * each fast-fluff group. The first block in each group resets history; later
 * blocks may reference earlier blocks in the same group.
 * n <= 0 restores the built-in default.
 */
void fluff_fast_set_group_blocks(int n);

/*
 * fluff_fast_get_group_blocks: return the effective group size used by the
 * fast-fluff block compressor.
 */
int fluff_fast_get_group_blocks(void);

#ifdef __cplusplus
}
#endif

#endif /* BP_FLUFF_FAST_H */
