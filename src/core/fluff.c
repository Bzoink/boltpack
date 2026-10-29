/*
 * fluff.c -- fluff compression codec for BoltPack
 *
 * Self-contained LZ77 + FSE/tANS compressor/decompressor.
 * Uses a 256 KB window, matches up to 258 bytes, and a three-distance
 * repeat ring. Compression levels control hash-chain search depth.
 *
 * The stream format is NOT compatible with DEFLATE/zlib/gzip.
 *
 * ANSI C (C89/C90) only. No compiler extensions.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifdef __PLAN9__
#include <u.h>
#include <libc.h>
#else
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifdef FLUFF_DEBUG_PARSE
#include <stdio.h>
static FILE *s_dbg_fp = NULL;
void fluff_set_debug_fp(FILE *fp) { s_dbg_fp = fp; }
#endif

/*
 * Plan 9 compatibility: Plan 9 doesn't provide <limits.h>.
 * Other POSIX systems should have it for full compliance.
 */
#if !defined(__PLAN9__)
  #include <limits.h>
#endif

#include "fluff.h"
#ifdef BOLTPACK_FAST_FLUFF
#include "fluff_fast.h"
#endif

static fluff_alloc_fn s_fluff_alloc = NULL;
static fluff_free_fn s_fluff_free = NULL;

void fluff_set_allocators(fluff_alloc_fn alloc_fn, fluff_free_fn free_fn)
{
    s_fluff_alloc = alloc_fn;
    s_fluff_free = free_fn;
}

static void *fl_malloc(size_t size)
{
    if (s_fluff_alloc != NULL) {
        return s_fluff_alloc((bp_uint32)size);
    }
    return malloc(size);
}

static void fl_free(void *ptr)
{
    if (ptr == NULL) return;
    if (s_fluff_free != NULL) {
        s_fluff_free(ptr);
    } else {
        free(ptr);
    }
}

static void *fl_calloc(size_t count, size_t size)
{
    void *ptr;
    if (s_fluff_alloc == NULL) return calloc(count, size);
    if (count != 0 && size > (size_t)-1 / count) return NULL;
    ptr = fl_malloc(count * size);
    if (ptr != NULL) memset(ptr, 0, count * size);
    return ptr;
}

#ifdef calloc
#undef calloc
#endif
#define malloc  fl_malloc
#define free    fl_free
#define calloc  fl_calloc

/*
 * FLUFF_STATS: when defined, the match finder maintains detailed parse and
 * chain-search counters (probes, comparisons, per-outcome and per-rep-slot
 * breakdowns) exposed through the fluff_ctx_get_*() API for analysis tools.
 * Off by default so release builds skip the per-search counter writes in the
 * hot loop.  The getters remain present and simply report zero when disabled.
 *
 * FL_STAT(stmt) expands to the statement only when stats are enabled; to an
 * empty statement otherwise.  Use only for pure bookkeeping with no effect on
 * the emitted bytes.
 */
#ifdef FLUFF_STATS
#  define FL_STAT(stmt) stmt
#else
#  define FL_STAT(stmt)
#endif

/* ─── Tunables ──────────────────────────────────────────────────────────── */

/*
 * Window bits: 18 = 256 KB window.  Going beyond 21 (2 MB) gives
 * diminishing returns on most data.
 */
#define FLUFF_WINDOW_BITS    18
#define FLUFF_WINDOW_SIZE    (1U << FLUFF_WINDOW_BITS)   /* 262144 */
#define FLUFF_WINDOW_MASK    (FLUFF_WINDOW_SIZE - 1U)

#define FLUFF_MIN_MATCH      3
#define FLUFF_MAX_MATCH      258

#define FLUFF_HASH_BITS      17
#define FLUFF_HASH_SIZE      (1U << FLUFF_HASH_BITS)
#define FLUFF_HASH_MASK      (FLUFF_HASH_SIZE - 1U)
#define FLUFF_HASH_SHIFT     5

/*
 * 3-byte sub-hash: 16-bit index into a 64-KB table.
 * Each entry stores the most-recent absolute position whose first 3 bytes
 * hash to that slot.  Used as a fast fallback when the 4-byte chain misses
 * or returns only a len=3 match — the 3-byte table is far more densely
 * populated and almost always finds a short near-match cheaply.
 */
#define FLUFF_H3_BITS        16
#define FLUFF_H3_SIZE        (1U << FLUFF_H3_BITS)   /* 65536 entries */
#define FLUFF_H3_MASK        (FLUFF_H3_SIZE - 1U)

#ifndef FLUFF_MAX_CHAIN
#define FLUFF_MAX_CHAIN      256    /* default chain depth */
#endif

#define FLUFF_GOOD_MATCH     64     /* stop lazy if match >= this */

/*
 * Distance-aware minimum match length.
 *
 * A len=3 match at dist=513 costs >=25 bits (18 token overhead + 7 dist extra)
 * but only replaces 24 bits of literals: net loss.  Similarly len=4 at dist
 * >32768 uses >=14 dist extra bits, making it borderline at best.
 *
 * These thresholds are the break-even points from the model:
 *   match_overhead (18) + dist_extra >= len * 8
 *
 * FLUFF_MIN_DIST_LEN4: minimum distance above which len==3 is rejected.
 *   dist_sym 16 (dist 513-768) has 7 extra bits: 18+7=25 >= 3*8=24 => reject.
 * FLUFF_MIN_DIST_LEN5: minimum distance above which len==4 is also rejected.
 *   v0x05 dist_sym 44 (dist 49153+) has 14 extra bits: 18+14=32 >= 4*8=32.
 */
#ifndef FLUFF_MIN_DIST_LEN4
#define FLUFF_MIN_DIST_LEN4  512U   /* reject len<=3 when dist > this */
#endif
#ifndef FLUFF_MIN_DIST_LEN5
#define FLUFF_MIN_DIST_LEN5  49152U /* reject len<=4 when dist > this */
#endif

/*
 * Huffman alphabet sizes.
 *
 * Literal/length alphabet:
 *   0..255   = literal bytes
 *   256      = end-of-block
 *   257..285 = match lengths 3..258 (same as DEFLATE)
 *   286..288 = rep-match symbols
 *
 * Distance alphabet:
 *   0..17    = same base distances as DEFLATE (up to 512)
 *   18..41   = denser medium-distance buckets up to 32768
 *   42..47   = extended distances out to the 256 KB window
 *
 * Code-length alphabet (for transmitting Huffman trees):
 *   0..18    = same as DEFLATE
 */
#define FLUFF_NUM_LITLEN     289  /* 0-255 literals, 256 EOB, 257-285 lengths, 286-288 rep-matches */
#define FLUFF_NUM_DIST       48
#define FLUFF_NUM_CODELEN    19
#define FLUFF_END_OF_BLOCK   256
#define FLUFF_REP_MATCH      286  /* reuse most-recent distance; encode length only */
#define FLUFF_REP_MATCH2     287  /* reuse second-most-recent distance */
#define FLUFF_REP_MATCH3     288  /* reuse third-most-recent distance */
#define FLUFF_MAX_BITS       15

#define FLUFF_STREAM_VERSION          0x06
#define FLUFF_STREAM_VERSION_FSE      0x07
/*
 * 0x87 = FLUFF_STREAM_VERSION_FSE_RESET: FSE block with context-independent
 * encoding. The decoder must call fluff_ctx_reset() before decoding this
 * block (no cross-block ring-buffer references). Written by fluff_fast.c.
 * Old decoders that only accept 0x06/0x07 will return BP_ERR_CORRUPT, which
 * is the correct behavior since they cannot decode correctly without a reset.
 */
#define FLUFF_STREAM_VERSION_FSE_RESET 0x87
/*
 * 0x08 / 0x88 = FLUFF_STREAM_VERSION_FSE2 and its reset variant: a more
 * compact FSE block (stream version 2).  Relative to 0x07 it (R1) stores the
 * normalized count tables with a varint + zero-run code instead of a raw
 * uint16 dump and (R3) shrinks the fixed header fields (initial states
 * uint16, stream lengths 24-bit).  The token streams — literals, regular
 * matches, and rep-match lengths (flat 5-bit raw index) — are byte-for-byte
 * identical to 0x07.  0x88 carries the same per-block context-reset signal
 * that 0x87 does for 0x07.
 *
 * (Entropy-coding rep-match lengths through the litlen FSE table was
 * evaluated and measured net-negative on every corpus file — folding rep
 * lengths into the litlen table distorts it enough to outweigh the saving —
 * so it was not adopted.)
 */
#define FLUFF_STREAM_VERSION_FSE2       0x08
#define FLUFF_STREAM_VERSION_FSE2_RESET 0x88
/*
 * 0x09 / 0x89 keep the compact header and token alphabets, but use ANS
 * virtual states in [L, 2L), stored with L subtracted. This changes both
 * FSE streams; the decoder retains the old interval layout for 0x07/0x08.
 * Public version constants are declared in fluff.h.
 */

/* ─── Portable helpers ──────────────────────────────────────────────────── */

typedef unsigned char  fl_u8;
typedef unsigned short fl_u16;
typedef unsigned int   fl_u32;

static fl_u32 fl_match_len_direct(const fl_u8 *a, const fl_u8 *b,
                                  fl_u32 max)
{
#ifdef BOLTPACK_FAST_FLUFF
    return (fl_u32)fluff_fast_match_len_ready((const bp_uint8 *)a,
                                              (const bp_uint8 *)b,
                                              (bp_uint32)max);
#else
    fl_u32 len = 0;
    fl_u32 wa, wb;
    /* Word-at-a-time: memcpy into fl_u32 is safe for unaligned access in C89.
     * Compilers reduce this to a single 32-bit load+compare on all real targets. */
    while (len + 4 <= max) {
        memcpy(&wa, a + len, 4);
        memcpy(&wb, b + len, 4);
        if (wa != wb) break;
        len += 4;
    }
    while (len < max && a[len] == b[len]) len++;
    return len;
#endif
}

/* ─── Portable 64-bit type for range coder carry accumulator ─────────── */
#if defined(__GNUC__) || defined(__clang__)
  __extension__ typedef unsigned long long fl_u64;
#elif defined(_MSC_VER)
  typedef unsigned __int64 fl_u64;
#elif (ULONG_MAX > 0xFFFFFFFFUL)
  typedef unsigned long fl_u64;    /* LP64: unsigned long is 64 bits */
#else
  typedef unsigned long fl_u64;    /* fallback; range coder needs 64-bit host */
#endif

#define FL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define FL_MAX(a, b) ((a) > (b) ? (a) : (b))

/* ─── Static tables: length and distance codes ──────────────────────────── */
/* NOTE: s_len_base, s_len_extra, s_dist_base, s_dist_extra are declared here
 * and referenced by the LUT builder below them. Forward declaration order in
 * the file keeps everything in one place. */

/*
 * Length code table: symbol 257..285 -> (base_length, extra_bits)
 * Identical to DEFLATE.
 */
static const fl_u16 s_len_base[] = {
    /* 257..264 */ 3,4,5,6,7,8,9,10,
    /* 265..268 */ 11,13,15,17,
    /* 269..272 */ 19,23,27,31,
    /* 273..276 */ 35,43,51,59,
    /* 277..280 */ 67,83,99,115,
    /* 281..284 */ 131,163,195,227,
    /* 285      */ 258
};

static const fl_u8 s_len_extra[] = {
    /* 257..264 */ 0,0,0,0,0,0,0,0,
    /* 265..268 */ 1,1,1,1,
    /* 269..272 */ 2,2,2,2,
    /* 273..276 */ 3,3,3,3,
    /* 277..280 */ 4,4,4,4,
    /* 281..284 */ 5,5,5,5,
    /* 285      */ 0
};

/*
 * v0x05 distance code table: symbol 0..47 -> (base_dist, extra_bits)
 * Short distances are unchanged, while 513..32768 gets four-way buckets
 * to shave one extra bit from the common medium-distance ranges.
 */
static const fl_u32 s_dist_base[] = {
    1,2,3,4, 5,7,9,13,
    17,25,33,49, 65,97,129,193,
    257,385,
    513,641,769,897,
    1025,1281,1537,1793,
    2049,2561,3073,3585,
    4097,5121,6145,7169,
    8193,10241,12289,14337,
    16385,20481,24577,28673,
    32769,40961,49153,65537,98305,131073
};

static const fl_u8 s_dist_extra[] = {
    0,0,0,0, 1,1,2,2,
    3,3,4,4, 5,5,6,6,
    7,7,
    7,7,7,7,
    8,8,8,8,
    9,9,9,9,
    10,10,10,10,
    11,11,11,11,
    12,12,12,12,
    13,13,14,15,15,17
};

#define FLUFF_NUM_LEN_SYMS      (sizeof(s_len_base) / sizeof(s_len_base[0]))
#define FLUFF_NUM_DIST_SYMS_V04 (sizeof(s_dist_base_v04) / sizeof(s_dist_base_v04[0]))
#define FLUFF_NUM_DIST_SYMS     (sizeof(s_dist_base) / sizeof(s_dist_base[0]))

/* ─── Length encoding LUT ────────────────────────────────────────────────── */

/*
 * Indexed by (length - FLUFF_MIN_MATCH) for length in [3, 258].
 * 256 entries; built once in fl_build_luts() called from fluff_init() and
 * fluff_ctx_reset().  After that, fl_len_to_sym is O(1).
 */
typedef struct {
    fl_u16 sym;        /* litlen symbol 257..285   */
    fl_u8  extra_bits; /* extra bits count          */
    fl_u16 base;       /* base length for this sym  */
} fl_len_lut_t;

static fl_len_lut_t s_len_lut[FLUFF_MAX_MATCH - FLUFF_MIN_MATCH + 1]; /* 256 */

/*
 * Fast distance extra-bits lookup for the savings estimator.
 * s_dist_eb_lut[dist-1] for dist in [1, 512] (symbols 0..17, 7 bits or less).
 * Built by fl_build_luts().
 */
static fl_u8 s_dist_eb_lut[512];

static int s_luts_built = 0;

/*
 * Portable floor(log2(x)) for non-zero x (equivalent to BSR on x86).
 * Used by fl_dist_extra_bits_fast for the medium-distance range.
 */
static fl_u32 fl_highbit32(fl_u32 x)
{
    fl_u32 n = 0;
    if (x >= 0x10000U) { n += 16; x >>= 16; }
    if (x >= 0x100U)   { n +=  8; x >>=  8; }
    if (x >= 0x10U)    { n +=  4; x >>=  4; }
    if (x >= 0x4U)     { n +=  2; x >>=  2; }
    if (x >= 0x2U)     { n +=  1; }
    return n;
}

static void fl_build_luts(void)
{
    fl_u32 i, j, s;

    if (s_luts_built) return;

    /* Length LUT: for each length symbol fill every length value it encodes. */
    for (i = 0; i < (fl_u32)FLUFF_NUM_LEN_SYMS; i++) {
        fl_u32 sym  = 257U + i;
        fl_u32 base = (fl_u32)s_len_base[i];
        fl_u32 eb   = (fl_u32)s_len_extra[i];
        fl_u32 cnt  = 1U << eb;
        for (j = 0; j < cnt; j++) {
            fl_u32 idx = base + j - FLUFF_MIN_MATCH;
            if (idx < 256U) {
                s_len_lut[idx].sym        = (fl_u16)sym;
                s_len_lut[idx].extra_bits = (fl_u8)eb;
                s_len_lut[idx].base       = (fl_u16)base;
            }
        }
    }

    /* Distance extra-bits LUT for dist 1..512 (one forward sweep). */
    s = 0;
    for (i = 0; i < 512U; i++) {
        fl_u32 dist = i + 1U;
        while (s + 1U < (fl_u32)FLUFF_NUM_DIST_SYMS &&
               dist >= (fl_u32)s_dist_base[s + 1U]) {
            s++;
        }
        s_dist_eb_lut[i] = s_dist_extra[s];
    }

    s_luts_built = 1;
}

/*
 * O(1)/O(log N) distance extra-bits lookup for fl_match_savings_est.
 *
 * dist 1..512   : direct 512-entry byte table (O(1)).
 * dist 513..32768: symbols 18..41 form regular 4-per-level groups where
 *                  eb = floor(log2(dist-1)) - 2, verified from the table.
 * dist > 32768  : scan only the last 6 symbols (42..47).
 */
static fl_u32 fl_dist_extra_bits_fast(fl_u32 dist)
{
    if (dist <= 512U) {
        return (fl_u32)s_dist_eb_lut[dist - 1U];
    }
    if (dist <= 32768U) {
        /* For syms 18..41 the table has eb = highbit(dist-1) - 2. */
        return fl_highbit32(dist - 1U) - 2U;
    }
    /* Scan symbols 42..47 only (6 entries). */
    {
        fl_u32 i;
        for (i = (fl_u32)FLUFF_NUM_DIST_SYMS; i > 42U; i--) {
            if (dist >= (fl_u32)s_dist_base[i - 1U]) {
                return (fl_u32)s_dist_extra[i - 1U];
            }
        }
        return (fl_u32)s_dist_extra[41]; /* defensive fallback */
    }
}

/* ─── Adaptive range coder ──────────────────────────────────────────── */

/*
 * Probability constants.
 *
 * FL_PROB_MAX = 4096 (12-bit fixed-point probability).
 * FL_PROB_HALF = 2048 = neutral 50/50 starting probability.
 * FL_ADAPT_SHIFT = 5 -> update step ~= p / 32 (fast convergence).
 */
