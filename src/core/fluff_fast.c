/*
 * fluff_fast.c -- parallel fluff compression for modern systems (C11 + OpenMP)
 *
 * Compresses multiple 256 KB blocks simultaneously using OpenMP.
 * Consecutive blocks are compressed in small groups.  When the archiver passes
 * the preceding decoded history, group leaders are warm-started from that
 * history and keep the normal stream version.  If a leader truly has to reset
 * history, its stream header is patched to the reset version.
 *
 * The stream-header version byte is patched from 0x08 to 0x88 only on the
 * first compressed block in each group.  The decoder resets its ring buffer
 * for those blocks and preserves it across the following in-group blocks.
 *
 * SIMD acceleration is provided by fluff_simd.h for the match-length
 * comparison in the match finder.  The global dispatch pointer is defined
 * here (FLUFF_SIMD_DEFINE_GLOBALS) and initialised by fluff_fast_ctx_alloc.
 *
 * C11 and OpenMP are required.  This file must NOT be compiled for vintage
 * or resource-constrained targets.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 201112L)
#  error "fluff_fast.c requires C11 or later (-std=c11 or higher)"
#endif

#ifndef _OPENMP
#  error "fluff_fast.c requires OpenMP (-fopenmp)"
#endif

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <omp.h>

#include "bp_types.h"
#include "fluff.h"
#include "fluff_fast.h"

/*
 * Define the global SIMD dispatch pointer here (once, in this translation
 * unit).  All other TUs that include fluff_simd.h see an extern declaration.
 */
#define FLUFF_SIMD_DEFINE_GLOBALS
#include "fluff_simd.h"

/* ── Module-level default thread count ─────────────────────────────────── */

#define FLUFF_FAST_DEFAULT_GROUP_BLOCKS 2

static int g_fast_nthreads = 0; /* 0 = delegate to omp_get_max_threads() */
static int g_fast_group_blocks = 0; /* 0 = use FLUFF_FAST_DEFAULT_GROUP_BLOCKS */
static atomic_int g_fast_simd_init_state = ATOMIC_VAR_INIT(0);

