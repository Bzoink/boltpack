/*
 * archiver.c -- mini archive read/write for BoltPack (archive format 7)
 *
 * Implements docs/specification-2.0.md, single- and multi-volume, for the
 * smallest targets, capping capabilities at:
 *   - Methods 0 (stored), 1 (BoltLite), and 2 (boltlz; compression on 32-bit
 *     builds only)
 *   - Block size  <= BP_MINI_MAX_BLOCK_SIZE  (32 KB)
 *   - Entry count <= BP_MINI_MAX_ENTRIES    (65 535)
 *   - File size   <= 4 GB (original_size.hi must be 0)
 *   - Volume size <= BP_MINI_MAX_ARCHIVE_SIZE (2 GB - 1)
 *
 * Memory: reading keeps 12 bytes per entry (a digest of its table record,
 * its compressed size and checksum) in 12 KB chunks, so a set is read volume
 * by volume without returning to the table.  No floating point is used.
 *
 * ANSI C (C89/C90) only, 16-bit int safe.  No fluff dependency.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "archiver.h"
#include "../archive_name.h"
#include "../bp_metadata.h"
#include "compress.h"
#include "stack_probe.h"
#include <stdlib.h>
#include <string.h>
#ifndef BP_MINI_QUIET
#include <stdio.h>
#endif

/* ---- format 7 layout --------------------------------------------------- */

#define M7_VOL_HDR      32UL
#define M7_VOL_TRL      32UL
#define M7_END_REC      48UL
#define M7_ENTRY_FIXED  34UL
#define M7_ENTRY_TRL    16UL
#define M7_BLOCK_HDR    12UL
#define M7_TABLE_HDR    16UL
#define M7_REC_TAIL     45UL
#define M7_MAX_NAME     65501UL
#define M7_MAX_VOLNUM   999U
#define M7_FLAG_LAST    0x0001U
#define M7_EOV_SPACE    512UL   /* body space worth an end-of-volume block */
#define M7_MIN_VOLUME   4096UL

static const bp_uint8 m7_trl_magic[4] = { 0x42, 0x56, 0x4F, 0x4C };  /* BVOL */
static const bp_uint8 m7_end_magic[4] = { 0x54, 0x4C, 0x4F, 0x42 };  /* TLOB */

/* ---- low-level I/O helpers -------------------------------------------- */

static int mini_io_write(bp_io_t *io, const void *buf, bp_uint32 n)
{
    long w;
    if (n == 0) return BP_OK;
    w = io->write(io->handle, buf, n);
    return (w == (long)n) ? BP_OK : BP_ERR_IO;
}

static int mini_io_read(bp_io_t *io, void *buf, bp_uint32 n)
{
    long r;
    if (n == 0) return BP_OK;
    r = io->read(io->handle, buf, n);
    return (r == (long)n) ? BP_OK : BP_ERR_TRUNCATED;
}

static int mini_io_seek32(bp_io_t *io, bp_uint32 offset)
{
    return io->seek(io->handle, (bp_off_t)offset, 0) == 0 ? BP_OK : BP_ERR_IO;
}

static int u64_eq(bp_uint64 a, bp_uint64 b)
{
    return a.lo == b.lo && a.hi == b.hi;
}

/* ---- bp_mini_u64_print ------------------------------------------------ */

/*
 * Divide the 64-bit value (hi:lo) by 10 using base-65536 long division.
 * Each intermediate value is at most 10 * 65535 < 700000, which fits in
 * a bp_uint32 -- so this works correctly on 16-bit targets where
 * unsigned long is 32 bits.
 */
static void mini_u64_div10(bp_uint32 hi, bp_uint32 lo,
                           bp_uint32 *rhi, bp_uint32 *rlo,
                           bp_uint32 *rem)
{
    bp_uint32 d[4];
    bp_uint32 q[4];
    bp_uint32 r;
    bp_uint32 combined;
    int i;

    d[0] = (hi >> 16) & 0xFFFFU;
    d[1] = hi         & 0xFFFFU;
    d[2] = (lo >> 16) & 0xFFFFU;
    d[3] = lo         & 0xFFFFU;

    r = 0;
    for (i = 0; i < 4; i++) {
        combined = r * 65536UL + d[i];
        q[i]     = combined / 10UL;
        r        = combined % 10UL;
    }

    *rhi = (q[0] << 16) | q[1];
    *rlo = (q[2] << 16) | q[3];
    *rem = r;
}

#ifndef BP_MINI_QUIET
void bp_mini_u64_print(bp_uint64 v)
{
    char buf[21]; /* max 20 decimal digits + NUL */
    int pos = 20;
    bp_uint32 nhi;
    bp_uint32 nlo;
    bp_uint32 rem;

    buf[pos] = '\0';

    if (v.hi == 0 && v.lo == 0) {
        putchar('0');
        return;
    }

    nhi = v.hi;
    nlo = v.lo;

    while (nhi != 0 || nlo != 0) {
        mini_u64_div10(nhi, nlo, &nhi, &nlo, &rem);
        buf[--pos] = (char)('0' + (int)rem);
    }

    fputs(buf + pos, stdout);
}
#endif /* BP_MINI_QUIET */

/* ---- set IDs ----------------------------------------------------------- */

static bp_uint32 g_mini_entropy;
static bp_uint32 g_mini_counter;

void bp_mini_archive_add_entropy(bp_uint32 value)
{
    bp_uint8 b[8];
    bp_write_le32(b, g_mini_entropy);
    bp_write_le32(b + 4, value);
    g_mini_entropy = bp_mini_crc32(g_mini_entropy ^ 0x5A17C0DEUL, b, 8);
}

static bp_uint64 mini_new_set_id(const void *salt)
{
    bp_uint8 b[16];
    bp_uint64 id;
    unsigned long a = (unsigned long)(size_t)salt;

    g_mini_counter++;
    bp_write_le32(b, g_mini_entropy);
    bp_write_le32(b + 4, g_mini_counter);
    bp_write_le32(b + 8, (bp_uint32)(a & 0xFFFFFFFFUL));
    bp_write_le32(b + 12, (bp_uint32)(unsigned long)(size_t)&b);
    id.lo = bp_mini_crc32(0, b, 16);
    id.hi = bp_mini_crc32(id.lo ^ 0x9E3779B9UL, b, 16);
    if (id.lo == 0 && id.hi == 0) id.lo = 1;
    return id;
}

/* ---- structure encoders ------------------------------------------------ */

static void m7_vol_header(bp_uint8 *h, const bp_mini_archive_t *ar)
{
    memset(h, 0, (size_t)M7_VOL_HDR);
    h[0] = BP_MAGIC_0; h[1] = BP_MAGIC_1; h[2] = BP_MAGIC_2; h[3] = BP_MAGIC_3;
    bp_write_le16(h + 4, BP_FORMAT7_VERSION);
    bp_write_le64(h + 8, ar->set_id);
    bp_write_le16(h + 16, ar->number);
    h[18] = ar->creator_os;
    h[19] = ar->creator_cpu;
    h[20] = BP_VERSION_MAJOR;
    h[21] = BP_VERSION_MINOR;
    bp_write_le32(h + 28, bp_mini_crc32(0, h, 28));
}

static void m7_vol_trailer(bp_uint8 *t, const bp_mini_archive_t *ar, int last,
                           bp_uint32 size)
{
    memset(t, 0, (size_t)M7_VOL_TRL);
    bp_write_le64(t, ar->set_id);
    bp_write_le16(t + 8, ar->number);
    bp_write_le16(t + 10, (bp_uint16)(last ? M7_FLAG_LAST : 0));
    bp_write_le32(t + 16, size);
    bp_write_le32(t + 24, bp_mini_crc32(0, t, 24));
    memcpy(t + 28, m7_trl_magic, 4);
}

/* ---- writer: volumes --------------------------------------------------- */

static bp_uint32 mini_limit(const bp_mini_archive_t *ar)
{
    return ar->limit != 0 ? ar->limit : (bp_uint32)BP_MINI_MAX_ARCHIVE_SIZE;
}

/* Body space left in the current volume (the trailer is reserved). */
static bp_uint32 mini_space(const bp_mini_archive_t *ar)
{
    bp_uint32 lim = mini_limit(ar);
    if (ar->pos + M7_VOL_TRL >= lim) return 0;
    return lim - ar->pos - M7_VOL_TRL;
}

static int mini_body_empty(const bp_mini_archive_t *ar)
{
    return ar->pos == M7_VOL_HDR;
}

static int mini_write(bp_mini_archive_t *ar, const void *buf, bp_uint32 n)
{
    int rc;
    if (n > mini_limit(ar) - ar->pos) return BP_ERR_OVERFLOW;
    rc = mini_io_write(ar->io, buf, n);
    if (rc == BP_OK) ar->pos += n;
    return rc;
}

static int mini_open_volume(bp_mini_archive_t *ar)
{
    bp_uint8 h[32];
    bp_io_t *io = ar->sink.create(ar->sink.user, ar->number);
    if (io == NULL) return BP_ERR_IO;
    ar->io = io;
    ar->vol_open = 1;
    ar->pos = 0;
    m7_vol_header(h, ar);
    return mini_write(ar, h, M7_VOL_HDR);
}

static int mini_close_volume(bp_mini_archive_t *ar, int last)
{
    bp_uint8 t[32];
    int rc, frc;

    if (!ar->vol_open) return BP_OK;
    m7_vol_trailer(t, ar, last, ar->pos + M7_VOL_TRL);
    rc = mini_io_write(ar->io, t, M7_VOL_TRL);
    if (rc == BP_OK) ar->pos += M7_VOL_TRL;
    ar->vol_open = 0;
    if (!ar->multi) return rc;
    frc = ar->sink.finish(ar->sink.user, ar->io, ar->number, last, rc);
    ar->io = NULL;
    if (rc != BP_OK) return rc;
    if (frc != 0) return frc < 0 ? frc : BP_ERR_IO;
    return BP_OK;
}

static int mini_next_volume(bp_mini_archive_t *ar)
{
    int rc;
    if (!ar->multi || mini_body_empty(ar) || ar->number >= M7_MAX_VOLNUM)
        return BP_ERR_OVERFLOW;
    rc = mini_close_volume(ar, 0);
    if (rc != BP_OK) return rc;
    ar->number++;
    return mini_open_volume(ar);
}