#define FL_RC_TOP_BITS  24
#define FL_RC_TOP       (1UL << FL_RC_TOP_BITS)   /* 16777216 */
#define FL_PROB_BITS    12
#define FL_PROB_MAX     (1U << FL_PROB_BITS)       /* 4096 */
#define FL_PROB_HALF    (FL_PROB_MAX >> 1)         /* 2048 */
#define FL_ADAPT_SHIFT  5

/*
 * Adaptive probability for a single binary choice.
 * Stored as an unsigned 16-bit value in [0, FL_PROB_MAX).
 * Value p means P(bit==0) = p / FL_PROB_MAX.
 */
typedef fl_u16 fl_prob_t;

/*
 * litlen model: 9-bit binary tree decomposition.
 * 2^9 = 512 leaves (symbols 0..288 used, 289..511 unused).
 * Tree uses internal node indices 1..511.
 */
#define FL_LL_BITS    9
#define FL_LL_NODES   (1U << FL_LL_BITS)   /* 512 */

/*
 * dist model: 6-bit binary tree decomposition.
 * 2^6 = 64 leaves (symbols 0..47 used, 48..63 unused).
 * Tree uses internal node indices 1..63.
 */
#define FL_D_BITS     6
#define FL_D_NODES    (1U << FL_D_BITS)    /* 64 */

/*
 * Range coder state (shared between encoder and decoder).
 * Encoder uses: low, range, cache, cache_size, out_buf/out_cap/out_pos.
 * Decoder uses: range, code, in_buf/in_len/in_pos.
 */
typedef struct {
    fl_u64  low;         /* encoder: lower bound (64-bit for carry safety) */
    fl_u32  range;       /* interval width */
    fl_u8   cache;       /* carry propagation: pending output byte */
    fl_u32  cache_size;  /* carry propagation: number of pending 0xFF bytes */
    fl_u8  *out_buf;     /* encoder output buffer */
    fl_u32  out_cap;     /* encoder output capacity */
    fl_u32  out_pos;     /* encoder bytes written */
    const fl_u8 *in_buf; /* decoder input buffer */
    fl_u32  in_len;      /* decoder input length */
    fl_u32  in_pos;      /* decoder bytes consumed */
    fl_u32  code;        /* decoder: current fractional position */
} fl_rc_t;

/* fl_rc_out_byte, fl_rc_shift_low, fl_rc_enc_bit, fl_rc_enc_raw,
 * fl_rc_enc_init, fl_rc_enc_finish removed: the v0x06 RC encoder is dead
 * code — compression always writes FSE v0x07.  The RC decoder below is
 * kept intact for reading legacy v0x06 archives. */

static fl_u8 fl_rc_in_byte(fl_rc_t *rc)
{
    if (rc->in_pos < rc->in_len)
        return rc->in_buf[rc->in_pos++];
    return 0;
}

/* Initialize decoder (primes the code register from the first 5 input bytes). */
static void fl_rc_dec_init(fl_rc_t *rc, const fl_u8 *in, fl_u32 in_len)
{
    int i;
    rc->low        = 0;
    rc->range      = 0xFFFFFFFFUL;
    rc->cache      = 0;
    rc->cache_size = 0;
    rc->out_buf    = NULL;
    rc->out_cap    = 0;
    rc->out_pos    = 0;
    rc->in_buf     = in;
    rc->in_len     = in_len;
    rc->in_pos     = 0;
    rc->code       = 0;
    for (i = 0; i < 5; i++)
        rc->code = (rc->code << 8) | fl_rc_in_byte(rc);
}

/*
 * Decode one bit with adaptive probability model.
 * Must use identical model as encoder for encoder/decoder synchrony.
 *
 * Uses the bit-mask idiom to guarantee branch-free code: GCC converts
 * ternaries back to branches, but cannot convert AND/OR of a 0/~0 mask.
 * bit=0 -> neg=0x00000000; bit=1 -> neg=0xFFFFFFFF.
 * This eliminates data-dependent branch mispredictions on the hot path.
 */
static fl_u32 fl_rc_dec_bit(fl_rc_t *rc, fl_prob_t *prob)
{
    fl_u32 p, mid, bit, neg, p0, p1;
    p   = (fl_u32)(*prob);
    mid = (rc->range >> FL_PROB_BITS) * p;
    bit = (fl_u32)(rc->code >= mid);   /* SETcc — no branch */
    neg = (fl_u32)0U - bit;            /* 0x00000000 or 0xFFFFFFFF */
    p0  = p + ((FL_PROB_MAX - p) >> FL_ADAPT_SHIFT);
    p1  = p - (p >> FL_ADAPT_SHIFT);
    rc->code  -= mid & neg;
    rc->range  = (mid & ~neg) | ((rc->range - mid) & neg);
    *prob      = (fl_prob_t)((p1 & neg) | (p0 & ~neg));
    while (rc->range < FL_RC_TOP) {
        rc->code = (rc->code << 8) | fl_rc_in_byte(rc);
        rc->range <<= 8;
    }
    return bit;
}

/* Decode one raw bit (uniform 1/2 probability, no model). */
static fl_u32 fl_rc_dec_raw(fl_rc_t *rc)
{
    fl_u32 mid = rc->range >> 1;
    fl_u32 bit;
    if (rc->code < mid) {
        rc->range = mid;
        bit = 0;
    } else {
        rc->code  -= mid;
        rc->range -= mid;
        bit = 1;
    }
    while (rc->range < FL_RC_TOP) {
        rc->code = (rc->code << 8) | fl_rc_in_byte(rc);
        rc->range <<= 8;
    }
    return bit;
}

/* fl_rc_encode_sym removed: dead code, see H2 note above. */

/*
 * Decode symbol (returns value in [0, 2^bits)) from binary tree.
 * Must use same probs[] as encoder.
 */
static fl_u32 fl_rc_decode_sym(fl_rc_t *rc, fl_prob_t *probs, fl_u32 bits)
{
    fl_u32 ctx = 1;
    fl_u32 i;
    for (i = 0; i < bits; i++) {
        fl_u32 bit = fl_rc_dec_bit(rc, &probs[ctx]);
        ctx = ctx * 2U + bit;
    }
    return ctx - (1U << bits);
}

/*
 * Decode litlen symbol (FL_LL_BITS=9, binary tree, fully unrolled).
 * Avoids loop overhead and loop-back branch in the hot decompression path;
 * also allows the compiler to pipeline adjacent fl_rc_dec_bit calls.
 */
static fl_u32 fl_rc_decode_ll(fl_rc_t *rc, fl_prob_t *probs)
{
    fl_u32 ctx = 1;
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    return ctx - (1U << FL_LL_BITS);
}

/*
 * Decode distance symbol (FL_D_BITS=6, binary tree, fully unrolled).
 */
static fl_u32 fl_rc_decode_dist(fl_rc_t *rc, fl_prob_t *probs)
{
    fl_u32 ctx = 1;
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    ctx = (ctx << 1) | fl_rc_dec_bit(rc, &probs[ctx]);
    return ctx - (1U << FL_D_BITS);
}

/* fl_rc_encode_bits removed: dead code, see H2 note above. */

/* Decode nbits raw bits, MSB first. */
static fl_u32 fl_rc_decode_bits(fl_rc_t *rc, fl_u32 nbits)
{
    fl_u32 val = 0;
    fl_u32 i;
    for (i = 0; i < nbits; i++)
        val = (val << 1) | fl_rc_dec_raw(rc);
    return val;
}

/* Initialize probability table: all entries = FL_PROB_HALF (neutral 50/50). */
static void fl_probs_init(fl_prob_t *probs, fl_u32 count)
{
    fl_u32 i;
    for (i = 0; i < count; i++)
        probs[i] = (fl_prob_t)FL_PROB_HALF;
}

/* ─── FSE (Finite State Entropy / tANS) v0x07 ──────────────────────────── */

/*
 * Finite State Entropy (tANS) implementation.
 *
 * Based on Jarek Duda's asymmetric numeral systems (ANS).  This variant
 * uses a power-of-two table of size L = 2^FSE_BITS and encodes each symbol
 * by looking up its next-state and renormalization shift.
 *
 * Encoder writes bits MSB-first into a uint32 accumulator, flushing bytes
 * to an output buffer.  The ANS state is encoded BACKWARDS (last token
 * first) so the decoder can stream forward.
 *
 * Table sizes:
 *   FSE_LL_BITS = 10  ->  L = 1024  for litlen (289 symbols)
 *   FSE_D_BITS  = 9   ->  L = 512   for dist   (48 symbols)
 *
 * Count table header per alphabet:
 *   One 16-bit value per symbol (little-endian): normalized count in [0,L].
 *   Symbol with count 0 contributes nothing; decoder skips it.
 *   Sum of non-zero counts == L.  Counts are transmitted in full (no RLE),
 *   which costs 289*2 + 48*2 = 674 bytes -- negligible vs block size.
 *
 * If a symbol alphabet degenerates to a single used symbol (all tokens
 * the same), that symbol gets count == L and all others get 0.
 *
 * Block layout (v0x07):
 *   [4-byte stream header]
 *   [2*(FLUFF_NUM_LITLEN) bytes: litlen counts, little-endian uint16]
 *   [2*(FLUFF_NUM_DIST)   bytes: dist   counts, little-endian uint16]
 *   [4 bytes: FSE litlen initial state (little-endian)]
 *   [4 bytes: FSE dist   initial state (little-endian)]
 *   [4 bytes: raw-bits bitbuf initial value (little-endian)]
 *   [4 bytes: raw-bits bitpos (little-endian)]
 *   [N bytes: interleaved FSE litlen/dist bitstream (ANS bits, MSB-first)]
 *   [M bytes: raw extra-bits bitstream (little-endian, backwards)]
 *
 * ANSI C89 compliant. No compiler extensions outside the fl_u64 typedef.
 */

#define FSE_LL_BITS   10
#define FSE_LL_L      (1U << FSE_LL_BITS)   /* 1024 */
#define FSE_D_BITS    9
#define FSE_D_L       (1U << FSE_D_BITS)    /* 512 */

/* Maximum number of bits emitted per FSE symbol decode step. */
#define FSE_MAX_BITS  (FSE_LL_BITS + 1)

/* ── FSE decode table entry ──────────────────────────────────────────── */

/* One entry per spread position (state in [0, L)). */
typedef struct {
    fl_u16 symbol;    /* decoded symbol (up to 288 for LL, up to 47 for D) */
    fl_u8  num_bits;  /* number of bits to read for next state */
    fl_u16 new_base;  /* base for next state: new_state = new_base + bits */
} fse_dtab_t;

/* ── FSE backwards bitstream (encoder) ──────────────────────────────── */

typedef struct {
    fl_u8  *buf;      /* output buffer */
    fl_u32  cap;      /* buffer capacity */
    fl_u32  pos;      /* bytes written */
    fl_u32  accum;    /* bit accumulator */
    fl_u32  nbits;    /* bits in accum */
    int reverse;     /* write bytes from the end of buf */
} fse_bits_t;

/* ── Frequency normalization ─────────────────────────────────────────── */

/*
 * Normalize raw symbol frequencies to sum exactly to L = 2^log2l.
 * Symbols with freq > 0 get at least count=1 (so they remain decodable).
 * Remaining slack is distributed proportionally via largest-remainder.
 * out_norm[] has 'nsym' entries; returns 0 on success, -1 on error.
 */
static int fse_normalize(const fl_u32 *freq, fl_u32 nsym,
                         fl_u16 *out_norm, fl_u32 log2l)
{
    fl_u32 L = 1U << log2l;
    fl_u32 total = 0;
    fl_u32 i, used, slack;
    fl_u32 *rem;   /* fractional remainder * L */
    fl_u32 active; /* number of symbols with freq > 0 */

    /* Count total frequency and active symbols. */
    active = 0;
    for (i = 0; i < nsym; i++) {
        total += freq[i];
        if (freq[i] > 0) active++;
    }

    if (active == 0) return -1;  /* no symbols at all */

    /* Single-symbol edge case: all L slots go to that symbol. */
    if (active == 1) {
        for (i = 0; i < nsym; i++) {
            out_norm[i] = (fl_u16)(freq[i] > 0 ? L : 0);
        }
        return 0;
    }

    /* Allocate: give each active symbol floor(freq/total * L), minimum 1. */
    rem = (fl_u32 *)malloc(nsym * sizeof(fl_u32));
    if (rem == NULL) return -1;

    used = 0;
    for (i = 0; i < nsym; i++) {
        if (freq[i] == 0) {
            out_norm[i] = 0;
            rem[i] = 0;
        } else {
            fl_u32 scaled;
            /* scaled = freq[i] * L / total (64-bit to avoid overflow) */
            scaled = (fl_u32)(((fl_u64)freq[i] * L) / total);
            if (scaled < 1) scaled = 1;
            out_norm[i] = (fl_u16)scaled;
            rem[i] = (fl_u32)(((fl_u64)freq[i] * L) % total);
            used += scaled;
        }
    }

    /*
     * If used > L (happens when many low-freq symbols each get minimum-1
     * but their sum exceeds L), reduce from the largest allocations first.
     * This prevents slack = L - used from underflowing to ~2^32.
     */
    if (used > L) {
        fl_u32 excess = used - L;
        while (excess > 0) {
            fl_u32 best = nsym;
            fl_u32 best_val = 1; /* only reduce symbols allocated > 1 */
            for (i = 0; i < nsym; i++) {
                if (out_norm[i] > best_val) {
                    best_val = out_norm[i];
                    best = i;
                }
            }
            if (best == nsym) break; /* no symbol can be reduced */
            out_norm[best]--;
            rem[best] = 0;
            excess--;
        }
        slack = 0;
    } else {
        slack = L - used;
    }

    /* Distribute remaining slack to symbols with largest fractional part. */
    while (slack > 0) {
        fl_u32 best = 0;
        fl_u32 best_rem = 0;
        for (i = 0; i < nsym; i++) {
            if (freq[i] > 0 && rem[i] > best_rem) {
                best_rem = rem[i];
                best = i;
            }
        }
        out_norm[best]++;
        rem[best] = 0;
        slack--;
    }

    free(rem);
    return 0;
}


/* ── Build decode table ──────────────────────────────────────────────── */

/*
 * Build the FSE decode table from normalized counts.
 * dtab[] must have L entries (indexed 0..L-1).
 */
static int fse_build_dtab(const fl_u16 *norm, fl_u32 nsym,
                           fse_dtab_t *dtab, fl_u32 log2l,
                           int legacy_layout)
{
    fl_u32 L = 1U << log2l;
    fl_u16 *spread;
    fl_u32 step;
    fl_u32 pos;
    fl_u32 s, i;
    fl_u32 *sym_k;   /* occurrence counter per symbol */

    spread = (fl_u16 *)malloc(L * sizeof(fl_u16));
    sym_k  = (fl_u32 *)malloc(nsym * 3U * sizeof(fl_u32));
    if (!spread || !sym_k) {
        free(spread); free(sym_k);
        return -1;
    }

    /* Same spread as ctab so encoder/decoder are consistent. */
    step = (L >> 1) + (L >> 3) + 3;
    pos = 0;
    for (s = 0; s < nsym; s++) {
        fl_u32 n = norm[s];
        fl_u32 j;
        for (j = 0; j < n; j++) {
            spread[pos] = (fl_u16)s;
            pos = (pos + step) & (L - 1);
        }
    }
    if (legacy_layout) {
        memset(sym_k, 0, nsym * sizeof(fl_u32));
        for (i = 0; i < L; i++) {
            fl_u32 sym = spread[i];
            fl_u32 ns  = norm[sym];
            fl_u32 k   = sym_k[sym]++;
            fl_u8  db;
            fl_u32 m, nb, new_base;

            db = 0;
            while ((ns << (db + 1)) <= L) db++;
            m = (L >> db) - ns;

            if (k < m) {
                nb       = (fl_u32)(db + 1U);
                new_base = k << nb;
            } else {
                nb       = (fl_u32)db;
                new_base = (m << (db + 1U)) + ((k - m) << db);
            }

            dtab[i].symbol   = (fl_u16)sym;
            dtab[i].num_bits = (fl_u8)nb;
            dtab[i].new_base = (fl_u16)new_base;
        }

    } else {
        /* The bit-width cutoff depends only on the symbol, not the state.
         * Cache it once per symbol instead of recomputing its logarithm
         * for every occurrence in the spread table. */
        for (s = 0; s < nsym; s++) {
            fl_u32 ns = norm[s], db = 0;
            if (ns) while ((ns << (db + 1)) <= L) db++;
            sym_k[s * 3] = ns;
            sym_k[s * 3 + 1] = L >> db;
            sym_k[s * 3 + 2] = db;
        }
        for (i = 0; i < L; i++) {
            fl_u32 sym = spread[i];
            fl_u32 *param = sym_k + sym * 3;
            fl_u32 next = param[0]++;
            fl_u32 nb = param[2] + (next < param[1]);
            /* Virtual states [L, 2L) are stored with L subtracted. */
            dtab[i].symbol = (fl_u16)sym;
            dtab[i].num_bits = (fl_u8)nb;
            dtab[i].new_base = (fl_u16)((next << nb) - L);
        }
    }

    free(spread);
    free(sym_k);
    return 0;
}

static void fse_bits_init_write(fse_bits_t *b, fl_u8 *buf, fl_u32 cap)
{
    b->buf   = buf;
    b->cap   = cap;
    b->pos   = 0;
    b->accum = 0;
    b->nbits = 0;
    b->reverse = 0;
}

static void fse_bits_write(fse_bits_t *b, fl_u32 val, fl_u32 nbits)
{
    if (nbits == 0) return;
    b->accum |= (val & ((1U << nbits) - 1U)) << b->nbits;
    b->nbits += nbits;
    while (b->nbits >= 8) {
        if (b->pos < b->cap)
            b->buf[b->reverse ? b->cap - 1U - b->pos : b->pos] =
                (fl_u8)(b->accum & 0xFFU);
        b->pos++;
        b->accum >>= 8;
        b->nbits  -= 8;
    }
}

