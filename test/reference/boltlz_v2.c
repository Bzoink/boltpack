/* Frozen pre-tuning encoder baseline. Source SHA-256: 6e0ec69bb7aafb2602cf90d61bd135384f197f9cfc9aef59a3a1296b6c208507
 * Keep this implementation unchanged when tuning the candidate. */
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
#include "boltlz_v2.h"

#define BOLTLZ_FORMAT_VERSION 2
#define BOLTLZ_MAX_BLOCK 262144UL
#define BOLTLZ_FAST 1
#define BOLTLZ_BALANCED 2
#include <string.h>

#define BL_HASH_BITS 13
#define BL_HASH_SIZE 8192UL

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

bp_uint32 boltlz_reference_work_size(int level)
{
    if (level != BOLTLZ_FAST && level != BOLTLZ_BALANCED) return 0;
    return BL_HASH_SIZE * (bp_uint32)sizeof(bp_uint32) * (bp_uint32)level;
}

bp_uint32 boltlz_reference_compress_bound(bp_uint32 src_len)
{
    if (src_len > BOLTLZ_MAX_BLOCK) return 0;
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
    if (length < 4) return BOLTLZ_MAX_BLOCK;
    return 1 + (distance == last_distance ? 0 : bl_var_size(distance)) +
           (length >= 19 ? bl_var_size(length - 19) : 0);
}

static bp_uint32 bl_count(const bp_uint8 *src, bp_uint32 size,
                         bp_uint32 pos, bp_uint32 ref)
{
    bp_uint32 length;
    length = 4;
    while (size - pos - length >= 4 &&
           bl_load32(src + pos + length) == bl_load32(src + ref + length))
        length += 4;
    while (length < size - pos && src[pos + length] == src[ref + length])
        length++;
    return length;
}

static bl_match bl_find(const bp_uint8 *src, bp_uint32 size, bp_uint32 pos,
                        bp_uint32 last, const bp_uint32 *table, int level)
{
    bl_match best;
    bp_uint32 hash, value, entries[3], count, i, ref, distance, length, cost;
    best.length = best.distance = best.saving = 0;
    value = bl_load32(src + pos);
    hash = (value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
    entries[0] = last != 0 && last <= pos ? pos - last + 1 : 0;
    entries[1] = table[hash];
    count = 2;
    if (level == BOLTLZ_BALANCED) {
        entries[2] = table[BL_HASH_SIZE + hash];
        count = 3;
    }
    for (i = 0; i < count; i++) {
        if (entries[i] == 0 || (i != 0 && entries[i] == entries[0])) continue;
        ref = entries[i] - 1;
        if (ref >= pos) continue;
        distance = pos - ref;
        if (value != bl_load32(src + ref)) continue;
        length = bl_count(src, size, pos, ref);
        cost = bl_match_cost(length, distance, last);
        if (length <= cost) continue;
        if (length - cost > best.saving ||
            (length - cost == best.saving && length > best.length)) {
            best.length = length;
            best.distance = distance;
            best.saving = length - cost;
        }
        if (best.length == size - pos ||
            (level == BOLTLZ_FAST && best.length >= 64)) break;
    }
    return best;
}

/* One hash candidate in fast mode. Repeated distances are still encoded
 * cheaply when selected; their extra search is reserved for balanced mode. */
static bl_match bl_fast_find(const bp_uint8 *src, bp_uint32 size, bp_uint32 pos,
                             bp_uint32 last, const bp_uint32 *table)
{
    bl_match match;
    bp_uint32 value, hash, entry, ref, length, distance, cost;
    match.length = match.distance = match.saving = 0;
    value = bl_load32(src + pos);
    hash = (value * (bp_uint32)2654435761UL) >> (32 - BL_HASH_BITS);
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

static void bl_insert(const bp_uint8 *src, bp_uint32 pos,
                       bp_uint32 *table, int level)
{
    bp_uint32 hash;
    hash = bl_hash(src + pos);
    if (level == BOLTLZ_BALANCED)
        table[BL_HASH_SIZE + hash] = table[hash];
    table[hash] = pos + 1;
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

int boltlz_reference_compress(const bp_uint8 *src, bp_uint32 src_len,
                   bp_uint8 *dst, bp_uint32 *dst_len, void *work, int level)
{
    bp_uint32 cap, out, pos, anchor, last, misses, step, insert_pos;
    bp_uint32 literal_length, needed, tag;
    bp_uint32 *table;
    bl_match match, next;
    int rc;
    if (dst_len == NULL) return BP_ERR_INVALID_ARG;
    cap = *dst_len;
    *dst_len = 0;
    if (dst == NULL || (src == NULL && src_len != 0) || work == NULL ||
        src_len > BOLTLZ_MAX_BLOCK || boltlz_reference_work_size(level) == 0 ||
        sizeof(bp_uint32) != 4 || (bp_uint32)(size_t)src_len != src_len ||
        (bp_uint32)(size_t)boltlz_reference_work_size(level) != boltlz_reference_work_size(level))
        return BP_ERR_INVALID_ARG;
    if (cap == 0) return BP_ERR_OVERFLOW;
    dst[0] = BOLTLZ_FORMAT_VERSION;
    out = 1;
    if (src_len == 0) { *dst_len = out; return BP_OK; }
    table = (bp_uint32 *)work;
    memset(table, 0, (size_t)boltlz_reference_work_size(level));
    pos = anchor = last = misses = 0;
    while (src_len - pos >= 4) {
        match = level == BOLTLZ_FAST ? bl_fast_find(src, src_len, pos, last, table) :
                                      bl_find(src, src_len, pos, last, table, level);
        bl_insert(src, pos, table, level);
        if (match.length == 0) {
            step = 1 + (misses >> (level == BOLTLZ_FAST ? 5 : 7));
            if (step > 16) step = 16;
            if (step > src_len - pos) step = src_len - pos;
            pos += step;
            if (misses < 2048) misses++;
            continue;
        }
        if (level == BOLTLZ_BALANCED && match.length < 32 && src_len - pos >= 5) {
            next = bl_find(src, src_len, pos + 1, last, table, level);
            if (next.saving > match.saving + 1) { pos++; continue; }
        }
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
        if (src_len - insert_pos >= 4) bl_insert(src, insert_pos, table, level);
        insert_pos = pos - 1;
        if (src_len - insert_pos >= 4) bl_insert(src, insert_pos, table, level);
    }
    rc = bl_literals(src + anchor, src_len - anchor, dst, cap, &out);
    if (rc != BP_OK) return rc;
    *dst_len = out;
    return BP_OK;
}

