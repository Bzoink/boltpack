/* Low-memory C89 BoltLite encoder/decoder for BoltPack method 1. */

#ifdef __PLAN9__
#include <u.h>
#include <libc.h>
#endif

#include "boltlite.h"
#include "archiver.h"
#include "mini/stack_probe.h"
#if !defined(__PLAN9__)
#include <string.h>
#endif

#define BOLTLITE_WINDOW_MASK (BOLTLITE_WINDOW_SIZE - 1U)
#define BOLTLITE_NIL  0xFFFFU
#if defined(BP_MINI) && defined(SIZEOF_INT) && (SIZEOF_INT < 4)
/* Mini blocks are capped at 32 KiB, so native 16-bit arithmetic is enough
 * for positions, lengths, and distances on DOS/OS/2 16-bit targets. */
typedef unsigned int boltlite_index;
#else
typedef bp_uint32 boltlite_index;
#endif
#ifndef BOLTLITE_SEARCH_LIMIT
#define BOLTLITE_SEARCH_LIMIT 64U
#endif

#if defined(BOLTLITE_8086_ASM)
#if !defined(__WATCOMC__) || !defined(__I86__) || \
    (!defined(__COMPACT__) && !defined(__LARGE__)) || \
    !defined(BP_MINI) || BOLTLITE_HASH_BUCKETS != 1024U
#error BOLTLITE_8086_ASM requires an Open Watcom 16-bit compact or large mini build
#endif

/* Compute the same hash with 251 = 256 - 5 to avoid 8086 multiplies.
 * The intermediate mask is redundant because the final modulus divides 65536. */
static unsigned int boltlite_hash3_8086(const bp_uint8 *src);
#pragma aux boltlite_hash3_8086 = \
    "xor ax,ax"     "mov al,es:[bx]" \
    "mov ch,al"     "xor cl,cl" \
    "mov dx,ax"     "shl ax,1"       "shl ax,1"  "add ax,dx" \
    "sub cx,ax" \
    "xor ax,ax"     "mov al,es:1[bx]" "add cx,ax" \
    "mov dh,cl"     "xor dl,dl" \
    "mov ax,cx"     "shl cx,1"       "shl cx,1"  "add cx,ax" \
    "sub dx,cx" \
    "xor ax,ax"     "mov al,es:2[bx]" "add ax,dx" "and ax,1023" \
    __parm [__es __bx] __value [__ax] __modify __exact [__ax __cx __dx];

/* Both spans are in the same source buffer and contain at least limit
 * bytes. CMPSB advances past a mismatch, so subtract that last byte. */
static boltlite_index boltlite_match_8086(const bp_uint8 *pos,
                                         boltlite_index distance,
                                         boltlite_index limit);
#pragma aux boltlite_match_8086 = \
    "push ds"       "push es"        "pop ds" \
    "mov si,di"     "sub si,bx"      "mov ax,cx" \
    "cld"           "repe cmpsb"     "je matched" \
    "dec ax"        "matched:"       "sub ax,cx" \
    "pop ds" \
    __parm [__es __di] [__bx] [__cx] __value [__ax] \
    __modify __exact [__ax __cx __si __di];

extern int __cdecl boltlite_decode_8086(const bp_uint8 *src, unsigned int src_len,
                                        bp_uint8 *dst, unsigned int dst_len);
#endif

static unsigned int boltlite_hash3(const bp_uint8 *src, boltlite_index src_len,
                               boltlite_index pos)
{
    unsigned int h;
    unsigned int a;
    unsigned int b;
    unsigned int c;
#if defined(BOLTLITE_8086_ASM)
    if (src_len - pos >= 3U) return boltlite_hash3_8086(src + pos);
#endif
    a = (pos < src_len) ? src[pos] : 0x20U;
    b = (pos + 1U < src_len) ? src[pos + 1U] : 0x20U;
    c = (pos + 2U < src_len) ? src[pos + 2U] : 0x20U;
    h = a;
    h = (h * 251U + b) & (BOLTLITE_HASH_BUCKETS - 1U);
    h = (h * 251U + c) & (BOLTLITE_HASH_BUCKETS - 1U);
    return h;
}