static void fse_bits_flush(fse_bits_t *b)
{
    /* Write sentinel bit (1) so decoder can find byte boundary alignment. */
    b->accum |= 1U << b->nbits;
    b->nbits++;
    if (b->pos < b->cap)
        b->buf[b->reverse ? b->cap - 1U - b->pos : b->pos] =
            (fl_u8)(b->accum & 0xFFU);
    b->pos++;
    b->accum = 0;
    b->nbits = 0;
}

typedef struct {
    const fl_u8 *buf;
    fl_u32       len;
    fl_u32       pos;
    fl_u32       accum;
    fl_u32       nbits;
} fse_bits_r_t;

/*
 * The decoder reads the bitstream backwards from the last byte toward the
 * first. Bits are extracted from the MSB side of the accumulator, matching
 * the LSB-first encoding order after the encoder stored bits from last to
 * first. A sentinel 1-bit written by fse_bits_flush marks where valid data
 * starts within the last byte; the decoder discards the sentinel on init.
 */
static void fse_bits_init_read(fse_bits_r_t *b, const fl_u8 *buf, fl_u32 len)
{
    fl_u8 last;
    fl_u32 sentinel_pos;
    b->buf   = buf;
    b->len   = len;
    b->accum = 0;
    b->nbits = 0;
    if (len == 0) { b->pos = 0; return; }
    /* Load the last byte; find the sentinel (highest set bit); discard it.
     * Bits below the sentinel are the first data bits to decode. */
    b->pos = len - 1;
    last = buf[len - 1];
    sentinel_pos = 0;
    if (last & 0x80U) sentinel_pos = 7;
    else if (last & 0x40U) sentinel_pos = 6;
    else if (last & 0x20U) sentinel_pos = 5;
    else if (last & 0x10U) sentinel_pos = 4;
    else if (last & 0x08U) sentinel_pos = 3;
    else if (last & 0x04U) sentinel_pos = 2;
    else if (last & 0x02U) sentinel_pos = 1;
    /* sentinel_pos == 0 means sentinel is bit 0; valid_bits = 0 */
    b->accum = (fl_u32)last & ((1U << sentinel_pos) - 1U);
    b->nbits = sentinel_pos;
}

static fl_u32 fse_bits_read(fse_bits_r_t *b, fl_u32 nbits)
{
    fl_u32 val;
    if (nbits == 0) return 0;
    if (b->nbits < nbits) {
        /* Refill three bytes together when the 32-bit accumulator has
         * room. Current callers request at most 17 bits. */
        if (b->nbits <= 8 && b->pos >= 3) {
            fl_u32 pos = b->pos - 3;
            b->accum = (b->accum << 24) | (fl_u32)b->buf[pos] |
                       ((fl_u32)b->buf[pos + 1] << 8) |
                       ((fl_u32)b->buf[pos + 2] << 16);
            b->pos = pos;
            b->nbits += 24;
        } else {
            do {
                fl_u8 byte = (b->pos > 0) ? b->buf[--b->pos] : 0;
                b->accum = (b->accum << 8) | (fl_u32)byte;
                b->nbits += 8;
            } while (b->nbits < nbits);
        }
    }
    b->nbits -= nbits;
    val = (b->accum >> b->nbits) & ((1U << nbits) - 1U);
    return val;
}

/* ── FSE encode table (ctab) + per-symbol encode params ─────────────── */

/*
 * Per-symbol encode parameters.
 * slots[slot_off .. slot_off+count-1] = spread positions for this symbol.
 * State y in [0, L):
 *   nb = db_lo + (y >= low_limit)
 *   k = ((y + table_size) >> nb) - count
 *   emit y & ((1<<nb)-1); new_state = slots[slot_off + k]
 */
typedef struct {
    fl_u32 slot_off; /* index into slots[] for this symbol's first occurrence */
    fl_u32 count;    /* normalized count ns */
    fl_u32 table_size; /* virtual state bias L */
    fl_u32 low_limit; /* states below this threshold use db_lo bits */
    fl_u8  db_lo;
} fse_sym_enc_t;

/*
 * Build encode slots and sym-enc table.
 * slots[L]: caller allocates (fl_u16[L]).
 * senc[nsym]: caller allocates.
 */
static int fse_build_ctab(const fl_u16 *norm, fl_u32 nsym,
                           fl_u16 *slots, fse_sym_enc_t *senc,
                           fl_u32 log2l)
{
    fl_u32 L = 1U << log2l;
    fl_u16 *spread;
    fl_u32 step;
    fl_u32 pos;
    fl_u32 s, i;
    fl_u32 *sym_k;    /* occurrence counter per symbol */
    fl_u32 *slot_off; /* start index into slots[] per symbol */

    spread   = (fl_u16 *)malloc(L * sizeof(fl_u16));
    sym_k    = (fl_u32 *)malloc(nsym * sizeof(fl_u32));
    slot_off = (fl_u32 *)malloc(nsym * sizeof(fl_u32));
    if (!spread || !sym_k || !slot_off) {
        free(spread); free(sym_k); free(slot_off);
        return -1;
    }

    /* Spread symbols across [0, L) using golden-ratio step. */
    step = (L >> 1) + (L >> 3) + 3;
    pos = 0;
    for (s = 0; s < nsym; s++) {
        fl_u32 n = norm[s];
        fl_u32 j;
        for (j = 0; j < n; j++) {
            spread[pos] = (fl_u16)s;
            pos = (pos + step) & (L - 1);
        }
    }

    /* Compute slot offsets (cumulative). */
    {
        fl_u32 cum = 0;
        for (s = 0; s < nsym; s++) {
            slot_off[s] = cum;
            cum += norm[s];
            sym_k[s] = 0;
        }
    }

    /* Fill slots[]: for each spread position i, record it as the k-th
     * occurrence of symbol spread[i]. */
    for (i = 0; i < L; i++) {
        fl_u32 sym = spread[i];
        fl_u32 k   = sym_k[sym]++;
        slots[slot_off[sym] + k] = (fl_u16)i;
    }

    /* Compute per-symbol encode parameters. */
    for (s = 0; s < nsym; s++) {
        fl_u32 ns = norm[s];
        fl_u8  db;
        if (ns == 0) {
            senc[s].slot_off = 0;
            senc[s].count    = 0;
            senc[s].table_size = 0;
            senc[s].low_limit = 0;
            senc[s].db_lo    = 0;
            continue;
        }
        /* db_lo: largest d such that ns << d <= L */
        db = 0;
        while ((ns << (db + 1)) <= L) db++;

        senc[s].slot_off = slot_off[s];
        senc[s].count    = ns;
        senc[s].db_lo    = db;
        senc[s].table_size = L;
        senc[s].low_limit = (ns << (db + 1)) - L;
    }

    free(spread); free(sym_k); free(slot_off);
    return 0;
}

/*
 * Encode symbol sym from state y (in [0, L)).
 * Emits bits to backwards bitstream and returns new state in [0, L).
 */
static fl_u32 fse_encode_sym_fast(fl_u32 y, fl_u32 sym,
                                   const fl_u16 *slots,
                                   const fse_sym_enc_t *senc,
                                   fse_bits_t *bits)
{
    fl_u32 ns = senc[sym].count;
    fl_u32 nb, k;

    if (ns == 0) return y; /* should not happen */
    nb = senc[sym].db_lo + (y >= senc[sym].low_limit);
    k = ((y + senc[sym].table_size) >> nb) - ns;
    fse_bits_write(bits, y & ((1U << nb) - 1U), nb);
    return slots[senc[sym].slot_off + k];
}

/* ── FSE ANS decoder ─────────────────────────────────────────────────── */

/*
 * Decode one symbol from state. Returns symbol; updates state.
 */
static fl_u32 fse_decode_sym(fl_u32 *state, const fse_dtab_t *dtab,
                              fse_bits_r_t *bits, fl_u32 log2l)
{
    fl_u32 x = *state;   /* x in [0, L) */
    const fse_dtab_t *d;
    fl_u32 sym;
    (void)log2l;

    d   = &dtab[x];
    sym = d->symbol;
    *state = (fl_u32)d->new_base + fse_bits_read(bits, d->num_bits);
    return sym;
}

/* ── Count table I/O ─────────────────────────────────────────────────── */

/*
 * Write normalized count table (nsym × uint16 LE) to dst[pos].
 * Returns number of bytes written, or -1 on overflow.
 */
static int fse_write_counts(fl_u8 *dst, fl_u32 cap, fl_u32 pos,
                             const fl_u16 *norm, fl_u32 nsym)
{
    fl_u32 i;
    if (pos + nsym * 2 > cap) return -1;
    for (i = 0; i < nsym; i++) {
        dst[pos + i * 2]     = (fl_u8)(norm[i] & 0xFF);
        dst[pos + i * 2 + 1] = (fl_u8)((norm[i] >> 8) & 0xFF);
    }
    return (int)(nsym * 2);
}

/*
 * Read normalized count table from src[pos].
 * Returns number of bytes consumed, or -1 on error.
 */
static int fse_read_counts(const fl_u8 *src, fl_u32 src_len, fl_u32 pos,
                            fl_u16 *norm, fl_u32 nsym, fl_u32 log2l)
{
    fl_u32 L = 1U << log2l;
    fl_u32 i;
    fl_u32 total = 0;
    if (pos + nsym * 2 > src_len) return -1;
    for (i = 0; i < nsym; i++) {
        norm[i] = (fl_u16)(src[pos + i * 2] | ((fl_u16)src[pos + i * 2 + 1] << 8));
        total += norm[i];
    }
    if (total != L) return -1;  /* corrupt count table */
    return (int)(nsym * 2);
}

/* ── Helper: write/read 32-bit LE ────────────────────────────────────── */
static void fse_write_u32(fl_u8 *dst, fl_u32 pos, fl_u32 val)
{
    dst[pos]   = (fl_u8)(val & 0xFF);
    dst[pos+1] = (fl_u8)((val >> 8) & 0xFF);
    dst[pos+2] = (fl_u8)((val >> 16) & 0xFF);
    dst[pos+3] = (fl_u8)((val >> 24) & 0xFF);
}

static fl_u32 fse_read_u32(const fl_u8 *src, fl_u32 pos)
{
    return (fl_u32)src[pos] | ((fl_u32)src[pos+1] << 8)
         | ((fl_u32)src[pos+2] << 16) | ((fl_u32)src[pos+3] << 24);
}

/* ── 16-bit / 24-bit LE helpers (v0x08 header, R3) ───────────────────── */
static void fse_write_u16(fl_u8 *dst, fl_u32 pos, fl_u32 val)
{
    dst[pos]   = (fl_u8)(val & 0xFF);
    dst[pos+1] = (fl_u8)((val >> 8) & 0xFF);
}

static fl_u32 fse_read_u16(const fl_u8 *src, fl_u32 pos)
{
    return (fl_u32)src[pos] | ((fl_u32)src[pos+1] << 8);
}

static void fse_write_u24(fl_u8 *dst, fl_u32 pos, fl_u32 val)
{
    dst[pos]   = (fl_u8)(val & 0xFF);
    dst[pos+1] = (fl_u8)((val >> 8) & 0xFF);
    dst[pos+2] = (fl_u8)((val >> 16) & 0xFF);
}

static fl_u32 fse_read_u24(const fl_u8 *src, fl_u32 pos)
{
    return (fl_u32)src[pos] | ((fl_u32)src[pos+1] << 8)
         | ((fl_u32)src[pos+2] << 16);
}

/* ── LEB128-style varint (LSB-first base-128) ────────────────────────── */
/*
 * Writes v to dst[pos], advancing pos.  Like the FSE bit writer, the
 * position counter advances even when it runs past cap (so a too-small
 * buffer is detected by the caller), but bytes are only stored within cap.
 * Returns the new position.
 */
static fl_u32 fl_varint_write(fl_u8 *dst, fl_u32 cap, fl_u32 pos, fl_u32 v)
{
    while (v >= 0x80U) {
        if (pos < cap) dst[pos] = (fl_u8)(v | 0x80U);
        pos++;
        v >>= 7;
    }
    if (pos < cap) dst[pos] = (fl_u8)v;
    pos++;
    return pos;
}

/*
 * Reads a varint from src starting at *pos, advancing *pos past it.
 * Returns 0 on success (value in *out), -1 on truncation/overlong input.
 */
static int fl_varint_read(const fl_u8 *src, fl_u32 src_len, fl_u32 *pos,
                          fl_u32 *out)
{
    fl_u32 v = 0;
    fl_u32 shift = 0;
    fl_u32 p = *pos;
    for (;;) {
        fl_u8 b;
        if (p >= src_len) return -1;
        b = src[p++];
        v |= (fl_u32)(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0) break;
        shift += 7;
        if (shift > 28U) return -1;  /* would overflow 32 bits */
    }
    *pos = p;
    *out = v;
    return 0;
}

/* ── Compact normalized count table (v0x08, R1) ──────────────────────── */
/*
 * Encoding: walk the nsym entries left to right.
 *   - A run of k>=1 zero counts  -> 0x00 marker byte, then varint(k).
 *   - A non-zero count c (1..L)   -> varint(c).  Its first byte is never
 *     0x00 (a varint of any value >= 1 has a non-zero leading byte), so the
 *     0x00 marker is unambiguous.
 * Returns bytes written, or -1 if it would exceed cap.
 */
static int fse_write_counts_compact(fl_u8 *dst, fl_u32 cap, fl_u32 pos,
                                    const fl_u16 *norm, fl_u32 nsym)
{
    fl_u32 start = pos;
    fl_u32 i = 0;
    while (i < nsym) {
        if (norm[i] == 0) {
            fl_u32 run = 0;
            while (i < nsym && norm[i] == 0) { run++; i++; }
            if (pos < cap) dst[pos] = 0x00;
            pos++;
            pos = fl_varint_write(dst, cap, pos, run);
        } else {
            pos = fl_varint_write(dst, cap, pos, (fl_u32)norm[i]);
            i++;
        }
    }
    if (pos > cap) return -1;
    return (int)(pos - start);
}

/*
 * Reads a compact count table written by fse_write_counts_compact.
 * Validates that the counts sum to exactly L = 1<<log2l (the same integrity
 * check the raw reader performs).  Returns bytes consumed, or -1 on error.
 */
static int fse_read_counts_compact(const fl_u8 *src, fl_u32 src_len, fl_u32 pos,
                                   fl_u16 *norm, fl_u32 nsym, fl_u32 log2l)
{
    fl_u32 L = 1U << log2l;
    fl_u32 i = 0;
    fl_u32 total = 0;
    fl_u32 start = pos;
    while (i < nsym) {
        if (pos >= src_len) return -1;
        if (src[pos] == 0x00) {
            fl_u32 run;
            pos++;  /* consume marker */
            if (fl_varint_read(src, src_len, &pos, &run) != 0) return -1;
            if (run == 0 || run > nsym - i) return -1;
            while (run-- > 0) norm[i++] = 0;
        } else {
            fl_u32 v;
            if (fl_varint_read(src, src_len, &pos, &v) != 0) return -1;
            if (v == 0 || v > L) return -1;
            norm[i++] = (fl_u16)v;
            total += v;
        }
    }
    if (total != L) return -1;  /* corrupt count table */
    return (int)(pos - start);
}

/* ─── Match finder ──────────────────────────────────────────────────────── */

typedef struct {
    fl_u8  *window;        /* circular dictionary */
    fl_u32 *hash_table;    /* head of hash chain */
    fl_u32 *hash_chain;    /* prev pointers for chaining */
    fl_u32  window_size;
    fl_u32  window_mask;
    fl_u32  pos;           /* absolute write position */
} fl_match_t;

struct fluff_ctx_s {
    /*
     * Match-finder tables (compress-only, ~1.75 MB total).  Lazily allocated
     * on first compress and freed by fluff_ctx_free.  A decode-only context
     * never allocates them, so decompression needs only the window + FSE
     * decode tables (~0.27 MB vs ~2 MB for a full encode context).
     */
    fl_u32 *hash_table;                   /* [FLUFF_HASH_SIZE] head of chains */
    fl_u32 *hash_chain;                   /* [FLUFF_WINDOW_SIZE] prev pointers */
    fl_u32 *hash3;                        /* [FLUFF_H3_SIZE] 3-byte sub-hash   */
    fl_u8  window[FLUFF_WINDOW_SIZE];     /* ring buffer (compress + decode)   */
    fl_u32 file_pos;                      /* cumulative file byte position  */
    fl_u32 max_chain;                     /* hash chain search depth        */
    /* Statistics (per-file) */
    fl_u32 total_literals;                /* count of literal symbols       */
    fl_u32 total_matches;                 /* count of match symbols         */
    fl_u32 total_match_bytes;             /* sum of match lengths           */
    fl_u32 total_chain_probes;            /* total nodes explored in chains */
    fl_u32 max_chain_depth;               /* maximum depth used             */
    fl_u32 total_match_searches;          /* fl_find_match_hyb calls        */
    fl_u32 total_no_match_saturations;    /* full chain used, no match      */
    fl_u32 total_subhash_wins;            /* sub-hash improved result       */
    fl_u32 total_chain_cmp_bytes;         /* bytes compared in chain probes */
    fl_u32 total_rep_searches;            /* fl_apply_rep_ring calls        */
    fl_u32 rep_slot_probes[3];            /* rep0/rep1/rep2 probes          */
    fl_u32 rep_slot_matches[3];           /* rep probes yielding matches    */
    fl_u32 rep_slot_wins[3];              /* rep slots selected as best     */
    fl_u32 rep_slot_cmp_bytes[3];         /* bytes compared in rep probes   */
    fl_u32 outcome_searches[5];           /* searches by parser outcome     */
    fl_u32 outcome_probes[5];             /* chain probes by outcome        */
    fl_u32 outcome_cmp_bytes[5];          /* cmp bytes by outcome           */
    /*
     * Per-block FSE working tables.  Embedded in the context so encode and
     * decode no longer malloc them per block (S3).  ll_norm/d_norm are
     * shared by the encoder and decoder (never used concurrently).
     */
    fl_u16        fse_ll_norm[FLUFF_NUM_LITLEN];
    fl_u16        fse_d_norm[FLUFF_NUM_DIST];
    fl_u16        fse_ll_slots[FSE_LL_L];   /* encoder spread positions */
    fl_u16        fse_d_slots[FSE_D_L];
    fse_sym_enc_t fse_ll_senc[FLUFF_NUM_LITLEN];
    fse_sym_enc_t fse_d_senc[FLUFF_NUM_DIST];
    fse_dtab_t    fse_ll_dtab[FSE_LL_L];    /* decoder tables */
    fse_dtab_t    fse_d_dtab[FSE_D_L];
    /*
     * Lazily-grown encoder scratch, sized to the largest block seen so far.
     * Owned by the context; allocated on first compress, reused thereafter,
     * freed by fluff_ctx_free.  Decode-only contexts never allocate these.
     */
    fl_u8  *scratch_tokens;     /* tokens, then raw bits (2*src_len + 16) */
    fl_u32  scratch_src_cap;    /* largest src_len this buffer can hold */
};

