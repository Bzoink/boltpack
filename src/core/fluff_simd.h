/*
 * fluff_simd.h -- SIMD-accelerated match-length comparison for fluff_fast
 *
 * Finds the number of equal leading bytes between two buffers (up to a
 * maximum), using the best SIMD available on the current platform.
 *
 * Supported accelerated paths:
 *   - x86-64 AVX2  (runtime-detected, requires __AVX2__ at compile time)
 *   - x86-64 SSE2  (always available on 64-bit x86, requires __SSE2__)
 *   - ARM64  NEON  (always available on AArch64)
 *   - Scalar fallback for all other platforms
 *
 * Usage:
 *   1. Call fluff_simd_init() once (sets the global dispatch pointer).
 *   2. Call fluff_simd_match_len(a, b, max) in the match-finder hot loop.
 *
 * C11 required.  Do NOT include this header in the C89 core.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_FLUFF_SIMD_H
#define BP_FLUFF_SIMD_H

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 201112L)
#  error "fluff_simd.h requires C11 or later"
#endif

#include <stddef.h>
#include <stdint.h>

#if defined(__SSE2__)
#  include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#  include <arm_neon.h>
#endif

/* ── Scalar fallback ────────────────────────────────────────────────────── */

static inline size_t
fluff_match_len_scalar(const uint8_t *a, const uint8_t *b, size_t max)
{
    size_t n = 0;
    while (n < max && a[n] == b[n]) {
        n++;
    }
    return n;
}

/* ── SSE2: 16 bytes per iteration ───────────────────────────────────────── */

#if defined(__SSE2__)
static inline size_t
fluff_match_len_sse2(const uint8_t *a, const uint8_t *b, size_t max)
{
    size_t n = 0;
    while (n + 16 <= max) {
        __m128i va   = _mm_loadu_si128((const __m128i *)(a + n));
        __m128i vb   = _mm_loadu_si128((const __m128i *)(b + n));
        int     mask = _mm_movemask_epi8(_mm_cmpeq_epi8(va, vb));
        if (mask != 0xFFFF) {
            /* Invert: 1-bits mark mismatches; ctz gives first mismatch offset */
            n += (size_t)__builtin_ctz((unsigned)(~mask & 0xFFFF));
            return n;
        }
        n += 16;
    }
    /* Scalar tail */
    while (n < max && a[n] == b[n]) {
        n++;
    }
    return n;
}
#endif /* __SSE2__ */

/* ── AVX2: 32 bytes per iteration, falls back to SSE2 for tail ──────────── */

#if defined(__AVX2__)
static inline size_t
fluff_match_len_avx2(const uint8_t *a, const uint8_t *b, size_t max)
{
    size_t   n = 0;
    unsigned mask;
    while (n + 32 <= max) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(a + n));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(b + n));
        mask = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(va, vb));
        if (mask != 0xFFFFFFFFU) {
            n += (size_t)__builtin_ctz(~mask);
            return n;
        }
        n += 32;
    }
    /* SSE2 tail (16-byte chunks) */
    while (n + 16 <= max) {
        __m128i va2  = _mm_loadu_si128((const __m128i *)(a + n));
        __m128i vb2  = _mm_loadu_si128((const __m128i *)(b + n));
        int     m16  = _mm_movemask_epi8(_mm_cmpeq_epi8(va2, vb2));
        if (m16 != 0xFFFF) {
            n += (size_t)__builtin_ctz((unsigned)(~m16 & 0xFFFF));
            return n;
        }
        n += 16;
    }
    /* Scalar tail */
    while (n < max && a[n] == b[n]) {
        n++;
    }
    return n;
}
#endif /* __AVX2__ */

/* ── ARM64 NEON: 16 bytes per iteration ─────────────────────────────────── */

#if defined(__aarch64__) || defined(__ARM_NEON)
static inline size_t
fluff_match_len_neon(const uint8_t *a, const uint8_t *b, size_t max)
{
    size_t n = 0;
    while (n + 16 <= max) {
        uint8x16_t va = vld1q_u8(a + n);
        uint8x16_t vb = vld1q_u8(b + n);
        uint8x16_t eq = vceqq_u8(va, vb);
        if (vminvq_u8(eq) == 0xFF) {
            n += 16;
            continue;
        }
        /* Byte-level scan for precise mismatch position */
        while (n < max && a[n] == b[n]) {
            n++;
        }
        return n;
    }
    /* Scalar tail */
    while (n < max && a[n] == b[n]) {
        n++;
    }
    return n;
}
#endif /* aarch64 / NEON */

/* ── Runtime dispatch ───────────────────────────────────────────────────── */

typedef size_t (*fluff_match_len_fn)(const uint8_t *, const uint8_t *, size_t);

/*
 * Global function pointer.  Set by fluff_simd_init().
 * Defined (once) in fluff_fast.c via FLUFF_SIMD_DEFINE_GLOBALS.
 */
#ifdef FLUFF_SIMD_DEFINE_GLOBALS
fluff_match_len_fn fluff_simd_match_len_ptr = fluff_match_len_scalar;
#else
extern fluff_match_len_fn fluff_simd_match_len_ptr;
#endif

/*
 * fluff_simd_init: select the best available SIMD path and store it in
 * fluff_simd_match_len_ptr.  Call once before any fluff_simd_match_len().
 * After initialization the pointer is read-only; concurrent reads are safe.
 */
static inline void
fluff_simd_init(void)
{
#if defined(__AVX2__)
    if (__builtin_cpu_supports("avx2")) {
        fluff_simd_match_len_ptr = fluff_match_len_avx2;
        return;
    }
#endif
#if defined(__SSE2__)
    fluff_simd_match_len_ptr = fluff_match_len_sse2;
    return;
#endif
#if defined(__aarch64__) || defined(__ARM_NEON)
    fluff_simd_match_len_ptr = fluff_match_len_neon;
    return;
#endif
    fluff_simd_match_len_ptr = fluff_match_len_scalar;
}

/*
 * fluff_simd_match_len: return count of equal leading bytes in [a, b),
 * up to max bytes.  Dispatches through the function pointer set by
 * fluff_simd_init().
 */
static inline size_t
fluff_simd_match_len(const uint8_t *a, const uint8_t *b, size_t max)
{
    return fluff_simd_match_len_ptr(a, b, max);
}

#endif /* BP_FLUFF_SIMD_H */