static void boltlite_insert(const bp_uint8 *src, boltlite_index src_len, boltlite_index pos,
                        unsigned int bucket,
                        unsigned short *heads, unsigned short *prev,
                        unsigned short *next)
{
    unsigned int slot;
    unsigned short before;

    slot = (unsigned int)(pos & BOLTLITE_WINDOW_MASK);
    if (pos >= BOLTLITE_WINDOW_SIZE) {
        /* Positions enter in order, so the oldest slot ends its chain.
         * Recover its hash only if it is also the chain's head. */
        before = prev[slot];
        if (before == (unsigned short)BOLTLITE_NIL) {
            unsigned int old_bucket = boltlite_hash3(src, src_len,
                                                    pos - BOLTLITE_WINDOW_SIZE);
            heads[old_bucket] = (unsigned short)BOLTLITE_NIL;
        } else {
            next[before] = (unsigned short)BOLTLITE_NIL;
        }
    }

    before = heads[bucket];
    prev[slot] = (unsigned short)BOLTLITE_NIL;
    next[slot] = before;
    if (before != (unsigned short)BOLTLITE_NIL) prev[before] = (unsigned short)slot;
    heads[bucket] = (unsigned short)slot;
}

static void boltlite_find(const bp_uint8 *src, boltlite_index src_len, boltlite_index pos,
                      unsigned short *heads, unsigned short *next, unsigned int bucket,
                      boltlite_index previous_distance,
                      boltlite_index *best_distance, boltlite_index *best_length)
{
    unsigned int searched;
    unsigned short candidate;
    boltlite_index distance;
    boltlite_index limit;
    boltlite_index length;
#ifdef BP_TEST_STACK_HIGHWATER
    static int stack_probed;
    if (!stack_probed) {
        BP_STACK_PROBE();
        stack_probed = 1;
    }
#endif

    *best_distance = 0;
    *best_length = 0;
    limit = src_len - pos;
    if (limit < BOLTLITE_MIN_MATCH) return;
    if (limit > BOLTLITE_MAX_MATCH) limit = BOLTLITE_MAX_MATCH;

    if (previous_distance != 0) {
#if defined(BOLTLITE_8086_ASM)
        length = boltlite_match_8086(src + pos, previous_distance, limit);
#else
        length = 0;
        while (length < limit &&
               src[pos + length] == src[pos - previous_distance + length])
            length++;
#endif
        if (length >= BOLTLITE_MIN_MATCH) {
            *best_distance = previous_distance;
            *best_length = length;
            if (length == limit) return;
        }
    }

    candidate = heads[bucket];
    searched = 0;
    while (candidate != (unsigned short)BOLTLITE_NIL &&
           searched < BOLTLITE_SEARCH_LIMIT) {
        searched++;
        /* Only earlier positions in the window are linked. A slot equal
         * to the current slot is exactly one window back. */
        distance = (pos - (boltlite_index)candidate) & BOLTLITE_WINDOW_MASK;
        if (distance == 0) distance = BOLTLITE_WINDOW_SIZE;
        if (distance != *best_distance) {
#if defined(BOLTLITE_8086_ASM)
            length = boltlite_match_8086(src + pos, distance, limit);
#else
            length = 0;
            while (length < limit &&
                   src[pos + length] == src[pos - distance + length])
                length++;
#endif
            if (length > *best_length) {
                *best_distance = distance;
                *best_length = length;
                if (length == limit) return;
            }
        }
        candidate = next[candidate];
    }
}

/*
 * Token groups use one flag byte per eight symbols, like the existing codec.
 * Match symbols have three encodings:
 *   00xxxxxx: distance 1..8 and length 3..10, packed into one byte.
 *   01xxxxxx: previous distance and length 3..66, packed into one byte.
 *   1xxxxxxx: followed by a byte (12-bit distance, 3-bit length 3..10).
 */