/* Make room for a whole structure of `len` bytes. */
static int mini_reserve(bp_mini_archive_t *ar, bp_uint32 len)
{
    int rc;
    if (mini_space(ar) >= len) return BP_OK;
    rc = mini_next_volume(ar);
    if (rc != BP_OK) return rc;
    return mini_space(ar) >= len ? BP_OK : BP_ERR_OVERFLOW;
}

static int mini_put_struct(bp_mini_archive_t *ar, const bp_uint8 *buf,
                           bp_uint32 len, bp_uint16 *vol, bp_uint32 *off)
{
    int rc = mini_reserve(ar, len);
    if (rc != BP_OK) return rc;
    if (vol != NULL) *vol = ar->number;
    if (off != NULL) *off = ar->pos;
    return mini_write(ar, buf, len);
}

static int mini_write_block(bp_mini_archive_t *ar, const bp_uint8 *data,
                            bp_uint32 clen, bp_uint32 olen, bp_uint64 *total)
{
    bp_uint8 bh[12];
    int rc;
    bp_write_le32(bh, clen);
    bp_write_le32(bh + 4, olen);
    bp_write_le32(bh + 8, bp_mini_crc32(0, data, clen));
    rc = mini_write(ar, bh, M7_BLOCK_HDR);
    if (rc == BP_OK) rc = mini_write(ar, data, clen);
    if (rc == BP_OK)
        *total = bp_u64_add(*total, bp_u64_from_u32(M7_BLOCK_HDR + clen));
    return rc;
}

/* Compress src[0..len) on its own into out (raw under the 99% rule; the
 * data is then taken from src).  Sets *data to the bytes to write. */
static int mini_compress_piece(bp_uint8 method, const bp_uint8 *src,
                               bp_uint32 len, bp_uint8 *out,
                               bp_uint32 out_cap, const bp_uint8 **data,
                               bp_uint32 *dlen)
{
    bp_uint32 cl = out_cap;
    int rc;
    if (method == BP_METHOD_STORED) {
        *data = src; *dlen = len;
        return BP_OK;
    }
    rc = bp_mini_compress_block(src, len, out, &cl, method);
    if (rc != BP_OK) return rc;
    /* stored when compression saves less than 1% (len <= 32 KB, no overflow) */
    if (cl == 0 || cl > len * 99UL / 100UL) {
        *data = src; *dlen = len;
    } else {
        *data = out; *dlen = cl;
    }
    return BP_OK;
}

/* Place one block.  orig/olen are the original bytes, data/clen the encoded
 * block (data == orig when raw).  out is the caller's compression buffer,
 * whose contents are no longer needed once the block must be split.  A block
 * that does not fit is cut to fill the volume (spec 2.0 §2.3.2). */
static int mini_put_block(bp_mini_archive_t *ar, bp_uint8 method,
                          const bp_uint8 *orig, bp_uint32 olen,
                          const bp_uint8 *data, bp_uint32 clen,
                          bp_uint8 *out, bp_uint32 out_cap, bp_uint64 *total)
{
    int rc;

    for (;;) {
        bp_uint32 room = mini_space(ar);
        bp_uint32 best_a = 0, best_pl = 0, too_big, too_pl = 0, a, pl;
        bp_uint32 slack;
        const bp_uint8 *pd = NULL;
        int tries;

        if (room >= M7_BLOCK_HDR + clen)
            return mini_write_block(ar, data, clen, olen, total);
        if (!ar->multi) return BP_ERR_OVERFLOW;
        if (room < M7_EOV_SPACE) {
            if (mini_body_empty(ar)) return BP_ERR_OVERFLOW;
            rc = mini_next_volume(ar);
            if (rc != BP_OK) return rc;
            continue;
        }
        room -= M7_BLOCK_HDR;   /* room < clen here, so the products below fit 32 bits */

        /* largest piece of orig that, compressed on its own, fits `room` */
        too_big = olen + 1UL;
        slack = room / 256UL < 128UL ? 128UL : room / 256UL;
        a = clen ? (bp_uint32)((room * (olen / 8UL + 1UL)) / clen) * 8UL : room;
        for (tries = 0; tries < 10; tries++) {
            if (best_a > 0 && too_pl > best_pl && tries % 3 != 2)
                /* interpolate between the bracket ends: best_pl is more
                 * than `slack` below room, so the subtraction is positive */
                a = best_a + ((room - best_pl) - slack / 2UL) *
                    (too_big - best_a) / (too_pl - best_pl);
            else if (best_a > 0 && too_pl > 0)
                a = best_a + (too_big - best_a) / 2UL;
            if (a >= too_big) a = too_big - 1UL;
            if (a <= best_a) a = best_a + (too_big - best_a) / 2UL;
            if (a <= best_a || a >= too_big) break;
            rc = mini_compress_piece(method, orig, a, out, out_cap, &pd, &pl);
            if (rc != BP_OK) return rc;
            if (pl <= room) {
                best_a = a; best_pl = pl;
                if (a == olen || room - pl <= slack) break;
                a = (bp_uint32)(((bp_uint32)(a / 4UL) * room / (pl ? pl : 1UL)) * 4UL);
            } else {
                too_big = a; too_pl = pl;
                a = (bp_uint32)(((bp_uint32)(a / 4UL) * (room - slack / 2UL) / pl) * 4UL);
            }
        }
        if (best_a < room && best_a < olen) {
            /* a raw piece always fits exactly and carries more data */
            best_a = room < olen ? room : olen;
            pd = orig; pl = best_a;
        } else {
            rc = mini_compress_piece(method, orig, best_a, out, out_cap, &pd, &pl);
            if (rc != BP_OK) return rc;
            if (pl > room) return BP_ERR_COMPRESS;
        }
        rc = mini_write_block(ar, pd, pl, best_a, total);
        if (rc != BP_OK) return rc;
        orig += best_a;
        olen -= best_a;
        if (olen == 0) return BP_OK;
        rc = mini_next_volume(ar);
        if (rc != BP_OK) return rc;
        rc = mini_compress_piece(method, orig, olen, out, out_cap, &data, &clen);
        if (rc != BP_OK) return rc;
    }
}

/* ---- writer: entries --------------------------------------------------- */

/* The table is grown with one realloc, whose size parameter is size_t.  On
 * 16-bit compact-model builds size_t is 16 bits, so do not let the byte count
 * wrap while retaining the format's wider 65,535-entry limit. */
static bp_uint32 mini_entry_list_max_capacity(void)
{
    size_t size_cap = (size_t)-1 / sizeof(bp_mini_entry_t);
#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
    return (bp_uint32)size_cap;
#else
    if (size_cap > (size_t)BP_MINI_MAX_ENTRIES)
        return BP_MINI_MAX_ENTRIES;
    return (bp_uint32)size_cap;
#endif
}

static int mini_entry_list_grow(bp_mini_archive_t *ar)
{
    bp_uint32 new_cap;
    bp_uint32 max_cap;
    bp_mini_entry_t *new_list;

    if (ar->entries_written < ar->entry_list_cap) return BP_OK;
    new_cap = ar->entry_list_cap ? ar->entry_list_cap * 2 : 64;
    max_cap = mini_entry_list_max_capacity();
    if (new_cap > max_cap) new_cap = max_cap;
    if (new_cap <= ar->entry_list_cap) return BP_ERR_OVERFLOW;
    new_list = (bp_mini_entry_t *)realloc(ar->entry_list,
                                          (size_t)new_cap * sizeof(bp_mini_entry_t));
    if (new_list == NULL) return BP_ERR_NOMEM;
    ar->entry_list     = new_list;
    ar->entry_list_cap = new_cap;
    return BP_OK;
}

static int mini_init(bp_mini_archive_t *ar, bp_uint32 block_size,
                     bp_uint8 method)
{
    if (method > BP_METHOD_BOLTLZ) return BP_ERR_UNSUPPORTED_METHOD;
#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
    /* boltlz compression is not available on 16-bit systems */
    if (method == BP_METHOD_BOLTLZ) return BP_ERR_UNSUPPORTED_METHOD;
#endif
    memset(ar, 0, sizeof(*ar));
    if (block_size == 0) block_size = BP_BLOCK_16K;
    if (block_size > BP_MINI_MAX_BLOCK_SIZE) block_size = BP_MINI_MAX_BLOCK_SIZE;
    ar->block_size = block_size;
    ar->method     = method;
#ifdef BP_CREATOR_OS
    ar->creator_os = BP_CREATOR_OS;
#else
#  if defined(__WATCOMC__) && defined(__DOS__)
    ar->creator_os = BP_OS_DOS;
#  elif defined(__WATCOMC__) && defined(__OS2__)
    ar->creator_os = BP_OS_OS2;
#  elif defined(__WATCOMC__) && defined(__WINDOWS__)
    ar->creator_os = BP_OS_WIN16;
#  elif defined(CPM)
    ar->creator_os = BP_OS_CPM86;
#  else
    ar->creator_os = BP_OS_UNKNOWN;
#  endif
#endif
#ifdef BP_CREATOR_CPU
    ar->creator_cpu = BP_CREATOR_CPU;
#else
    ar->creator_cpu = BP_CPU_8086;
#endif
    ar->set_id = mini_new_set_id(ar);
    return BP_OK;
}

int bp_mini_archive_create(bp_mini_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method)
{
    bp_uint8 h[32];
    int rc;
    BP_STACK_PROBE();

    if (ar == NULL || io == NULL) return BP_ERR_INVALID_ARG;
    rc = mini_init(ar, block_size, method);
    if (rc != BP_OK) return rc;
    ar->io = io;
    ar->vol_open = 1;
    m7_vol_header(h, ar);
    return mini_write(ar, h, M7_VOL_HDR);
}