#define FL_SEARCH_RESULT_NONE               0
#define FL_SEARCH_RESULT_SHORT_FILTERED     1
#define FL_SEARCH_RESULT_MATCH              2

#define FL_SEARCH_OUTCOME_NO_MATCH          0
#define FL_SEARCH_OUTCOME_SHORT_FILTERED    1
#define FL_SEARCH_OUTCOME_ACCEPTED_MATCH    2
#define FL_SEARCH_OUTCOME_REP_SELECTED      3
#define FL_SEARCH_OUTCOME_LOOKAHEAD_DROP    4

typedef struct {
    fl_u32 probes;
    fl_u32 cmp_bytes;
    fl_u8  result_kind;
    fl_u8  did_search;
} fl_search_stats_t;

/* Chain depths for BoltPack levels 1-9. */
static const fl_u32 s_chain_table[9] = {8, 16, 32, 64, 96, 128, 192, 256, 512};

/* Module-level default chain depth, applied by fluff_ctx_reset(). */
static fl_u32 s_default_chain = FLUFF_MAX_CHAIN;

void fluff_set_level(int level)
{
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    s_default_chain = s_chain_table[level - 1];
}

fluff_ctx_t *fluff_ctx_alloc(void)
{
    struct fluff_ctx_s *ctx =
        (struct fluff_ctx_s *)malloc(sizeof(struct fluff_ctx_s));
    if (ctx != NULL) {
        ctx->hash_table        = NULL;
        ctx->hash_chain        = NULL;
        ctx->hash3             = NULL;
        ctx->scratch_tokens    = NULL;
        ctx->scratch_src_cap   = 0;
    }
    return (fluff_ctx_t *)ctx;
}

void fluff_ctx_free(fluff_ctx_t *ctx)
{
    if (ctx != NULL) {
        free(ctx->hash_table);
        free(ctx->hash_chain);
        free(ctx->hash3);
        free(ctx->scratch_tokens);
    }
    free(ctx);
}

/*
 * Ensure the match-finder tables are allocated (compress-only).  Allocated
 * zeroed (calloc) on first use; reused thereafter.  Decode-only contexts
 * never call this, so they never pay the ~1.75 MB.  Returns BP_OK/NOMEM.
 */
static int fl_ctx_ensure_matchfinder(fluff_ctx_t *ctx)
{
    if (ctx->hash_table != NULL) {
        return BP_OK;
    }
    ctx->hash_table = (fl_u32 *)calloc(FLUFF_HASH_SIZE,   sizeof(fl_u32));
    ctx->hash_chain = (fl_u32 *)calloc(FLUFF_WINDOW_SIZE, sizeof(fl_u32));
    ctx->hash3      = (fl_u32 *)calloc(FLUFF_H3_SIZE,     sizeof(fl_u32));
    if (ctx->hash_table == NULL || ctx->hash_chain == NULL ||
        ctx->hash3 == NULL) {
        free(ctx->hash_table);
        free(ctx->hash_chain);
        free(ctx->hash3);
        ctx->hash_table = NULL;
        ctx->hash_chain = NULL;
        ctx->hash3      = NULL;
        return BP_ERR_NOMEM;
    }
    return BP_OK;
}

/*
 * Grow the token/raw buffer to fit src_len, retaining it across blocks.
 * Allocate before freeing so a failed growth preserves the previous buffer.
 */
static int fl_ctx_ensure_scratch(fluff_ctx_t *ctx, fl_u32 src_len)
{
    fl_u8 *tokens;
    if (ctx->scratch_src_cap >= src_len && ctx->scratch_tokens != NULL) return BP_OK;
    tokens = (fl_u8 *)malloc(src_len * 2U + 16U);
    if (tokens == NULL) return BP_ERR_NOMEM;
    free(ctx->scratch_tokens);
    ctx->scratch_tokens = tokens;
    ctx->scratch_src_cap = src_len;
    return BP_OK;
}

void fluff_ctx_reset(fluff_ctx_t *ctx)
{
    if (ctx == NULL) return;
    fl_build_luts();
#ifdef BOLTPACK_FAST_FLUFF
    fluff_fast_init_matcher();
#endif
    /*
     * Allocate the match-finder tables on first reset of a compress context
     * (calloc gives a zeroed start); on reuse for a later file, re-zero them.
     * If allocation fails the tables stay NULL — fluff_compress_block_ctx
     * re-checks and reports BP_ERR_NOMEM before any use.
     */
    if (fl_ctx_ensure_matchfinder(ctx) == BP_OK) {
        memset(ctx->hash_table, 0, FLUFF_HASH_SIZE   * sizeof(fl_u32));
        memset(ctx->hash_chain, 0, FLUFF_WINDOW_SIZE * sizeof(fl_u32));
        memset(ctx->hash3,      0, FLUFF_H3_SIZE     * sizeof(fl_u32));
    }
    ctx->file_pos = 0;
    ctx->max_chain = s_default_chain;
    ctx->total_literals = 0;
    ctx->total_matches = 0;
    ctx->total_match_bytes = 0;
    ctx->total_chain_probes = 0;
    ctx->max_chain_depth = 0;
    ctx->total_match_searches = 0;
    ctx->total_no_match_saturations = 0;
    ctx->total_subhash_wins = 0;
    ctx->total_chain_cmp_bytes = 0;
    ctx->total_rep_searches = 0;
    memset(ctx->rep_slot_probes, 0, sizeof(ctx->rep_slot_probes));
    memset(ctx->rep_slot_matches, 0, sizeof(ctx->rep_slot_matches));
    memset(ctx->rep_slot_wins, 0, sizeof(ctx->rep_slot_wins));
    memset(ctx->rep_slot_cmp_bytes, 0, sizeof(ctx->rep_slot_cmp_bytes));
    memset(ctx->outcome_searches, 0, sizeof(ctx->outcome_searches));
    memset(ctx->outcome_probes, 0, sizeof(ctx->outcome_probes));
    memset(ctx->outcome_cmp_bytes, 0, sizeof(ctx->outcome_cmp_bytes));
    /* window: not zeroed; stale content is unreachable after hash reset */
}

/*
 * fluff_ctx_reset_decode: lightweight reset for decode-only paths.
 * Zeroes only file_pos, skipping the ~1.75 MB hash memsets that are
 * irrelevant during decompression.  Used by the 0x87 block decode path.
 */
void fluff_ctx_reset_decode(fluff_ctx_t *ctx)
{
    if (ctx == NULL) return;
    ctx->file_pos = 0;
}

/*
 * fluff_ctx_reset_level: per-context reset with an explicit compression level.
 * Bypasses the module-global s_default_chain so concurrent callers each get
 * their own level without data races.  level is the BoltPack scale (1-9);
 * values outside that range are clamped to the nearest endpoint.
 */
void fluff_ctx_reset_level(fluff_ctx_t *ctx, int level)
{
    fl_u32 chain;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    chain = s_chain_table[level - 1];

    if (ctx == NULL) return;
    fl_build_luts();
#ifdef BOLTPACK_FAST_FLUFF
    fluff_fast_init_matcher();
#endif
    if (fl_ctx_ensure_matchfinder(ctx) == BP_OK) {
        memset(ctx->hash_table, 0, FLUFF_HASH_SIZE   * sizeof(fl_u32));
        memset(ctx->hash_chain, 0, FLUFF_WINDOW_SIZE * sizeof(fl_u32));
        memset(ctx->hash3,      0, FLUFF_H3_SIZE     * sizeof(fl_u32));
    }
    ctx->file_pos = 0;
    ctx->max_chain = chain;
    ctx->total_literals = 0;
    ctx->total_matches = 0;
    ctx->total_match_bytes = 0;
    ctx->total_chain_probes = 0;
    ctx->max_chain_depth = 0;
    ctx->total_match_searches = 0;
    ctx->total_no_match_saturations = 0;
    ctx->total_subhash_wins = 0;
    ctx->total_chain_cmp_bytes = 0;
    ctx->total_rep_searches = 0;
    memset(ctx->rep_slot_probes, 0, sizeof(ctx->rep_slot_probes));
    memset(ctx->rep_slot_matches, 0, sizeof(ctx->rep_slot_matches));
    memset(ctx->rep_slot_wins, 0, sizeof(ctx->rep_slot_wins));
    memset(ctx->rep_slot_cmp_bytes, 0, sizeof(ctx->rep_slot_cmp_bytes));
    memset(ctx->outcome_searches, 0, sizeof(ctx->outcome_searches));
    memset(ctx->outcome_probes, 0, sizeof(ctx->outcome_probes));
    memset(ctx->outcome_cmp_bytes, 0, sizeof(ctx->outcome_cmp_bytes));
}

/*
 * fluff_ctx_feed_raw: advance the context ring buffer with raw (uncompressed)
 * bytes WITHOUT performing any decompression.
 *
 * When the archiver falls back to storing a block verbatim after calling
 * fluff_compress_block_ctx (because the compressed size exceeded the 99%
 * threshold), the compressor's context has already been advanced for that
 * block.  On the decompressor side the block arrives with comp_sz == orig_sz
 * (stored), so fluff_decompress_block_ctx is never called — leaving the
 * decompressor's ring buffer and file_pos behind by one block.  Any
 * subsequent block that contains cross-block back-references will then read
 * from the wrong ring-buffer positions, producing corrupt output.
 *
 * The fix: call fluff_ctx_feed_raw from the decompressor whenever a stored
 * block is encountered inside a fluff-method file.  This mirrors exactly the
 * bulk-flush that fluff_decompress_block_ctx performs at its end.
 */
void fluff_ctx_feed_raw(fluff_ctx_t *ctx, const bp_uint8 *data, bp_uint32 len)
{
    fl_u32 ring_start;
    fl_u32 chunk1;

    if (ctx == NULL || data == NULL || len == 0) {
        return;
    }

    ring_start = ctx->file_pos & FLUFF_WINDOW_MASK;
    chunk1     = FLUFF_WINDOW_SIZE - ring_start;
    if (chunk1 > len) { chunk1 = len; }
    memcpy(ctx->window + ring_start, data, chunk1);
    if (chunk1 < len) {
        memcpy(ctx->window, data + chunk1, len - chunk1);
    }
    ctx->file_pos += len;
}

static void fl_copy_to_window(fluff_ctx_t *ctx, fl_u32 abs_pos,
                              const fl_u8 *data, fl_u32 len)
{
    fl_u32 ring_start;
    fl_u32 chunk1;

    if (ctx == NULL || data == NULL || len == 0) {
        return;
    }

    ring_start = abs_pos & FLUFF_WINDOW_MASK;
    chunk1 = FLUFF_WINDOW_SIZE - ring_start;
    if (chunk1 > len) {
        chunk1 = len;
    }
    memcpy(ctx->window + ring_start, data, chunk1);
    if (chunk1 < len) {
        memcpy(ctx->window, data + chunk1, len - chunk1);
    }
}

static fl_u32 fl_hash3_window(const fluff_ctx_t *ctx, fl_u32 abs_pos)
{
    fl_u32 h;

    h  = (fl_u32)ctx->window[abs_pos & FLUFF_WINDOW_MASK];
    h |= (fl_u32)ctx->window[(abs_pos + 1U) & FLUFF_WINDOW_MASK] << 8;
    h |= (fl_u32)ctx->window[(abs_pos + 2U) & FLUFF_WINDOW_MASK] << 16;
    h ^= h >> 11;
    h *= 0x9e3779b9U;
    return (h >> 16) & FLUFF_H3_MASK;
}

static fl_u32 fl_hash4_window(const fluff_ctx_t *ctx, fl_u32 abs_pos)
{
    fl_u32 h;

    h  = (fl_u32)ctx->window[abs_pos & FLUFF_WINDOW_MASK];
    h |= (fl_u32)ctx->window[(abs_pos + 1U) & FLUFF_WINDOW_MASK] << 8;
    h |= (fl_u32)ctx->window[(abs_pos + 2U) & FLUFF_WINDOW_MASK] << 16;
    h |= (fl_u32)ctx->window[(abs_pos + 3U) & FLUFF_WINDOW_MASK] << 24;
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    return (h >> (32 - FLUFF_HASH_BITS)) & FLUFF_HASH_MASK;
}

static void fl_insert_window_hyb(fluff_ctx_t *ctx, fl_u32 abs_pos)
{
    fl_u32 h4;
    fl_u32 h3;

    h4 = fl_hash4_window(ctx, abs_pos);
    h3 = fl_hash3_window(ctx, abs_pos);
    ctx->hash_chain[abs_pos & FLUFF_WINDOW_MASK] = ctx->hash_table[h4];
    ctx->hash_table[h4] = abs_pos;
    ctx->hash3[h3] = abs_pos;
}

/*
 * Preload a compression context with bytes that the decoder already has in
 * its ring buffer.  history1 followed by history2 is treated as the tail of
 * the uncompressed stream immediately preceding file_pos; only the last
 * window worth of bytes is kept.  The function resets the context first, then
 * rebuilds the match-finder chains from the preloaded window.  This is used by
 * the modern parallel encoder to make independently-compressed groups behave
 * like the sequential stream without emitting a reset marker.
 */