static bp_uint32 boltlite_compress_capacity(const bp_uint8 *src, boltlite_index src_len,
                             bp_uint8 *dst, bp_uint32 dst_capacity,
                             bp_uint8 *work)
{
    unsigned short *heads;
    unsigned short *prev;
    unsigned short *next;
    bp_uint8 code[17]; /* one flag + 8 worst-case two-byte matches */
    boltlite_index pos;
    boltlite_index out;
    boltlite_index distance;
    boltlite_index length;
    boltlite_index next_distance;
    boltlite_index next_length;
    boltlite_index previous_distance;
    boltlite_index consumed;
    unsigned int bucket;
    unsigned int next_bucket;
    unsigned int code_pos;
    unsigned int mask;
    unsigned int flags;
    unsigned int i;
    unsigned int d;
    unsigned int packed;
    int have_match;

    BP_STACK_PROBE();

    if (src == NULL || dst == NULL || work == NULL || src_len == 0) return 0;
    if (dst_capacity < BOLTLITE_COMPRESS_BOUND(src_len)) return 0;

    heads = (unsigned short *)(void *)work;
    prev = (unsigned short *)(void *)(work + BOLTLITE_HASH_BUCKETS * 2U);
    next = (unsigned short *)(void *)(work + BOLTLITE_HASH_BUCKETS * 2U +
                                      BOLTLITE_WINDOW_SIZE * 2U);
    /* A slot's links are initialized when it enters a chain. */
    memset(heads, 0xFF, BOLTLITE_HASH_BUCKETS * sizeof(*heads));

    pos = 0;
    out = 0;
    previous_distance = 0;
    code[0] = 0;
    code_pos = 1;
    mask = 1;
    flags = 0;
    have_match = 0;
    next_bucket = 0;
    next_distance = 0;
    next_length = 0;

    while (pos < src_len) {
        if (have_match) {
            bucket = next_bucket;
            distance = next_distance;
            length = next_length;
            have_match = 0;
        } else {
            bucket = boltlite_hash3(src, src_len, pos);
            boltlite_find(src, src_len, pos, heads, next, bucket, previous_distance,
                      &distance, &length);
        }

        boltlite_insert(src, src_len, pos, bucket, heads, prev, next);
        if (length >= BOLTLITE_MIN_MATCH) {
            /* Reuse the lookahead hash in the next search or insertion. */
            next_bucket = boltlite_hash3(src, src_len, pos + 1U);
            boltlite_find(src, src_len, pos + 1U, heads, next, next_bucket,
                          previous_distance, &next_distance, &next_length);
            if (next_length > length) {
                have_match = 1;
                length = 0;
            }
        }

        if (length >= BOLTLITE_MIN_MATCH) {
            if (distance != previous_distance && length > 10U)
                length = 10U; /* explicit-distance form carries three length bits */
            if (distance <= 8U && length <= 10U) {
                d = (unsigned int)(distance - 1U);
                code[code_pos++] = (bp_uint8)((d << 3) | (unsigned int)(length - 3U));
            } else if (distance == previous_distance && length <= 66U) {
                code[code_pos++] = (bp_uint8)(0x40U | (unsigned int)(length - 3U));
            } else {
                packed = (unsigned int)(((distance - 1U) << 3) | (length - 3U));
                code[code_pos++] = (bp_uint8)(0x80U | (packed >> 8));
                code[code_pos++] = (bp_uint8)(packed & 0xFFU);
            }
            previous_distance = distance;
            consumed = length;
            boltlite_insert(src, src_len, pos + 1U, next_bucket, heads, prev, next);
            for (i = 2; i < consumed; i++) {
                bucket = boltlite_hash3(src, src_len, pos + i);
                boltlite_insert(src, src_len, pos + i, bucket, heads, prev, next);
            }
            pos += consumed;
        } else {
            flags |= mask;
            code[code_pos++] = src[pos];
            pos++;
        }

        mask <<= 1;
        if (mask == 0x100U) {
            code[0] = (bp_uint8)flags;
            if (out + code_pos > dst_capacity) return 0;
            memcpy(dst + out, code, code_pos);
            out += code_pos;
            code[0] = 0;
            code_pos = 1;
            mask = 1;
            flags = 0;
        }
    }

    if (code_pos > 1U) {
        code[0] = (bp_uint8)flags;
        if (out + code_pos > dst_capacity) return 0;
        memcpy(dst + out, code, code_pos);
        out += code_pos;
    }
    return out;
}