int bp_mini_archive_create_volumes(bp_mini_archive_t *ar,
                                   const bp_volume_sink_t *sink,
                                   bp_uint32 volume_size,
                                   bp_uint32 block_size, bp_uint8 method)
{
    int rc;
    BP_STACK_PROBE();

    if (ar == NULL || sink == NULL || sink->create == NULL ||
        sink->finish == NULL)
        return BP_ERR_INVALID_ARG;
    if (volume_size != 0 &&
        (volume_size < M7_MIN_VOLUME || volume_size > BP_MINI_MAX_ARCHIVE_SIZE))
        return BP_ERR_INVALID_ARG;
    rc = mini_init(ar, block_size, method);
    if (rc != BP_OK) return rc;
    ar->multi = 1;
    ar->sink = *sink;
    ar->limit = volume_size;
    rc = mini_open_volume(ar);
    if (rc != BP_OK && ar->vol_open) {
        (void)ar->sink.finish(ar->sink.user, ar->io, ar->number, 0, rc);
        ar->vol_open = 0;
    }
    return rc;
}

int bp_mini_archive_add_entry_ex(bp_mini_archive_t *ar, const char *name,
                                 bp_uint8 entry_type, bp_uint8 platform,
                                 bp_io_t *file_io, bp_uint64 file_size,
                                 bp_uint64 mtime, bp_uint16 flags)
{
    bp_uint16 name_len, hdr_size, loc_vol = 0;
    bp_uint32 loc_off = 0;
    bp_uint8 *hdr;
    bp_uint8 trl[16];
    bp_mini_entry_t *ent;
    bp_uint64 total_comp;
    bp_uint32 crc;
    bp_uint8  method;
    bp_uint32 block_size;
    int rc;
    BP_STACK_PROBE();

    if (ar == NULL || name == NULL || ar->finalized || !ar->vol_open)
        return BP_ERR_INVALID_ARG;
    if (ar->entries_written >= BP_MINI_MAX_ENTRIES) {
#ifndef BP_MINI_QUIET
        fprintf(stderr, "bpmini: archive entry limit (%u) reached\n",
                (unsigned)BP_MINI_MAX_ENTRIES);
#endif
        return BP_ERR_OVERFLOW;
    }
    if (file_size.hi != 0) {
#ifndef BP_MINI_QUIET
        fprintf(stderr, "bpmini: file too large for mini (> 4 GB): %s\n", name);
#endif
        return BP_ERR_OVERFLOW;
    }
    if ((unsigned long)strlen(name) > BP_MAX_NAME_LEN) return BP_ERR_NAME_TOO_LONG;
    name_len = (bp_uint16)strlen(name);
    if (!bp_archive_name_valid(name, (bp_uint32)name_len) ||
        !bp_archive_name_type_valid(name, (bp_uint32)name_len, entry_type) ||
        (!(flags & BP_ENTRY_FLAG_MTIME_VALID) && !bp_u64_is_zero(mtime)) ||
        (entry_type == BP_TYPE_DIR && !bp_u64_is_zero(file_size)) ||
        (!bp_u64_is_zero(file_size) && file_io == NULL))
        return BP_ERR_INVALID_ARG;

    rc = mini_entry_list_grow(ar);
    if (rc != BP_OK) return rc;

    method = (entry_type == BP_TYPE_DIR || entry_type == BP_TYPE_SYMLINK ||
              bp_u64_is_zero(file_size)) ? BP_METHOD_STORED : ar->method;
    block_size = bp_u64_is_zero(file_size) ? 0 : ar->block_size;

    /* entry header: everything known before the data */
    hdr_size = (bp_uint16)(M7_ENTRY_FIXED + name_len);
    hdr = (bp_uint8 *)malloc((size_t)hdr_size);
    if (hdr == NULL) return BP_ERR_NOMEM;
    bp_write_le16(hdr, hdr_size);
    bp_write_le16(hdr + 2, name_len);
    memcpy(hdr + 4, name, (size_t)name_len);
    bp_write_le64(hdr + 4 + name_len, file_size);
    hdr[12 + name_len] = method;
    hdr[13 + name_len] = BP_CKSUM_CRC32;
    bp_write_le32(hdr + 14 + name_len, block_size);
    hdr[18 + name_len] = entry_type;
    hdr[19 + name_len] = platform;
    bp_write_le64(hdr + 20 + name_len, mtime);
    bp_write_le16(hdr + 28 + name_len, flags);
    bp_write_le32(hdr + 30 + name_len,
                  bp_mini_crc32(0, hdr, (bp_uint32)hdr_size - 4UL));
    rc = mini_put_struct(ar, hdr, hdr_size, &loc_vol, &loc_off);
    free(hdr);
    if (rc != BP_OK) return rc;

    total_comp.lo = 0; total_comp.hi = 0;
    crc = bp_mini_checksum_seed(BP_CKSUM_CRC32);

    if (!bp_u64_is_zero(file_size)) {
        bp_uint32 remaining = file_size.lo;
        bp_uint32 bsize = ar->block_size;
        bp_uint32 out_cap = bsize + bsize / 8 + 16;
        bp_uint8 *in_buf, *out_buf;

        if (bsize + bsize / 63 + 16 > out_cap) out_cap = bsize + bsize / 63 + 16;
        in_buf  = (bp_uint8 *)malloc((size_t)bsize);
        out_buf = (bp_uint8 *)malloc((size_t)out_cap);
        if (in_buf == NULL || out_buf == NULL) {
            free(in_buf); free(out_buf);
            return BP_ERR_NOMEM;
        }
        while (remaining != 0) {
            bp_uint32 chunk = remaining > bsize ? bsize : remaining;
            const bp_uint8 *data;
            bp_uint32 dlen;
            long n = file_io->read(file_io->handle, in_buf, chunk);
            if (n != (long)chunk) { rc = BP_ERR_IO; break; }
            crc = bp_mini_checksum_typed(BP_CKSUM_CRC32, crc, in_buf, chunk);
            rc = mini_compress_piece(method, in_buf, chunk, out_buf, out_cap,
                                     &data, &dlen);
            if (rc == BP_OK)
                rc = mini_put_block(ar, method, in_buf, chunk, data, dlen,
                                    out_buf, out_cap, &total_comp);
            if (rc != BP_OK) break;
            remaining -= chunk;
        }
        free(in_buf);
        free(out_buf);
        if (rc != BP_OK) return rc;
    }

    bp_write_le64(trl, total_comp);
    bp_write_le32(trl + 8, crc);
    bp_write_le32(trl + 12, bp_mini_crc32(0, trl, 12));
    rc = mini_put_struct(ar, trl, M7_ENTRY_TRL, NULL, NULL);
    if (rc != BP_OK) return rc;

    ar->archive_orig_total = bp_u64_add(ar->archive_orig_total, file_size);
    ar->archive_comp_total = bp_u64_add(ar->archive_comp_total, total_comp);

    ent = &ar->entry_list[ar->entries_written];
    memset(ent, 0, sizeof(*ent));
    ent->name = (char *)malloc((size_t)(name_len + 1));
    if (ent->name == NULL) return BP_ERR_NOMEM;
    memcpy(ent->name, name, (size_t)name_len);
    ent->name[name_len]  = '\0';
    ent->name_len        = name_len;
    ent->original_size   = file_size;
    ent->compressed_size = total_comp;
    ent->checksum        = crc;
    ent->method          = method;
    ent->checksum_type   = BP_CKSUM_CRC32;
    ent->block_size      = block_size;
    ent->entry_type      = entry_type;
    ent->platform        = platform;
    ent->mtime           = mtime;
    ent->flags           = flags;
    ent->volume          = loc_vol;
    ent->offset          = bp_u64_from_u32(loc_off);
    ar->entries_written++;
    return BP_OK;
}

int bp_mini_archive_add_entry(bp_mini_archive_t *ar, const char *name,
                              bp_uint8 entry_type, bp_uint8 platform,
                              bp_io_t *file_io, bp_uint64 file_size,
                              bp_uint64 mtime)
{
    return bp_mini_archive_add_entry_ex(ar, name, entry_type, platform,
                                        file_io, file_size, mtime,
                                        BP_ENTRY_FLAG_MTIME_VALID);
}

/* ---- writer: table, end record, finalize ------------------------------- */

static void mini_build_record_tail(bp_uint8 *t, const bp_mini_entry_t *e)
{
    bp_write_le64(t + 0, e->original_size);
    bp_write_le64(t + 8, e->compressed_size);
    t[16] = e->entry_type;
    t[17] = e->platform;
    bp_write_le64(t + 18, e->mtime);
    bp_write_le16(t + 26, e->volume);
    bp_write_le64(t + 28, e->offset);
    t[36] = e->method;
    bp_write_le16(t + 37, (bp_uint16)(M7_ENTRY_FIXED + e->name_len));
    bp_write_le32(t + 39, e->checksum);
    bp_write_le16(t + 43, e->flags);
}