int fluff_ctx_preload(fluff_ctx_t *ctx,
                      const bp_uint8 *history1, bp_uint32 history1_len,
                      const bp_uint8 *history2, bp_uint32 history2_len,
                      bp_uint32 file_pos)
{
    fl_u32 total_len;
    fl_u32 keep;
    fl_u32 skip;
    fl_u32 hist_abs;
    fl_u32 copied;
    fl_u32 insert_abs;

    if (ctx == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if ((history1_len != 0 && history1 == NULL) ||
        (history2_len != 0 && history2 == NULL)) {
        return BP_ERR_INVALID_ARG;
    }
    if (history1_len > 0xFFFFFFFFU - history2_len) {
        return BP_ERR_INVALID_ARG;
    }

    total_len = history1_len + history2_len;
    if (total_len == 0 || file_pos == 0) {
        fluff_ctx_reset(ctx);
        return BP_OK;
    }

    fluff_ctx_reset(ctx);
    if (fl_ctx_ensure_matchfinder(ctx) != BP_OK) {
        return BP_ERR_NOMEM;
    }

    keep = total_len;
    if (keep > FLUFF_WINDOW_SIZE) {
        keep = FLUFF_WINDOW_SIZE;
    }
    if (keep > file_pos) {
        keep = file_pos;
    }
    skip = total_len - keep;
    hist_abs = file_pos - keep;
    copied = 0;

    if (skip < history1_len) {
        fl_u32 n1;

        n1 = history1_len - skip;
        if (n1 > keep) {
            n1 = keep;
        }
        fl_copy_to_window(ctx, hist_abs, history1 + skip, n1);
        copied += n1;
        skip = 0;
    } else {
        skip -= history1_len;
    }

    if (copied < keep) {
        fl_u32 n2;

        n2 = keep - copied;
        fl_copy_to_window(ctx, hist_abs + copied, history2 + skip, n2);
        copied += n2;
    }

    insert_abs = hist_abs;
    while (insert_abs + 3U < file_pos) {
        fl_insert_window_hyb(ctx, insert_abs);
        insert_abs++;
    }

    ctx->file_pos = file_pos;
    return BP_OK;
}

bp_uint32 fluff_window_size(void)
{
    return FLUFF_WINDOW_SIZE;
}

void fluff_ctx_get_literal_count(fluff_ctx_t *ctx, bp_uint32 *count)
{
    if (ctx != NULL && count != NULL) {
        *count = ctx->total_literals;
    }
}

void fluff_ctx_get_match_count(fluff_ctx_t *ctx, bp_uint32 *count)
{
    if (ctx != NULL && count != NULL) {
        *count = ctx->total_matches;
    }
}

void fluff_ctx_get_total_match_bytes(fluff_ctx_t *ctx, bp_uint32 *bytes)
{
    if (ctx != NULL && bytes != NULL) {
        *bytes = ctx->total_match_bytes;
    }
}

void fluff_ctx_get_chain_stats(fluff_ctx_t *ctx, bp_uint32 *total_probes,
                                bp_uint32 *max_depth)
{
    if (ctx != NULL) {
        if (total_probes != NULL) *total_probes = ctx->total_chain_probes;
        if (max_depth != NULL) *max_depth = ctx->max_chain_depth;
    }
}

void fluff_ctx_get_matchfinder_stats(fluff_ctx_t *ctx,
                                      bp_uint32 *searches,
                                      bp_uint32 *no_match_saturations,
                                      bp_uint32 *subhash_wins,
                                      bp_uint32 *chain_cmp_bytes)
{
    if (ctx != NULL) {
        if (searches != NULL) *searches = ctx->total_match_searches;
        if (no_match_saturations != NULL) {
            *no_match_saturations = ctx->total_no_match_saturations;
        }
        if (subhash_wins != NULL) *subhash_wins = ctx->total_subhash_wins;
        if (chain_cmp_bytes != NULL) *chain_cmp_bytes = ctx->total_chain_cmp_bytes;
    }
}

void fluff_ctx_get_matchfinder_outcome_stats(
    fluff_ctx_t *ctx,
    fluff_matchfinder_outcome_stats_t *stats)
{
    if (ctx == NULL || stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    stats->no_match.searches = ctx->outcome_searches[FL_SEARCH_OUTCOME_NO_MATCH];
    stats->no_match.probes = ctx->outcome_probes[FL_SEARCH_OUTCOME_NO_MATCH];
    stats->no_match.cmp_bytes = ctx->outcome_cmp_bytes[FL_SEARCH_OUTCOME_NO_MATCH];

    stats->short_filtered.searches =
        ctx->outcome_searches[FL_SEARCH_OUTCOME_SHORT_FILTERED];
    stats->short_filtered.probes =
        ctx->outcome_probes[FL_SEARCH_OUTCOME_SHORT_FILTERED];
    stats->short_filtered.cmp_bytes =
        ctx->outcome_cmp_bytes[FL_SEARCH_OUTCOME_SHORT_FILTERED];

    stats->accepted_match.searches =
        ctx->outcome_searches[FL_SEARCH_OUTCOME_ACCEPTED_MATCH];
    stats->accepted_match.probes =
        ctx->outcome_probes[FL_SEARCH_OUTCOME_ACCEPTED_MATCH];
    stats->accepted_match.cmp_bytes =
        ctx->outcome_cmp_bytes[FL_SEARCH_OUTCOME_ACCEPTED_MATCH];

    stats->rep_selected.searches =
        ctx->outcome_searches[FL_SEARCH_OUTCOME_REP_SELECTED];
    stats->rep_selected.probes =
        ctx->outcome_probes[FL_SEARCH_OUTCOME_REP_SELECTED];
    stats->rep_selected.cmp_bytes =
        ctx->outcome_cmp_bytes[FL_SEARCH_OUTCOME_REP_SELECTED];

    stats->lookahead_discarded.searches =
        ctx->outcome_searches[FL_SEARCH_OUTCOME_LOOKAHEAD_DROP];
    stats->lookahead_discarded.probes =
        ctx->outcome_probes[FL_SEARCH_OUTCOME_LOOKAHEAD_DROP];
    stats->lookahead_discarded.cmp_bytes =
        ctx->outcome_cmp_bytes[FL_SEARCH_OUTCOME_LOOKAHEAD_DROP];
}

void fluff_ctx_get_rep_stats(fluff_ctx_t *ctx, fluff_rep_stats_t *stats)
{
    if (ctx == NULL || stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    stats->searches = ctx->total_rep_searches;

    stats->rep0.probes = ctx->rep_slot_probes[0];
    stats->rep0.matches = ctx->rep_slot_matches[0];
    stats->rep0.wins = ctx->rep_slot_wins[0];
    stats->rep0.cmp_bytes = ctx->rep_slot_cmp_bytes[0];

    stats->rep1.probes = ctx->rep_slot_probes[1];
    stats->rep1.matches = ctx->rep_slot_matches[1];
    stats->rep1.wins = ctx->rep_slot_wins[1];
    stats->rep1.cmp_bytes = ctx->rep_slot_cmp_bytes[1];

    stats->rep2.probes = ctx->rep_slot_probes[2];
    stats->rep2.matches = ctx->rep_slot_matches[2];
    stats->rep2.wins = ctx->rep_slot_wins[2];
    stats->rep2.cmp_bytes = ctx->rep_slot_cmp_bytes[2];
}

static void fl_record_search_outcome(fluff_ctx_t *ctx, fl_u8 outcome,
                                     const fl_search_stats_t *stats)
{
#ifdef FLUFF_STATS
    if (ctx == NULL || stats == NULL || stats->did_search == 0) {
        return;
    }

    ctx->outcome_searches[outcome]++;
    ctx->outcome_probes[outcome] += stats->probes;
    ctx->outcome_cmp_bytes[outcome] += stats->cmp_bytes;
#else
    (void)ctx; (void)outcome; (void)stats;
#endif
}

static void fl_record_search_by_result(fluff_ctx_t *ctx,
                                       const fl_search_stats_t *stats,
                                       fl_u8 match_outcome)
{
#ifndef FLUFF_STATS
    (void)ctx; (void)stats; (void)match_outcome;
#else
    if (stats == NULL || stats->did_search == 0) {
        return;
    }

    if (stats->result_kind == FL_SEARCH_RESULT_MATCH) {
        fl_record_search_outcome(ctx, match_outcome, stats);
    } else if (stats->result_kind == FL_SEARCH_RESULT_SHORT_FILTERED) {
        fl_record_search_outcome(ctx, FL_SEARCH_OUTCOME_SHORT_FILTERED,
                                 stats);
    } else {
        fl_record_search_outcome(ctx, FL_SEARCH_OUTCOME_NO_MATCH, stats);
    }
#endif
}

/*
 * fl_byte_at: read a byte from the correct source.
 *
 * For positions within the current block (abs_pos >= file_pos) we read from
 * cur_src (the raw input buffer).  For positions in previous blocks we read
 * from ctx->window which was populated by post-copy at the end of each block.
 *
 * This respects the invariant: the ring buffer always reflects what was
 * previously compressed / decompressed, never bytes that are "in the future"
 * from the perspective of the current position.
 */
static fl_u8 fl_byte_at(const fluff_ctx_t *ctx, const fl_u8 *cur_src,
                         fl_u32 abs_pos, fl_u32 file_pos)
{
    if (abs_pos >= file_pos) {
        return cur_src[abs_pos - file_pos];  /* current block: read directly */
    }
    return ctx->window[abs_pos & FLUFF_WINDOW_MASK]; /* history: ring buffer  */
}

/*
 * 3-byte hash for the sub-hash table (Jenkins one-at-a-time style).
 * abs_pos must satisfy abs_pos >= file_pos (all callers pass a current-block
 * position, so fl_byte_at's history branch is never taken here).
 */
static fl_u32 fl_hash3_hyb(const fl_u8 *cur_src,
                             fl_u32 abs_pos, fl_u32 file_pos)
{
    const fl_u8 *p = cur_src + (abs_pos - file_pos);
    fl_u32 h;
    h  = (fl_u32)p[0];
    h |= (fl_u32)p[1] << 8;
    h |= (fl_u32)p[2] << 16;
    h ^= h >> 11;
    h *= 0x9e3779b9U;
    return (h >> 16) & FLUFF_H3_MASK;
}

/*
 * 4-byte multiplicative hash using direct pointer access.
 * abs_pos must satisfy abs_pos >= file_pos (all callers pass a current-block
 * position, so fl_byte_at's history branch is never taken here).
 */
static fl_u32 fl_hash4_hyb(const fl_u8 *cur_src,
                             fl_u32 abs_pos, fl_u32 file_pos)
{
    const fl_u8 *p = cur_src + (abs_pos - file_pos);
    fl_u32 h;
    h  = (fl_u32)p[0];
    h |= (fl_u32)p[1] << 8;
    h |= (fl_u32)p[2] << 16;
    h |= (fl_u32)p[3] << 24;
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    return (h >> (32 - FLUFF_HASH_BITS)) & FLUFF_HASH_MASK;
}

/*
 * Probe the dense 3-byte sub-hash for one recent candidate.
 *
 * This is used to seed the main chain search with a stronger minimum match
 * length. The caller still lets the 4-byte chain win ties, so parse choices
 * remain unchanged versus the old fallback-only ordering.
 */
static fl_u32 fl_find_subhash_match_hyb(fluff_ctx_t *ctx,
                                        const fl_u8 *cur_src,
                                        const fl_u8 *cur_p,
                                        fl_u32 cur_abs,
                                        fl_u32 file_pos,
                                        fl_u32 src_rem,
                                        fl_u32 max_dist,
                                        fl_u32 *out_dist)
{
    fl_u32 h3;
    fl_u32 probe3;
    fl_u32 dist3;
    fl_u32 len3;
    fl_u32 max3;

    *out_dist = 0;
    if (src_rem < 3) return 0;

    h3 = fl_hash3_hyb(cur_src, cur_abs, file_pos);
    probe3 = ctx->hash3[h3];
    if (probe3 == 0 || probe3 >= cur_abs) {
        return 0;
    }

    dist3 = cur_abs - probe3;
    if (dist3 == 0 || dist3 > max_dist) {
        return 0;
    }

    if (cur_p[0] != fl_byte_at(ctx, cur_src, probe3, file_pos)) {
        return 0;
    }

    max3 = FL_MIN(FLUFF_MAX_MATCH, src_rem);
    if (probe3 >= file_pos) {
        const fl_u8 *pb3;

        pb3 = cur_src + (probe3 - file_pos);
        len3 = fl_match_len_direct(cur_p, pb3, max3);
    } else {
        for (len3 = 0; len3 < max3; len3++) {
            if (cur_p[len3] !=
                fl_byte_at(ctx, cur_src, probe3 + len3, file_pos)) {
                break;
            }
        }
    }

    if (len3 < FLUFF_MIN_MATCH) {
        return 0;
    }

    *out_dist = dist3;
    return len3;
}

/*
 * Find best match at cur_abs using the hybrid byte-access function.
 *
 * cur_abs is always >= file_pos (current encoding position within this block).
 * Probe positions from the hash table may be < file_pos (previous blocks) and
 * must be read through fl_byte_at() which dispatches to ctx->window.
 * When probe >= file_pos, both sides are in cur_src and a direct pointer
 * comparison avoids the conditional dispatch on every byte.
 * max_dist is capped at WINDOW_SIZE - 1 to avoid ring aliasing.
 */
static fl_u32 fl_find_match_hyb(fluff_ctx_t *ctx, const fl_u8 *cur_src,
                                  fl_u32 cur_abs, fl_u32 file_pos,
                                  fl_u32 src_rem, fl_u32 *out_dist,
                                  fl_search_stats_t *search_stats)
{
    fl_u32 best_len  = FLUFF_MIN_MATCH - 1;
    fl_u32 best_dist = 0;
    fl_u32 main_best_len;
    fl_u32 subhash_len;
    fl_u32 subhash_dist;
    fl_u32 max_match = FL_MIN(FLUFF_MAX_MATCH, src_rem);
    fl_u32 max_dist  = FL_MIN(cur_abs, FLUFF_WINDOW_SIZE - 1);
    fl_u32 chain_len = ctx->max_chain;
    fl_u32 probe;
    fl_u32 chain_depth = 0;  /* track depth for statistics */
    fl_u32 cmp_bytes = 0;
    const fl_u8 *cur_p;      /* direct pointer to cur_abs in cur_src */

    if (search_stats != NULL) {
        search_stats->probes = 0;
        search_stats->cmp_bytes = 0;
        search_stats->result_kind = FL_SEARCH_RESULT_NONE;
        search_stats->did_search = 0;
    }

    *out_dist = 0;
    if (src_rem < 4) return 0;
    FL_STAT(ctx->total_match_searches++);
    if (search_stats != NULL) {
        search_stats->did_search = 1;
    }

    /* cur_abs >= file_pos always: skip the fl_byte_at branch for the
     * current side by caching a direct pointer into cur_src. */
    cur_p = cur_src + (cur_abs - file_pos);

    subhash_len = fl_find_subhash_match_hyb(ctx, cur_src, cur_p, cur_abs,
                                            file_pos, src_rem, max_dist,
                                            &subhash_dist);
    if (subhash_len >= FLUFF_MIN_MATCH) {
        best_len = subhash_len - 1;
    }

    main_best_len = FLUFF_MIN_MATCH - 1;

    probe = ctx->hash_table[fl_hash4_hyb(cur_src, cur_abs, file_pos)];

    while (probe != 0 && chain_len > 0) {
        fl_u32 dist;
        fl_u32 len;

        chain_depth++;  /* count each probe */

        if (probe >= cur_abs) {
            probe = ctx->hash_chain[probe & FLUFF_WINDOW_MASK];
            chain_len--;
            continue;
        }
        dist = cur_abs - probe;
        if (dist > max_dist || dist == 0) break;

        /* quick rejection: cur side always via cur_p; probe side via ring */
        cmp_bytes++;
        if (cur_p[best_len] !=
            fl_byte_at(ctx, cur_src, probe + best_len, file_pos)) {
            probe = ctx->hash_chain[probe & FLUFF_WINDOW_MASK];
            chain_len--;
            continue;
        }

        /* full comparison: fast path when probe is also within cur_src */
        if (probe >= file_pos) {
            const fl_u8 *pb = cur_src + (probe - file_pos);
            len = fl_match_len_direct(cur_p, pb, max_match);
            cmp_bytes += len;
            if (len < max_match) {
                cmp_bytes++;
            }
        } else {
            for (len = 0; len < max_match; len++) {
                if (cur_p[len] !=
                    fl_byte_at(ctx, cur_src, probe + len, file_pos))
                    break;
            }
            cmp_bytes += len;
            if (len < max_match) {
                cmp_bytes++;
            }
        }

        if (len > main_best_len) {
            main_best_len = len;
        }

        if (len > best_len) {
            best_len  = len;
            best_dist = dist;
            if (len >= max_match || len >= FLUFF_GOOD_MATCH) break;
        }

        probe = ctx->hash_chain[probe & FLUFF_WINDOW_MASK];
        chain_len--;
    }

    /* Update statistics */
    FL_STAT(ctx->total_chain_probes += chain_depth);
    FL_STAT(ctx->total_chain_cmp_bytes += cmp_bytes);
    if (search_stats != NULL) {
        search_stats->probes = chain_depth;
        search_stats->cmp_bytes = cmp_bytes;
    }
#ifdef FLUFF_STATS
    if (chain_depth > ctx->max_chain_depth) {
        ctx->max_chain_depth = chain_depth;
    }
    if (chain_len == 0 && main_best_len < FLUFF_MIN_MATCH) {
        ctx->total_no_match_saturations++;
    }
#endif

    if (subhash_len > main_best_len) {
        best_len = subhash_len;
        best_dist = subhash_dist;
        FL_STAT(ctx->total_subhash_wins++);
    }

    if (best_len >= FLUFF_MIN_MATCH) {
        /* Distance-aware minimum match filter.
         * Short matches at large distances cost more bits than they save.
         * Reject rather than emit a token that expands the output. */
        if (best_len <= 3 && best_dist > FLUFF_MIN_DIST_LEN4) {
            if (search_stats != NULL) {
                search_stats->result_kind = FL_SEARCH_RESULT_SHORT_FILTERED;
            }
            return 0;
        }
        if (best_len <= 4 && best_dist > FLUFF_MIN_DIST_LEN5) {
            if (search_stats != NULL) {
                search_stats->result_kind = FL_SEARCH_RESULT_SHORT_FILTERED;
            }
            return 0;
        }
        if (search_stats != NULL) {
            search_stats->result_kind = FL_SEARCH_RESULT_MATCH;
        }
        *out_dist = best_dist;
        return best_len;
    }

    if (search_stats != NULL) {
        search_stats->result_kind = FL_SEARCH_RESULT_NONE;
    }
    return 0;
}

/* Insert hash for absolute position abs_pos.  Does NOT write to the ring
 * buffer; that happens once per block in the post-copy at end of the block. */
static void fl_insert_hyb(fluff_ctx_t *ctx, const fl_u8 *cur_src,
                           fl_u32 abs_pos, fl_u32 file_pos)
{
    fl_u32 h4 = fl_hash4_hyb(cur_src, abs_pos, file_pos);
    fl_u32 h3 = fl_hash3_hyb(cur_src, abs_pos, file_pos);
    ctx->hash_chain[abs_pos & FLUFF_WINDOW_MASK] = ctx->hash_table[h4];
    ctx->hash_table[h4] = abs_pos;
    ctx->hash3[h3]      = abs_pos;  /* always keep most-recent 3-byte hit */
}

static fl_u32 fl_match_savings_est(fl_u32 length, fl_u32 distance,
                                   fl_u8 token_type);

static fl_u32 fl_match_at_distance(fluff_ctx_t *ctx, const fl_u8 *cur_src,
                                   fl_u32 cur_abs, fl_u32 file_pos,
                                   fl_u32 src_rem, fl_u32 distance,
                                   fl_u32 *cmp_bytes_out)
{
    fl_u32 probe_pos;
    fl_u32 max_match;
    fl_u32 cmp_bytes;
    fl_u32 len;
    const fl_u8 *cur_p;

    cmp_bytes = 0;
    if (cmp_bytes_out != NULL) {
        *cmp_bytes_out = 0;
    }

    if (distance == 0 || distance >= cur_abs || src_rem < FLUFF_MIN_MATCH) {
        return 0;
    }

    probe_pos = cur_abs - distance;
    max_match = FL_MIN(FLUFF_MAX_MATCH, src_rem);
    cur_p     = cur_src + (cur_abs - file_pos);

    cmp_bytes++;
    if (cur_p[0] != fl_byte_at(ctx, cur_src, probe_pos, file_pos)) {
        if (cmp_bytes_out != NULL) {
            *cmp_bytes_out = cmp_bytes;
        }
        return 0;
    }

    if (probe_pos >= file_pos) {
        const fl_u8 *pb;

        pb = cur_src + (probe_pos - file_pos);
        len = 1 + fl_match_len_direct(cur_p + 1, pb + 1, max_match - 1);
        cmp_bytes += len - 1;
        if (len < max_match) cmp_bytes++;
    } else {
        for (len = 1; len < max_match; len++) {
            if (cur_p[len] != fl_byte_at(ctx, cur_src, probe_pos + len,
                                         file_pos)) {
                cmp_bytes++;
                break;
            }
            cmp_bytes++;
        }
    }

    if (cmp_bytes_out != NULL) {
        *cmp_bytes_out = cmp_bytes;
    }

    if (len >= FLUFF_MIN_MATCH) {
        return len;
    }
    return 0;
}

static fl_u8 fl_apply_rep_ring(fluff_ctx_t *ctx, const fl_u8 *cur_src,
                               fl_u32 cur_abs, fl_u32 file_pos,
                               fl_u32 src_rem,
                               fl_u32 rep0, fl_u32 rep1, fl_u32 rep2,
                               fl_u32 *match_len, fl_u32 *match_dist)
{
    fl_u32 best_savings;
    fl_u32 rep_cmp_bytes;
    fl_u32 rep_len;
    fl_u32 rep_savings;
    fl_u8  best_type;
    fl_u8  token_type;

    best_type = 0;
    best_savings = 0;
    if (*match_len >= FLUFF_MIN_MATCH) {
        best_type = 1;
        best_savings = fl_match_savings_est(*match_len, *match_dist, 1);
    }

    token_type = 0;

    FL_STAT(ctx->total_rep_searches++);

    rep_len = 0;
    if (rep0 != 0 && rep0 < cur_abs && src_rem >= FLUFF_MIN_MATCH) {
        FL_STAT(ctx->rep_slot_probes[0]++);
        rep_len = fl_match_at_distance(ctx, cur_src, cur_abs, file_pos,
                                       src_rem, rep0, &rep_cmp_bytes);
        FL_STAT(ctx->rep_slot_cmp_bytes[0] += rep_cmp_bytes);
        FL_STAT(if (rep_len >= FLUFF_MIN_MATCH) ctx->rep_slot_matches[0]++);
    }
    rep_savings = fl_match_savings_est(rep_len, rep0, 2);
    if (rep_len >= FLUFF_MIN_MATCH &&
        (rep_savings > best_savings ||
         (rep_savings == best_savings && rep_len > *match_len) ||
         (rep_savings == best_savings && rep_len == *match_len &&
          best_type == 1))) {
        *match_len  = rep_len;
        *match_dist = rep0;
        best_savings = rep_savings;
        best_type = 2;
        token_type  = 2;
    }

    rep_len = 0;
    if (rep1 != 0 && rep1 < cur_abs && src_rem >= FLUFF_MIN_MATCH) {
        FL_STAT(ctx->rep_slot_probes[1]++);
        rep_len = fl_match_at_distance(ctx, cur_src, cur_abs, file_pos,
                                       src_rem, rep1, &rep_cmp_bytes);
        FL_STAT(ctx->rep_slot_cmp_bytes[1] += rep_cmp_bytes);
        FL_STAT(if (rep_len >= FLUFF_MIN_MATCH) ctx->rep_slot_matches[1]++);
    }
    rep_savings = fl_match_savings_est(rep_len, rep1, 3);
    if (rep_len >= FLUFF_MIN_MATCH &&
        (rep_savings > best_savings ||
         (rep_savings == best_savings && rep_len > *match_len) ||
         (rep_savings == best_savings && rep_len == *match_len &&
          best_type == 1))) {
        *match_len  = rep_len;
        *match_dist = rep1;
        best_savings = rep_savings;
        best_type = 3;
        token_type  = 3;
    }

    rep_len = 0;
    if (rep2 != 0 && rep2 < cur_abs && src_rem >= FLUFF_MIN_MATCH) {
        FL_STAT(ctx->rep_slot_probes[2]++);
        rep_len = fl_match_at_distance(ctx, cur_src, cur_abs, file_pos,
                                       src_rem, rep2, &rep_cmp_bytes);
        FL_STAT(ctx->rep_slot_cmp_bytes[2] += rep_cmp_bytes);
        FL_STAT(if (rep_len >= FLUFF_MIN_MATCH) ctx->rep_slot_matches[2]++);
    }
    rep_savings = fl_match_savings_est(rep_len, rep2, 4);
    if (rep_len >= FLUFF_MIN_MATCH &&
        (rep_savings > best_savings ||
         (rep_savings == best_savings && rep_len > *match_len) ||
         (rep_savings == best_savings && rep_len == *match_len &&
          best_type == 1))) {
        *match_len  = rep_len;
        *match_dist = rep2;
        best_savings = rep_savings;
        best_type = 4;
        token_type  = 4;
    }

    FL_STAT(if (token_type >= 2 && token_type <= 4)
                ctx->rep_slot_wins[token_type - 2]++);

    return token_type;
}

static void fl_update_rep_ring(fl_u32 *rep0, fl_u32 *rep1, fl_u32 *rep2,
                               fl_u32 distance)
{
    fl_u32 old0;
    fl_u32 old1;

    if (distance == 0 || distance == *rep0) {
        return;
    }
    if (distance == *rep1) {
        old0  = *rep0;
        *rep0 = *rep1;
        *rep1 = old0;
        return;
    }
    if (distance == *rep2) {
        old0  = *rep0;
        old1  = *rep1;
        *rep0 = *rep2;
        *rep1 = old0;
        *rep2 = old1;
        return;
    }

    old0  = *rep0;
    old1  = *rep1;
    *rep0 = distance;
    *rep1 = old0;
    *rep2 = old1;
}

static fl_u32 fl_rep_token_to_sym(fl_u8 token_type)
{
    if (token_type == 2) return FLUFF_REP_MATCH;
    if (token_type == 3) return FLUFF_REP_MATCH2;
    return FLUFF_REP_MATCH3;
}

/* ─── Encode length/distance to symbols ─────────────────────────────────── */

static fl_u32 fl_len_to_sym(fl_u32 length, fl_u32 *extra_val,
                            fl_u32 *extra_bits)
{
    fl_u32 idx = length - FLUFF_MIN_MATCH;
    *extra_bits = (fl_u32)s_len_lut[idx].extra_bits;
    *extra_val  = length - (fl_u32)s_len_lut[idx].base;
    return (fl_u32)s_len_lut[idx].sym;
}

static fl_u32 fl_dist_to_sym(fl_u32 dist, fl_u32 *extra_val,
                             fl_u32 *extra_bits)
{
    fl_u32 i;
    fl_u32 n = (fl_u32)FLUFF_NUM_DIST_SYMS;
    for (i = n; i > 0; i--) {
        if (dist >= s_dist_base[i - 1]) {
            *extra_val  = dist - s_dist_base[i - 1];
            *extra_bits = s_dist_extra[i - 1];
            return i - 1;
        }
    }
    *extra_val = 0;
    *extra_bits = 0;
    return 0;
}

static fl_u32 fl_match_savings_est(fl_u32 length, fl_u32 distance,
                                   fl_u8 token_type)
{
    fl_u32 len_eb;
    fl_u32 cost;

    if (length < FLUFF_MIN_MATCH) {
        return 0;
    }

    len_eb = (fl_u32)s_len_lut[length - FLUFF_MIN_MATCH].extra_bits;

    if (token_type == 1) {
        cost = 18U + len_eb + fl_dist_extra_bits_fast(distance);
    } else {
        cost = 11U + len_eb;
    }

    if (length * 8U <= cost) {
        return 0;
    }
    return length * 8U - cost;
}

static void fl_insert_range_hyb(fluff_ctx_t *ctx, const fl_u8 *src,
                                fl_u32 file_pos, fl_u32 src_len,
                                fl_u32 begin_abs, fl_u32 end_abs,
                                fl_u32 *next_insert_abs)
{
    fl_u32 abs_pos;
    fl_u32 block_end;

    block_end = file_pos + src_len;
    abs_pos = begin_abs;
    if (abs_pos < *next_insert_abs) {
        abs_pos = *next_insert_abs;
    }

    while (abs_pos < end_abs && abs_pos + 3 < block_end) {
        fl_insert_hyb(ctx, src, abs_pos, file_pos);
        abs_pos++;
    }

    if (*next_insert_abs < abs_pos) {
        *next_insert_abs = abs_pos;
    }
}

static int fl_emit_literal_token(fluff_ctx_t *ctx, fl_u8 *tokens,
                                 fl_u32 token_cap, fl_u32 *num_tokens,
                                 fl_u32 *ll_freq, fl_u8 literal,
                                 int update_stats)
{
    if (tokens != NULL) {
        if (*num_tokens + 2 > token_cap) {
            return BP_ERR_OVERFLOW;
        }
        tokens[*num_tokens] = literal;
        tokens[*num_tokens + 1] = 0;
        *num_tokens += 2;
    }

    ll_freq[literal]++;
    if (update_stats && ctx != NULL) {
        ctx->total_literals++;
    }
    return BP_OK;
}

static int fl_emit_match_token(fluff_ctx_t *ctx, fl_u8 *tokens,
                               fl_u32 token_cap, fl_u32 *num_tokens,
                               fl_u32 *ll_freq, fl_u32 *d_freq,
                               fl_u32 match_len, fl_u32 match_dist,
                               fl_u8 token_type, int update_stats)
{
    if (token_type == 1) {
        fl_u32 len_sym;
        fl_u32 len_ev;
        fl_u32 len_eb;
        fl_u32 dist_sym;
        fl_u32 dist_ev;
        fl_u32 dist_eb;

        if (tokens != NULL) {
            if (*num_tokens + 6 > token_cap) {
                return BP_ERR_OVERFLOW;
            }
            tokens[*num_tokens] = (fl_u8)(match_len & 0xFF);
            tokens[*num_tokens + 1] = (fl_u8)((match_len >> 8) & 0xFF);
            tokens[*num_tokens + 2] = (fl_u8)(match_dist & 0xFF);
            tokens[*num_tokens + 3] = (fl_u8)((match_dist >> 8) & 0xFF);
            tokens[*num_tokens + 4] = (fl_u8)((match_dist >> 16) & 0xFF);
            tokens[*num_tokens + 5] = 1;
            *num_tokens += 6;
        }

        len_sym = fl_len_to_sym(match_len, &len_ev, &len_eb);
        dist_sym = fl_dist_to_sym(match_dist, &dist_ev, &dist_eb);
        (void)len_ev;
        (void)len_eb;
        (void)dist_ev;
        (void)dist_eb;
        ll_freq[len_sym]++;
        d_freq[dist_sym]++;
    } else {
        if (tokens != NULL) {
            if (*num_tokens + 3 > token_cap) {
                return BP_ERR_OVERFLOW;
            }
            tokens[*num_tokens] = (fl_u8)(match_len & 0xFF);
            tokens[*num_tokens + 1] = (fl_u8)((match_len >> 8) & 0xFF);
            tokens[*num_tokens + 2] = token_type;
            *num_tokens += 3;
        }

        ll_freq[fl_rep_token_to_sym(token_type)]++;
    }

    if (update_stats && ctx != NULL) {
        ctx->total_matches++;
        ctx->total_match_bytes += match_len;
    }

    return BP_OK;
}

/* ─── Compress (flatten) ────────────────────────────────────────────────── */

/*
 * Stream header: 4 bytes
 *   byte 0: 'F' (0x46)
 *   byte 1: 'L' (0x4C)
 *   byte 2: version
 *   byte 3: window_bits
 */
#define FLUFF_HDR_SIZE 4

static int fl_check_header(const fl_u8 *src, fl_u32 src_size,
                           fl_u32 *win_bits, fl_u32 *stream_version)
{
    if (src_size < FLUFF_HDR_SIZE) return -1;
    if (src[0] != 0x46 || src[1] != 0x4C) return -1;
    if (src[2] != FLUFF_STREAM_VERSION
        && src[2] != FLUFF_STREAM_VERSION_FSE
        && src[2] != FLUFF_STREAM_VERSION_FSE_RESET
        && src[2] != FLUFF_STREAM_VERSION_FSE2
        && src[2] != FLUFF_STREAM_VERSION_FSE2_RESET
        && src[2] != FLUFF_STREAM_VERSION_FSE3
        && src[2] != FLUFF_STREAM_VERSION_FSE3_RESET) return -1;
    *win_bits = src[3];
    if (stream_version != NULL) {
        *stream_version = src[2];
    }
    return 0;
}

/* (Huffman tree write/read removed: range coder needs no tree header) */

static int fl_parse_greedy(fluff_ctx_t *ctx, const fl_u8 *src,
                           fl_u32 src_len, fl_u32 file_pos,
                           fl_u8 *tokens, fl_u32 token_cap,
                           fl_u32 *out_num_tokens,
                           fl_u32 *ll_freq, fl_u32 *d_freq)
{
    fl_u32 i;
    fl_u32 rep0;
    fl_u32 rep1;
    fl_u32 rep2;
    fl_u32 next_insert_abs;
    fl_u32 num_tokens;
    int rc;

    i = 0;
    rep0 = 0;
    rep1 = 0;
    rep2 = 0;
    next_insert_abs = file_pos;
    num_tokens = 0;

    while (i < src_len) {
        fl_u32 abs;
        fl_u32 match_len;
        fl_u32 match_dist;
        fl_u32 m_savings;
        fl_search_stats_t match_search_stats;
        fl_u8  match_token_type;

        abs = file_pos + i;
        match_len = fl_find_match_hyb(ctx, src, abs, file_pos,
                                      src_len - i, &match_dist,
                                      &match_search_stats);
        match_token_type = fl_apply_rep_ring(ctx, src, abs, file_pos,
                                             src_len - i,
                                             rep0, rep1, rep2,
                                             &match_len, &match_dist);
        fl_insert_range_hyb(ctx, src, file_pos, src_len,
                            abs, abs + 1, &next_insert_abs);

        if (match_len >= FLUFF_MIN_MATCH) {
            m_savings = fl_match_savings_est(match_len, match_dist,
                                             (fl_u8)(match_token_type != 0 ?
                                                     match_token_type : 1));

            if (match_len < FLUFF_GOOD_MATCH && i + 1 < src_len &&
                src_len - (i + 1) >= 4) {
                fl_u32 lazy1_dist;
                fl_u32 lazy1_len;
                fl_u32 lazy1_savings;
                fl_search_stats_t lazy1_search_stats;
                fl_u8  lazy1_token_type;

                lazy1_len = fl_find_match_hyb(ctx, src, abs + 1,
                                              file_pos, src_len - (i + 1),
                                              &lazy1_dist,
                                              &lazy1_search_stats);
                lazy1_token_type = fl_apply_rep_ring(ctx, src, abs + 1,
                                                     file_pos,
                                                     src_len - (i + 1),
                                                     rep0, rep1, rep2,
                                                     &lazy1_len, &lazy1_dist);
                fl_insert_range_hyb(ctx, src, file_pos, src_len,
                                    abs + 1, abs + 2, &next_insert_abs);
                lazy1_savings = fl_match_savings_est(lazy1_len, lazy1_dist,
                                                     (fl_u8)(lazy1_token_type != 0 ?
                                                             lazy1_token_type : 1));

                if (lazy1_len >= FLUFF_MIN_MATCH &&
                    lazy1_savings > m_savings + 8) {
                    if (lazy1_len < FLUFF_GOOD_MATCH &&
                        i + 2 < src_len &&
                        src_len - (i + 2) >= 4) {
                        fl_u32 lazy2_dist;
                        fl_u32 lazy2_len;
                        fl_u32 lazy2_savings;
                        fl_search_stats_t lazy2_search_stats;
                        fl_u8  lazy2_token_type;

                        lazy2_len = fl_find_match_hyb(ctx, src, abs + 2,
                                                      file_pos,
                                                      src_len - (i + 2),
                                                      &lazy2_dist,
                                                      &lazy2_search_stats);
                        lazy2_token_type = fl_apply_rep_ring(ctx, src,
                                                             abs + 2,
                                                             file_pos,
                                                             src_len - (i + 2),
                                                             rep0, rep1, rep2,
                                                             &lazy2_len,
                                                             &lazy2_dist);
                        fl_insert_range_hyb(ctx, src, file_pos, src_len,
                                            abs + 2, abs + 3,
                                            &next_insert_abs);
                        lazy2_savings = fl_match_savings_est(lazy2_len,
                                                             lazy2_dist,
                                                             (fl_u8)(lazy2_token_type != 0 ?
                                                                     lazy2_token_type : 1));

                        if (lazy2_len >= FLUFF_MIN_MATCH &&
                            lazy2_savings > lazy1_savings + 8) {
                            fl_record_search_by_result(
                                ctx, &match_search_stats,
                                FL_SEARCH_OUTCOME_LOOKAHEAD_DROP);
                            fl_record_search_by_result(
                                ctx, &lazy1_search_stats,
                                FL_SEARCH_OUTCOME_LOOKAHEAD_DROP);
                            rc = fl_emit_literal_token(ctx, tokens, token_cap,
                                                       &num_tokens, ll_freq,
                                                       src[i], 1);
                            if (rc != BP_OK) return rc;
#ifdef FLUFF_DEBUG_PARSE
                            if (s_dbg_fp) fprintf(s_dbg_fp, "%u,literal,1,0,%u,1\n",
                                (unsigned)(file_pos + i), (unsigned)src[i]);
#endif
                            rc = fl_emit_literal_token(ctx, tokens, token_cap,
                                                       &num_tokens, ll_freq,
                                                       src[i + 1], 1);
                            if (rc != BP_OK) return rc;
#ifdef FLUFF_DEBUG_PARSE
                            if (s_dbg_fp) fprintf(s_dbg_fp, "%u,literal,1,0,%u,1\n",
                                (unsigned)(file_pos + i + 1), (unsigned)src[i + 1]);
#endif
                            i += 2;
                            abs = file_pos + i;
                            match_len = lazy2_len;
                            match_dist = lazy2_dist;
                            match_token_type = lazy2_token_type;
                            match_search_stats = lazy2_search_stats;
                        } else {
                            fl_record_search_by_result(
                                ctx, &match_search_stats,
                                FL_SEARCH_OUTCOME_LOOKAHEAD_DROP);
                            fl_record_search_by_result(
                                ctx, &lazy2_search_stats,
                                FL_SEARCH_OUTCOME_LOOKAHEAD_DROP);
                            rc = fl_emit_literal_token(ctx, tokens, token_cap,
                                                       &num_tokens, ll_freq,
                                                       src[i], 1);
                            if (rc != BP_OK) return rc;
#ifdef FLUFF_DEBUG_PARSE
                            if (s_dbg_fp) fprintf(s_dbg_fp, "%u,literal,1,0,%u,1\n",
                                (unsigned)(file_pos + i), (unsigned)src[i]);
#endif
                            i++;
                            abs = file_pos + i;
                            match_len = lazy1_len;
                            match_dist = lazy1_dist;
                            match_token_type = lazy1_token_type;
                            match_search_stats = lazy1_search_stats;
                        }
                    } else {
                        fl_record_search_by_result(
                            ctx, &match_search_stats,
                            FL_SEARCH_OUTCOME_LOOKAHEAD_DROP);
                        rc = fl_emit_literal_token(ctx, tokens, token_cap,
                                                   &num_tokens, ll_freq,
                                                   src[i], 1);
                        if (rc != BP_OK) return rc;
#ifdef FLUFF_DEBUG_PARSE
                        if (s_dbg_fp) fprintf(s_dbg_fp, "%u,literal,1,0,%u,1\n",
                            (unsigned)(file_pos + i), (unsigned)src[i]);
#endif
                        i++;
                        abs = file_pos + i;
                        match_len = lazy1_len;
                        match_dist = lazy1_dist;
                        match_token_type = lazy1_token_type;
                        match_search_stats = lazy1_search_stats;
                    }
                } else {
                    fl_record_search_by_result(ctx, &lazy1_search_stats,
                                               FL_SEARCH_OUTCOME_LOOKAHEAD_DROP);
                }
            }

            if (match_token_type != 0) {
                fl_record_search_outcome(ctx, FL_SEARCH_OUTCOME_REP_SELECTED,
                                         &match_search_stats);
            } else {
                fl_record_search_outcome(ctx,
                                         FL_SEARCH_OUTCOME_ACCEPTED_MATCH,
                                         &match_search_stats);
            }

            rc = fl_emit_match_token(ctx, tokens, token_cap, &num_tokens,
                                     ll_freq,
                                     d_freq, match_len, match_dist,
                                     (fl_u8)(match_token_type != 0 ?
                                             match_token_type : 1), 1);
            if (rc != BP_OK) return rc;
#ifdef FLUFF_DEBUG_PARSE
            if (s_dbg_fp) {
                const char *dbg_a = (match_token_type != 0) ? "rep_match" : "match";
                fprintf(s_dbg_fp, "%u,%s,%u,%u,0,0\n",
                    (unsigned)abs, dbg_a,
                    (unsigned)match_len, (unsigned)match_dist);
            }
#endif
            fl_update_rep_ring(&rep0, &rep1, &rep2, match_dist);
            fl_insert_range_hyb(ctx, src, file_pos, src_len,
                                abs + 1, abs + match_len, &next_insert_abs);
            i += match_len;
        } else {
            fl_record_search_by_result(ctx, &match_search_stats,
                                       FL_SEARCH_OUTCOME_ACCEPTED_MATCH);
            rc = fl_emit_literal_token(ctx, tokens, token_cap, &num_tokens,
                                       ll_freq, src[i], 1);
            if (rc != BP_OK) return rc;
#ifdef FLUFF_DEBUG_PARSE
            if (s_dbg_fp) fprintf(s_dbg_fp, "%u,literal,1,0,%u,0\n",
                (unsigned)(file_pos + i), (unsigned)src[i]);
#endif
            i++;
        }
    }

    *out_num_tokens = num_tokens;
    return BP_OK;
}

/*
 * Compress one block with a persistent context. The v0x09 compact header
 * stores lit/len and distance count tables, two uint16 ANS states, and two
 * uint24 stream lengths. The lit/len and mixed distance/extra-bit streams
 * follow it; both are written and read backwards.
 */
int fluff_compress_block_ctx(const bp_uint8 *src, bp_uint32 src_len,
                              bp_uint8 *dst, bp_uint32 *dst_len,
                              fluff_ctx_t *ctx)
{
    fl_u32    i;
    fl_u32    bound;
    fl_u32    file_pos;
    fl_u8    *tokens;
    fl_u32    num_tokens;
    fl_u32    token_cap;
    fl_u32    ll_freq[FLUFF_NUM_LITLEN];
    fl_u32    d_freq[FLUFF_NUM_DIST];
    fl_u16   *ll_norm;
    fl_u16   *d_norm;
    fl_u16 *ll_slots;       /* spread positions per symbol, ll alphabet */
    fl_u16 *d_slots;        /* spread positions per symbol, d alphabet */
    fse_sym_enc_t *ll_senc;
    fse_sym_enc_t *d_senc;
    /* Backwards ANS encoder: encode tokens last-to-first. */
    fl_u8    *fse_buf;    /* LL stream written directly into dst */
    fl_u8    *raw_buf;    /* distance/raw stream reuses consumed tokens */
    fl_u32    fse_cap;
    fl_u32    raw_cap;
    fse_bits_t fse_bits;
    fse_bits_t raw_bits;
    fl_u32    ll_state;
    fl_u32    d_state;
    fl_u32    hdr_pos;
    fl_u32    tail_pos;
    fl_u32    fse_len;
    fl_u32    raw_len;
    int       parse_rc;
    int       norm_rc;

    if (src == NULL || dst == NULL || dst_len == NULL || ctx == NULL)
        return BP_ERR_INVALID_ARG;

    bound    = fluff_compress_bound(src_len);
    file_pos = ctx->file_pos;

    /* Match-finder tables and scratch buffers are context-owned and lazily
     * allocated (the match finder is compress-only, so decode contexts never
     * pay for it).  Normally fluff_ctx_reset has already allocated the tables;
     * this re-check is the safety net if that allocation failed. */
    if (fl_ctx_ensure_matchfinder(ctx) != BP_OK) {
        return BP_ERR_NOMEM;
    }
    if (fl_ctx_ensure_scratch(ctx, src_len) != BP_OK) {
        return BP_ERR_NOMEM;
    }
    token_cap = src_len * 2 + 16;
    tokens    = ctx->scratch_tokens;

    /* --- Pass 1: LZ77 greedy parse → token buffer + frequencies --- */
    memset(ll_freq, 0, sizeof(ll_freq));
    memset(d_freq,  0, sizeof(d_freq));
    ll_freq[FLUFF_END_OF_BLOCK] = 1;  /* EOB always present */
    parse_rc = fl_parse_greedy(ctx, src, src_len, file_pos,
                               tokens, token_cap, &num_tokens,
                               ll_freq, d_freq);
    if (parse_rc != BP_OK) { return parse_rc; }

    /* FSE working tables: embedded in the context (no per-block malloc). */
    ll_norm  = ctx->fse_ll_norm;
    d_norm   = ctx->fse_d_norm;
    ll_slots = ctx->fse_ll_slots;
    d_slots  = ctx->fse_d_slots;
    ll_senc  = ctx->fse_ll_senc;
    d_senc   = ctx->fse_d_senc;
    raw_cap  = token_cap;
    raw_buf  = tokens;

    /* --- Pass 2a: normalize frequencies → count tables --- */
    /* Ensure EOB has at least 1 so it always gets a slot. */
    if (ll_freq[FLUFF_END_OF_BLOCK] == 0) ll_freq[FLUFF_END_OF_BLOCK] = 1;
    /* Ensure dist has at least one active symbol even if no matches. */
    if (d_freq[0] == 0) { d_freq[0] = 1; }
    norm_rc  = fse_normalize(ll_freq, FLUFF_NUM_LITLEN, ll_norm, FSE_LL_BITS);
    norm_rc |= fse_normalize(d_freq,  FLUFF_NUM_DIST,   d_norm,  FSE_D_BITS);
    if (norm_rc != 0) {
        return BP_ERR_COMPRESS;
    }

    /* --- Pass 2b: build encode slots + per-symbol encode params --- */
    if (fse_build_ctab(ll_norm, FLUFF_NUM_LITLEN, ll_slots, ll_senc, FSE_LL_BITS) != 0 ||
        fse_build_ctab(d_norm,  FLUFF_NUM_DIST,   d_slots,  d_senc,  FSE_D_BITS)  != 0) {
        return BP_ERR_COMPRESS;
    }

    {
        int nll, nd;

        hdr_pos = 0;
        dst[hdr_pos++] = 0x46; /* 'F' */
        dst[hdr_pos++] = 0x4C; /* 'L' */
        dst[hdr_pos++] = FLUFF_STREAM_VERSION_FSE3;
        dst[hdr_pos++] = (fl_u8)FLUFF_WINDOW_BITS;

        /* Count tables (compact). */
        nll = fse_write_counts_compact(dst, bound, hdr_pos,
                                       ll_norm, FLUFF_NUM_LITLEN);
        if (nll < 0) return BP_ERR_COMPRESS;
        hdr_pos += (fl_u32)nll;
        nd = fse_write_counts_compact(dst, bound, hdr_pos,
                                      d_norm, FLUFF_NUM_DIST);
        if (nd < 0) return BP_ERR_COMPRESS;
        hdr_pos += (fl_u32)nd;
    }
    if (hdr_pos + 10U > bound) return BP_ERR_COMPRESS;
    tail_pos = hdr_pos;
    hdr_pos += 10;
    fse_buf = dst + hdr_pos;
    fse_cap = bound - hdr_pos;

    /* --- Pass 2c: ANS-encode tokens BACKWARDS into fse_buf --- */
    fse_bits_init_write(&fse_bits, fse_buf, fse_cap);
    fse_bits_init_write(&raw_bits, raw_buf, raw_cap);
    raw_bits.reverse = 1;

    /* Initial states for both alphabets: start at state 0 (in [0, L)). */
    ll_state = 0;
    d_state  = 0;

    /*
     * Encode all tokens last-to-first.  For each token:
     *   - encode the litlen/rep symbol via fse_encode_sym_fast
     *   - raw extra bits go into raw_bits (also backwards)
     *   - dist symbol encoded via fse_encode_sym_fast when present
     *
     * First encode EOB (the last token conceptually).
     */
    ll_state = fse_encode_sym_fast(ll_state, FLUFF_END_OF_BLOCK,
                                   ll_slots, ll_senc, &fse_bits);

    /*
     * Each internal token ends with its type, so it can be read backwards
     * without a separate index. Raw bytes grow downwards from token_cap.
     * A normal token consumes 6 bytes before emitting at most 31 raw bits
     * (17 distance extras + 9 distance ANS bits + 5 length extras); a repeat
     * consumes 3 before emitting at most 10 bits; a literal consumes 2 and
     * emits none. Even with a partial byte pending, neither can write more
     * bytes than it consumes. All fields are loaded before any raw write.
     * The 16-byte margin also covers the final sentinel. Thus the raw writer
     * cannot reach unread tokens, even when the token buffer is full.
     */
    i = num_tokens;
    while (i > 0) {
        fl_u8 tok = tokens[i - 1];
        if (tok == 1) i -= 6;
        else if (tok == 0) i -= 2;
        else i -= 3;

        if (tok == 0) {
            /* Literal */
            fl_u32 lit = tokens[i];
            ll_state = fse_encode_sym_fast(ll_state, lit,
                                           ll_slots, ll_senc, &fse_bits);
        } else if (tok >= 2 && tok <= 4) {
            /* Rep-match: encode 5-bit len index + extra bits (raw), then sym */
            fl_u32 mlen = (fl_u32)tokens[i] | ((fl_u32)tokens[i + 1] << 8);
            fl_u32 lev, leb, lsym;
            lsym = fl_len_to_sym(mlen, &lev, &leb);
            /* raw extra bits: backwards into raw_buf */
            if (leb > 0) fse_bits_write(&raw_bits, lev, leb);
            fse_bits_write(&raw_bits, lsym - 257, 5);
            ll_state = fse_encode_sym_fast(ll_state, fl_rep_token_to_sym(tok),
                                           ll_slots, ll_senc, &fse_bits);
        } else {
            /* Regular match */
            fl_u32 mlen  = (fl_u32)tokens[i] | ((fl_u32)tokens[i + 1] << 8);
            fl_u32 mdist = (fl_u32)tokens[i + 2] | ((fl_u32)tokens[i + 3] << 8)
                         | ((fl_u32)tokens[i + 4] << 16);
            fl_u32 lsym, lev, leb;
            fl_u32 dsym, dev, deb;
            lsym = fl_len_to_sym(mlen,  &lev, &leb);
            dsym = fl_dist_to_sym(mdist, &dev, &deb);
            /* encode dist extra bits + dist sym (backwards) */
            if (deb > 0) fse_bits_write(&raw_bits, dev, deb);
            d_state = fse_encode_sym_fast(d_state, dsym,
                                          d_slots, d_senc, &raw_bits);
            /* encode len extra bits + len sym */
            if (leb > 0) fse_bits_write(&raw_bits, lev, leb);
            ll_state = fse_encode_sym_fast(ll_state, lsym,
                                           ll_slots, ll_senc, &fse_bits);
        }
    }

    fse_bits_flush(&fse_bits);
    fse_bits_flush(&raw_bits);
    fse_len = fse_bits.pos;
    raw_len = raw_bits.pos;

    if (fse_len > bound - hdr_pos || raw_len > bound - hdr_pos - fse_len)
        return BP_ERR_COMPRESS;
    fse_write_u16(dst, tail_pos, ll_state);
    fse_write_u16(dst, tail_pos + 2, d_state);
    fse_write_u24(dst, tail_pos + 4, fse_len);
    fse_write_u24(dst, tail_pos + 7, raw_len);
    hdr_pos += fse_len;
    /* Restore the stream's original byte order after the downward writes. */
    for (i = 0; i < raw_len; i++)
        dst[hdr_pos + i] = raw_buf[raw_cap - 1U - i];
    hdr_pos += raw_len;

    *dst_len = hdr_pos;

    /* Scratch and FSE tables are context-owned; nothing to free here. */

    /* Post-copy ring buffer. */
    {
        fl_u32 ring_start = file_pos & FLUFF_WINDOW_MASK;
        fl_u32 chunk1     = FLUFF_WINDOW_SIZE - ring_start;
        if (chunk1 > src_len) chunk1 = src_len;
        memcpy(ctx->window + ring_start, src, chunk1);
        if (chunk1 < src_len)
            memcpy(ctx->window, src + chunk1, src_len - chunk1);
    }
    ctx->file_pos += src_len;
    return BP_OK;
}

/*
 * fluff_compress_block: single-block compress with a temporary context.
 * Backward-compatible entry point; allocates and frees a context itself.
 */
int fluff_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                         bp_uint8 *dst, bp_uint32 *dst_len)
{
    fluff_ctx_t *ctx;
    int rc;

    if (src == NULL || dst == NULL || dst_len == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    ctx = fluff_ctx_alloc();
    if (ctx == NULL) return BP_ERR_NOMEM;
    fluff_ctx_reset(ctx);

    rc = fluff_compress_block_ctx(src, src_len, dst, dst_len, ctx);
    fluff_ctx_free(ctx);
    return rc;
}

/* ─── Decompress (fluff) ────────────────────────────────────────────────── */

/*
 * fl_apply_match: copy length bytes from distance back in the sliding window.
 * Used by both v0x06 (RC) and v0x07 (FSE) decoders.
 */
static int fl_apply_match(fl_u8 *dst, fl_u32 *out_pos, fl_u32 out_cap,
                           fl_u8 *window, fl_u32 file_pos,
                           fl_u32 length, fl_u32 distance)
{
    fl_u32 abs_out = file_pos + *out_pos;
    if (distance == 0 || distance >= FLUFF_WINDOW_SIZE ||
        abs_out < distance || *out_pos + length > out_cap)
        return BP_ERR_CORRUPT;
    if (distance <= *out_pos) {
        const fl_u8 *from = dst + *out_pos - distance;
        fl_u8       *to   = dst + *out_pos;
        if (distance >= length) {
            memcpy(to, from, length);
        } else {
            fl_u32 step = distance;
            fl_u32 rem  = length;
            while (rem >= step) {
                memcpy(to, from, step);
                to  += step;
                rem -= step;
                step += step;
            }
            if (rem > 0) memcpy(to, from, rem);
        }
    } else {
        fl_u32 hist = distance - *out_pos;
        fl_u32 blk;
        if (hist > length) hist = length;
        blk = length - hist;
        {
            fl_u32 ring_start = (abs_out - distance) & FLUFF_WINDOW_MASK;
            fl_u32 chunk1 = FLUFF_WINDOW_SIZE - ring_start;
            if (chunk1 > hist) chunk1 = hist;
            memcpy(dst + *out_pos, window + ring_start, chunk1);
            if (chunk1 < hist)
                memcpy(dst + *out_pos + chunk1, window, hist - chunk1);
        }
        *out_pos += hist;
        if (blk > 0) {
            const fl_u8 *from = dst;
            fl_u8       *to   = dst + *out_pos;
            fl_u32 step = *out_pos;
            fl_u32 rem  = blk;
            while (rem >= step) {
                memcpy(to, from, step);
                to  += step;
                rem -= step;
                step += step;
            }
            if (rem > 0) memcpy(to, from, rem);
            *out_pos += blk;
        }
        return BP_OK;
    }
    *out_pos += length;
    return BP_OK;
}

/* ── v0x06: adaptive range coder decode ─────────────────────────────────── */

static int fl_decompress_rc(const bp_uint8 *src, bp_uint32 src_len,
                             bp_uint8 *dst, bp_uint32 *dst_len,
                             fluff_ctx_t *ctx)
{
    fl_rc_t   rc;
    fl_u32    win_bits;
    fl_u32    stream_version;
    fl_u32    out_pos;
    fl_u32    out_cap;
    fl_u32    file_pos;
    fl_u32    rep0, rep1, rep2;
    fl_prob_t ll_probs[FL_LL_NODES];
    fl_prob_t d_probs[FL_D_NODES];

    out_cap  = *dst_len;
    out_pos  = 0;
    file_pos = ctx->file_pos;
    rep0 = 0; rep1 = 0; rep2 = 0;

    if (fl_check_header(src, src_len, &win_bits, &stream_version) != 0)
        return BP_ERR_CORRUPT;

    fl_rc_dec_init(&rc, src + FLUFF_HDR_SIZE, src_len - FLUFF_HDR_SIZE);
    fl_probs_init(ll_probs, FL_LL_NODES);
    fl_probs_init(d_probs,  FL_D_NODES);

    for (;;) {
        fl_u32 sym = fl_rc_decode_ll(&rc, ll_probs);
        int mrc;

        if (sym < 256) {
            if (out_pos >= out_cap) return BP_ERR_OVERFLOW;
            dst[out_pos++] = (fl_u8)sym;
        } else if (sym == FLUFF_END_OF_BLOCK) {
            break;
        } else if (sym >= 257 && sym <= 285) {
            fl_u32 len_idx = sym - 257;
            fl_u32 length, dsym, distance;
            if (len_idx >= FLUFF_NUM_LEN_SYMS) return BP_ERR_CORRUPT;
            length = s_len_base[len_idx];
            if (s_len_extra[len_idx] > 0)
                length += fl_rc_decode_bits(&rc, (fl_u32)s_len_extra[len_idx]);
            dsym = fl_rc_decode_dist(&rc, d_probs);
            if (dsym >= FLUFF_NUM_DIST) return BP_ERR_CORRUPT;
            distance = s_dist_base[dsym];
            if (s_dist_extra[dsym] > 0)
                distance += fl_rc_decode_bits(&rc, (fl_u32)s_dist_extra[dsym]);
            mrc = fl_apply_match(dst, &out_pos, out_cap, ctx->window,
                                 file_pos, length, distance);
            if (mrc != BP_OK) return mrc;
            fl_update_rep_ring(&rep0, &rep1, &rep2, distance);
        } else if (sym == FLUFF_REP_MATCH || sym == FLUFF_REP_MATCH2 ||
                   sym == FLUFF_REP_MATCH3) {
            fl_u32 len_idx = fl_rc_decode_bits(&rc, 5);
            fl_u32 length, distance;
            if (len_idx >= (fl_u32)FLUFF_NUM_LEN_SYMS) return BP_ERR_CORRUPT;
            length = s_len_base[len_idx];
            if (s_len_extra[len_idx] > 0)
                length += fl_rc_decode_bits(&rc, (fl_u32)s_len_extra[len_idx]);
            if      (sym == FLUFF_REP_MATCH)  distance = rep0;
            else if (sym == FLUFF_REP_MATCH2) distance = rep1;
            else                              distance = rep2;
            if (distance == 0) return BP_ERR_CORRUPT;
            mrc = fl_apply_match(dst, &out_pos, out_cap, ctx->window,
                                 file_pos, length, distance);
            if (mrc != BP_OK) return mrc;
            fl_update_rep_ring(&rep0, &rep1, &rep2, distance);
        } else {
            return BP_ERR_CORRUPT;
        }
    }

    *dst_len = out_pos;
    {
        fl_u32 ring_start = file_pos & FLUFF_WINDOW_MASK;
        fl_u32 chunk1     = FLUFF_WINDOW_SIZE - ring_start;
        if (chunk1 > out_pos) chunk1 = out_pos;
        memcpy(ctx->window + ring_start, dst, chunk1);
        if (chunk1 < out_pos)
            memcpy(ctx->window, dst + chunk1, out_pos - chunk1);
    }
    ctx->file_pos += out_pos;
    return BP_OK;
}

/* ── v0x07: FSE (tANS) decode ───────────────────────────────────────────── */

static int fl_decompress_fse(const bp_uint8 *src, bp_uint32 src_len,
                              bp_uint8 *dst, bp_uint32 *dst_len,
                              fluff_ctx_t *ctx)
{
    fl_u32    win_bits;
    fl_u32    stream_version;
    fl_u32    out_pos;
    fl_u32    out_cap;
    fl_u32    file_pos;
    fl_u32    rep0, rep1, rep2;
    fl_u16     *ll_norm = ctx->fse_ll_norm;
    fl_u16     *d_norm  = ctx->fse_d_norm;
    fse_dtab_t *ll_dtab = ctx->fse_ll_dtab;
    fse_dtab_t *d_dtab  = ctx->fse_d_dtab;
    fl_u32    ll_state;
    fl_u32    d_state;
    fse_bits_r_t fse_bits;
    fse_bits_r_t raw_bits;
    fl_u32    hdr_pos;
    fl_u32    fse_len;
    fl_u32    raw_len;
    int       norm_ok;
    int       v2;
    int       legacy_layout;
    int       rc = BP_OK;

    if (fl_check_header(src, src_len, &win_bits, &stream_version) != 0)
        { rc = BP_ERR_CORRUPT; goto cleanup; }

    legacy_layout = (stream_version != FLUFF_STREAM_VERSION_FSE3 &&
                     stream_version != FLUFF_STREAM_VERSION_FSE3_RESET);
    v2 = (stream_version == FLUFF_STREAM_VERSION_FSE2 ||
          stream_version == FLUFF_STREAM_VERSION_FSE2_RESET || !legacy_layout);

    out_cap  = *dst_len;
    out_pos  = 0;
    file_pos = ctx->file_pos;
    rep0 = 0; rep1 = 0; rep2 = 0;

    /* Count and decode tables are embedded in the context (no malloc). */
    hdr_pos = FLUFF_HDR_SIZE;

    /* Read count tables (compact for v0x08, raw uint16 for v0x07). */
    if (v2) {
        norm_ok = fse_read_counts_compact(src, src_len, hdr_pos,
                                          ll_norm, FLUFF_NUM_LITLEN, FSE_LL_BITS);
    } else {
        if (src_len < hdr_pos + FLUFF_NUM_LITLEN * 2 + FLUFF_NUM_DIST * 2 + 16)
            { rc = BP_ERR_CORRUPT; goto cleanup; }
        norm_ok = fse_read_counts(src, src_len, hdr_pos,
                                  ll_norm, FLUFF_NUM_LITLEN, FSE_LL_BITS);
    }
    if (norm_ok < 0) { rc = BP_ERR_CORRUPT; goto cleanup; }
    hdr_pos += (fl_u32)norm_ok;
    if (v2) {
        norm_ok = fse_read_counts_compact(src, src_len, hdr_pos,
                                          d_norm, FLUFF_NUM_DIST, FSE_D_BITS);
    } else {
        norm_ok = fse_read_counts(src, src_len, hdr_pos,
                                  d_norm, FLUFF_NUM_DIST, FSE_D_BITS);
    }
    if (norm_ok < 0) { rc = BP_ERR_CORRUPT; goto cleanup; }
    hdr_pos += (fl_u32)norm_ok;

    /* Build decode tables. */
    if (fse_build_dtab(ll_norm, FLUFF_NUM_LITLEN, ll_dtab, FSE_LL_BITS,
                       legacy_layout) != 0 ||
        fse_build_dtab(d_norm, FLUFF_NUM_DIST, d_dtab, FSE_D_BITS,
                       legacy_layout) != 0)
        { rc = BP_ERR_CORRUPT; goto cleanup; }

    /* Read initial states and stream lengths.
     * v0x08: uint16 states + uint24 lengths (10 bytes, R3).
     * v0x07: uint32 states + uint32 lengths (16 bytes). */
    if (v2) {
        if (hdr_pos + 10 > src_len) { rc = BP_ERR_CORRUPT; goto cleanup; }
        ll_state = fse_read_u16(src, hdr_pos); hdr_pos += 2;
        d_state  = fse_read_u16(src, hdr_pos); hdr_pos += 2;
        fse_len  = fse_read_u24(src, hdr_pos); hdr_pos += 3;
        raw_len  = fse_read_u24(src, hdr_pos); hdr_pos += 3;
    } else {
        if (hdr_pos + 16 > src_len) { rc = BP_ERR_CORRUPT; goto cleanup; }
        ll_state = fse_read_u32(src, hdr_pos); hdr_pos += 4;
        d_state  = fse_read_u32(src, hdr_pos); hdr_pos += 4;
        fse_len  = fse_read_u32(src, hdr_pos); hdr_pos += 4;
        raw_len  = fse_read_u32(src, hdr_pos); hdr_pos += 4;
    }

    /* Use subtraction after validating the offset so malicious v0x07
     * uint32 lengths cannot wrap the combined end offset. */
    if (hdr_pos > src_len || fse_len > src_len - hdr_pos ||
        raw_len > src_len - hdr_pos - fse_len)
        { rc = BP_ERR_CORRUPT; goto cleanup; }

    fse_bits_init_read(&fse_bits, src + hdr_pos, fse_len);
    fse_bits_init_read(&raw_bits, src + hdr_pos + fse_len, raw_len);

    /* Validate initial states fall within table range. */
    if (ll_state >= FSE_LL_L || d_state >= FSE_D_L)
        { rc = BP_ERR_CORRUPT; goto cleanup; }

    /* Decode loop. */
    for (;;) {
        fl_u32 sym;
        int mrc;
        sym = fse_decode_sym(&ll_state, ll_dtab, &fse_bits, FSE_LL_BITS);

        if (sym < 256) {
            if (out_pos >= out_cap) { rc = BP_ERR_OVERFLOW; goto cleanup; }
            dst[out_pos++] = (fl_u8)sym;
        } else if (sym == FLUFF_END_OF_BLOCK) {
            break;
        } else if (sym >= 257 && sym <= 285) {
            fl_u32 len_idx = sym - 257;
            fl_u32 length, dsym, distance;
            if (len_idx >= FLUFF_NUM_LEN_SYMS)
                { rc = BP_ERR_CORRUPT; goto cleanup; }
            length = s_len_base[len_idx];
            if (s_len_extra[len_idx] > 0)
                length += fse_bits_read(&raw_bits, (fl_u32)s_len_extra[len_idx]);
            /* Decode dist symbol from raw_bits (it was encoded into raw_bits). */
            dsym = fse_decode_sym(&d_state, d_dtab, &raw_bits, FSE_D_BITS);
            if (dsym >= FLUFF_NUM_DIST) { rc = BP_ERR_CORRUPT; goto cleanup; }
            distance = s_dist_base[dsym];
            if (s_dist_extra[dsym] > 0)
                distance += fse_bits_read(&raw_bits, (fl_u32)s_dist_extra[dsym]);
            mrc = fl_apply_match(dst, &out_pos, out_cap, ctx->window,
                                 file_pos, length, distance);
            if (mrc != BP_OK) { rc = mrc; goto cleanup; }
            fl_update_rep_ring(&rep0, &rep1, &rep2, distance);
        } else if (sym == FLUFF_REP_MATCH || sym == FLUFF_REP_MATCH2 ||
                   sym == FLUFF_REP_MATCH3) {
            fl_u32 len_idx = fse_bits_read(&raw_bits, 5);
            fl_u32 length, distance;
            if (len_idx >= (fl_u32)FLUFF_NUM_LEN_SYMS)
                { rc = BP_ERR_CORRUPT; goto cleanup; }
            length = s_len_base[len_idx];
            if (s_len_extra[len_idx] > 0)
                length += fse_bits_read(&raw_bits, (fl_u32)s_len_extra[len_idx]);
            if      (sym == FLUFF_REP_MATCH)  distance = rep0;
            else if (sym == FLUFF_REP_MATCH2) distance = rep1;
            else                              distance = rep2;
            if (distance == 0) { rc = BP_ERR_CORRUPT; goto cleanup; }
            mrc = fl_apply_match(dst, &out_pos, out_cap, ctx->window,
                                 file_pos, length, distance);
            if (mrc != BP_OK) { rc = mrc; goto cleanup; }
            fl_update_rep_ring(&rep0, &rep1, &rep2, distance);
        } else {
            rc = BP_ERR_CORRUPT; goto cleanup;
        }
    }

    *dst_len = out_pos;
    {
        fl_u32 ring_start = file_pos & FLUFF_WINDOW_MASK;
        fl_u32 chunk1     = FLUFF_WINDOW_SIZE - ring_start;
        if (chunk1 > out_pos) chunk1 = out_pos;
        memcpy(ctx->window + ring_start, dst, chunk1);
        if (chunk1 < out_pos)
            memcpy(ctx->window, dst + chunk1, out_pos - chunk1);
    }
    ctx->file_pos += out_pos;
    rc = BP_OK;

cleanup:
    /* Tables are context-owned; nothing to free. */
    return rc;
}

/* ── Public dispatch ─────────────────────────────────────────────────────── */

int fluff_decompress_block_ctx(const bp_uint8 *src, bp_uint32 src_len,
                                bp_uint8 *dst, bp_uint32 *dst_len,
                                fluff_ctx_t *ctx)
{
    if (src == NULL || dst == NULL || dst_len == NULL || ctx == NULL)
        return BP_ERR_INVALID_ARG;
    if (src_len < FLUFF_HDR_SIZE) return BP_ERR_CORRUPT;

    if (src[2] == FLUFF_STREAM_VERSION_FSE_RESET ||
        src[2] == FLUFF_STREAM_VERSION_FSE2_RESET ||
        src[2] == FLUFF_STREAM_VERSION_FSE3_RESET) {
        /* Context-independent block: reset ring buffer before decoding.
         * Use the lightweight decode-only reset to avoid the 1.75 MB hash
         * memset which is irrelevant during decompression. */
        fluff_ctx_reset_decode(ctx);
        return fl_decompress_fse(src, src_len, dst, dst_len, ctx);
    }
    if (src[2] == FLUFF_STREAM_VERSION_FSE ||
        src[2] == FLUFF_STREAM_VERSION_FSE2 ||
        src[2] == FLUFF_STREAM_VERSION_FSE3)
        return fl_decompress_fse(src, src_len, dst, dst_len, ctx);
    return fl_decompress_rc(src, src_len, dst, dst_len, ctx);
}

/*
 * Wrapper around fluff_decompress_block_ctx that provides tracing.
 */
int fluff_decompress_block_ctx_trace(const bp_uint8 *src, bp_uint32 src_len,
                                      bp_uint8 *dst, bp_uint32 *dst_len,
                                      fluff_ctx_t *ctx,
                                      fluff_trace_t *trace_ctx)
{
    /* For now, just delegate to the base function. */
    (void)trace_ctx;  /* parameter passed but not yet used */
    return fluff_decompress_block_ctx(src, src_len, dst, dst_len, ctx);
}

/*
 * Wrapper around fluff_compress_block_ctx that provides tracing.
 */
int fluff_compress_block_ctx_trace(const bp_uint8 *src, bp_uint32 src_len,
                                    bp_uint8 *dst, bp_uint32 *dst_len,
                                    fluff_ctx_t *ctx,
                                    fluff_trace_t *trace_ctx)
{
    (void)trace_ctx;
    return fluff_compress_block_ctx(src, src_len, dst, dst_len, ctx);
}

/* ─── Public helpers ────────────────────────────────────────────────────── */

int fluff_init(void)
{
    fl_build_luts();
#ifdef BOLTPACK_FAST_FLUFF
    fluff_fast_init_matcher();
#endif
    return BP_OK;
}

bp_uint32 fluff_compress_bound(bp_uint32 src_len)
{
    /*
     * v0x07 FSE block: 694-byte fixed header + two tANS bitstreams whose
     * combined size is ~1.0 bytes/symbol even on incompressible input (the
     * entropy coder cannot meaningfully expand random data).  A 12.5 %
     * margin plus a 1 KB fixed slack comfortably covers tANS rounding, the
     * fixed header, and tiny blocks.
     *
     * This value sizes both the caller's dst buffer and the internal
     * fse_buf/raw_buf scratch.  It is NOT a hard safety limit: the bit
     * writers (fse_bits_write) drop bytes past their cap while still
     * counting the true length, and the assembly path verifies
     * "bound >= 694 + fse_len + raw_len" before the final memcpy.  If the
     * bound is ever too small, compression returns BP_ERR_COMPRESS and the
     * caller stores the block raw — never an overflow.
     */
    return src_len + (src_len >> 3) + 1024U;
}

bp_uint32 fluff_compress_work_size(void)
{
    return 0; /* allocated internally */
}
