/* Low-memory C89 BoltLite encoder/decoder for BoltPack method 1. */

#ifdef __PLAN9__
#include <u.h>
#include <libc.h>
#endif

#include "boltlite.h"
#include "archiver.h"
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

static unsigned int boltlite_hash3(const bp_uint8 *src, boltlite_index src_len,
                               boltlite_index pos)
{
    unsigned int h;
    unsigned int a;
    unsigned int b;
    unsigned int c;
    a = (pos < src_len) ? src[pos] : 0x20U;
    b = (pos + 1U < src_len) ? src[pos + 1U] : 0x20U;
    c = (pos + 2U < src_len) ? src[pos + 2U] : 0x20U;
    h = a;
    h = (h * 251U + b) & (BOLTLITE_HASH_BUCKETS - 1U);
    h = (h * 251U + c) & (BOLTLITE_HASH_BUCKETS - 1U);
    return h;
}

static void boltlite_insert(const bp_uint8 *src, boltlite_index src_len, boltlite_index pos,
                        unsigned short *heads, unsigned short *prev,
                        unsigned short *next)
{
    unsigned int bucket;
    unsigned int slot;
    unsigned int old_bucket;
    unsigned short before;
    unsigned short after;

    slot = (unsigned int)(pos & BOLTLITE_WINDOW_MASK);
    if (pos >= BOLTLITE_WINDOW_SIZE) {
        old_bucket = boltlite_hash3(src, src_len, pos - BOLTLITE_WINDOW_SIZE);
        before = prev[slot];
        after = next[slot];
        if (before == (unsigned short)BOLTLITE_NIL)
            heads[old_bucket] = after;
        else
            next[before] = after;
        if (after != (unsigned short)BOLTLITE_NIL)
            prev[after] = before;
    }

    bucket = boltlite_hash3(src, src_len, pos);
    before = heads[bucket];
    prev[slot] = (unsigned short)BOLTLITE_NIL;
    next[slot] = before;
    if (before != (unsigned short)BOLTLITE_NIL) prev[before] = (unsigned short)slot;
    heads[bucket] = (unsigned short)slot;
}

static void boltlite_find(const bp_uint8 *src, boltlite_index src_len, boltlite_index pos,
                      unsigned short *heads, unsigned short *next,
                      boltlite_index produced,
                      boltlite_index previous_distance,
                      boltlite_index *best_distance, boltlite_index *best_length)
{
    unsigned int bucket;
    unsigned int searched;
    unsigned short candidate;
    boltlite_index distance;
    boltlite_index limit;
    boltlite_index length;

    *best_distance = 0;
    *best_length = 0;
    limit = src_len - pos;
    if (limit > BOLTLITE_MAX_MATCH) limit = BOLTLITE_MAX_MATCH;

    if (previous_distance != 0 && previous_distance <= produced) {
        length = 0;
        while (length < limit &&
               src[pos + length] == src[pos - previous_distance + length])
            length++;
        if (length >= BOLTLITE_MIN_MATCH) {
            *best_distance = previous_distance;
            *best_length = length;
        }
    }

    if (limit < BOLTLITE_MIN_MATCH) return;
    bucket = boltlite_hash3(src, src_len, pos);
    candidate = heads[bucket];
    searched = 0;
    while (candidate != (unsigned short)BOLTLITE_NIL &&
           searched < BOLTLITE_SEARCH_LIMIT) {
        searched++;
        distance = (pos - (boltlite_index)candidate) & BOLTLITE_WINDOW_MASK;
        if (distance == 0 && produced >= BOLTLITE_WINDOW_SIZE)
            distance = BOLTLITE_WINDOW_SIZE;
        if (distance == 0 || distance > produced || distance > BOLTLITE_WINDOW_SIZE)
            candidate = next[candidate];
        else {
            if (distance != *best_distance) {
                length = 0;
                while (length < limit &&
                       src[pos + length] == src[pos - distance + length])
                    length++;
                if (length > *best_length) {
                    *best_distance = distance;
                    *best_length = length;
                    if (length == limit) return;
                }
            }
            candidate = next[candidate];
        }
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
    unsigned int code_pos;
    unsigned int mask;
    unsigned int flags;
    unsigned int i;
    unsigned int d;
    unsigned int packed;
    int have_match;

    if (src == NULL || dst == NULL || work == NULL || src_len == 0) return 0;
    if (dst_capacity < BOLTLITE_COMPRESS_BOUND(src_len)) return 0;

    heads = (unsigned short *)(void *)work;
    prev = (unsigned short *)(void *)(work + BOLTLITE_HASH_BUCKETS * 2U);
    next = (unsigned short *)(void *)(work + BOLTLITE_HASH_BUCKETS * 2U +
                                      BOLTLITE_WINDOW_SIZE * 2U);
    for (i = 0; i < BOLTLITE_HASH_BUCKETS; i++)
        heads[i] = (unsigned short)BOLTLITE_NIL;
    for (i = 0; i < BOLTLITE_WINDOW_SIZE; i++) {
        prev[i] = (unsigned short)BOLTLITE_NIL;
        next[i] = (unsigned short)BOLTLITE_NIL;
    }

    pos = 0;
    out = 0;
    previous_distance = 0;
    code[0] = 0;
    code_pos = 1;
    mask = 1;
    flags = 0;
    have_match = 0;
    next_distance = 0;
    next_length = 0;

    while (pos < src_len) {
        if (have_match) {
            distance = next_distance;
            length = next_length;
            have_match = 0;
        } else {
            boltlite_find(src, src_len, pos, heads, next, pos, previous_distance,
                      &distance, &length);
        }

        if (length >= BOLTLITE_MIN_MATCH) {
            /* Insert this position before checking whether one-byte lazy
             * parsing produces a longer next-position match. */
            boltlite_insert(src, src_len, pos, heads, prev, next);
            if (pos + 1U < src_len) {
                boltlite_find(src, src_len, pos + 1U, heads, next, pos + 1U,
                          previous_distance, &next_distance, &next_length);
                if (next_length > length) {
                    flags |= mask;
                    code[code_pos++] = src[pos++];
                    have_match = 1;
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
                    continue;
                }
            }

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
            for (i = 1; i < consumed; i++)
                boltlite_insert(src, src_len, pos + i, heads, prev, next);
            pos += consumed;
        } else {
            flags |= mask;
            code[code_pos++] = src[pos];
            boltlite_insert(src, src_len, pos, heads, prev, next);
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

int boltlite_decompress(const bp_uint8 *src, bp_uint32 src_len,
                    bp_uint8 *dst, bp_uint32 dst_len, bp_uint8 *work)
{
    (void)work;
#if defined(BP_MINI) && defined(SIZEOF_INT) && (SIZEOF_INT < 4)
    if (src_len > BOLTLITE_COMPRESS_BOUND(BP_MINI_MAX_BLOCK_SIZE) ||
        dst_len > BP_MINI_MAX_BLOCK_SIZE)
        return BP_ERR_CORRUPT;
#endif
    return boltlite_decompress_capacity(src, (boltlite_index)src_len, dst,
                                    (boltlite_index)dst_len)
           ? BP_OK : BP_ERR_CORRUPT;
}