int bp_mini_archive_finalize(bp_mini_archive_t *ar)
{
    bp_uint32 i, crc = 0, table_bytes = M7_TABLE_HDR;
    bp_uint64 total_orig;
    bp_uint8  buf[48];
    bp_uint8 *rec = NULL;
    bp_uint32 rec_cap = 0;
    bp_uint16 tvol = 0;
    bp_uint32 toff = 0;
    int rc;
    BP_STACK_PROBE();

    if (ar == NULL || ar->finalized || !ar->vol_open) return BP_ERR_INVALID_ARG;
    {
        bp_metadata_group_t group = { NULL, 0, 0, NULL, 0 };
        for (i = 0; i < ar->entries_written; i++) {
            char *name = ar->entry_list[i].name;
            rc = bp_metadata_group_record(&group, &name,
                                          ar->entry_list[i].entry_type,
                                          ar->entry_list[i].platform, 0);
            if (rc != BP_OK) {
                bp_metadata_group_close(&group);
                return rc == BP_ERR_CORRUPT ? BP_ERR_INVALID_ARG : rc;
            }
        }
        bp_metadata_group_close(&group);
    }

    total_orig.lo = 0; total_orig.hi = 0;
    for (i = 0; i < ar->entries_written; i++) {
        bp_uint32 rs = 2UL + ar->entry_list[i].name_len + M7_REC_TAIL;
        if (table_bytes + rs < table_bytes) return BP_ERR_OVERFLOW;
        table_bytes += rs;
        total_orig = bp_u64_add(total_orig, ar->entry_list[i].original_size);
    }
    if (table_bytes > (bp_uint32)BP_MINI_MAX_ARCHIVE_SIZE) return BP_ERR_OVERFLOW;

    /* Keep the whole table in the last volume when a fresh volume can hold
     * it but the current one cannot. */
    if (ar->multi && ar->limit != 0 && !mini_body_empty(ar) &&
        table_bytes + M7_END_REC > mini_space(ar) &&
        table_bytes + M7_END_REC <= ar->limit - M7_VOL_HDR - M7_VOL_TRL) {
        rc = mini_next_volume(ar);
        if (rc != BP_OK) return rc;
    }

    buf[0] = BP_TABLE_MAGIC_0; buf[1] = BP_TABLE_MAGIC_1;
    buf[2] = BP_TABLE_MAGIC_2; buf[3] = BP_TABLE_MAGIC_3;
    bp_write_le32(buf + 4, ar->entries_written);
    bp_write_le64(buf + 8, total_orig);
    rc = mini_put_struct(ar, buf, M7_TABLE_HDR, &tvol, &toff);
    if (rc != BP_OK) return rc;
    crc = bp_mini_crc32(crc, buf, M7_TABLE_HDR);

    for (i = 0; i < ar->entries_written; i++) {
        const bp_mini_entry_t *e = &ar->entry_list[i];
        bp_uint32 rs = 2UL + e->name_len + M7_REC_TAIL;
        if (rs > rec_cap) {
            free(rec);
            rec = (bp_uint8 *)malloc((size_t)rs);
            if (rec == NULL) return BP_ERR_NOMEM;
            rec_cap = rs;
        }
        bp_write_le16(rec, e->name_len);
        memcpy(rec + 2, e->name, (size_t)e->name_len);
        mini_build_record_tail(rec + 2 + e->name_len, e);
        rc = mini_put_struct(ar, rec, rs, NULL, NULL);
        if (rc != BP_OK) { free(rec); return rc; }
        crc = bp_mini_crc32(crc, rec, rs);
    }
    free(rec);

    rc = mini_reserve(ar, M7_END_REC);
    if (rc != BP_OK) return rc;
    memset(buf, 0, sizeof(buf));
    bp_write_le64(buf, ar->set_id);
    bp_write_le16(buf + 8, (bp_uint16)(ar->number + 1));
    bp_write_le16(buf + 10, tvol);
    bp_write_le32(buf + 12, crc);
    bp_write_le32(buf + 16, toff);
    bp_write_le32(buf + 24, table_bytes);
    bp_write_le32(buf + 32, ar->entries_written);
    bp_write_le32(buf + 40, bp_mini_crc32(0, buf, 40));
    memcpy(buf + 44, m7_end_magic, 4);
    rc = mini_write(ar, buf, M7_END_REC);
    if (rc != BP_OK) return rc;
    rc = mini_close_volume(ar, 1);
    if (rc == BP_OK) ar->finalized = 1;
    return rc;
}

void bp_mini_archive_close(bp_mini_archive_t *ar)
{
    bp_uint32 i;

    if (ar == NULL) return;
    if (ar->multi && ar->vol_open) {
        (void)ar->sink.finish(ar->sink.user, ar->io, ar->number, 0,
                              BP_ERR_ABORTED);
        ar->vol_open = 0;
    }
    if (ar->entry_list) {
        for (i = 0; i < ar->entries_written; i++) free(ar->entry_list[i].name);
        free(ar->entry_list);
        ar->entry_list = NULL;
    }
}

/* ---- reading: volumes -------------------------------------------------- */

typedef struct {
    bp_uint32 digest;     /* CRC-32 of the table record without size/checksum */
    bp_uint32 comp;       /* compressed size (mini entries stay below 4 GB) */
    bp_uint32 checksum;
} mini_dig_t;

#define MINI_DIG_CHUNK 1024UL  /* 12 KB per chunk: fits 16-bit allocations */

typedef struct {
    bp_volume_source_t src;
    bp_io_t   *io;
    int        cur;           /* current volume, -1 = none */
    bp_uint32  size, body_end, pos;
    int        have_set;
    bp_uint64  set_id;
    bp_uint16  count, last;   /* count 0 until the end record is read */
    bp_uint8   creator_os, creator_cpu, ver_major, ver_minor;
    bp_uint32  last_size, end_offset;
    bp_uint16  table_volume;
    bp_uint32  table_crc, table_offset, table_size, entry_count;
    mini_dig_t **chunks;
    bp_uint32  nchunks;
    /* read-ahead: small structures come from g_mvr_buf, so a table record
     * costs no DOS call of its own; io_pos is the file position, or
     * MVR_UNKNOWN after the volume was opened or checked */
    bp_uint32  io_pos;
    bp_uint32  buf_pos;       /* file offset of g_mvr_buf[0] */
    bp_uint32  buf_len;       /* valid bytes, 0 = empty */
} mvr_t;

#define MVR_BUF      512UL
#define MVR_UNKNOWN  0xFFFFFFFFUL
static bp_uint8 g_mvr_buf[MVR_BUF];   /* one reader at a time */

static void mvr_close_io(mvr_t *vr)
{
    if (vr->io != NULL && vr->src.close != NULL)
        vr->src.close(vr->src.user, vr->io);
    vr->io = NULL;
    vr->cur = -1;
}

static void mvr_free(mvr_t *vr)
{
    bp_uint32 i;
    mvr_close_io(vr);
    if (vr->chunks != NULL) {
        for (i = 0; i < vr->nchunks; i++) free(vr->chunks[i]);
        free(vr->chunks);
        vr->chunks = NULL;
    }
}

static void mvr_init(mvr_t *vr, const bp_volume_source_t *src)
{
    memset(vr, 0, sizeof(*vr));
    vr->src = *src;
    vr->cur = -1;
}

typedef struct {
    bp_uint16 number;
    int       last;
    bp_uint32 size;
    bp_uint64 set_id;
    bp_uint8  hdr[32];
} mvol_t;

/* Validate the header and trailer of one volume stream. */
static int mini_check_volume(bp_io_t *io, mvol_t *vi)
{
    bp_uint8 t[32];
    bp_uint8 *h = vi->hdr;
    bp_off_t end;
    bp_uint16 tflags;
    int i;
    BP_STACK_PROBE();

    if (io->seek(io->handle, 0, 0) != 0) return BP_ERR_IO;
    if (mini_io_read(io, h, M7_VOL_HDR) != BP_OK) return BP_ERR_MAGIC;
    if (h[0] != BP_MAGIC_0 || h[1] != BP_MAGIC_1 || h[2] != BP_MAGIC_2 ||
        h[3] != BP_MAGIC_3)
        return BP_ERR_MAGIC;
    if (bp_read_le16(h + 4) != BP_FORMAT7_VERSION) return BP_ERR_VERSION;
    if (bp_read_le32(h + 28) != bp_mini_crc32(0, h, 28) ||
        bp_read_le16(h + 6) != 0)
        return BP_ERR_CORRUPT;
    for (i = 22; i < 28; i++) if (h[i] != 0) return BP_ERR_CORRUPT;
    vi->set_id = bp_read_le64(h + 8);
    vi->number = bp_read_le16(h + 16);
    if (bp_u64_is_zero(vi->set_id) || vi->number > M7_MAX_VOLNUM)
        return BP_ERR_CORRUPT;
    if (io->seek(io->handle, 0, 2) != 0) return BP_ERR_IO;
    end = io->tell(io->handle);
    if (end < 0) return BP_ERR_IO;
    if ((unsigned long)end > BP_MINI_MAX_ARCHIVE_SIZE) return BP_ERR_OVERFLOW;
    vi->size = (bp_uint32)end;
    if (vi->size < M7_VOL_HDR + M7_VOL_TRL + 1UL) return BP_ERR_TRUNCATED;
    if (mini_io_seek32(io, vi->size - M7_VOL_TRL) != BP_OK) return BP_ERR_IO;
    if (mini_io_read(io, t, M7_VOL_TRL) != BP_OK) return BP_ERR_TRUNCATED;
    if (memcmp(t + 28, m7_trl_magic, 4) != 0 ||
        bp_read_le32(t + 24) != bp_mini_crc32(0, t, 24))
        return BP_ERR_TRUNCATED;
    tflags = bp_read_le16(t + 10);
    if ((tflags & ~M7_FLAG_LAST) != 0 || bp_read_le32(t + 12) != 0)
        return BP_ERR_CORRUPT;
    if (!u64_eq(bp_read_le64(t), vi->set_id) || bp_read_le16(t + 8) != vi->number)
        return BP_ERR_CORRUPT;
    if (bp_read_le32(t + 20) != 0 || bp_read_le32(t + 16) != vi->size)
        return BP_ERR_TRUNCATED;
    vi->last = (tflags & M7_FLAG_LAST) != 0;
    return BP_OK;
}

/* Make volume `req` (or the last one) current, retrying through the source
 * with the reason a stream was rejected.  The last volume is always asked
 * for as BP_VOLUME_LAST_REQUEST: it carries the archive name. */
static int mvr_load(mvr_t *vr, bp_uint16 req)
{
    int reason = BP_OK;
    mvol_t vi;

    if (vr->io != NULL) {
        if (req == BP_VOLUME_LAST_REQUEST && vr->count != 0 &&
            vr->cur == (int)vr->last)
            return BP_OK;
        if (req != BP_VOLUME_LAST_REQUEST && vr->cur == (int)req) return BP_OK;
    }
    mvr_close_io(vr);
    if (req == BP_VOLUME_LAST_REQUEST && vr->count != 0) req = vr->last;
    for (;;) {
        bp_uint16 ask = (vr->count != 0 && req == vr->last)
                        ? (bp_uint16)BP_VOLUME_LAST_REQUEST : req;
        bp_io_t *io = vr->src.open(vr->src.user, ask, reason);
        int rc;
        if (io == NULL) return reason != BP_OK ? reason : BP_ERR_VOLUME;
        rc = mini_check_volume(io, &vi);
        if (rc == BP_OK && vr->have_set && !u64_eq(vi.set_id, vr->set_id))
            rc = BP_ERR_VOLUME;
        if (rc == BP_OK && req == BP_VOLUME_LAST_REQUEST && !vi.last)
            rc = BP_ERR_VOLUME;
        if (rc == BP_OK && req != BP_VOLUME_LAST_REQUEST && vi.number != req)
            rc = BP_ERR_VOLUME;
        if (rc == BP_OK && vr->count != 0 &&
            (vi.last != 0) != (vi.number == vr->last))
            rc = BP_ERR_CORRUPT;
        if (rc == BP_OK) {
            vr->io = io;
            vr->cur = (int)vi.number;
            vr->size = vi.size;
            vr->body_end = vi.size - M7_VOL_TRL;
            vr->pos = vi.size;
            vr->io_pos = MVR_UNKNOWN;
            vr->buf_len = 0;
            if (!vr->have_set) { vr->have_set = 1; vr->set_id = vi.set_id; }
            if (vi.last) {
                vr->creator_os = vi.hdr[18];
                vr->creator_cpu = vi.hdr[19];
                vr->ver_major = vi.hdr[20];
                vr->ver_minor = vi.hdr[21];
            }
            return BP_OK;
        }
        if (vr->src.close != NULL) vr->src.close(vr->src.user, io);
        reason = rc;
    }
}

