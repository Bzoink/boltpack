/* Frozen speed-tuning baseline; do not change. BSD 2-Clause License. */
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
#include "boltlz_speed_base.h"
#include "boltlz_speed_base_config.h"
#include <string.h>

#define BL_HASH_BITS SB_BOLTLZ_HASH_BITS
#define BL_HASH_SIZE (1UL << BL_HASH_BITS)

/* Share the current prefix between lookup and insertion in this experiment.
 * Disabled builds retain the original lookup interfaces. */
#if SB_BOLTLZ_HASH_CACHE
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

#if SB_BOLTLZ_DUAL_HASH
static bp_uint32 bl_hash6(const bp_uint8 *p)
{
    bp_uint32 suffix, value;
    suffix = p[4] | ((bp_uint32)p[5] << 8);
    value = bl_load32(p) ^ (suffix * (bp_uint32)2654435761UL);
    return (value * (bp_uint32)2246822519UL) >> (32 - BL_HASH_BITS);
}
#endif

bp_uint32 boltlz_speed_reference_work_size(int level)
{
    if (level != SB_BOLTLZ_FAST && level != SB_BOLTLZ_BALANCED) return 0;
    return BL_HASH_SIZE * (bp_uint32)sizeof(bp_uint32) * (bp_uint32)level;
}

bp_uint32 boltlz_speed_reference_compress_bound(bp_uint32 src_len)
{
    if (src_len > SB_BOLTLZ_MAX_BLOCK) return 0;
    return src_len + src_len / 63 + 16;
}

static bp_uint32 bl_var_size(bp_uint32 value)
{
    bp_uint32 size;
    size = 1;
    while (value >= 128) { value >>= 7; size++; }
    return size;
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
    if (length < 4) return SB_BOLTLZ_MAX_BLOCK;
    return 1 + (distance == last_distance ? 0 : bl_var_size(distance)) +
           (length >= 19 ? bl_var_size(length - 19) : 0);
}

