/*
 * native_attrs.h -- BoltPack native attribute container (BPNA, version 1)
 *
 * A BP_TYPE_META entry whose platform byte is BP_META_NATIVE_ATTRS (0xFD)
 * carries a set of named attribute values that exist only on the creating
 * platform (for example macOS extended attributes).  The archive format only
 * recognises the container; the frontend of the origin platform interprets
 * and restores the items.  Every other reader skips it and reports it as
 * unsupported metadata, and archive edits (add/delete/copy) preserve it.
 *
 * Payload layout (all integers little-endian):
 *
 *   offset size field
 *   0      4    magic "BPNA"
 *   4      1    container version, currently 1
 *   5      1    origin platform (a BP_OS_* creator code, e.g. 0x03 macOS)
 *   6      1    subject entry type: 0 (file) or 4 (directory)
 *   7      1    reserved, writers store 0, readers ignore
 *   8      2    item count (0..64)
 *   10     ...  items, each:
 *                 u16 name length (1..255)
 *                 u32 value length (0..32768)
 *                 name bytes  (no NUL; opaque, UTF-8 on macOS)
 *                 value bytes
 *
 * The whole payload is at most 65536 bytes.  Names are unique within one
 * container.  The entry follows its subject entry with the exact same
 * archive name and must not be applied unless the subject extracted.
 *
 * This header is C89, byte-order neutral, and has no dependencies beyond the
 * C library.  See docs/specification-1.0.md section 2.3.1.2.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_NATIVE_ATTRS_H
#define BP_NATIVE_ATTRS_H

#include <stddef.h>
#include <string.h>

#define BP_META_NATIVE_ATTRS     0xFD
#define BP_NATTR_VERSION         1U
#define BP_NATTR_HEADER_SIZE     10UL
#define BP_NATTR_MAX_PAYLOAD     65536UL
#define BP_NATTR_MAX_ITEMS       64U
#define BP_NATTR_MAX_NAME        255U
#define BP_NATTR_MAX_VALUE       32768UL
#define BP_NATTR_ITEM_OVERHEAD   6UL

/* Result codes.  Builder errors are negative; parser results are 0, a
 * positive "unsupported" value, or negative corruption. */
#define BP_NATTR_OK              0
#define BP_NATTR_UNSUPPORTED     1   /* well framed, unknown version */
#define BP_NATTR_CORRUPT        -1
#define BP_NATTR_FULL           -2   /* item count or payload limit reached */
#define BP_NATTR_TOO_BIG        -3   /* single value over the per-item cap */
#define BP_NATTR_BAD_NAME       -4   /* empty, too long, or contains NUL */
#define BP_NATTR_DUPLICATE      -5

typedef struct {
    unsigned char *buf;       /* caller storage, BP_NATTR_MAX_PAYLOAD bytes */
    unsigned long  len;
    unsigned       count;
} bp_nattr_builder_t;

typedef struct {
    const unsigned char *data;
    unsigned long        size;
    unsigned char        version;
    unsigned char        origin;
    unsigned char        subject_type;
    unsigned             count;
} bp_nattr_info_t;

typedef struct {
    const unsigned char *data;
    unsigned long        size;
    unsigned long        pos;
    unsigned             remaining;
} bp_nattr_iter_t;