static int mvr_seek(mvr_t *vr, bp_uint16 vol, bp_uint32 off)
{
    int rc = mvr_load(vr, vol);
    if (rc != BP_OK) return rc;
    if (off < M7_VOL_HDR || off > vr->body_end) return BP_ERR_CORRUPT;
    vr->pos = off;          /* the file is positioned on the next read */
    return BP_OK;
}

/* Read n bytes at file offset `at` of the current volume. */
static int mvr_raw_read(mvr_t *vr, bp_uint32 at, void *buf, bp_uint32 n)
{
    int rc;
    if (vr->io_pos != at) {
        rc = mini_io_seek32(vr->io, at);
        if (rc != BP_OK) { vr->io_pos = MVR_UNKNOWN; return rc; }
        vr->io_pos = at;
    }
    rc = mini_io_read(vr->io, buf, n);
    vr->io_pos = rc == BP_OK ? at + n : MVR_UNKNOWN;
    return rc;
}

/* Position at the next structure, moving on when the body is used up. */
static int mvr_begin(mvr_t *vr)
{
    if (vr->pos < vr->body_end) return BP_OK;
    if (vr->cur < 0 || vr->count == 0 || vr->cur >= (int)vr->last)
        return BP_ERR_CORRUPT;
    return mvr_seek(vr, (bp_uint16)(vr->cur + 1), M7_VOL_HDR);
}

static int mvr_avail(const mvr_t *vr, bp_uint32 len)
{
    return vr->body_end - vr->pos >= len;
}

static int mvr_read(mvr_t *vr, void *buf, bp_uint32 len)
{
    int rc;
    if (!mvr_avail(vr, len)) return BP_ERR_CORRUPT;
    if (len == 0) return BP_OK;
    if (len >= MVR_BUF) {
        /* block data: straight into the caller's buffer */
        rc = mvr_raw_read(vr, vr->pos, buf, len);
        if (rc == BP_OK) vr->pos += len;
        return rc;
    }
    if (vr->buf_len == 0 || vr->pos < vr->buf_pos ||
        vr->pos + len > vr->buf_pos + vr->buf_len) {
        bp_uint32 n = vr->body_end - vr->pos;
        if (n > MVR_BUF) n = MVR_BUF;
        vr->buf_len = 0;
        rc = mvr_raw_read(vr, vr->pos, g_mvr_buf, n);
        if (rc != BP_OK) return rc;
        vr->buf_pos = vr->pos;
        vr->buf_len = n;
    }
    memcpy(buf, g_mvr_buf + (vr->pos - vr->buf_pos), (size_t)len);
    vr->pos += len;
    return BP_OK;
}

static int mvr_skip(mvr_t *vr, bp_uint32 len)
{
    if (!mvr_avail(vr, len)) return BP_ERR_CORRUPT;
    vr->pos += len;
    return BP_OK;
}

static void mvr_next_location(const mvr_t *vr, bp_uint16 *vol, bp_uint32 *off)
{
    if (vr->pos >= vr->body_end) {
        *vol = (bp_uint16)(vr->cur + 1);
        *off = M7_VOL_HDR;
    } else {
        *vol = (bp_uint16)vr->cur;
        *off = vr->pos;
    }
}

/* Open the set: last volume and end record. */
static int mvr_open_set(mvr_t *vr)
{
    bp_uint8 e[48];
    int rc;
    BP_STACK_PROBE();

    rc = mvr_load(vr, BP_VOLUME_LAST_REQUEST);
    if (rc != BP_OK) return rc;
    vr->last = (bp_uint16)vr->cur;
    vr->last_size = vr->size;
    if (vr->body_end < M7_VOL_HDR + M7_END_REC) return BP_ERR_CORRUPT;
    vr->end_offset = vr->body_end - M7_END_REC;
    rc = mvr_raw_read(vr, vr->end_offset, e, M7_END_REC);
    if (rc != BP_OK) return rc;
    vr->pos = vr->body_end;
    if (memcmp(e + 44, m7_end_magic, 4) != 0 ||
        bp_read_le32(e + 40) != bp_mini_crc32(0, e, 40) ||
        bp_read_le32(e + 36) != 0 || !u64_eq(bp_read_le64(e), vr->set_id))
        return BP_ERR_CORRUPT;
    vr->count = bp_read_le16(e + 8);
    if (vr->count == 0 || vr->count > BP_VOLUME_MAX_COUNT ||
        vr->count != (bp_uint16)(vr->last + 1))
        return BP_ERR_CORRUPT;
    vr->table_volume = bp_read_le16(e + 10);
    vr->table_crc = bp_read_le32(e + 12);
    if (bp_read_le32(e + 20) != 0 || bp_read_le32(e + 28) != 0)
        return BP_ERR_OVERFLOW;           /* beyond any mini volume */
    vr->table_offset = bp_read_le32(e + 16);
    vr->table_size = bp_read_le32(e + 24);
    vr->entry_count = bp_read_le32(e + 32);
    if (vr->table_volume >= vr->count || vr->table_offset < M7_VOL_HDR ||
        vr->table_size < M7_TABLE_HDR ||
        (vr->table_volume == vr->last && vr->table_offset > vr->end_offset) ||
        vr->entry_count > (vr->table_size - M7_TABLE_HDR) / (3UL + M7_REC_TAIL))
        return BP_ERR_CORRUPT;
    return BP_OK;
}

/* ---- reading: file table ----------------------------------------------- */

/* Per-record callback of mvr_scan_table: return nonzero to stop. */
typedef int (*mvr_record_fn)(mvr_t *vr, bp_mini_entry_t *entry,
                             bp_uint32 index, bp_uint32 digest, void *user);

static int loc_after(bp_uint16 av, bp_uint32 ao, bp_uint16 bv, bp_uint32 bo)
{
    return av > bv || (av == bv && ao > bo);
}

/* What mvr_scan_table checks besides record structure and order. */
#define MVR_SCAN_CRC     1   /* the table CRC and size */
#define MVR_SCAN_DIGEST  2   /* per-record digests for fn */

/* Stream the file table once: validate every record and its order (and,
 * per `check`, the table CRC), and hand each record to fn (fn may be NULL).
 * entry->name is owned by this function unless fn sets it to NULL. */
static int mvr_scan_table(mvr_t *vr, mvr_record_fn fn, void *user,
                          int *stopped, int check)
{
    bp_uint8 th[16], nl[2], tail[45];
    bp_uint32 crc = 0, bytes = M7_TABLE_HDR, i;
    bp_uint16 prev_vol = 0, nv;
    bp_uint32 prev_off = 0, no;
    int rc;
    BP_STACK_PROBE();

    if (stopped != NULL) *stopped = 0;
    rc = mvr_seek(vr, vr->table_volume, vr->table_offset);
    if (rc == BP_OK) rc = mvr_read(vr, th, M7_TABLE_HDR);
    if (rc != BP_OK) return rc;
    if (th[0] != BP_TABLE_MAGIC_0 || th[1] != BP_TABLE_MAGIC_1 ||
        th[2] != BP_TABLE_MAGIC_2 || th[3] != BP_TABLE_MAGIC_3 ||
        bp_read_le32(th + 4) != vr->entry_count)
        return BP_ERR_CORRUPT;
    if (check & MVR_SCAN_CRC) crc = bp_mini_crc32(crc, th, M7_TABLE_HDR);

    for (i = 0; i < vr->entry_count; i++) {
        bp_mini_entry_t e;
        bp_uint32 digest;
        bp_uint32 off;

        rc = mvr_begin(vr);
        if (rc == BP_OK) rc = mvr_read(vr, nl, 2);
        if (rc != BP_OK) return rc;
        memset(&e, 0, sizeof(e));
        e.name_len = bp_read_le16(nl);
        if (e.name_len == 0 || e.name_len > M7_MAX_NAME ||
            !mvr_avail(vr, (bp_uint32)e.name_len + M7_REC_TAIL))
            return BP_ERR_CORRUPT;
        e.name = (char *)malloc((size_t)e.name_len + 1);
        if (e.name == NULL) return BP_ERR_NOMEM;
        rc = mvr_read(vr, e.name, e.name_len);
        if (rc == BP_OK) rc = mvr_read(vr, tail, M7_REC_TAIL);
        if (rc != BP_OK) { free(e.name); return rc; }
        e.name[e.name_len] = '\0';
        if (check & MVR_SCAN_CRC) {
            crc = bp_mini_crc32(crc, nl, 2);
            crc = bp_mini_crc32(crc, (const bp_uint8 *)e.name, e.name_len);
            crc = bp_mini_crc32(crc, tail, M7_REC_TAIL);
        }
        bytes += 2UL + e.name_len + M7_REC_TAIL;

        /* digest of everything the entry header and location repeat */
        digest = 0;
        if (check & MVR_SCAN_DIGEST) {
            digest = bp_mini_crc32(0, nl, 2);
            digest = bp_mini_crc32(digest, (const bp_uint8 *)e.name, e.name_len);
            digest = bp_mini_crc32(digest, tail, 8);
            digest = bp_mini_crc32(digest, tail + 16, 23);
            digest = bp_mini_crc32(digest, tail + 43, 2);
        }

        e.original_size = bp_read_le64(tail);
        e.compressed_size = bp_read_le64(tail + 8);
        e.entry_type = tail[16];
        e.platform = tail[17];
        e.mtime = bp_read_le64(tail + 18);
        e.volume = bp_read_le16(tail + 26);
        e.offset = bp_read_le64(tail + 28);
        e.method = tail[36];
        e.checksum = bp_read_le32(tail + 39);
        e.flags = bp_read_le16(tail + 43);
        e.checksum_type = BP_CKSUM_CRC32;
        off = e.offset.lo;
        if (!bp_archive_name_valid(e.name, e.name_len) ||
            !bp_archive_name_type_valid(e.name, e.name_len, e.entry_type) ||
            (!(e.flags & BP_ENTRY_FLAG_MTIME_VALID) && !bp_u64_is_zero(e.mtime)) ||
            e.entry_type > BP_TYPE_DIR ||
            (bp_uint32)bp_read_le16(tail + 37) != M7_ENTRY_FIXED + e.name_len ||
            e.volume >= vr->count || e.offset.hi != 0 || off < M7_VOL_HDR ||
            (e.entry_type == BP_TYPE_DIR && !bp_u64_is_zero(e.original_size)) ||
            (bp_u64_is_zero(e.original_size) && !bp_u64_is_zero(e.compressed_size)) ||
            (i == 0 ? (e.volume != 0 || off != M7_VOL_HDR)
                    : !loc_after(e.volume, off, prev_vol, prev_off)) ||
            !loc_after(vr->table_volume, vr->table_offset, e.volume, off)) {
            free(e.name);
            return BP_ERR_CORRUPT;
        }
        prev_vol = e.volume;
        prev_off = off;
        if (fn != NULL && (stopped == NULL || !*stopped)) {
            rc = fn(vr, &e, i, digest, user);
            if (rc > 0) { if (stopped != NULL) *stopped = 1; rc = BP_OK; }
            if (rc != BP_OK) { free(e.name); return rc; }
        }
        free(e.name);
    }
    if (((check & MVR_SCAN_CRC) && crc != vr->table_crc) ||
        bytes != vr->table_size)
        return BP_ERR_CORRUPT;
    mvr_next_location(vr, &nv, &no);
    if (nv != vr->last || no != vr->end_offset) return BP_ERR_CORRUPT;
    if (vr->entry_count == 0 &&
        (vr->table_volume != 0 || vr->table_offset != M7_VOL_HDR))
        return BP_ERR_CORRUPT;
    return BP_OK;
}