void fluff_fast_init_matcher(void)
{
    int expected;

    if (atomic_load_explicit(&g_fast_simd_init_state, memory_order_acquire) == 2) {
        return;
    }

    expected = 0;
    if (atomic_compare_exchange_strong_explicit(&g_fast_simd_init_state,
                                                &expected, 1,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        fluff_simd_init();
        atomic_store_explicit(&g_fast_simd_init_state, 2, memory_order_release);
        return;
    }

    while (atomic_load_explicit(&g_fast_simd_init_state, memory_order_acquire) != 2) {
        /* another thread is selecting the dispatch target */
    }
}

bp_uint32 fluff_fast_match_len(const bp_uint8 *a, const bp_uint8 *b,
                               bp_uint32 max)
{
    if (a == NULL || b == NULL || max == 0) {
        return 0;
    }

    fluff_fast_init_matcher();
    return fluff_fast_match_len_ready(a, b, max);
}

bp_uint32 fluff_fast_match_len_ready(const bp_uint8 *a, const bp_uint8 *b,
                                     bp_uint32 max)
{
    size_t len;

    if (a == NULL || b == NULL || max == 0) {
        return 0;
    }

    len = fluff_simd_match_len(a, b, (size_t)max);
    if (len > (size_t)max) {
        len = (size_t)max;
    }
    return (bp_uint32)len;
}

void fluff_fast_set_threads(int n)
{
    g_fast_nthreads = (n > 0) ? n : 0;
}

int fluff_fast_get_threads(void)
{
    if (g_fast_nthreads > 0) {
        return g_fast_nthreads;
    }
    return omp_get_max_threads();
}

void fluff_fast_set_group_blocks(int n)
{
    g_fast_group_blocks = (n > 0) ? n : 0;
}

int fluff_fast_get_group_blocks(void)
{
    if (g_fast_group_blocks > 0) {
        return g_fast_group_blocks;
    }
    return FLUFF_FAST_DEFAULT_GROUP_BLOCKS;
}

/* ── Context ─────────────────────────────────────────────────────────────── */

struct fluff_fast_ctx_s {
    fluff_ctx_t **thread_ctxs; /* one per thread, index = omp_get_thread_num() */
    int           nthreads;
};

fluff_fast_ctx_t *fluff_fast_ctx_alloc(int nthreads)
{
    fluff_fast_ctx_t *fctx;
    int               nt;
    int               i;

    /* Ensure the SIMD dispatch table is selected before any worker uses it. */
    fluff_fast_init_matcher();

    nt = (nthreads > 0) ? nthreads
       : (g_fast_nthreads > 0) ? g_fast_nthreads
       : omp_get_max_threads();
    if (nt < 1) nt = 1;

    fctx = (fluff_fast_ctx_t *)malloc(sizeof(fluff_fast_ctx_t));
    if (!fctx) return NULL;

    fctx->thread_ctxs = (fluff_ctx_t **)calloc((size_t)nt, sizeof(fluff_ctx_t *));
    if (!fctx->thread_ctxs) {
        free(fctx);
        return NULL;
    }
    fctx->nthreads = nt;

    for (i = 0; i < nt; i++) {
        fctx->thread_ctxs[i] = fluff_ctx_alloc();
        if (!fctx->thread_ctxs[i]) {
            /* Free already-allocated contexts */
            while (--i >= 0) {
                fluff_ctx_free(fctx->thread_ctxs[i]);
            }
            free(fctx->thread_ctxs);
            free(fctx);
            return NULL;
        }
        fluff_ctx_reset(fctx->thread_ctxs[i]);
    }

    return fctx;
}

void fluff_fast_ctx_free(fluff_fast_ctx_t *fctx)
{
    int i;
    if (!fctx) return;
    for (i = 0; i < fctx->nthreads; i++) {
        fluff_ctx_free(fctx->thread_ctxs[i]);
    }
    free(fctx->thread_ctxs);
    free(fctx);
}

static int fluff_fast_sources_contiguous(const bp_uint8 **srcs,
                                         const bp_uint32 *src_lens,
                                         int num_blocks)
{
    int i;

    if (num_blocks <= 1) {
        return 1;
    }
    for (i = 1; i < num_blocks; i++) {
        if (srcs[i] != srcs[i - 1] + src_lens[i - 1]) {
            return 0;
        }
    }
    return 1;
}

static bp_uint32 fluff_fast_prefix_len(const bp_uint32 *src_lens,
                                       int count)
{
    bp_uint32 total;
    int i;

    total = 0;
    for (i = 0; i < count; i++) {
        total += src_lens[i];
    }
    return total;
}

/* ── Parallel block compressor ──────────────────────────────────────────── */

int fluff_fast_compress_blocks(
    const bp_uint8  **srcs,
    const bp_uint32  *src_lens,
    bp_uint8        **dsts,
    bp_uint32        *dst_lens,
    int               num_blocks,
    fluff_fast_ctx_t *fctx)
{
    return fluff_fast_compress_blocks_with_history(srcs, src_lens, dsts,
                                                   dst_lens, num_blocks,
                                                   fctx, NULL, 0, 0);
}

int fluff_fast_compress_blocks_with_history(
    const bp_uint8  **srcs,
    const bp_uint32  *src_lens,
    bp_uint8        **dsts,
    bp_uint32        *dst_lens,
    int               num_blocks,
    fluff_fast_ctx_t *fctx,
    const bp_uint8   *history,
    bp_uint32         history_len,
    bp_uint32         start_file_pos)
{
    int global_rc = BP_OK;
    int group_blocks;
    int num_groups;
    int stream_warm;

    if (!srcs || !src_lens || !dsts || !dst_lens || !fctx) {
        return BP_ERR_INVALID_ARG;
    }
    if (history_len != 0 && history == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (num_blocks <= 0) return BP_OK;

    group_blocks = fluff_fast_get_group_blocks();
    if (group_blocks < 1) group_blocks = 1;
    if (group_blocks > num_blocks) group_blocks = num_blocks;
    num_groups = (num_blocks + group_blocks - 1) / group_blocks;
    stream_warm = fluff_fast_sources_contiguous(srcs, src_lens, num_blocks);

    /*
     * Compress block groups in parallel. schedule(dynamic) distributes work
     * evenly when groups vary in size or compressibility. Each thread picks
     * its own fluff_ctx_t by thread ID to avoid races.
     *
     * Data sharing is declared explicitly (default(none)). Every value the
     * workers only read — the loop bound, the group geometry, and the array
     * base pointers — is firstprivate, so each thread reads a thread-local
     * copy instead of the master thread's stack frame. Without this, TSan
     * reports the workers' read of e.g. num_groups as a data race against the
     * master's stack (a benign read-only access, but it pollutes the report
     * and hides any real race). Only global_rc is shared, and every write to
     * it is inside an omp critical section. The slab memory the base pointers
     * refer to is owned by the caller and handed off before this region.
     */
#pragma omp parallel for schedule(dynamic) num_threads(fctx->nthreads) \
        default(none) \
        firstprivate(num_groups, group_blocks, num_blocks, stream_warm, \
                     start_file_pos, history, history_len, \
                     srcs, src_lens, dsts, dst_lens, fctx) \
        shared(global_rc)
    for (int g = 0; g < num_groups; g++) {
        int             tid  = omp_get_thread_num();
        fluff_ctx_t    *ctx;
        int             first_block;
        int             last_block;
        int             i;
        int             group_is_cold;
        bp_uint32       prefix_len;
        bp_uint32       group_file_pos;

        /* Clamp tid defensively — OpenMP may give fewer threads than requested */
        if (tid < 0 || tid >= fctx->nthreads) tid = 0;
        ctx = fctx->thread_ctxs[tid];

        first_block = g * group_blocks;
        last_block = first_block + group_blocks;
        if (last_block > num_blocks) last_block = num_blocks;

        prefix_len = fluff_fast_prefix_len(src_lens, first_block);
        group_file_pos = start_file_pos + prefix_len;
        group_is_cold = 1;

        if (stream_warm && (history_len != 0 || prefix_len != 0)) {
            int preload_rc;

            preload_rc = fluff_ctx_preload(ctx, history, history_len,
                                           srcs[0], prefix_len,
                                           group_file_pos);
            if (preload_rc == BP_OK) {
                group_is_cold = 0;
            } else {
#pragma omp critical
                {
                    if (global_rc == BP_OK) global_rc = preload_rc;
                }
                fluff_ctx_reset(ctx);
            }
        } else {
            fluff_ctx_reset(ctx);
        }

        for (i = first_block; i < last_block; i++) {
            bp_uint32       comp_len;
            int             rc;
            int             is_group_first;
            int             force_reset_block;

            is_group_first = (i == first_block);
            force_reset_block = (is_group_first && group_is_cold &&
                                 group_file_pos != 0);
            comp_len = fluff_compress_bound(src_lens[i]);

            rc = fluff_compress_block_ctx(srcs[i], src_lens[i],
                                          dsts[i], &comp_len, ctx);

            if (rc != BP_OK) {
#pragma omp critical
                {
                    if (global_rc == BP_OK) global_rc = rc;
                }
                /*
                 * Stored fallback: copy raw so dst_lens[i] is always valid.
                 * Mirror the raw bytes into the context so later blocks in
                 * this group see the same history the decoder will reconstruct.
                 */
                memcpy(dsts[i], srcs[i], (size_t)src_lens[i]);
                dst_lens[i] = src_lens[i];
                fluff_ctx_feed_raw(ctx, srcs[i], src_lens[i]);
                continue;
            }

            if (is_group_first && group_is_cold && comp_len >= 3) {
                dsts[i][2] = (bp_uint8)FLUFF_STREAM_VERSION_FSE2_RESET;
            }

            if (force_reset_block && comp_len == src_lens[i] &&
                comp_len < fluff_compress_bound(src_lens[i])) {
                dsts[i][comp_len] = 0;
                comp_len++;
            }

            /*
             * Stored fallback: if the compressed block is >= 99 % of the
             * original size, the space saving is negligible — store raw.
             * fluff_compress_block_ctx already advanced ctx with the source.
             */
            if (!force_reset_block && comp_len > (src_lens[i] * 99u) / 100u) {
                memcpy(dsts[i], srcs[i], (size_t)src_lens[i]);
                dst_lens[i] = src_lens[i];
                continue;
            }

            /*
             * Patch the stream-header version byte (byte 2) from
             * FLUFF_STREAM_VERSION_FSE2 (0x08) to FLUFF_STREAM_VERSION_FSE2_RESET
             * (0x88) only on the first compressed block in the group.
             * Later compressed blocks keep 0x08 so the decoder preserves the
             * group-local ring buffer across them.
             *
             * The stream header layout is:
             *   [0] 'F' (0x46)
             *   [1] 'L' (0x4C)
             *   [2] version byte  ← patch here
             *   [3] window_bits
             */
            dst_lens[i] = comp_len;
        }
    }

    return global_rc;
}