bp_uint32 boltlite_compress(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint8 *work)
{
    bp_uint32 capacity;
#if defined(BP_MINI) && defined(SIZEOF_INT) && (SIZEOF_INT < 4)
    if (src_len > BP_MINI_MAX_BLOCK_SIZE) return 0;
#endif
    capacity = BOLTLITE_COMPRESS_BOUND(src_len);
    return boltlite_compress_capacity(src, (boltlite_index)src_len, dst, capacity, work);
}

#if !defined(BOLTLITE_8086_ASM)
static int boltlite_decompress_capacity(const bp_uint8 *src, boltlite_index src_len,
                         bp_uint8 *dst, boltlite_index dst_capacity)
{
    boltlite_index in;
    boltlite_index out;
    boltlite_index distance;
    boltlite_index length;
    boltlite_index j;
    unsigned int packed;
    unsigned int flags;
    unsigned int count;
    unsigned int token;
    unsigned int b0;
    unsigned int b1;
    unsigned int mask;

    BP_STACK_PROBE();

    if (src == NULL || dst == NULL || src_len == 0 || dst_capacity == 0) return 0;
    in = 0;
    out = 0;
    distance = 0;
    flags = 0;
    count = 0;
    while (out < dst_capacity) {
        if (count == 0) {
            if (in >= src_len) return 0;
            flags = src[in++];
            count = 8;
        }
        mask = flags & 1U;
        flags >>= 1;
        count--;
        if (mask != 0) {
            if (in >= src_len) return 0;
            dst[out++] = src[in++];
            continue;
        }
        if (in >= src_len) return 0;
        token = src[in++];
        if ((token & 0xC0U) == 0) {
            distance = ((token >> 3) & 7U) + 1U;
            length = (token & 7U) + 3U;
        } else if ((token & 0xC0U) == 0x40U) {
            if (distance == 0) return 0;
            length = (token & 0x3FU) + 3U;
        } else {
            if (in >= src_len) return 0;
            b0 = token & 0x7FU;
            b1 = src[in++];
            packed = (b0 << 8) | b1;
            distance = ((packed >> 3) & 0x0FFFU) + 1U;
            length = (packed & 7U) + 3U;
        }
        if (distance == 0 || distance > out || length > dst_capacity - out) return 0;
        for (j = 0; j < length; j++) {
            dst[out] = dst[out - distance];
            out++;
        }
    }
    return in == src_len;
}
#endif

int boltlite_decompress(const bp_uint8 *src, bp_uint32 src_len,
                    bp_uint8 *dst, bp_uint32 dst_len, bp_uint8 *work)
{
    int ok;
    (void)work;
#if defined(BP_MINI) && defined(SIZEOF_INT) && (SIZEOF_INT < 4)
    if (src_len > BOLTLITE_COMPRESS_BOUND(BP_MINI_MAX_BLOCK_SIZE) ||
        dst_len > BP_MINI_MAX_BLOCK_SIZE)
        return BP_ERR_CORRUPT;
#endif
#if defined(BOLTLITE_8086_ASM)
    BP_STACK_PROBE();
    if (src == NULL || dst == NULL || src_len == 0 || dst_len == 0)
        return BP_ERR_CORRUPT;
    ok = boltlite_decode_8086(src, (boltlite_index)src_len, dst, (boltlite_index)dst_len);
#else
    ok = boltlite_decompress_capacity(src, (boltlite_index)src_len, dst,
                                      (boltlite_index)dst_len);
#endif
    return ok ? BP_OK : BP_ERR_CORRUPT;
}