/* ---- reading: per-entry digests ---------------------------------------- */

static mini_dig_t *mvr_dig(mvr_t *vr, bp_uint32 i)
{
    return &vr->chunks[i / MINI_DIG_CHUNK][i % MINI_DIG_CHUNK];
}

static int mvr_alloc_digs(mvr_t *vr)
{
    bp_uint32 i;
    vr->nchunks = (vr->entry_count + MINI_DIG_CHUNK - 1UL) / MINI_DIG_CHUNK;
    if (vr->nchunks == 0) return BP_OK;
    vr->chunks = (mini_dig_t **)calloc((size_t)vr->nchunks, sizeof(*vr->chunks));
    if (vr->chunks == NULL) { vr->nchunks = 0; return BP_ERR_NOMEM; }
    for (i = 0; i < vr->nchunks; i++) {
        vr->chunks[i] = (mini_dig_t *)malloc((size_t)(MINI_DIG_CHUNK * sizeof(mini_dig_t)));
        if (vr->chunks[i] == NULL) return BP_ERR_NOMEM;
    }
    return BP_OK;
}

typedef struct {
    bp_metadata_group_t group;
    int check_capabilities;
} mvr_validate_t;

static int mvr_store_record(mvr_t *vr, bp_mini_entry_t *e, bp_uint32 i,
                            bp_uint32 digest, void *user)
{
    mvr_validate_t *v = (mvr_validate_t *)user;
    mini_dig_t *d = mvr_dig(vr, i);
    int rc;

    if (v->check_capabilities) {
        if (e->method > BP_METHOD_BOLTLZ) return BP_ERR_UNSUPPORTED_METHOD;
        if (e->original_size.hi != 0) return BP_ERR_OVERFLOW;
        if (e->entry_type == BP_TYPE_SYMLINK) return BP_ERR_PARTIAL;
    }
    if (e->compressed_size.hi != 0) return BP_ERR_OVERFLOW;
    if (e->entry_type == BP_TYPE_META && e->platform == BP_META_PORTABLE &&
        (e->original_size.hi != 0 || e->original_size.lo > BP_META_MAX_SIZE))
        return BP_ERR_CORRUPT;
    if (bp_u64_is_zero(e->original_size) && e->checksum != 0)
        return BP_ERR_CHECKSUM;
    d->digest = digest;
    d->comp = e->compressed_size.lo;
    d->checksum = e->checksum;
    rc = bp_metadata_group_record(&v->group, &e->name, e->entry_type,
                                  e->platform, 1);
    return rc;
}

/* Open the set, validate the whole table and keep the per-entry digests. */
static int mvr_open_validated(mvr_t *vr, int check_capabilities)
{
    mvr_validate_t v;
    int rc;

    rc = mvr_open_set(vr);
    if (rc != BP_OK) return rc;
    if (vr->entry_count > BP_MINI_MAX_ENTRIES) return BP_ERR_OVERFLOW;
    rc = mvr_alloc_digs(vr);
    if (rc != BP_OK) return rc;
    memset(&v, 0, sizeof(v));
    v.check_capabilities = check_capabilities;
    rc = mvr_scan_table(vr, mvr_store_record, &v, NULL,
                        MVR_SCAN_CRC | MVR_SCAN_DIGEST);
    bp_metadata_group_close(&v.group);
    return rc;
}

/* ---- reading: entries -------------------------------------------------- */

/* Read entry i's header at the current stream position and check it against
 * the table digest.  entry->name is malloc'd on success. */
static int mvr_read_entry(mvr_t *vr, bp_uint32 i, bp_mini_entry_t *e)
{
    bp_uint8 pre[4], loc[10];
    bp_uint8 *buf;
    bp_uint32 hs, n, digest;
    const bp_uint8 *f;
    mini_dig_t *d = mvr_dig(vr, i);
    int rc;
    BP_STACK_PROBE();

    memset(e, 0, sizeof(*e));
    rc = mvr_begin(vr);
    if (rc != BP_OK) return rc;
    bp_write_le16(loc, (bp_uint16)vr->cur);
    bp_write_le32(loc + 2, vr->pos);
    bp_write_le32(loc + 6, 0);
    rc = mvr_read(vr, pre, 4);
    if (rc != BP_OK) return rc;
    hs = bp_read_le16(pre);
    n = bp_read_le16(pre + 2);
    if (n == 0 || n > M7_MAX_NAME || hs != M7_ENTRY_FIXED + n ||
        !mvr_avail(vr, hs - 4UL))
        return BP_ERR_CORRUPT;
    buf = (bp_uint8 *)malloc((size_t)hs);
    if (buf == NULL) return BP_ERR_NOMEM;
    memcpy(buf, pre, 4);
    rc = mvr_read(vr, buf + 4, hs - 4UL);
    if (rc == BP_OK && bp_read_le32(buf + hs - 4) != bp_mini_crc32(0, buf, hs - 4UL))
        rc = BP_ERR_CORRUPT;
    if (rc != BP_OK) { free(buf); return rc; }
    f = buf + 4 + n;
    /* rebuild the digested record fields from the header and location */
    digest = bp_mini_crc32(0, pre + 2, 2);
    digest = bp_mini_crc32(digest, buf + 4, n);
    digest = bp_mini_crc32(digest, f, 8);                 /* original size */
    digest = bp_mini_crc32(digest, f + 14, 2);            /* type, platform */
    digest = bp_mini_crc32(digest, f + 16, 8);            /* mtime */
    digest = bp_mini_crc32(digest, loc, 10);              /* volume, offset */
    digest = bp_mini_crc32(digest, f + 8, 1);             /* method */
    digest = bp_mini_crc32(digest, pre, 2);               /* header size */
    digest = bp_mini_crc32(digest, f + 24, 2);            /* flags */
    if (digest != d->digest || f[9] != BP_CKSUM_CRC32) { free(buf); return BP_ERR_CORRUPT; }
    e->name = (char *)malloc((size_t)n + 1);
    if (e->name == NULL) { free(buf); return BP_ERR_NOMEM; }
    memcpy(e->name, buf + 4, (size_t)n);
    e->name[n] = '\0';
    e->name_len = (bp_uint16)n;
    e->original_size = bp_read_le64(f);
    e->method = f[8];
    e->checksum_type = f[9];
    e->block_size = bp_read_le32(f + 10);
    e->entry_type = f[14];
    e->platform = f[15];
    e->mtime = bp_read_le64(f + 16);
    e->flags = bp_read_le16(f + 24);
    e->volume = (bp_uint16)vr->cur;
    e->offset = bp_u64_from_u32(bp_read_le32(loc + 2));
    e->compressed_size = bp_u64_from_u32(d->comp);
    e->checksum = d->checksum;
    free(buf);
    if (!bp_u64_is_zero(e->original_size) &&
        (e->block_size == 0 || e->block_size > 16777216UL)) {
        free(e->name); e->name = NULL;
        return BP_ERR_CORRUPT;
    }
    return BP_OK;
}

/* Extraction buffer pair shared across entries (grow-only: the DOS far heap
 * fragments easily). */
typedef struct {
    bp_uint8  *in_buf;
    bp_uint8  *out_buf;
    bp_uint32  in_cap;
    bp_uint32  out_cap;
} mini_extract_bufs_t;

static void mini_bufs_free(mini_extract_bufs_t *b)
{
    free(b->in_buf);
    free(b->out_buf);
    b->in_buf = NULL;
    b->out_buf = NULL;
    b->in_cap = 0;
    b->out_cap = 0;
}

static int mini_bufs_ensure(mini_extract_bufs_t *b, bp_uint32 in_need,
                            bp_uint32 out_need)
{
    if (b->in_cap < in_need) {
        free(b->in_buf);
        b->in_cap = 0;
        b->in_buf = (bp_uint8 *)malloc((size_t)in_need);
        if (b->in_buf == NULL) return BP_ERR_NOMEM;
        b->in_cap = in_need;
    }
    if (b->out_cap < out_need) {
        free(b->out_buf);
        b->out_cap = 0;
        b->out_buf = (bp_uint8 *)malloc((size_t)out_need);
        if (b->out_buf == NULL) return BP_ERR_NOMEM;
        b->out_cap = out_need;
    }
    return BP_OK;
}