static unsigned long bp_nattr_rd16(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

static unsigned long bp_nattr_rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static const char *bp_nattr_strerror(int rc)
{
    switch (rc) {
    case BP_NATTR_OK:          return "ok";
    case BP_NATTR_UNSUPPORTED: return "unsupported container version";
    case BP_NATTR_CORRUPT:     return "malformed container";
    case BP_NATTR_FULL:        return "container is full (64 items or 64 KiB)";
    case BP_NATTR_TOO_BIG:     return "attribute value exceeds 32 KiB";
    case BP_NATTR_BAD_NAME:    return "invalid attribute name";
    case BP_NATTR_DUPLICATE:   return "duplicate attribute name";
    default:                   return "unknown container error";
    }
}

/* ---- builder ------------------------------------------------------------ */

static void bp_nattr_begin(bp_nattr_builder_t *b, unsigned char *buf,
                           unsigned char origin, unsigned char subject_type)
{
    buf[0] = 'B'; buf[1] = 'P'; buf[2] = 'N'; buf[3] = 'A';
    buf[4] = (unsigned char)BP_NATTR_VERSION;
    buf[5] = origin;
    buf[6] = subject_type;
    buf[7] = 0;
    buf[8] = 0;
    buf[9] = 0;
    b->buf = buf;
    b->len = BP_NATTR_HEADER_SIZE;
    b->count = 0;
}

/* Does the builder already hold an item with this name? */
static int bp_nattr_builder_has(const bp_nattr_builder_t *b,
                                const unsigned char *name,
                                unsigned long name_len)
{
    unsigned long pos = BP_NATTR_HEADER_SIZE;
    unsigned i;
    for (i = 0; i < b->count; i++) {
        unsigned long nl = bp_nattr_rd16(b->buf + pos);
        unsigned long vl = bp_nattr_rd32(b->buf + pos + 2);
        if (nl == name_len &&
            memcmp(b->buf + pos + BP_NATTR_ITEM_OVERHEAD, name,
                   (size_t)name_len) == 0)
            return 1;
        pos += BP_NATTR_ITEM_OVERHEAD + nl + vl;
    }
    return 0;
}

static int bp_nattr_add(bp_nattr_builder_t *b, const char *name,
                        unsigned long name_len, const void *value,
                        unsigned long value_len)
{
    unsigned char *p;
    unsigned long need;

    if (name_len == 0 || name_len > BP_NATTR_MAX_NAME ||
        memchr(name, '\0', (size_t)name_len) != NULL)
        return BP_NATTR_BAD_NAME;
    if (value_len > BP_NATTR_MAX_VALUE) return BP_NATTR_TOO_BIG;
    if (b->count >= BP_NATTR_MAX_ITEMS) return BP_NATTR_FULL;
    need = BP_NATTR_ITEM_OVERHEAD + name_len + value_len;
    if (need > BP_NATTR_MAX_PAYLOAD - b->len) return BP_NATTR_FULL;
    if (bp_nattr_builder_has(b, (const unsigned char *)name, name_len))
        return BP_NATTR_DUPLICATE;

    p = b->buf + b->len;
    p[0] = (unsigned char)(name_len & 0xFFUL);
    p[1] = (unsigned char)((name_len >> 8) & 0xFFUL);
    p[2] = (unsigned char)(value_len & 0xFFUL);
    p[3] = (unsigned char)((value_len >> 8) & 0xFFUL);
    p[4] = (unsigned char)((value_len >> 16) & 0xFFUL);
    p[5] = (unsigned char)((value_len >> 24) & 0xFFUL);
    memcpy(p + BP_NATTR_ITEM_OVERHEAD, name, (size_t)name_len);
    if (value_len > 0)
        memcpy(p + BP_NATTR_ITEM_OVERHEAD + name_len, value,
               (size_t)value_len);
    b->len += need;
    b->count++;
    return BP_NATTR_OK;
}

/* Store the item count and return the final payload length. */
static unsigned long bp_nattr_finish(bp_nattr_builder_t *b)
{
    b->buf[8] = (unsigned char)(b->count & 0xFFU);
    b->buf[9] = (unsigned char)((b->count >> 8) & 0xFFU);
    return b->len;
}

/* ---- parser ------------------------------------------------------------- */

/* Validate a container.  Returns BP_NATTR_OK, BP_NATTR_UNSUPPORTED (valid
 * magic but a version this reader does not know; nothing else is checked) or
 * BP_NATTR_CORRUPT.  On OK, info->count items can be walked with the iterator.
 * A reader must not apply any item unless this returns BP_NATTR_OK. */
static int bp_nattr_parse(const unsigned char *data, unsigned long size,
                          bp_nattr_info_t *info)
{
    unsigned long pos;
    unsigned i, j;

    memset(info, 0, sizeof(*info));
    if (data == NULL || size < BP_NATTR_HEADER_SIZE ||
        size > BP_NATTR_MAX_PAYLOAD)
        return BP_NATTR_CORRUPT;
    if (data[0] != 'B' || data[1] != 'P' || data[2] != 'N' || data[3] != 'A')
        return BP_NATTR_CORRUPT;
    info->data = data;
    info->size = size;
    info->version = data[4];
    info->origin = data[5];
    info->subject_type = data[6];
    if (info->version != BP_NATTR_VERSION) return BP_NATTR_UNSUPPORTED;
    if (info->subject_type != 0 && info->subject_type != 4)
        return BP_NATTR_CORRUPT;
    info->count = (unsigned)bp_nattr_rd16(data + 8);
    if (info->count > BP_NATTR_MAX_ITEMS) return BP_NATTR_CORRUPT;

    pos = BP_NATTR_HEADER_SIZE;
    for (i = 0; i < info->count; i++) {
        unsigned long nl, vl;
        if (size - pos < BP_NATTR_ITEM_OVERHEAD) return BP_NATTR_CORRUPT;
        nl = bp_nattr_rd16(data + pos);
        vl = bp_nattr_rd32(data + pos + 2);
        pos += BP_NATTR_ITEM_OVERHEAD;
        if (nl == 0 || nl > BP_NATTR_MAX_NAME || vl > BP_NATTR_MAX_VALUE)
            return BP_NATTR_CORRUPT;
        if (size - pos < nl || size - pos - nl < vl) return BP_NATTR_CORRUPT;
        if (memchr(data + pos, '\0', (size_t)nl) != NULL)
            return BP_NATTR_CORRUPT;
        pos += nl + vl;
    }
    if (pos != size) return BP_NATTR_CORRUPT;

    /* Names must be unique (at most 64 items, so quadratic is fine). */
    for (i = 0, pos = BP_NATTR_HEADER_SIZE; i < info->count; i++) {
        unsigned long nl = bp_nattr_rd16(data + pos);
        unsigned long vl = bp_nattr_rd32(data + pos + 2);
        unsigned long other = pos + BP_NATTR_ITEM_OVERHEAD + nl + vl;
        for (j = i + 1; j < info->count; j++) {
            unsigned long nl2 = bp_nattr_rd16(data + other);
            unsigned long vl2 = bp_nattr_rd32(data + other + 2);
            if (nl2 == nl &&
                memcmp(data + pos + BP_NATTR_ITEM_OVERHEAD,
                       data + other + BP_NATTR_ITEM_OVERHEAD,
                       (size_t)nl) == 0)
                return BP_NATTR_CORRUPT;
            other += BP_NATTR_ITEM_OVERHEAD + nl2 + vl2;
        }
        pos += BP_NATTR_ITEM_OVERHEAD + nl + vl;
    }
    return BP_NATTR_OK;
}

/* Iterate the items of a container that bp_nattr_parse accepted. */
static void bp_nattr_iter_begin(const bp_nattr_info_t *info,
                                bp_nattr_iter_t *it)
{
    it->data = info->data;
    it->size = info->size;
    it->pos = BP_NATTR_HEADER_SIZE;
    it->remaining = info->count;
}

/* Returns 1 and fills the outputs while items remain, else 0.  The name is
 * not NUL terminated; value may be empty (value_len 0). */
static int bp_nattr_iter_next(bp_nattr_iter_t *it, const char **name,
                              unsigned long *name_len,
                              const unsigned char **value,
                              unsigned long *value_len)
{
    unsigned long nl, vl;
    if (it->remaining == 0) return 0;
    nl = bp_nattr_rd16(it->data + it->pos);
    vl = bp_nattr_rd32(it->data + it->pos + 2);
    *name = (const char *)(it->data + it->pos + BP_NATTR_ITEM_OVERHEAD);
    *name_len = nl;
    *value = it->data + it->pos + BP_NATTR_ITEM_OVERHEAD + nl;
    *value_len = vl;
    it->pos += BP_NATTR_ITEM_OVERHEAD + nl + vl;
    it->remaining--;
    return 1;
}

#endif /* BP_NATIVE_ATTRS_H */