#if SB_BOLTLZ_EXACT_COST
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
#if SB_BOLTLZ_WORD_MATCH && (defined(__GNUC__) || defined(__clang__)) && \
    defined(__BYTE_ORDER__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    unsigned long current, previous, difference;
#endif
    length = 4;
#if SB_BOLTLZ_WORD_MATCH && (defined(__GNUC__) || defined(__clang__)) && \
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
#if SB_BOLTLZ_WIDE_MATCH
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

static bl_match bl_find(const bp_uint8 *src, bp_uint32 size, bp_uint32 pos,
                        bp_uint32 last, const bp_uint32 *table, int level,
                        bp_uint32 known_repeat_length BL_PREFIX_PARAMS)
{
    bl_match best;
    bp_uint32 entries[3], count, i, ref, distance, length, cost;
#if !SB_BOLTLZ_HASH_CACHE
    bp_uint32 hash, value;
#endif
#if SB_BOLTLZ_EXACT_COST
    bp_uint32 horizon, best_cost, candidate_cost;
#endif
    best.length = best.distance = best.saving = 0;
#if SB_BOLTLZ_REUSE_MATCH
    if (known_repeat_length >= 4) {
        best.length = known_repeat_length;
        best.distance = last;
        best.saving = known_repeat_length - bl_match_cost(known_repeat_length, last, last);
        if (best.length == size - pos) return best;
    }
#else
    (void)known_repeat_length;
#endif
#if !SB_BOLTLZ_HASH_CACHE
    value = bl_load32(src + pos);
    hash = (value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
#endif
    entries[0] = last != 0 && last <= pos ? pos - last + 1 : 0;
    entries[1] = table[hash];
    count = 2;
    if (level == SB_BOLTLZ_BALANCED) {
#if SB_BOLTLZ_DUAL_HASH
        entries[2] = size - pos >= 6 ? table[BL_HASH_SIZE + bl_hash6(src + pos)] : 0;
#else
        entries[2] = table[BL_HASH_SIZE + hash];
#endif
        count = 3;
    }
    for (i = 0; i < count; i++) {
#if SB_BOLTLZ_REUSE_MATCH
        if (i == 0 && known_repeat_length >= 4) continue;
#endif
#if SB_BOLTLZ_SECOND_LIMIT == 0
        if (i == 2) continue;
#else
        if (i == 2 && best.length >= SB_BOLTLZ_SECOND_LIMIT) continue;
#endif
        if (entries[i] == 0 || (i != 0 && entries[i] == entries[0])) continue;
        ref = entries[i] - 1;
        if (ref >= pos) continue;
        distance = pos - ref;
        if (value != bl_load32(src + ref)) continue;
#if SB_BOLTLZ_PRUNE_MATCH && !SB_BOLTLZ_EXACT_COST
        /* A mismatch here bounds length <= best.saving. Every match costs
         * at least one byte, so this candidate cannot tie or beat the best.
         * best.saving < best.length <= size-pos also proves the read bound. */
        if (best.saving >= 4 && src[pos + best.saving] != src[ref + best.saving])
            continue;
#endif
        length = bl_count(src, size, pos, ref);
        cost = bl_match_cost(length, distance, last);
        if (length <= cost) continue;
#if SB_BOLTLZ_EXACT_COST
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
            (level == SB_BOLTLZ_FAST && best.length >= 64)) break;
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
#if !SB_BOLTLZ_HASH_CACHE
    bp_uint32 value, hash;
#endif
    match.length = match.distance = match.saving = 0;
#if !SB_BOLTLZ_HASH_CACHE
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
    if (level == SB_BOLTLZ_BALANCED) {
#if SB_BOLTLZ_DUAL_HASH
        if (size - pos >= 6)
            table[BL_HASH_SIZE + bl_hash6(src + pos)] = pos + 1;
#else
        table[BL_HASH_SIZE + hash] = table[hash];
#endif
    }
#if !SB_BOLTLZ_DUAL_HASH
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

int boltlz_speed_reference_compress(const bp_uint8 *src, bp_uint32 src_len,
                   bp_uint8 *dst, bp_uint32 *dst_len, void *work, int level)
{
    bp_uint32 cap, out, pos, anchor, last, misses, step, insert_pos;
    bp_uint32 literal_length, needed, tag;
    bp_uint32 *table;
    bl_match match;
#if SB_BOLTLZ_HASH_CACHE
    bp_uint32 value, hash;
#if SB_BOLTLZ_LAZY_LIMIT > 0
    bp_uint32 next_value, next_hash;
#endif
#endif
#if SB_BOLTLZ_LAZY_LIMIT > 0
    bl_match next;
    bp_uint32 known_repeat_length;
#if SB_BOLTLZ_EXACT_COST
    bp_uint32 horizon, current_cost, next_cost;
#endif
#endif
    int rc;
    if (dst_len == NULL) return BP_ERR_INVALID_ARG;
    cap = *dst_len;
    *dst_len = 0;
    if (dst == NULL || (src == NULL && src_len != 0) || work == NULL ||
        src_len > SB_BOLTLZ_MAX_BLOCK || boltlz_speed_reference_work_size(level) == 0 ||
        sizeof(bp_uint32) != 4 || (bp_uint32)(size_t)src_len != src_len ||
        (bp_uint32)(size_t)boltlz_speed_reference_work_size(level) != boltlz_speed_reference_work_size(level))
        return BP_ERR_INVALID_ARG;
    if (cap == 0) return BP_ERR_OVERFLOW;
    dst[0] = SB_BOLTLZ_FORMAT_VERSION;
    out = 1;
    if (src_len == 0) { *dst_len = out; return BP_OK; }
    table = (bp_uint32 *)work;
    memset(table, 0, (size_t)boltlz_speed_reference_work_size(level));
    pos = anchor = last = misses = 0;
    while (src_len - pos >= 4) {
#if SB_BOLTLZ_HASH_CACHE
        value = bl_load32(src + pos);
        hash = (value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
#endif
        match = level == SB_BOLTLZ_FAST ? bl_fast_find(src, src_len, pos, last, table BL_PREFIX_ARGS) :
                                      bl_find(src, src_len, pos, last, table, level, 0 BL_PREFIX_ARGS);
#if SB_BOLTLZ_HASH_CACHE
        bl_insert_hash(src, src_len, pos, table, level, hash);
#else
        bl_insert(src, src_len, pos, table, level);
#endif
        if (match.length == 0) {
            step = 1 + (misses >> (level == SB_BOLTLZ_FAST ? 5 : 7));
            if (step > 16) step = 16;
            if (step > src_len - pos) step = src_len - pos;
            pos += step;
            if (misses < 2048) misses++;
            continue;
        }
#if SB_BOLTLZ_LAZY_LIMIT > 0
        if (level == SB_BOLTLZ_BALANCED && match.length < SB_BOLTLZ_LAZY_LIMIT &&
            src_len - pos >= 5) {
            /* A repeated match of length L at pos has exactly L-1 bytes
             * at pos+1. Reuse that evidence rather than counting it twice. */
            known_repeat_length = SB_BOLTLZ_REUSE_MATCH && match.distance == last &&
                                  match.length >= 5 ? match.length - 1 : 0;
#if SB_BOLTLZ_HASH_CACHE
            next_value = bl_load32(src + pos + 1);
            next_hash = (next_value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
#endif
            next = bl_find(src, src_len, pos + 1, last, table, level,
                           known_repeat_length BL_NEXT_PREFIX_ARGS);
#if SB_BOLTLZ_EXACT_COST
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
#if SB_BOLTLZ_BACKWARD_MATCH
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

int boltlz_speed_reference_decompress(const bp_uint8 *src, bp_uint32 src_len,
                     bp_uint8 *dst, bp_uint32 dst_len)
{
    bp_uint32 in, out, tag, length, extra, distance, last;
    bp_uint32 ref, chunk, available;
    int rc;
    if (src == NULL || (dst == NULL && dst_len != 0) ||
        dst_len > SB_BOLTLZ_MAX_BLOCK ||
        (bp_uint32)(size_t)src_len != src_len ||
        (bp_uint32)(size_t)dst_len != dst_len) return BP_ERR_INVALID_ARG;
    if (src_len == 0) return BP_ERR_TRUNCATED;
    if (src[0] != SB_BOLTLZ_FORMAT_VERSION) return BP_ERR_VERSION;
    in = 1;
    out = last = 0;
    while (in < src_len) {
        tag = src[in++];
        length = (tag >> 4) & 7;
        if (length == 7) {
            rc = bl_read_var(src, src_len, &in, &extra);
            if (rc != BP_OK) return rc;
            length += extra;
        }
        if (length > dst_len - out) return BP_ERR_CORRUPT;
        if (length > src_len - in) return BP_ERR_TRUNCATED;
        if (length != 0) {
            memcpy(dst + out, src + in, (size_t)length);
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
    }
    return out == dst_len ? BP_OK : BP_ERR_TRUNCATED;
}