/* Read entry e's blocks and trailer.  decode: verify block CRCs, decode,
 * write to out (may be NULL) and check the payload checksum; otherwise only
 * check the framing. */
static int mvr_read_data(mvr_t *vr, const bp_mini_entry_t *e, int decode,
                         bp_io_t *out, mini_extract_bufs_t *bufs)
{
    bp_uint32 remaining = e->original_size.lo;
    bp_uint32 packed = 0;
    bp_uint32 crc = bp_mini_checksum_seed(BP_CKSUM_CRC32);
    bp_uint32 bsize = e->block_size;
    bp_uint32 in_cap = bsize + bsize / 16 + 64 + 3;
    bp_uint8 trl[16];
    int rc;
    BP_STACK_PROBE();

    if (decode && remaining != 0) {
        if (bsize > BP_MINI_MAX_BLOCK_SIZE) return BP_ERR_OVERFLOW;
        rc = mini_bufs_ensure(bufs, in_cap, bsize);
        if (rc != BP_OK) return rc;
    }
    while (remaining != 0) {
        bp_uint8 bh[12];
        bp_uint32 comp, orig;
        int first, lastv;

        rc = mvr_begin(vr);
        if (rc != BP_OK) return rc;
        first = vr->pos == M7_VOL_HDR;
        rc = mvr_read(vr, bh, M7_BLOCK_HDR);
        if (rc != BP_OK) return rc;
        comp = bp_read_le32(bh);
        orig = bp_read_le32(bh + 4);
        if (comp == 0 || orig == 0 || comp > orig || orig > bsize ||
            orig > remaining || !mvr_avail(vr, comp))
            return BP_ERR_CORRUPT;
        if (decode) {
            if (comp > in_cap) return BP_ERR_CORRUPT;
            rc = mvr_read(vr, bufs->in_buf, comp);
        } else {
            rc = mvr_skip(vr, comp);
        }
        if (rc != BP_OK) return rc;
        lastv = vr->pos >= vr->body_end;
        packed += M7_BLOCK_HDR + comp;
        remaining -= orig;
        if (orig < bsize && remaining != 0 && !first && !lastv)
            return BP_ERR_CORRUPT;
        if (!decode) continue;
        if (bp_read_le32(bh + 8) != bp_mini_crc32(0, bufs->in_buf, comp))
            return BP_ERR_CHECKSUM;
        if (comp == orig) {
            memcpy(bufs->out_buf, bufs->in_buf, (size_t)orig);
        } else {
            bp_uint32 dl = orig;
            rc = bp_mini_decompress_block(bufs->in_buf, comp, bufs->out_buf,
                                          &dl, e->method);
            if (rc != BP_OK) return rc;
            if (dl != orig) return BP_ERR_DECOMPRESS;
        }
        crc = bp_mini_checksum_typed(BP_CKSUM_CRC32, crc, bufs->out_buf, orig);
        if (out != NULL && mini_io_write(out, bufs->out_buf, orig) != BP_OK)
            return BP_ERR_IO;
    }
    rc = mvr_begin(vr);
    if (rc == BP_OK) rc = mvr_read(vr, trl, M7_ENTRY_TRL);
    if (rc != BP_OK) return rc;
    if (bp_read_le32(trl + 12) != bp_mini_crc32(0, trl, 12) ||
        bp_read_le32(trl + 4) != 0 || bp_read_le32(trl) != packed ||
        packed != e->compressed_size.lo || bp_read_le32(trl + 8) != e->checksum)
        return BP_ERR_CORRUPT;
    if (decode && crc != e->checksum) return BP_ERR_CHECKSUM;
    return BP_OK;
}

static int mvr_check_table_follows(mvr_t *vr)
{
    bp_uint16 v;
    bp_uint32 o;
    mvr_next_location(vr, &v, &o);
    return (v == vr->table_volume && o == vr->table_offset) ? BP_OK
                                                            : BP_ERR_CORRUPT;
}

/* Framing walk over a single-volume archive before anything is written,
 * including the per-entry block size limit. */
static int mvr_prewalk(mvr_t *vr)
{
    bp_uint32 i;
    int rc;
    if (vr->entry_count == 0) return BP_OK;
    rc = mvr_seek(vr, 0, M7_VOL_HDR);
    for (i = 0; rc == BP_OK && i < vr->entry_count; i++) {
        bp_mini_entry_t e;
        rc = mvr_read_entry(vr, i, &e);
        if (rc != BP_OK) break;
        if (e.block_size > BP_MINI_MAX_BLOCK_SIZE) rc = BP_ERR_OVERFLOW;
        if (rc == BP_OK) rc = mvr_read_data(vr, &e, 0, NULL, NULL);
        free(e.name);
    }
    if (rc == BP_OK) rc = mvr_check_table_follows(vr);
    return rc;
}

/* ---- single-stream source ---------------------------------------------- */

static bp_io_t *mini_single_open(void *user, bp_uint16 number, int reason)
{
    (void)number;
    return reason == BP_OK ? (bp_io_t *)user : NULL;
}

static void mini_single_source(bp_volume_source_t *src, bp_io_t *io)
{
    src->open = mini_single_open;
    src->close = NULL;
    src->user = io;
}

/* ---- list and info ----------------------------------------------------- */

typedef struct {
    bp_mini_list_fn fn;
    void *user;
} mvr_list_t;

static int mvr_list_record(mvr_t *vr, bp_mini_entry_t *e, bp_uint32 i,
                           bp_uint32 digest, void *user)
{
    mvr_list_t *l = (mvr_list_t *)user;
    (void)vr; (void)i; (void)digest;
    return l->fn(e, l->user) != 0 ? 1 : BP_OK;
}

int bp_mini_archive_list_volumes(const bp_volume_source_t *src,
                                 bp_mini_list_fn fn, void *user_data)
{
    mvr_t vr;
    mvr_list_t l;
    int rc;
    BP_STACK_PROBE();

    if (src == NULL || src->open == NULL) return BP_ERR_INVALID_ARG;
    mvr_init(&vr, src);
    rc = mvr_open_set(&vr);
    /* first pass validates the whole table, the second reports it */
    if (rc == BP_OK) rc = mvr_scan_table(&vr, NULL, NULL, NULL, MVR_SCAN_CRC);
    if (rc == BP_OK && fn != NULL) {
        int stopped;
        l.fn = fn;
        l.user = user_data;
        rc = mvr_scan_table(&vr, mvr_list_record, &l, &stopped, 0);
    }
    mvr_free(&vr);
    return rc;
}

int bp_mini_archive_list(bp_io_t *io, bp_mini_list_fn fn, void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    mini_single_source(&src, io);
    return bp_mini_archive_list_volumes(&src, fn, user_data);
}

static int mvr_info_record(mvr_t *vr, bp_mini_entry_t *e, bp_uint32 i,
                           bp_uint32 digest, void *user)
{
    bp_mini_archive_info_t *info = (bp_mini_archive_info_t *)user;
    (void)vr; (void)i; (void)digest;
    info->entry_count++;
    if (e->entry_type == BP_TYPE_DIR) info->dir_count++;
    else if (e->method < 5) info->method_counts[e->method]++;
    info->total_original = bp_u64_add(info->total_original, e->original_size);
    info->total_compressed = bp_u64_add(info->total_compressed, e->compressed_size);
    return BP_OK;
}

int bp_mini_archive_info_volumes(const bp_volume_source_t *src,
                                 bp_mini_archive_info_t *info)
{
    mvr_t vr;
    int rc;

    if (src == NULL || src->open == NULL || info == NULL) return BP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    mvr_init(&vr, src);
    rc = mvr_open_set(&vr);
    if (rc == BP_OK)
        rc = mvr_scan_table(&vr, mvr_info_record, info, NULL, MVR_SCAN_CRC);
    if (rc == BP_OK) {
        info->version = BP_FORMAT7_VERSION;
        info->file_count = vr.entry_count;
        info->creator_os = vr.creator_os;
        info->creator_cpu = vr.creator_cpu;
        info->creator_ver_major = vr.ver_major;
        info->creator_ver_minor = vr.ver_minor;
        info->archive_size = bp_u64_from_u32(vr.last_size);
        info->volume_count = vr.count;
    } else {
        memset(info, 0, sizeof(*info));
    }
    mvr_free(&vr);
    return rc;
}

int bp_mini_archive_info(bp_io_t *io, bp_mini_archive_info_t *info)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    mini_single_source(&src, io);
    return bp_mini_archive_info_volumes(&src, info);
}

/* ---- extraction -------------------------------------------------------- */

