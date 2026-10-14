/* Frozen code-review baseline; do not change. BSD 2-Clause License. */
/*
 * boltlz.c -- experimental byte-oriented compression codec
 *
 * ANSI C (C89/C90) only.
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 *
 * Original implementation of byte-oriented LZ77. Design references:
 * LZ4/Snappy literal and match tags, S2/MinLZ repeated distances.
 * No implementation code is copied from those projects.
 */
#include "boltlz_review_base.h"
#include "boltlz_review_base_config.h"
#include <string.h>

#define BL_HASH_BITS RB_BOLTLZ_HASH_BITS
#define BL_HASH_SIZE (1UL << BL_HASH_BITS)

#if defined(__GNUC__) || defined(__clang__)
#define BL_FORCE_INLINE static __inline__ __attribute__((always_inline))
#else
#define BL_FORCE_INLINE static
#endif

/* Share the current prefix between lookup and insertion in this experiment.
 * Disabled builds retain the original lookup interfaces. */
#if RB_BOLTLZ_HASH_CACHE
#define BL_PREFIX_PARAMS , bp_uint32 value, bp_uint32 hash
#define BL_PREFIX_ARGS , value, hash
#define BL_NEXT_PREFIX_ARGS , next_value, next_hash
#else
#define BL_PREFIX_PARAMS
#define BL_PREFIX_ARGS
#define BL_NEXT_PREFIX_ARGS
#endif

typedef struct {
    bp_uint32 length;
    bp_uint32 distance;
    bp_uint32 saving;
} bl_match;

static bp_uint32 bl_load32(const bp_uint8 *p)
{
    bp_uint32 value;
    memcpy(&value, p, 4);
    return value;
}

static bp_uint32 bl_hash(const bp_uint8 *p)
{
    return (bl_load32(p) * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
}

#if RB_BOLTLZ_DUAL_HASH
static bp_uint32 bl_hash6(const bp_uint8 *p)
{
    bp_uint32 suffix, value;
    suffix = p[4] | ((bp_uint32)p[5] << 8);
    value = bl_load32(p) ^ (suffix * (bp_uint32)2654435761UL);
    return (value * (bp_uint32)2246822519UL) >> (32 - BL_HASH_BITS);
}
#endif

bp_uint32 boltlz_review_reference_work_size(int level)
{
    if (level != RB_BOLTLZ_FAST && level != RB_BOLTLZ_BALANCED) return 0;
    return BL_HASH_SIZE * (bp_uint32)sizeof(bp_uint32) * (bp_uint32)level;
}

bp_uint32 boltlz_review_reference_compress_bound(bp_uint32 src_len)
{
    if (src_len > RB_BOLTLZ_MAX_BLOCK) return 0;
    return src_len + src_len / 63 + 16;
}

static bp_uint32 bl_var_size(bp_uint32 value)
{
#if RB_BOLTLZ_CHEAP_COST
    /* All encoder fields are bounded by the 256 KiB block limit. */
    return 1 + (value >= 128) + (value >= 16384);
#else
    bp_uint32 size;
    size = 1;
    while (value >= 128) { value >>= 7; size++; }
    return size;
#endif
}

static void bl_write_var(bp_uint8 *dst, bp_uint32 *pos, bp_uint32 value)
{
    while (value >= 128) {
        dst[(*pos)++] = (bp_uint8)((value & 127) | 128);
        value >>= 7;
    }
    dst[(*pos)++] = (bp_uint8)value;
}

static bp_uint32 bl_match_cost(bp_uint32 length, bp_uint32 distance,
                             bp_uint32 last_distance)
{
    if (length < 4) return RB_BOLTLZ_MAX_BLOCK;
    return 1 + (distance == last_distance ? 0 : bl_var_size(distance)) +
           (length >= 19 ? bl_var_size(length - 19) : 0);
}

#if RB_BOLTLZ_EXACT_COST
static bp_uint32 bl_literal_cost(bp_uint32 length)
{
    if (length == 0) return 0;
    return length + 1 + (length >= 7 ? bl_var_size(length - 7) : 0);
}

static bp_uint32 bl_sequence_cost(bp_uint32 literals, bl_match match, bp_uint32 last)
{
    return literals + bl_match_cost(match.length, match.distance, last) +
           (literals >= 7 ? bl_var_size(literals - 7) : 0);
}
#endif

static bp_uint32 bl_count(const bp_uint8 *src, bp_uint32 size,
                         bp_uint32 pos, bp_uint32 ref)
{
    bp_uint32 length;
#if RB_BOLTLZ_WORD_MATCH && (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    unsigned long current, previous, difference;
#endif
    length = 4;
#if RB_BOLTLZ_WORD_MATCH && (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    /* memcpy permits unaligned input and avoids aliasing assumptions. Never
     * load past the input. Nonzero XOR identifies the first different byte. */
    while (size - pos - length >= sizeof(unsigned long)) {
        memcpy(&current, src + pos + length, sizeof(current));
        memcpy(&previous, src + ref + length, sizeof(previous));
        difference = current ^ previous;
        if (difference != 0) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return length + (bp_uint32)(__builtin_ctzl(difference) / CHAR_BIT);
#else
            return length + (bp_uint32)(__builtin_clzl(difference) / CHAR_BIT);
#endif
        }
        length += (bp_uint32)sizeof(unsigned long);
    }
#endif
#if RB_BOLTLZ_WIDE_MATCH
    /* Exact eight-byte comparisons are valid without padding or native
     * 64-bit types; a modern compiler can lower this to a word comparison. */
    while (size - pos - length >= 8 &&
           memcmp(src + pos + length, src + ref + length, 8) == 0)
        length += 8;
#endif
    while (size - pos - length >= 4 &&
           bl_load32(src + pos + length) == bl_load32(src + ref + length))
        length += 4;
    while (length < size - pos && src[pos + length] == src[ref + length])
        length++;
    return length;
}