static int mini_extract_impl(const bp_volume_source_t *src,
                             bp_mini_extract_fn open_fn,
                             bp_mini_extract_done_fn done_fn,
                             bp_mini_extract_done_status_fn status_fn,
                             bp_mini_metadata_fn metadata_fn,
                             bp_mini_extract_directory_fn directory_fn,
                             void *user_data)
{
    mvr_t vr;
    mini_extract_bufs_t bufs = {NULL, NULL, 0, 0};
    bp_uint32 i;
    int rc;
    int partial = 0;
    bp_uint8 subject_type = 0xFF;
    int subject_ready = 0;
    BP_STACK_PROBE();

    if (src == NULL || src->open == NULL) return BP_ERR_INVALID_ARG;
    mvr_init(&vr, src);
    rc = mvr_open_validated(&vr, 1);
    /* a single volume is checked in full before anything is written; a set
     * is read once, volume by volume (spec 2.0 §5.4) */
    if (rc == BP_OK && vr.count == 1) rc = mvr_prewalk(&vr);
    if (rc == BP_OK && vr.entry_count != 0) rc = mvr_seek(&vr, 0, M7_VOL_HDR);
    if (rc != BP_OK) { mvr_free(&vr); return rc; }

    for (i = 0; i < vr.entry_count; i++) {
        bp_mini_entry_t entry;
        bp_io_t *out;
        int skip_metadata, portable;
        bp_metadata_sink_t *metadata = NULL;
        bp_portable_metadata_t parsed;

        rc = mvr_read_entry(&vr, i, &entry);
        if (rc != BP_OK) break;
        if (entry.entry_type == BP_TYPE_DATA || entry.entry_type == BP_TYPE_DIR ||
            entry.entry_type == BP_TYPE_SYMLINK) {
            subject_type = entry.entry_type;
            subject_ready = 0;
        }
        if (entry.block_size > BP_MINI_MAX_BLOCK_SIZE) {
#ifndef BP_MINI_QUIET
            fprintf(stderr,
                    "bpmini: archive uses %u KB blocks -- re-create with -b %uk for mini\n",
                    (unsigned)(entry.block_size / 1024),
                    (unsigned)(BP_MINI_MAX_BLOCK_SIZE / 1024));
#endif
            free(entry.name);
            rc = BP_ERR_OVERFLOW;
            break;
        }
        skip_metadata = entry.entry_type == BP_TYPE_META ||
                        entry.entry_type == BP_TYPE_RSRC;
        portable = entry.entry_type == BP_TYPE_META &&
                   entry.platform == BP_META_PORTABLE;
        if (portable) {
            metadata = (bp_metadata_sink_t *)malloc(sizeof(*metadata));
            if (metadata == NULL) { free(entry.name); rc = BP_ERR_NOMEM; break; }
            bp_metadata_sink_init(metadata);
        }
        if (skip_metadata && !(portable && metadata_fn != NULL)) {
#ifndef BP_MINI_QUIET
            fprintf(stderr, "bpmini: metadata not restored: %s\n", entry.name);
#endif
            partial = 1;
        }
        out = NULL;
        if (entry.entry_type == BP_TYPE_DIR && directory_fn != NULL) {
            rc = directory_fn(&entry, user_data);
            if (rc == BP_OK) subject_ready = 1;
            else if (rc == BP_ERR_PARTIAL) partial = 1;
            else { free(metadata); free(entry.name); break; }
        } else if (!skip_metadata && open_fn != NULL) {
            out = open_fn(&entry, user_data);
        }
        if (!skip_metadata && entry.entry_type != BP_TYPE_DIR && out == NULL)
            partial = 1;

        rc = mvr_read_data(&vr, &entry, 1, portable ? &metadata->io : out, &bufs);
        if (rc != BP_OK) {
            if (out && status_fn) (void)status_fn(&entry, out, rc, user_data);
            else if (out && done_fn) done_fn(&entry, out, user_data);
            free(metadata);
            free(entry.name);
            break;
        }
        if (portable) {
            int prc = bp_metadata_parse(metadata->data, metadata->length, &parsed);
            free(metadata);
            if (prc == BP_ERR_CORRUPT ||
                (parsed.supported_version && parsed.subject_type != subject_type)) {
                free(entry.name); rc = BP_ERR_CORRUPT; break;
            }
            if (prc == BP_ERR_PARTIAL) partial = 1;
            if (metadata_fn != NULL && parsed.supported_version && subject_ready) {
                prc = metadata_fn(&entry, &parsed, user_data);
                if (prc == BP_ERR_PARTIAL) partial = 1;
                else if (prc != BP_OK) { free(entry.name); rc = prc; break; }
            } else partial = 1;
        }
        if (out && status_fn) {
            int src_rc = status_fn(&entry, out, BP_OK, user_data);
            if (src_rc == BP_ERR_PARTIAL) partial = 1;
            else if (src_rc != BP_OK) { free(entry.name); rc = src_rc; break; }
            if (entry.entry_type == BP_TYPE_DATA) subject_ready = 1;
        } else if (out && done_fn) {
            done_fn(&entry, out, user_data);
        }
        free(entry.name);
    }
    if (rc == BP_OK && vr.entry_count != 0) rc = mvr_check_table_follows(&vr);
    mini_bufs_free(&bufs);
    mvr_free(&vr);
    if (rc != BP_OK) return rc;
    return partial ? BP_ERR_PARTIAL : BP_OK;
}

int bp_mini_archive_extract_volumes(const bp_volume_source_t *src,
                                    bp_mini_extract_fn open_fn,
                                    bp_mini_extract_done_status_fn done_fn,
                                    bp_mini_extract_directory_fn directory_fn,
                                    bp_mini_metadata_fn metadata_fn,
                                    void *user_data)
{
    if (done_fn == NULL) return BP_ERR_INVALID_ARG;
    return mini_extract_impl(src, open_fn, NULL, done_fn, metadata_fn,
                             directory_fn, user_data);
}

int bp_mini_archive_extract_all(bp_io_t *io, bp_mini_extract_fn open_fn,
                                bp_mini_extract_done_fn done_fn, void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    mini_single_source(&src, io);
    return mini_extract_impl(&src, open_fn, done_fn, NULL, NULL, NULL, user_data);
}

int bp_mini_archive_extract_all_ex(bp_io_t *io, bp_mini_extract_fn open_fn,
                                   bp_mini_extract_done_status_fn done_fn,
                                   void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL || done_fn == NULL) return BP_ERR_INVALID_ARG;
    mini_single_source(&src, io);
    return mini_extract_impl(&src, open_fn, NULL, done_fn, NULL, NULL, user_data);
}

int bp_mini_archive_extract_all_with_metadata(bp_io_t *io,
                                   bp_mini_extract_fn open_fn,
                                   bp_mini_extract_done_status_fn done_fn,
                                   bp_mini_metadata_fn metadata_fn,
                                   void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL || done_fn == NULL || metadata_fn == NULL)
        return BP_ERR_INVALID_ARG;
    mini_single_source(&src, io);
    return mini_extract_impl(&src, open_fn, NULL, done_fn, metadata_fn, NULL,
                             user_data);
}

int bp_mini_archive_extract_all_with_directories(bp_io_t *io,
                                   bp_mini_extract_fn open_fn,
                                   bp_mini_extract_done_status_fn done_fn,
                                   bp_mini_extract_directory_fn directory_fn,
                                   bp_mini_metadata_fn metadata_fn,
                                   void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL || done_fn == NULL || directory_fn == NULL ||
        metadata_fn == NULL)
        return BP_ERR_INVALID_ARG;
    mini_single_source(&src, io);
    return mini_extract_impl(&src, open_fn, NULL, done_fn, metadata_fn,
                             directory_fn, user_data);
}

/* ---- test -------------------------------------------------------------- */

int bp_mini_archive_test_volumes(const bp_volume_source_t *src,
                                 bp_mini_test_fn fn, void *user_data)
{
    mvr_t vr;
    mini_extract_bufs_t bufs = {NULL, NULL, 0, 0};
    bp_uint32 i;
    int rc;
    int overall = BP_OK;
    bp_uint8 subject_type = 0xFF;
    BP_STACK_PROBE();

    if (src == NULL || src->open == NULL) return BP_ERR_INVALID_ARG;
    mvr_init(&vr, src);
    rc = mvr_open_validated(&vr, 0);
    if (rc == BP_OK && vr.entry_count != 0) rc = mvr_seek(&vr, 0, M7_VOL_HDR);
    if (rc != BP_OK) { mvr_free(&vr); return rc; }

    for (i = 0; i < vr.entry_count; i++) {
        bp_mini_entry_t entry;
        int result, portable;
        bp_metadata_sink_t *metadata = NULL;
        bp_portable_metadata_t parsed;

        rc = mvr_read_entry(&vr, i, &entry);
        if (rc != BP_OK) break;
        if (entry.entry_type == BP_TYPE_DATA || entry.entry_type == BP_TYPE_DIR ||
            entry.entry_type == BP_TYPE_SYMLINK) subject_type = entry.entry_type;
        portable = entry.entry_type == BP_TYPE_META &&
                   entry.platform == BP_META_PORTABLE;
        if (entry.method > BP_METHOD_BOLTLZ) {
            result = BP_ERR_UNSUPPORTED_METHOD;
        } else if (entry.original_size.hi != 0) {
#ifndef BP_MINI_QUIET
            fprintf(stderr, "bpmini: skipping test of %s: too large for mini\n",
                    entry.name);
#endif
            result = BP_ERR_OVERFLOW;
        } else if (entry.block_size > BP_MINI_MAX_BLOCK_SIZE) {
#ifndef BP_MINI_QUIET
            fprintf(stderr,
                    "bpmini: archive uses %u KB blocks -- re-create with -b %uk for mini\n",
                    (unsigned)(entry.block_size / 1024),
                    (unsigned)(BP_MINI_MAX_BLOCK_SIZE / 1024));
#endif
            result = BP_ERR_OVERFLOW;
        } else {
            result = BP_OK;
        }
        if (result != BP_OK) {
            /* skip what this build cannot decode, but check its framing */
            rc = mvr_read_data(&vr, &entry, 0, NULL, NULL);
        } else {
            if (portable) {
                metadata = (bp_metadata_sink_t *)malloc(sizeof(*metadata));
                if (metadata == NULL) { free(entry.name); rc = BP_ERR_NOMEM; break; }
                bp_metadata_sink_init(metadata);
            }
            result = mvr_read_data(&vr, &entry, 1,
                                   portable ? &metadata->io : NULL, &bufs);
            if (result == BP_OK && portable) {
                result = bp_metadata_parse(metadata->data, metadata->length, &parsed);
                if (parsed.supported_version && parsed.subject_type != subject_type)
                    result = BP_ERR_CORRUPT;
            }
            free(metadata);
            /* a structural failure leaves the stream position unknown */
            if (result != BP_OK && result != BP_ERR_PARTIAL) rc = result;
        }
        if (result != BP_OK && overall == BP_OK) overall = result;
        if (fn != NULL) fn(&entry, result, user_data);
        free(entry.name);
        if (rc != BP_OK) break;
    }
    if (rc == BP_OK && vr.entry_count != 0) rc = mvr_check_table_follows(&vr);
    mini_bufs_free(&bufs);
    mvr_free(&vr);
    if (rc != BP_OK && overall == BP_OK) overall = rc;
    return overall;
}

int bp_mini_archive_test(bp_io_t *io, bp_mini_test_fn fn, void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    mini_single_source(&src, io);
    return bp_mini_archive_test_volumes(&src, fn, user_data);
}