#if RB_BOLTLZ_INLINE_FIND
BL_FORCE_INLINE
#else
static
#endif
bl_match bl_find(const bp_uint8 *src, bp_uint32 size, bp_uint32 pos,
                        bp_uint32 last, const bp_uint32 *table, int level,
                        bp_uint32 known_repeat_length BL_PREFIX_PARAMS)
{
    bl_match best;
    bp_uint32 entries[3], count, i, ref, distance, length, cost;
#if !RB_BOLTLZ_HASH_CACHE
    bp_uint32 hash, value;
#endif
#if RB_BOLTLZ_EXACT_COST
    bp_uint32 horizon, best_cost, candidate_cost;
#endif
    best.length = best.distance = best.saving = 0;
#if RB_BOLTLZ_REUSE_MATCH
    if (known_repeat_length >= 4) {
        best.length = known_repeat_length;
        best.distance = last;
        best.saving = known_repeat_length - bl_match_cost(known_repeat_length, last, last);
        if (best.length == size - pos) return best;
    }
#else
    (void)known_repeat_length;
#endif
#if !RB_BOLTLZ_HASH_CACHE
    value = bl_load32(src + pos);
    hash = (value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
#endif
    entries[0] = last != 0 && last <= pos ? pos - last + 1 : 0;
    entries[1] = table[hash];
    count = 2;
    if (level == RB_BOLTLZ_BALANCED) {
#if RB_BOLTLZ_DUAL_HASH
        entries[2] = size - pos >= 6 ? table[BL_HASH_SIZE + bl_hash6(src + pos)] : 0;
#else
        entries[2] = table[BL_HASH_SIZE + hash];
#endif
        count = 3;
    }
    for (i = 0; i < count; i++) {
#if RB_BOLTLZ_REUSE_MATCH
        if (i == 0 && known_repeat_length >= 4) continue;
#endif
#if RB_BOLTLZ_SECOND_LIMIT == 0
        if (i == 2) continue;
#else
        if (i == 2 && best.length >= RB_BOLTLZ_SECOND_LIMIT) continue;
#endif
        if (entries[i] == 0 || (i != 0 && entries[i] == entries[0])) continue;
        ref = entries[i] - 1;
        if (ref >= pos) continue;
        distance = pos - ref;
        if (value != bl_load32(src + ref)) continue;
#if RB_BOLTLZ_PRUNE_MATCH && !RB_BOLTLZ_EXACT_COST
        /* A mismatch here bounds length <= best.saving. Every match costs
         * at least one byte, so this candidate cannot tie or beat the best.
         * best.saving < best.length <= size-pos also proves the read bound. */
        if (best.saving >= 4 && src[pos + best.saving] != src[ref + best.saving])
            continue;
#endif
        length = bl_count(src, size, pos, ref);
        cost = bl_match_cost(length, distance, last);
        if (length <= cost) continue;
#if RB_BOLTLZ_EXACT_COST
        horizon = length > best.length ? length : best.length;
        candidate_cost = cost + bl_literal_cost(horizon - length);
        best_cost = best.length == 0 ? bl_literal_cost(horizon) :
                    bl_match_cost(best.length, best.distance, last) +
                    bl_literal_cost(horizon - best.length);
        if (candidate_cost < best_cost ||
            (candidate_cost == best_cost && length > best.length)) {
#else
        if (length - cost > best.saving ||
            (length - cost == best.saving && length > best.length)) {
#endif
            best.length = length;
            best.distance = distance;
            best.saving = length - cost;
        }
        if (best.length == size - pos ||
            (level == RB_BOLTLZ_FAST && best.length >= 64)) break;
    }
    return best;
}

/* One hash candidate in fast mode. Repeated distances are still encoded
 * cheaply when selected; their extra search is reserved for balanced mode. */
static bl_match bl_fast_find(const bp_uint8 *src, bp_uint32 size, bp_uint32 pos,
                             bp_uint32 last, const bp_uint32 *table BL_PREFIX_PARAMS)
{
    bl_match match;
    bp_uint32 entry, ref, length, distance, cost;
#if !RB_BOLTLZ_HASH_CACHE
    bp_uint32 value, hash;
#endif
    match.length = match.distance = match.saving = 0;
#if !RB_BOLTLZ_HASH_CACHE
    value = bl_load32(src + pos);
    hash = (value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
#endif
    entry = table[hash];
    if (entry == 0 || entry - 1 >= pos) return match;
    ref = entry - 1;
    distance = pos - ref;
    if (value != bl_load32(src + ref)) return match;
    length = bl_count(src, size, pos, ref);
    cost = bl_match_cost(length, distance, last);
    if (length > cost) {
        match.length = length;
        match.distance = distance;
        match.saving = length - cost;
    }
    return match;
}

static void bl_insert_hash(const bp_uint8 *src, bp_uint32 size, bp_uint32 pos,
                            bp_uint32 *table, int level, bp_uint32 hash)
{
    if (level == RB_BOLTLZ_BALANCED) {
#if RB_BOLTLZ_DUAL_HASH
        if (size - pos >= 6)
            table[BL_HASH_SIZE + bl_hash6(src + pos)] = pos + 1;
#else
        table[BL_HASH_SIZE + hash] = table[hash];
#endif
    }
#if !RB_BOLTLZ_DUAL_HASH
    (void)src;
    (void)size;
#endif
    table[hash] = pos + 1;
}

static void bl_insert(const bp_uint8 *src, bp_uint32 size, bp_uint32 pos,
                       bp_uint32 *table, int level)
{
    bl_insert_hash(src, size, pos, table, level, bl_hash(src + pos));
}

static int bl_literals(const bp_uint8 *src, bp_uint32 length,
                        bp_uint8 *dst, bp_uint32 cap, bp_uint32 *out)
{
    bp_uint32 header;
    if (length == 0) return BP_OK;
    header = 1 + (length >= 7 ? bl_var_size(length - 7) : 0);
    if (header > cap - *out || length > cap - *out - header)
        return BP_ERR_OVERFLOW;
    if (length < 7) dst[(*out)++] = (bp_uint8)(length << 4);
    else {
        dst[(*out)++] = 112;
        bl_write_var(dst, out, length - 7);
    }
    memcpy(dst + *out, src, (size_t)length);
    *out += length;
    return BP_OK;
}

#if RB_BOLTLZ_SPECIALIZE
BL_FORCE_INLINE int bl_compress_impl(const bp_uint8 *src, bp_uint32 src_len,
#else
int boltlz_review_reference_compress(const bp_uint8 *src, bp_uint32 src_len,
#endif
                   bp_uint8 *dst, bp_uint32 *dst_len, void *work, int level)
{
    bp_uint32 cap, out, pos, anchor, last, misses, step, insert_pos;
    bp_uint32 literal_length, needed, tag;
    bp_uint32 *table;
    bl_match match;
#if RB_BOLTLZ_HASH_CACHE
    bp_uint32 value, hash;
#if RB_BOLTLZ_LAZY_LIMIT > 0
    bp_uint32 next_value, next_hash;
#endif
#endif
#if RB_BOLTLZ_LAZY_LIMIT > 0
    bl_match next;
    bp_uint32 known_repeat_length;
#if RB_BOLTLZ_EXACT_COST
    bp_uint32 horizon, current_cost, next_cost;
#endif
#endif
    int rc;
    if (dst_len == NULL) return BP_ERR_INVALID_ARG;
    cap = *dst_len;
    *dst_len = 0;
    if (dst == NULL || (src == NULL && src_len != 0) || work == NULL ||
        src_len > RB_BOLTLZ_MAX_BLOCK || boltlz_review_reference_work_size(level) == 0 ||
        sizeof(bp_uint32) != 4 || (bp_uint32)(size_t)src_len != src_len ||
        (bp_uint32)(size_t)boltlz_review_reference_work_size(level) != boltlz_review_reference_work_size(level))
        return BP_ERR_INVALID_ARG;
    if (cap == 0) return BP_ERR_OVERFLOW;
    dst[0] = RB_BOLTLZ_FORMAT_VERSION;
    out = 1;
    if (src_len == 0) { *dst_len = out; return BP_OK; }
    table = (bp_uint32 *)work;
    memset(table, 0, (size_t)boltlz_review_reference_work_size(level));
    pos = anchor = last = misses = 0;
    while (src_len - pos >= 4) {
#if RB_BOLTLZ_HASH_CACHE
        value = bl_load32(src + pos);
        hash = (value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
#endif
        match = level == RB_BOLTLZ_FAST ? bl_fast_find(src, src_len, pos, last, table BL_PREFIX_ARGS) :
                                      bl_find(src, src_len, pos, last, table, level, 0 BL_PREFIX_ARGS);
#if RB_BOLTLZ_HASH_CACHE
        bl_insert_hash(src, src_len, pos, table, level, hash);
#else
        bl_insert(src, src_len, pos, table, level);
#endif
        if (match.length == 0) {
            step = 1 + (misses >> (level == RB_BOLTLZ_FAST ? 5 : 7));
            if (step > 16) step = 16;
            if (step > src_len - pos) step = src_len - pos;
            pos += step;
            if (misses < 2048) misses++;
            continue;
        }
#if RB_BOLTLZ_LAZY_LIMIT > 0
        if (level == RB_BOLTLZ_BALANCED && match.length < RB_BOLTLZ_LAZY_LIMIT &&
            src_len - pos >= 5) {
            /* A repeated match of length L at pos has exactly L-1 bytes
             * at pos+1. Reuse that evidence rather than counting it twice. */
            known_repeat_length = RB_BOLTLZ_REUSE_MATCH && match.distance == last &&
                                  match.length >= 5 ? match.length - 1 : 0;
#if RB_BOLTLZ_HASH_CACHE
            next_value = bl_load32(src + pos + 1);
            next_hash = (next_value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
#endif
            next = bl_find(src, src_len, pos + 1, last, table, level,
                           known_repeat_length BL_NEXT_PREFIX_ARGS);
#if RB_BOLTLZ_EXACT_COST
            if (next.length != 0) {
                horizon = match.length > next.length + 1 ? match.length : next.length + 1;
                current_cost = bl_sequence_cost(pos - anchor, match, last) +
                               bl_literal_cost(horizon - match.length);
                next_cost = bl_sequence_cost(pos + 1 - anchor, next, last) +
                            bl_literal_cost(horizon - next.length - 1);
                if (next_cost < current_cost) { pos++; continue; }
            }
#else
            if (next.saving > match.saving + 1) { pos++; continue; }
#endif
        }
#endif
#if RB_BOLTLZ_BACKWARD_MATCH
        /* Recover a skipped matching prefix from pending literals. Preserve
         * the selected match's endpoint, distance, and earlier sequences. */
        while (pos > anchor && pos > match.distance &&
               src[pos - 1] == src[pos - match.distance - 1]) {
            pos--;
            match.length++;
        }
#endif
        literal_length = pos - anchor;
        needed = literal_length + bl_match_cost(match.length, match.distance, last) +
                 (literal_length >= 7 ? bl_var_size(literal_length - 7) : 0);
        if (needed > cap - out) return BP_ERR_OVERFLOW;
        tag = (literal_length < 7 ? literal_length : 7) << 4;
        tag |= match.length < 19 ? match.length - 4 : 15;
        if (match.distance == last) tag |= 128;
        dst[out++] = (bp_uint8)tag;
        if (literal_length >= 7) bl_write_var(dst, &out, literal_length - 7);
        if (literal_length != 0) {
            memcpy(dst + out, src + anchor, (size_t)literal_length);
            out += literal_length;
        }
        if (match.length >= 19) bl_write_var(dst, &out, match.length - 19);
        if (match.distance != last) bl_write_var(dst, &out, match.distance);
        last = match.distance;
        pos += match.length;
        anchor = pos;
        misses = 0;
        /* Retain a little history at the end of a long match. */
        insert_pos = pos - 2;
        if (src_len - insert_pos >= 4) bl_insert(src, src_len, insert_pos, table, level);
        insert_pos = pos - 1;
        if (src_len - insert_pos >= 4) bl_insert(src, src_len, insert_pos, table, level);
    }
    rc = bl_literals(src + anchor, src_len - anchor, dst, cap, &out);
    if (rc != BP_OK) return rc;
    *dst_len = out;
    return BP_OK;
}

#if RB_BOLTLZ_SPECIALIZE
int boltlz_review_reference_compress(const bp_uint8 *src, bp_uint32 src_len,
                   bp_uint8 *dst, bp_uint32 *dst_len, void *work, int level)
{
    if (level == RB_BOLTLZ_FAST)
        return bl_compress_impl(src, src_len, dst, dst_len, work, RB_BOLTLZ_FAST);
    if (level == RB_BOLTLZ_BALANCED)
        return bl_compress_impl(src, src_len, dst, dst_len, work, RB_BOLTLZ_BALANCED);
    if (dst_len != NULL) *dst_len = 0;
    return BP_ERR_INVALID_ARG;
}
#endif

/* Length extensions and distances use canonical unsigned LEB128, limited
 * to block size. At most three bytes are accepted. */
static int bl_read_var(const bp_uint8 *src, bp_uint32 size, bp_uint32 *pos,
                        bp_uint32 *value)
{
    bp_uint32 byte, shift, result;
    shift = result = 0;
    do {
        if (*pos == size) return BP_ERR_TRUNCATED;
        byte = src[(*pos)++];
        if (shift == 14 && (byte & 127) > 16) return BP_ERR_CORRUPT;
        result |= (byte & 127) << shift;
        if ((byte & 128) == 0) {
            if (shift != 0 && byte == 0) return BP_ERR_CORRUPT;
            *value = result;
            return BP_OK;
        }
        if (shift == 14) return BP_ERR_CORRUPT;
        shift += 7;
    } while (1);
}

#if RB_BOLTLZ_SHORT_COPY || RB_BOLTLZ_SMALL_DISTANCE || RB_BOLTLZ_FAST_DECODE
/* Exact copies: every load/store stays within the logical copy range. */
BL_FORCE_INLINE void bl_copy_short(bp_uint8 *dst, const bp_uint8 *src,
                                   bp_uint32 length)
{
#if RB_BOLTLZ_SHORT_COPY == 2
    /* First/last copies can overlap each other, but source and destination
     * ranges must be disjoint. Length is at most 32. No padding is accessed. */
    if (length >= 16) {
        memcpy(dst, src, 16);
        memcpy(dst + length - 16, src + length - 16, 16);
    } else if (length >= 8) {
        memcpy(dst, src, 8);
        memcpy(dst + length - 8, src + length - 8, 8);
    } else if (length >= 4) {
        memcpy(dst, src, 4);
        memcpy(dst + length - 4, src + length - 4, 4);
    } else if (length >= 2) {
        memcpy(dst, src, 2);
        memcpy(dst + length - 2, src + length - 2, 2);
    } else if (length != 0) *dst = *src;
#else
    while (length >= 8) {
        memcpy(dst, src, 8);
        dst += 8; src += 8; length -= 8;
    }
    if (length >= 4) {
        memcpy(dst, src, 4);
        dst += 4; src += 4; length -= 4;
    }
    if (length >= 2) {
        memcpy(dst, src, 2);
        dst += 2; src += 2; length -= 2;
    }
    if (length != 0) *dst = *src;
#endif
}

BL_FORCE_INLINE void bl_copy_literal(bp_uint8 *dst, const bp_uint8 *src,
                                     bp_uint32 length)
{
#if RB_BOLTLZ_SHORT_COPY
    if (length <= 16) { bl_copy_short(dst, src, length); return; }
#endif
    if (length != 0) memcpy(dst, src, (size_t)length);
}

BL_FORCE_INLINE void bl_copy_match(bp_uint8 *dst, bp_uint32 length,
                                   bp_uint32 distance)
{
    const bp_uint8 *ref;
    bp_uint32 available, chunk;
#if RB_BOLTLZ_SMALL_DISTANCE
    bp_uint8 pattern[12];
#endif
    ref = dst - distance;
    if (distance == 1) { memset(dst, *ref, (size_t)length); return; }
#if RB_BOLTLZ_SMALL_DISTANCE
    if (distance == 3) {
        memcpy(pattern, ref, 3);
        memcpy(pattern + 3, pattern, 3);
        memcpy(pattern + 6, pattern, 6);
        while (length >= 12) {
            memcpy(dst, pattern, 12); dst += 12; length -= 12;
        }
        bl_copy_short(dst, pattern, length);
        return;
    }
    if (distance == 2 || distance == 4) {
        if (distance == 2) {
            memcpy(pattern, ref, 2);
            memcpy(pattern + 2, pattern, 2);
        } else memcpy(pattern, ref, 4);
        memcpy(pattern + 4, pattern, 4);
        while (length >= 8) {
            memcpy(dst, pattern, 8); dst += 8; length -= 8;
        }
        bl_copy_short(dst, pattern, length);
        return;
    }
#endif
#if RB_BOLTLZ_SHORT_COPY
#if RB_BOLTLZ_SHORT_COPY == 2
    if (length <= 32 && length <= distance) {
        bl_copy_short(dst, ref, length);
        return;
    }
    if (length <= 32 && distance >= 8) {
        while (length >= 8) {
            memcpy(dst, ref, 8);
            dst += 8; ref += 8; length -= 8;
        }
        bl_copy_short(dst, ref, length);
        return;
    }
#else
    /* For distance >= 8, every individual short-copy chunk is disjoint,
     * even when the complete match overlaps. Earlier chunks seed later ones. */
    if (length <= 32 && (distance >= 8 || length <= distance)) {
        bl_copy_short(dst, ref, length);
        return;
    }
#endif
#endif
    available = distance;
    while (length != 0) {
        chunk = length < available ? length : available;
        memcpy(dst, ref, (size_t)chunk);
        dst += chunk;
        length -= chunk;
        available += chunk;
    }
}
#endif

int boltlz_review_reference_decompress(const bp_uint8 *src, bp_uint32 src_len,
                     bp_uint8 *dst, bp_uint32 dst_len)
{
    bp_uint32 in, out, tag, length, extra, distance, last;
#if !RB_BOLTLZ_SHORT_COPY && !RB_BOLTLZ_SMALL_DISTANCE && !RB_BOLTLZ_FAST_DECODE
    bp_uint32 ref, chunk, available;
#endif
#if RB_BOLTLZ_FAST_DECODE
    bp_uint32 cursor, literals, match_length, byte;
#endif
    int rc;
    if (src == NULL || (dst == NULL && dst_len != 0) ||
        dst_len > RB_BOLTLZ_MAX_BLOCK ||
        (bp_uint32)(size_t)src_len != src_len ||
        (bp_uint32)(size_t)dst_len != dst_len) return BP_ERR_INVALID_ARG;
    if (src_len == 0) return BP_ERR_TRUNCATED;
    if (src[0] != RB_BOLTLZ_FORMAT_VERSION) return BP_ERR_VERSION;
    in = 1;
    out = last = 0;
    while (in < src_len) {
        tag = src[in++];
#if RB_BOLTLZ_FAST_DECODE
        literals = (tag >> 4) & 7;
        match_length = (tag & 15) + 4;
        /* Fast path for inline lengths and one-/two-byte explicit distances
         * or a repeat. Terminal literals and uncommon fields fall through.
         * No output is touched until all fields and bounds are validated. */
        if (literals < 7 && match_length < 19 &&
#if RB_BOLTLZ_FAST_DECODE == 2
            src_len - in >= 16 && dst_len - out >= 32) {
#else
            literals <= src_len - in &&
            literals <= dst_len - out && match_length <= dst_len - out - literals) {
#endif
            cursor = in + literals;
            distance = last;
            if ((tag & 128) == 0) {
                distance = 0;
#if RB_BOLTLZ_FAST_DECODE != 2
                if (cursor < src_len) {
#endif
                    byte = src[cursor++];
                    if (byte < 128) distance = byte;
                    else if (
#if RB_BOLTLZ_FAST_DECODE != 2
                             cursor < src_len &&
#endif
                             src[cursor] > 0 && src[cursor] < 128) {
                        distance = (byte & 127) | ((bp_uint32)src[cursor++] << 7);
                    }
#if RB_BOLTLZ_FAST_DECODE != 2
                }
#endif
            }
            if (distance != 0 && distance <= out + literals) {
#if RB_BOLTLZ_FAST_DECODE == 2
                /* Extra bytes remain strictly inside the supplied buffers.
                 * Future sequences overwrite them before they enter history.
                 * The 32-byte output guard covers 6 literals plus 18 match
                 * bytes; distance >= 16 makes each match copy disjoint. */
                memcpy(dst + out, src + in, 16);
                out += literals;
                if (distance >= 16) {
                    memcpy(dst + out, dst + out - distance, 16);
                    memcpy(dst + out + 16, dst + out - distance + 16, 2);
                } else bl_copy_match(dst + out, match_length, distance);
#else
                bl_copy_literal(dst + out, src + in, literals);
                out += literals;
                bl_copy_match(dst + out, match_length, distance);
#endif
                out += match_length;
                in = cursor;
                last = distance;
                continue;
            }
        }
#endif
        length = (tag >> 4) & 7;
        if (length == 7) {
            rc = bl_read_var(src, src_len, &in, &extra);
            if (rc != BP_OK) return rc;
            length += extra;
        }
        if (length > dst_len - out) return BP_ERR_CORRUPT;
        if (length > src_len - in) return BP_ERR_TRUNCATED;
        if (length != 0) {
#if RB_BOLTLZ_SHORT_COPY || RB_BOLTLZ_SMALL_DISTANCE || RB_BOLTLZ_FAST_DECODE
            bl_copy_literal(dst + out, src + in, length);
#else
            memcpy(dst + out, src + in, (size_t)length);
#endif
            in += length;
            out += length;
        }
        if (in == src_len && (tag & 128) == 0) {
            return (tag & 143) == 0 && out == dst_len && length != 0 ? BP_OK : BP_ERR_CORRUPT;
        }
        length = (tag & 15) + 4;
        if (length == 19) {
            rc = bl_read_var(src, src_len, &in, &extra);
            if (rc != BP_OK) return rc;
            length += extra;
        }
        distance = last;
        if ((tag & 128) == 0) {
            rc = bl_read_var(src, src_len, &in, &distance);
            if (rc != BP_OK) return rc;
        }
        if (distance == 0 || distance > out || length > dst_len - out)
            return BP_ERR_CORRUPT;
        last = distance;
#if RB_BOLTLZ_SHORT_COPY || RB_BOLTLZ_SMALL_DISTANCE || RB_BOLTLZ_FAST_DECODE
        bl_copy_match(dst + out, length, distance);
        out += length;
#else
        if (distance == 1) {
            memset(dst + out, dst[out - 1], (size_t)length);
            out += length;
        } else {
            /* Each chunk is nonoverlapping. Grow the available prefix so
             * later chunks can use bytes generated by earlier chunks. */
            ref = out - distance;
            available = distance;
            while (length != 0) {
                chunk = length < available ? length : available;
                memcpy(dst + out, dst + ref, (size_t)chunk);
                out += chunk;
                length -= chunk;
                available += chunk;
            }
        }
#endif
    }
    return out == dst_len ? BP_OK : BP_ERR_TRUNCATED;
}
