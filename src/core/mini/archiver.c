/*
 * archiver.c -- mini archive read/write for BoltPack
 *
 * Implements the full .bolt format but caps capabilities at:
 *   - Methods 0 (stored), 1 (BoltLite), and 2 (boltlz)
 *   - Block size <= BP_MINI_MAX_BLOCK_SIZE  (32 KB)
 *   - Entry count <= BP_MINI_MAX_ENTRIES   (65 535)
 *   - File size   <= 4 GB (original_size.hi must be 0)
 *
 * List and info always work regardless of what an existing archive contains.
 *
 * ANSI C (C89/C90) only.  No fluff dependency.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "archiver.h"
#include "../archive_name.h"
#include "../bp_metadata.h"
#include "compress.h"
#include <stdlib.h>
#include <string.h>
#ifndef BP_MINI_QUIET
#include <stdio.h>
#endif

/* ---- low-level I/O helpers -------------------------------------------- */

static int mini_io_write(bp_io_t *io, const void *buf, bp_uint32 n)
{
    long w = io->write(io->handle, buf, n);
    return (w == (long)n) ? BP_OK : BP_ERR_IO;
}

static int mini_io_read(bp_io_t *io, void *buf, bp_uint32 n)
{
    long r = io->read(io->handle, buf, n);
    return (r == (long)n) ? BP_OK : BP_ERR_TRUNCATED;
}

static int mini_io_seek(bp_io_t *io, bp_off_t offset, int origin)
{
    return io->seek(io->handle, offset, origin);
}

static bp_off_t mini_io_tell(bp_io_t *io)
{
    return io->tell(io->handle);
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

/* ---- entry list growth ------------------------------------------------- */

static int mini_entry_list_grow(bp_mini_archive_t *ar)
{
    bp_uint32 new_cap;
    bp_mini_entry_t *new_list;

    if (ar->entries_written < ar->entry_list_cap) {
        return BP_OK;
    }
    new_cap = ar->entry_list_cap ? ar->entry_list_cap * 2 : 64;
    if (new_cap > BP_MINI_MAX_ENTRIES) {
        new_cap = BP_MINI_MAX_ENTRIES;
    }
    new_list = (bp_mini_entry_t *)realloc(ar->entry_list,
                                          (size_t)new_cap * sizeof(bp_mini_entry_t));
    if (new_list == NULL) {
        return BP_ERR_NOMEM;
    }
    ar->entry_list     = new_list;
    ar->entry_list_cap = new_cap;
    return BP_OK;
}

/* ---- bp_mini_archive_create -------------------------------------------- */

int bp_mini_archive_create(bp_mini_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method)
{
    bp_uint8 hdr[BP_HEADER_SIZE];

    if (ar == NULL || io == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (method > BP_METHOD_BOLTLZ) {
        return BP_ERR_UNSUPPORTED_METHOD;
    }

    /* On 16-bit systems boltlz compression is not available; return unsupported-method when requested. */
#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
    if (method == BP_METHOD_BOLTLZ) {
        return BP_ERR_UNSUPPORTED_METHOD;
    }
    /* BoltLite (method 1) is now supported on 16-bit; no fallback needed. */
#endif

    memset(ar, 0, sizeof(*ar));
    ar->io = io;

    if (block_size == 0) {
        block_size = BP_BLOCK_32K;
    }
    if (block_size > BP_MINI_MAX_BLOCK_SIZE) {
        block_size = BP_MINI_MAX_BLOCK_SIZE;
    }
    ar->block_size = block_size;
    ar->method     = method;

    /* Detect creator OS / CPU at compile time */
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

    {
        bp_uint64 z;
        z.lo = 0; z.hi = 0;
        ar->archive_orig_total = z;
        ar->archive_comp_total = z;
    }

    /* Write placeholder header (file_count and archive_size patched in finalize) */
    memset(hdr, 0, BP_HEADER_SIZE);
    hdr[0] = BP_MAGIC_0;
    hdr[1] = BP_MAGIC_1;
    hdr[2] = BP_MAGIC_2;
    hdr[3] = BP_MAGIC_3;
    bp_write_le16(hdr + 4, BP_FORMAT_VERSION);
    hdr[12] = ar->creator_os;
    hdr[13] = ar->creator_cpu;
    hdr[14] = BP_VERSION_MAJOR;
    hdr[15] = BP_VERSION_MINOR;

    return mini_io_write(io, hdr, BP_HEADER_SIZE);
}

/* ---- bp_mini_archive_add_entry ----------------------------------------- */

int bp_mini_archive_add_entry_ex(bp_mini_archive_t *ar, const char *name,
                                 bp_uint8 entry_type, bp_uint8 platform,
                                 bp_io_t *file_io, bp_uint64 file_size,
                                 bp_uint64 mtime, bp_uint16 flags)
{
    bp_uint16 name_len;
    bp_uint16 hdr_size;
    bp_uint8  ehdr[4];
    bp_uint8  fields[38];
    bp_mini_entry_t *ent;
    bp_off_t  entry_offset;
    bp_off_t  fields_pos;
    bp_uint64 total_comp;
    bp_uint32 adler;
    bp_uint8  actual_method;
    bp_uint8 *in_buf;
    bp_uint8 *out_buf;
    bp_uint32 out_buf_size;
    int rc;

    if (ar == NULL || name == NULL) {
        return BP_ERR_INVALID_ARG;
    }
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

    if ((unsigned long)strlen(name) > BP_MAX_NAME_LEN) {
        return BP_ERR_NAME_TOO_LONG;
    }
    name_len = (bp_uint16)strlen(name);
    if (!bp_archive_name_valid(name, (bp_uint32)name_len) ||
        (!(flags & BP_ENTRY_FLAG_MTIME_VALID) && !bp_u64_is_zero(mtime)))
        return BP_ERR_INVALID_ARG;
    hdr_size = (bp_uint16)(BP_ENTRY_FIXED + name_len);

    rc = mini_entry_list_grow(ar);
    if (rc != BP_OK) return rc;

    entry_offset = mini_io_tell(ar->io);
    if (entry_offset < 0) return BP_ERR_IO;

    /* Write header_size(2) + name_len(2) */
    bp_write_le16(ehdr,     hdr_size);
    bp_write_le16(ehdr + 2, name_len);
    rc = mini_io_write(ar->io, ehdr, 4);
    if (rc != BP_OK) return rc;

    /* Write name */
    rc = mini_io_write(ar->io, (const bp_uint8 *)name, name_len);
    if (rc != BP_OK) return rc;

    /* Build and write the 38-byte fields block.
     * compressed_size (fields[8..15]) and checksum (fields[16..19]) are
     * patched in after compression. */
    actual_method = (entry_type == BP_TYPE_DIR || bp_u64_is_zero(file_size))
                    ? BP_METHOD_STORED : ar->method;

    memset(fields, 0, 38);
    bp_write_le64(fields + 0, file_size);               /* original_size   */
    /* compressed_size at fields[8..15]: filled below  */
    /* checksum at fields[16..19]: filled below        */
    fields[20] = actual_method;
    fields[21] = BP_CKSUM_CRC32;
    if (!bp_u64_is_zero(file_size) && entry_type != BP_TYPE_DIR) {
        bp_write_le32(fields + 22, ar->block_size);
    }
    fields[26] = entry_type;
    fields[27] = platform;
    bp_write_le64(fields + 28, mtime);
    bp_write_le16(fields + 36, flags);

    fields_pos = mini_io_tell(ar->io);
    if (fields_pos < 0) return BP_ERR_IO;
    rc = mini_io_write(ar->io, fields, 38);
    if (rc != BP_OK) return rc;

    /* --- Compress and write data blocks --------------------------------- */
    total_comp.lo = 0; total_comp.hi = 0;
    adler = bp_mini_checksum_seed(BP_CKSUM_CRC32); /* CRC-32 seed (0) */

    if (!bp_u64_is_zero(file_size)) {
        bp_uint64 remaining = file_size;
        bp_uint32 bsize = ar->block_size;
        bp_uint32 boltlite_bound;
        bp_uint32 boltlz_bound;
        int any_compressed_block = 0;

        /* Allocate for the larger of the BoltLite and boltlz worst-case bounds. */
        boltlite_bound = bsize + bsize / 8 + 16;
        boltlz_bound = bsize + bsize / 63 + 16;
        out_buf_size = (boltlite_bound > boltlz_bound) ? boltlite_bound : boltlz_bound;
        in_buf  = (bp_uint8 *)malloc((size_t)bsize);
        out_buf = (bp_uint8 *)malloc((size_t)out_buf_size);
        if (in_buf == NULL || out_buf == NULL) {
            free(in_buf);
            free(out_buf);
            return BP_ERR_NOMEM;
        }

        while (!bp_u64_is_zero(remaining)) {
            bp_uint32 chunk = (remaining.hi != 0 || remaining.lo > bsize)
                              ? bsize : remaining.lo;
            bp_uint32 comp_len = out_buf_size;
            bp_uint8 blk_hdr[BP_BLOCK_HDR_V3];
            long n;

            n = file_io->read(file_io->handle, in_buf, chunk);
            if (n != (long)chunk) {
                free(in_buf); free(out_buf);
                return BP_ERR_IO;
            }

            adler = bp_mini_checksum_typed(BP_CKSUM_CRC32, adler, in_buf, chunk);

            if (actual_method == BP_METHOD_BOLTLZ ||
                    actual_method == BP_METHOD_BOLTLITE) {
                rc = bp_mini_compress_block(in_buf, chunk, out_buf, &comp_len,
                                            actual_method);
                if (rc != BP_OK) { free(in_buf); free(out_buf); return rc; }

                /* Fall back to stored if compression saves less than 1% */
                if (comp_len > (chunk * 99UL) / 100UL) {
                    comp_len = chunk;
                    memcpy(out_buf, in_buf, (size_t)chunk);
                }
            } else {
                comp_len = chunk;
                memcpy(out_buf, in_buf, (size_t)chunk);
            }

            if (comp_len < chunk) {
                any_compressed_block = 1;
            }

            bp_write_le32(blk_hdr,     comp_len);
            bp_write_le32(blk_hdr + 4, chunk);
            bp_write_le32(blk_hdr + 8, bp_mini_crc32(0, out_buf, comp_len));
            rc = mini_io_write(ar->io, blk_hdr, BP_BLOCK_HDR_V3);
            if (rc != BP_OK) { free(in_buf); free(out_buf); return rc; }
            rc = mini_io_write(ar->io, out_buf, comp_len);
            if (rc != BP_OK) { free(in_buf); free(out_buf); return rc; }

            total_comp = bp_u64_add(total_comp,
                                    bp_u64_from_u32(comp_len + BP_BLOCK_HDR_V3));
            remaining = bp_u64_sub(remaining, bp_u64_from_u32(chunk));
        }

        free(in_buf);
        free(out_buf);

        if (actual_method != BP_METHOD_STORED && !any_compressed_block) {
            actual_method = BP_METHOD_STORED;
        }
    }

    /* Patch compressed_size, checksum, and effective method */
    {
        bp_uint8 patch[13];
        bp_write_le64(patch,    total_comp);
        bp_write_le32(patch + 8, adler);
        patch[12] = actual_method;

        rc = mini_io_seek(ar->io, fields_pos + 8, 0); /* SEEK_SET */
        if (rc != 0) return BP_ERR_IO;
        rc = mini_io_write(ar->io, patch, 13);
        if (rc != BP_OK) return rc;

        rc = mini_io_seek(ar->io, 0, 2); /* SEEK_END */
        if (rc != 0) return BP_ERR_IO;
    }

    ar->archive_orig_total = bp_u64_add(ar->archive_orig_total, file_size);
    ar->archive_comp_total = bp_u64_add(ar->archive_comp_total, total_comp);

    /* Record entry for file table */
    ent = &ar->entry_list[ar->entries_written];
    memset(ent, 0, sizeof(*ent));
    ent->name = (char *)malloc((size_t)(name_len + 1));
    if (ent->name == NULL) return BP_ERR_NOMEM;
    memcpy(ent->name, name, name_len);
    ent->name[name_len]    = '\0';
    ent->name_len          = name_len;
    ent->original_size     = file_size;
    ent->compressed_size   = total_comp;
    ent->checksum          = adler;
    ent->method            = actual_method;
    ent->checksum_type     = BP_CKSUM_CRC32;
    ent->block_size        = (!bp_u64_is_zero(file_size) && entry_type != BP_TYPE_DIR)
                             ? ar->block_size : 0;
    ent->entry_type        = entry_type;
    ent->platform          = platform;
    ent->mtime             = mtime;
    ent->flags             = flags;
    ent->header_size       = hdr_size;
    ent->offset            = bp_off_to_u64(entry_offset);

    ar->entries_written++;
    return BP_OK;
}

/* ---- bp_mini_archive_finalize ------------------------------------------ */

/* Write `len` bytes while folding them into the running CRC-32 *crc. */
int bp_mini_archive_add_entry(bp_mini_archive_t *ar, const char *name,
                              bp_uint8 entry_type, bp_uint8 platform,
                              bp_io_t *file_io, bp_uint64 file_size,
                              bp_uint64 mtime)
{
    return bp_mini_archive_add_entry_ex(ar, name, entry_type, platform,
                                        file_io, file_size, mtime,
                                        BP_ENTRY_FLAG_MTIME_VALID);
}

static int mini_write_crc(bp_io_t *io, bp_uint32 *crc,
                          const bp_uint8 *buf, bp_uint32 len)
{
    *crc = bp_mini_crc32(*crc, buf, len);
    return mini_io_write(io, buf, len);
}

/* Write one v3 file-table record, folding bytes into *crc. */
static int mini_write_table_record(bp_io_t *io, bp_uint32 *crc,
                                   const bp_mini_entry_t *e)
{
    bp_uint8 rec_hdr[2];
    bp_uint8 rec_fields[BP_TABLE_REC_TAIL_V6];
    int rc;

    bp_write_le16(rec_hdr, e->name_len);
    rc = mini_write_crc(io, crc, rec_hdr, 2);
    if (rc != BP_OK) return rc;

    rc = mini_write_crc(io, crc, (const bp_uint8 *)e->name, e->name_len);
    if (rc != BP_OK) return rc;

    bp_write_le64(rec_fields +  0, e->original_size);
    bp_write_le64(rec_fields +  8, e->compressed_size);
    rec_fields[16] = e->entry_type;
    rec_fields[17] = e->platform;
    bp_write_le64(rec_fields + 18, e->mtime);
    bp_write_le64(rec_fields + 26, e->offset);
    rec_fields[34] = e->method;                     /* v3: method */
    bp_write_le16(rec_fields + 35, e->header_size); /* v3: header_size */
    bp_write_le32(rec_fields + 37, e->checksum);    /* v3: payload checksum */
    bp_write_le16(rec_fields + 41, e->flags);
    return mini_write_crc(io, crc, rec_fields, BP_TABLE_REC_TAIL_V6);
}

int bp_mini_archive_finalize(bp_mini_archive_t *ar)
{
    bp_off_t  table_start;
    bp_off_t  table_end;
    bp_off_t  archive_end;
    bp_uint32 i;
    bp_uint32 table_crc;
    bp_uint8  buf[32];
    int rc;

    if (ar == NULL) return BP_ERR_INVALID_ARG;

    table_start = mini_io_tell(ar->io);
    if (table_start < 0) return BP_ERR_IO;

    table_crc = 0;

    /* --- file table header: magic(4) + entry_count(4) + total_orig(8) = 16 (v3) */
    {
        bp_uint32 fi;
        bp_uint64 total_orig;

        total_orig.lo = 0; total_orig.hi = 0;
        for (fi = 0; fi < ar->entries_written; fi++) {
            total_orig = bp_u64_add(total_orig, ar->entry_list[fi].original_size);
        }
        buf[0] = BP_TABLE_MAGIC_0;
        buf[1] = BP_TABLE_MAGIC_1;
        buf[2] = BP_TABLE_MAGIC_2;
        buf[3] = BP_TABLE_MAGIC_3;
        bp_write_le32(buf + 4, ar->entries_written);
        bp_write_le64(buf + 8, total_orig);
        rc = mini_write_crc(ar->io, &table_crc, buf, BP_TABLE_HDR_V3);
        if (rc != BP_OK) return rc;
    }

    /* --- file table records (v3) */
    for (i = 0; i < ar->entries_written; i++) {
        rc = mini_write_table_record(ar->io, &table_crc, &ar->entry_list[i]);
        if (rc != BP_OK) return rc;
    }

    table_end = mini_io_tell(ar->io);
    if (table_end < 0) return BP_ERR_IO;

    /* --- footer */
    {
        bp_uint8  footer[BP_FOOTER_SIZE];
        bp_uint64 tbl_off = bp_off_to_u64(table_start);
        bp_uint64 tbl_sz  = bp_off_to_u64(table_end - table_start);

        bp_write_le64(footer + 0, tbl_off);
        bp_write_le64(footer + 8, tbl_sz);
        footer[16] = BP_FOOTER_MAGIC_0;
        footer[17] = BP_FOOTER_MAGIC_1;
        footer[18] = BP_FOOTER_MAGIC_2;
        footer[19] = BP_FOOTER_MAGIC_3;

        rc = mini_io_write(ar->io, footer, BP_FOOTER_SIZE);
        if (rc != BP_OK) return rc;
    }

    archive_end = mini_io_tell(ar->io);
    if (archive_end < 0) return BP_ERR_IO;

    /* --- rebuild the full 32-byte header with final values + v3 CRCs and
     * write it once.  Building in memory (rather than reading the on-disk
     * placeholder back and patching) means the output stream need only be
     * seekable + writable, not readable -- some mini frontends open the
     * archive write-only. */
    {
        bp_uint8  hdr[BP_HEADER_SIZE];
        bp_uint32 hcrc;

        memset(hdr, 0, BP_HEADER_SIZE);
        hdr[0] = BP_MAGIC_0;
        hdr[1] = BP_MAGIC_1;
        hdr[2] = BP_MAGIC_2;
        hdr[3] = BP_MAGIC_3;
        bp_write_le16(hdr + 4, BP_FORMAT_VERSION);
        bp_write_le32(hdr + 8, ar->entries_written);
        hdr[12] = ar->creator_os;
        hdr[13] = ar->creator_cpu;
        hdr[14] = BP_VERSION_MAJOR;
        hdr[15] = BP_VERSION_MINOR;
        bp_write_le64(hdr + 16, bp_off_to_u64(archive_end));
        bp_write_le32(hdr + BP_HEADER_TABLE_CRC_OFF, table_crc);
        hcrc = bp_mini_crc32(0, hdr, BP_HEADER_CRC_OFF);
        bp_write_le32(hdr + BP_HEADER_CRC_OFF, hcrc);

        rc = mini_io_seek(ar->io, 0, 0);
        if (rc != 0) return BP_ERR_IO;
        rc = mini_io_write(ar->io, hdr, BP_HEADER_SIZE);
        if (rc != BP_OK) return rc;
    }

    return BP_OK;
}

void bp_mini_archive_close(bp_mini_archive_t *ar)
{
    bp_uint32 i;

    if (ar == NULL) return;
    if (ar->entry_list) {
        for (i = 0; i < ar->entries_written; i++) {
            free(ar->entry_list[i].name);
        }
        free(ar->entry_list);
        ar->entry_list = NULL;
    }
}

/* ---- reading: shared footer/header helpers ----------------------------- */

static int mini_read_footer(bp_io_t *io,
                            bp_uint64 *table_offset,
                            bp_uint64 *table_size)
{
    bp_uint8 buf[BP_FOOTER_SIZE];
    int rc;

    rc = mini_io_seek(io, -(bp_off_t)BP_FOOTER_SIZE, 2); /* SEEK_END */
    if (rc != 0) return BP_ERR_IO;

    rc = mini_io_read(io, buf, BP_FOOTER_SIZE);
    if (rc != BP_OK) return rc;

    if (buf[16] != BP_FOOTER_MAGIC_0 || buf[17] != BP_FOOTER_MAGIC_1 ||
        buf[18] != BP_FOOTER_MAGIC_2 || buf[19] != BP_FOOTER_MAGIC_3) {
        return BP_ERR_MAGIC;
    }

    *table_offset = bp_read_le64(buf);
    *table_size   = bp_read_le64(buf + 8);
    return BP_OK;
}

static int mini_read_archive_header(bp_io_t *io, bp_uint32 *file_count,
                                    bp_uint8 *creator_os,
                                    bp_uint8 *creator_cpu,
                                    bp_uint8 *ver_major,
                                    bp_uint8 *ver_minor,
                                    bp_uint16 *version,
                                    bp_uint64 *archive_size)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    int rc;

    rc = mini_io_seek(io, 0, 0); /* SEEK_SET */
    if (rc != 0) return BP_ERR_IO;

    rc = mini_io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    if (hdr[0] != BP_MAGIC_0 || hdr[1] != BP_MAGIC_1 ||
        hdr[2] != BP_MAGIC_2 || hdr[3] != BP_MAGIC_3) {
        return BP_ERR_MAGIC;
    }
    *version      = bp_read_le16(hdr + 4);
    if (*version != BP_FORMAT_VERSION)
        return BP_ERR_VERSION;

    if (*version == BP_FORMAT_VERSION) {
        bp_uint32 stored = bp_read_le32(hdr + BP_HEADER_CRC_OFF);
        bp_uint32 calc   = bp_mini_crc32(0, hdr, BP_HEADER_CRC_OFF);
        if (stored != calc) return BP_ERR_CORRUPT;
    }

    if (file_count)   *file_count   = bp_read_le32(hdr +  8);
    if (creator_os)   *creator_os   = hdr[12];
    if (creator_cpu)  *creator_cpu  = hdr[13];
    if (ver_major)    *ver_major    = hdr[14];
    if (ver_minor)    *ver_minor    = hdr[15];
    if (archive_size) *archive_size = bp_read_le64(hdr + 16);
    return BP_OK;
}

/* For v3 archives, verify the header's file-table CRC-32 against the actual
 * table bytes.  A no-op (BP_OK) for v2.  io position is left undefined. */
static int mini_verify_table_crc(bp_io_t *io, bp_uint16 version)
{
    bp_uint8  hdr[BP_HEADER_SIZE];
    bp_uint64 tbl_off, tbl_sz, rem;
    bp_uint32 stored, calc;
    bp_uint8  buf[256];
    int rc;

    if (version < BP_FORMAT_VERSION) return BP_OK;

    rc = mini_io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;
    rc = mini_io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;
    stored = bp_read_le32(hdr + BP_HEADER_TABLE_CRC_OFF);

    rc = mini_read_footer(io, &tbl_off, &tbl_sz);
    if (rc != BP_OK) return rc;
    {
        bp_off_t end_pos = mini_io_tell(io);
        bp_uint64 table_end = bp_u64_add(tbl_off, tbl_sz);
        bp_uint64 expected = bp_u64_add(table_end, bp_u64_from_u32(BP_FOOTER_SIZE));
        if (end_pos < 0 || bp_u64_cmp(table_end, tbl_off) < 0 ||
            bp_u64_cmp(expected, table_end) < 0 ||
            bp_u64_cmp(expected, bp_off_to_u64(end_pos)) != 0 ||
            bp_u64_cmp(expected, bp_read_le64(hdr + 16)) != 0)
            return BP_ERR_CORRUPT;
    }

    rc = mini_io_seek(io, bp_u64_to_off(tbl_off), 0);
    if (rc != 0) return BP_ERR_IO;

    calc = 0;
    rem = tbl_sz;
    while (!bp_u64_is_zero(rem)) {
        bp_uint32 chunk = (rem.hi != 0 || rem.lo > (bp_uint32)sizeof(buf))
                          ? (bp_uint32)sizeof(buf) : rem.lo;
        rc = mini_io_read(io, buf, chunk);
        if (rc != BP_OK) return rc;
        calc = bp_mini_crc32(calc, buf, chunk);
        rem = bp_u64_sub(rem, bp_u64_from_u32(chunk));
    }
    if (calc != stored) return BP_ERR_CORRUPT;
    return BP_OK;
}

/* Read one table record.  version selects the tail layout (v2: 34 bytes,
 * v3: 41 with method + header_size + checksum).  entry->name is malloc'd;
 * caller must free. */
static int mini_read_table_record(bp_io_t *io, bp_mini_entry_t *entry,
                                  bp_uint16 version)
{
    bp_uint8 buf2[2];
    bp_uint8 fixed[BP_TABLE_REC_TAIL_V6];
    bp_uint32 tail;
    bp_uint16 name_len;
    char *name;
    int rc;

    tail = (version >= BP_FORMAT_VERSION) ? BP_TABLE_REC_TAIL_V6
                                          : BP_TABLE_REC_TAIL_V2;

    rc = mini_io_read(io, buf2, 2);
    if (rc != BP_OK) return rc;
    name_len = bp_read_le16(buf2);
    if (name_len == 0 || name_len > BP_MAX_NAME_LEN) return BP_ERR_CORRUPT;

    name = (char *)malloc((size_t)(name_len + 1));
    if (name == NULL) return BP_ERR_NOMEM;

    rc = mini_io_read(io, name, (bp_uint32)name_len);
    if (rc != BP_OK) { free(name); return rc; }
    if (!bp_archive_name_valid(name, (bp_uint32)name_len)) {
        free(name); return BP_ERR_CORRUPT;
    }
    name[name_len] = '\0';

    rc = mini_io_read(io, fixed, tail);
    if (rc != BP_OK) { free(name); return rc; }

    memset(entry, 0, sizeof(*entry));
    entry->name           = name;
    entry->name_len       = name_len;
    entry->original_size  = bp_read_le64(fixed +  0);
    entry->compressed_size= bp_read_le64(fixed +  8);
    entry->entry_type     = fixed[16];
    entry->platform       = fixed[17];
    entry->mtime          = bp_read_le64(fixed + 18);
    entry->offset         = bp_read_le64(fixed + 26);
    if (tail >= BP_TABLE_REC_TAIL_V3) {
        entry->method      = fixed[34];
        entry->header_size = bp_read_le16(fixed + 35);
        entry->checksum    = bp_read_le32(fixed + 37);
    }
    if (tail >= BP_TABLE_REC_TAIL_V6)
        entry->flags = bp_read_le16(fixed + 41);
    if (!(entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
        !bp_u64_is_zero(entry->mtime)) {
        free(entry->name); entry->name = NULL; return BP_ERR_CORRUPT;
    }
    return BP_OK;
}

/*
 * Read the inline entry header at the given archive offset.
 * After this call, io is positioned at the first block header.
 * Fills: method, block_size, checksum, checksum_type.
 */
static int mini_read_inline_meta(bp_io_t *io, bp_uint64 entry_offset,
                                 bp_uint8 *method, bp_uint32 *block_size,
                                 bp_uint32 *checksum, bp_uint8 *checksum_type,
                                 const bp_mini_entry_t *expected)
{
    bp_uint8 buf4[4];
    bp_uint8 fields[38];
    bp_uint8 name_buf[128];
    bp_uint16 name_len;
    bp_uint32 pos;
    int rc;

    rc = mini_io_seek(io, bp_u64_to_off(entry_offset), 0); /* SEEK_SET */
    if (rc != 0) return BP_ERR_IO;

    /* header_size(2) + name_len(2) */
    rc = mini_io_read(io, buf4, 4);
    if (rc != BP_OK) return rc;
    name_len = bp_read_le16(buf4 + 2);
    if (name_len == 0 || name_len > BP_MAX_NAME_LEN ||
        bp_read_le16(buf4) != (bp_uint16)(BP_ENTRY_FIXED + name_len))
        return BP_ERR_CORRUPT;

    if (expected != NULL) {
        if (name_len != expected->name_len ||
            bp_read_le16(buf4) != expected->header_size) return BP_ERR_CORRUPT;
        pos = 0;
        while (pos < name_len) {
            bp_uint32 n = (bp_uint32)name_len - pos;
            if (n > sizeof(name_buf)) n = sizeof(name_buf);
            rc = mini_io_read(io, name_buf, n);
            if (rc != BP_OK) return rc;
            if (memcmp(name_buf, expected->name + pos, (size_t)n) != 0)
                return BP_ERR_CORRUPT;
            pos += n;
        }
    } else {
        rc = mini_io_seek(io, (bp_off_t)name_len, 1);
        if (rc != 0) return BP_ERR_IO;
    }

    /* 38-byte fields block */
    rc = mini_io_read(io, fields, 38);
    if (rc != BP_OK) return rc;

    /* original_size  at fields[0..7]  -- already known from table */
    /* compressed_size at fields[8..15] -- already known from table */
    *checksum      = bp_read_le32(fields + 16);
    *method        = fields[20];
    *checksum_type = fields[21];
    *block_size    = bp_read_le32(fields + 22);
    if (expected != NULL &&
        (bp_u64_cmp(bp_read_le64(fields), expected->original_size) != 0 ||
         bp_u64_cmp(bp_read_le64(fields + 8), expected->compressed_size) != 0 ||
         *checksum != expected->checksum || *method != expected->method ||
         *checksum_type != BP_CKSUM_CRC32 || fields[26] != expected->entry_type ||
         fields[27] != expected->platform ||
         bp_u64_cmp(bp_read_le64(fields + 28), expected->mtime) != 0 ||
         bp_read_le16(fields + 36) != expected->flags))
        return BP_ERR_CORRUPT;
    return BP_OK;
}

/* Validate all table/header pairs and resource limits before opening output. */
static int mini_preflight_extract(bp_io_t *io, bp_uint32 count,
                                  bp_uint16 version, bp_uint64 table_offset)
{
    bp_uint32 i;
    bp_uint64 next = bp_u64_from_u32(BP_HEADER_SIZE);
    bp_off_t table_start = mini_io_tell(io);
    for (i = 0; i < count; i++) {
        bp_mini_entry_t entry;
        bp_off_t resume;
        bp_uint8 method, checksum_type;
        bp_uint32 block_size, checksum;
        int rc = mini_read_table_record(io, &entry, version);
        if (rc != BP_OK) return rc;
        resume = mini_io_tell(io);
        if (resume < 0 || bp_u64_cmp(entry.offset, next) != 0) {
            free(entry.name); return BP_ERR_CORRUPT;
        }
        rc = mini_read_inline_meta(io, entry.offset, &method, &block_size,
                                   &checksum, &checksum_type, &entry);
        if (rc == BP_OK && method > BP_METHOD_BOLTLZ) rc = BP_ERR_UNSUPPORTED_METHOD;
        if (rc == BP_OK && (entry.original_size.hi != 0 ||
                            block_size > BP_MINI_MAX_BLOCK_SIZE)) rc = BP_ERR_OVERFLOW;
        if (rc == BP_OK && entry.entry_type > BP_TYPE_DIR) rc = BP_ERR_CORRUPT;
        if (rc == BP_OK && entry.entry_type == BP_TYPE_SYMLINK) rc = BP_ERR_PARTIAL;
        if (rc == BP_OK && entry.entry_type == BP_TYPE_META &&
            entry.platform == BP_META_PORTABLE && entry.original_size.lo > BP_META_MAX_SIZE)
            rc = BP_ERR_CORRUPT;
        next = bp_u64_add(entry.offset, bp_u64_from_u32(entry.header_size));
        next = bp_u64_add(next, entry.compressed_size);
        if (bp_u64_cmp(next, entry.offset) < 0 || bp_u64_cmp(next, table_offset) > 0)
            rc = BP_ERR_CORRUPT;
        free(entry.name);
        if (rc != BP_OK) return rc;
        if (mini_io_seek(io, resume, 0) != 0) return BP_ERR_IO;
    }
    if (bp_u64_cmp(next, table_offset) != 0) return BP_ERR_CORRUPT;
    return mini_io_seek(io, table_start, 0) == 0 ? BP_OK : BP_ERR_IO;
}

/* ---- decompress & write one entry's data ------------------------------- */

/*
 * Extraction buffer pair shared across entries within one extract/test run.
 * Grow-only: buffers are (re)allocated when an entry needs more capacity
 * and freed once by the caller, avoiding per-entry heap churn (the DOS far
 * heap fragments easily).
 */
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
    b->in_buf  = NULL;
    b->out_buf = NULL;
    b->in_cap  = 0;
    b->out_cap = 0;
}

static int mini_bufs_ensure(mini_extract_bufs_t *b,
                            bp_uint32 in_need, bp_uint32 out_need)
{
    if (b->in_cap < in_need) {
        /* Contents need not survive a grow: free + malloc avoids the
         * realloc copy. */
        free(b->in_buf);
        b->in_cap = 0;
        b->in_buf = (bp_uint8 *)malloc((size_t)in_need);
        if (b->in_buf == NULL) {
            return BP_ERR_NOMEM;
        }
        b->in_cap = in_need;
    }
    if (b->out_cap < out_need) {
        free(b->out_buf);
        b->out_cap = 0;
        b->out_buf = (bp_uint8 *)malloc((size_t)out_need);
        if (b->out_buf == NULL) {
            return BP_ERR_NOMEM;
        }
        b->out_cap = out_need;
    }
    return BP_OK;
}

static int mini_extract_data(bp_io_t *arc_io, bp_io_t *out_io,
                             bp_uint64 original_size, bp_uint32 block_size,
                             bp_uint8 method, bp_uint32 expected_checksum,
                             mini_extract_bufs_t *bufs, bp_uint16 version)
{
    bp_uint8  *in_buf;
    bp_uint8  *out_buf;
    bp_uint32  in_buf_size;
    bp_uint64  remaining;
    bp_uint8   ck_type     = BP_CKSUM_CRC32;
    bp_uint32  adler       = bp_mini_checksum_seed(ck_type);
    bp_uint32  blk_hdr_sz  = (version >= BP_FORMAT_VERSION)
                             ? BP_BLOCK_HDR_V3 : BP_BLOCK_HDR_V2;
    bp_uint8   blk_hdr[BP_BLOCK_HDR_V3];
    int        rc;

    if (bp_u64_is_zero(original_size) || block_size == 0) {
        return (expected_checksum == bp_mini_checksum_seed(ck_type))
               ? BP_OK : BP_ERR_CHECKSUM;
    }

    in_buf_size = block_size + block_size / 16 + 64 + 3;
    rc = mini_bufs_ensure(bufs, in_buf_size, block_size);
    if (rc != BP_OK) {
        return rc;
    }
    in_buf  = bufs->in_buf;
    out_buf = bufs->out_buf;

    remaining = original_size;

    while (!bp_u64_is_zero(remaining)) {
        bp_uint32 comp_size;
        bp_uint32 orig_size;
        bp_uint32 out_len;
        long n;

        rc = mini_io_read(arc_io, blk_hdr, blk_hdr_sz);
        if (rc != BP_OK) { return rc; }

        comp_size = bp_read_le32(blk_hdr);
        orig_size = bp_read_le32(blk_hdr + 4);

        /* Compare against this entry's limit, not the cached capacity,
         * so validation strictness is unchanged. */
        if (comp_size > in_buf_size || orig_size > block_size) {
            return BP_ERR_CORRUPT;
        }
        if (orig_size == 0 ||
            bp_u64_cmp(bp_u64_from_u32(orig_size), remaining) > 0) {
            return BP_ERR_CORRUPT;
        }

        n = arc_io->read(arc_io->handle, in_buf, comp_size);
        if (n != (long)comp_size) {
            return BP_ERR_TRUNCATED;
        }

        /* v3: verify block CRC before decompression */
        if (blk_hdr_sz == BP_BLOCK_HDR_V3) {
            bp_uint32 stored_crc = bp_read_le32(blk_hdr + 8);
            bp_uint32 calc_crc   = bp_mini_crc32(0, in_buf, comp_size);
            if (stored_crc != calc_crc) {
                return BP_ERR_CHECKSUM;
            }
        }

        if (comp_size == orig_size) {
            /* Block was stored (no compression, or fallback-to-stored) */
            memcpy(out_buf, in_buf, orig_size);
            out_len = orig_size;
        } else {
            out_len = orig_size;
            rc = bp_mini_decompress_block(in_buf, comp_size, out_buf, &out_len,
                                          method);
            if (rc != BP_OK) { return rc; }
            if (out_len != orig_size) {
                return BP_ERR_DECOMPRESS;
            }
        }

        adler = bp_mini_checksum_typed(ck_type, adler, out_buf, orig_size);

        if (out_io != NULL) {
            n = out_io->write(out_io->handle, out_buf, orig_size);
            if (n != (long)orig_size) {
                return BP_ERR_IO;
            }
        }

        remaining = bp_u64_sub(remaining, bp_u64_from_u32(orig_size));
    }

    if (adler != expected_checksum) return BP_ERR_CHECKSUM;
    return BP_OK;
}

/* ---- bp_mini_archive_list ---------------------------------------------- */

int bp_mini_archive_list(bp_io_t *io, bp_mini_list_fn fn, void *user_data)
{
    bp_uint64  table_offset;
    bp_uint64  table_size;
    bp_uint8   tbl_hdr[BP_TABLE_HDR_V3];
    bp_uint32  entry_count;
    bp_uint32  header_count;
    bp_uint32  i;
    bp_uint16  dummy_version;
    bp_uint32  hdr_sz;
    int        rc;

    rc = mini_read_archive_header(io, &header_count, NULL, NULL, NULL, NULL,
                                  &dummy_version, NULL);
    if (rc != BP_OK) return rc;

    rc = mini_verify_table_crc(io, dummy_version);
    if (rc != BP_OK) return rc;

    hdr_sz = (dummy_version >= BP_FORMAT_VERSION) ? BP_TABLE_HDR_V3 : BP_TABLE_HDR_V2;

    rc = mini_read_footer(io, &table_offset, &table_size);
    if (rc != BP_OK) return rc;

    rc = mini_io_seek(io, bp_u64_to_off(table_offset), 0);
    if (rc != 0) return BP_ERR_IO;

    rc = mini_io_read(io, tbl_hdr, hdr_sz);
    if (rc != BP_OK) return rc;

    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3) {
        return BP_ERR_MAGIC;
    }
    entry_count = bp_read_le32(tbl_hdr + 4);
    if (entry_count != header_count) return BP_ERR_CORRUPT;

    for (i = 0; i < entry_count; i++) {
        bp_mini_entry_t entry;
        rc = mini_read_table_record(io, &entry, dummy_version);
        if (rc != BP_OK) return rc;

        if (fn != NULL) {
            rc = fn(&entry, user_data);
        }
        free(entry.name);
        if (rc != 0) return BP_OK; /* callback requested stop */
    }

    return BP_OK;
}

/* ---- bp_mini_archive_info ---------------------------------------------- */

int bp_mini_archive_info(bp_io_t *io, bp_mini_archive_info_t *info)
{
    bp_uint64  table_offset;
    bp_uint64  table_size;
    bp_uint8   tbl_hdr[BP_TABLE_HDR_V3];
    bp_uint32  entry_count;
    bp_uint32  i;
    bp_uint32  hdr_sz;
    int        rc;
    bp_off_t   saved_pos;
    bp_uint8   m;
    bp_uint32  bs;
    bp_uint32  cs;
    bp_uint8   ct;

    if (info == NULL) return BP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));

    rc = mini_read_archive_header(io,
                                  &info->file_count,
                                  &info->creator_os,
                                  &info->creator_cpu,
                                  &info->creator_ver_major,
                                  &info->creator_ver_minor,
                                  &info->version,
                                  &info->archive_size);
    if (rc != BP_OK) return rc;

    rc = mini_verify_table_crc(io, info->version);
    if (rc != BP_OK) return rc;

    hdr_sz = (info->version >= BP_FORMAT_VERSION) ? BP_TABLE_HDR_V3 : BP_TABLE_HDR_V2;

    rc = mini_read_footer(io, &table_offset, &table_size);
    if (rc != BP_OK) return rc;

    rc = mini_io_seek(io, bp_u64_to_off(table_offset), 0);
    if (rc != 0) return BP_ERR_IO;

    rc = mini_io_read(io, tbl_hdr, hdr_sz);
    if (rc != BP_OK) return rc;

    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3) {
        return BP_ERR_MAGIC;
    }
    entry_count = bp_read_le32(tbl_hdr + 4);
    if (entry_count != info->file_count) return BP_ERR_CORRUPT;

    for (i = 0; i < entry_count; i++) {
        bp_mini_entry_t entry;
        rc = mini_read_table_record(io, &entry, info->version);
        if (rc != BP_OK) return rc;

        info->entry_count++;
        if (entry.entry_type == BP_TYPE_DIR) {
            info->dir_count++;
        } else if (info->version >= BP_FORMAT_VERSION) {
            /* v3: method is already in the table record -- no seek needed */
            if (entry.method < 5) info->method_counts[entry.method]++;
        } else {
            /* v2: seek to inline entry header to read compression method */
            saved_pos = mini_io_tell(io);
            if (mini_read_inline_meta(io, entry.offset,
                                      &m, &bs, &cs, &ct, NULL) == BP_OK) {
                if (m < 5) info->method_counts[m]++;
            }
            mini_io_seek(io, saved_pos, 0);
        }

        info->total_original   = bp_u64_add(info->total_original,
                                             entry.original_size);
        info->total_compressed = bp_u64_add(info->total_compressed,
                                             entry.compressed_size);
        free(entry.name);
    }

    return BP_OK;
}

/* ---- bp_mini_archive_extract_all --------------------------------------- */

static int mini_extract_all_impl(bp_io_t *io, bp_mini_extract_fn open_fn,
                                 bp_mini_extract_done_fn done_fn,
                                 bp_mini_extract_done_status_fn status_fn,
                                 void *user_data)
{
    bp_uint64  table_offset;
    bp_uint64  table_size;
    bp_uint8   tbl_hdr[BP_TABLE_HDR_V3];
    bp_uint32  entry_count;
    bp_uint32  header_count;
    bp_uint32  i;
    bp_uint16  dummy_version;
    bp_uint32  hdr_sz;
    mini_extract_bufs_t bufs = {NULL, NULL, 0, 0};
    int        rc;
    int        partial = 0;

    rc = mini_read_archive_header(io, &header_count, NULL, NULL, NULL, NULL,
                                  &dummy_version, NULL);
    if (rc != BP_OK) return rc;

    rc = mini_verify_table_crc(io, dummy_version);
    if (rc != BP_OK) return rc;

    hdr_sz = (dummy_version >= BP_FORMAT_VERSION) ? BP_TABLE_HDR_V3 : BP_TABLE_HDR_V2;

    rc = mini_read_footer(io, &table_offset, &table_size);
    if (rc != BP_OK) return rc;

    rc = mini_io_seek(io, bp_u64_to_off(table_offset), 0);
    if (rc != 0) return BP_ERR_IO;

    rc = mini_io_read(io, tbl_hdr, hdr_sz);
    if (rc != BP_OK) return rc;

    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3) {
        return BP_ERR_MAGIC;
    }
    entry_count = bp_read_le32(tbl_hdr + 4);
    if (entry_count != header_count) return BP_ERR_CORRUPT;
    if (entry_count > BP_MINI_MAX_ENTRIES) return BP_ERR_OVERFLOW;

    rc = mini_preflight_extract(io, entry_count, dummy_version, table_offset);
    if (rc != BP_OK) return rc;

    for (i = 0; i < entry_count; i++) {
        bp_mini_entry_t entry;
        bp_uint8  method;
        bp_uint32 block_size;
        bp_uint32 checksum;
        bp_uint8  checksum_type;
        bp_io_t  *out;
        bp_off_t  table_resume;
        int skip_metadata;
        int portable;
        bp_metadata_sink_t *metadata = NULL;
        bp_portable_metadata_t parsed;

        rc = mini_read_table_record(io, &entry, dummy_version);
        if (rc != BP_OK) {
            mini_bufs_free(&bufs);
            return rc;
        }

        /* Save position in table so we can return after seeking into data */
        table_resume = mini_io_tell(io);

        /* Read inline header to get method, block_size, checksum */
        method = BP_METHOD_STORED;
        block_size = 0;
        checksum = 0;
        checksum_type = BP_CKSUM_CRC32;

        rc = mini_read_inline_meta(io, entry.offset,
                                   &method, &block_size, &checksum,
                                   &checksum_type, &entry);
        if (rc != BP_OK) {
            free(entry.name);
            mini_bufs_free(&bufs);
            return rc;
        }
        entry.method        = method;
        entry.block_size    = block_size;
        entry.checksum      = checksum;
        entry.checksum_type = checksum_type;

        /* Capability checks */
        if (method > BP_METHOD_BOLTLZ) {
#ifndef BP_MINI_QUIET
            fprintf(stderr,
                    "bpmini: skipping %s: method %u not supported by mini\n",
                    entry.name, (unsigned)method);
#endif
            free(entry.name);
            mini_bufs_free(&bufs);
            return BP_ERR_UNSUPPORTED_METHOD;
        }
        if (entry.original_size.hi != 0) {
#ifndef BP_MINI_QUIET
            fprintf(stderr,
                    "bpmini: skipping %s: file too large for mini\n",
                    entry.name);
#endif
            free(entry.name);
            mini_bufs_free(&bufs);
            return BP_ERR_OVERFLOW;
        }
        if (block_size > BP_MINI_MAX_BLOCK_SIZE) {
#ifndef BP_MINI_QUIET
            fprintf(stderr,
                    "bpmini: archive uses %u KB blocks -- re-create with -b %uk for mini\n",
                    (unsigned)(block_size / 1024),
                    (unsigned)(BP_MINI_MAX_BLOCK_SIZE / 1024));
#endif
            free(entry.name);
            mini_bufs_free(&bufs);
            return BP_ERR_OVERFLOW;
        }

        if (entry.entry_type > BP_TYPE_DIR) {
            free(entry.name); mini_bufs_free(&bufs); return BP_ERR_CORRUPT;
        }
        if (entry.entry_type == BP_TYPE_SYMLINK) {
#ifndef BP_MINI_QUIET
            fprintf(stderr, "bpmini: symbolic link not supported: %s\n", entry.name);
#endif
            free(entry.name); mini_bufs_free(&bufs); return BP_ERR_PARTIAL;
        }
        skip_metadata = entry.entry_type == BP_TYPE_META || entry.entry_type == BP_TYPE_RSRC;
        portable = entry.entry_type == BP_TYPE_META && entry.platform == BP_META_PORTABLE;
        if (portable && entry.original_size.lo > BP_META_MAX_SIZE) {
            free(entry.name); mini_bufs_free(&bufs); return BP_ERR_CORRUPT;
        }
        if (portable) {
            metadata = (bp_metadata_sink_t *)malloc(sizeof(*metadata));
            if (metadata == NULL) { free(entry.name); mini_bufs_free(&bufs); return BP_ERR_NOMEM; }
            bp_metadata_sink_init(metadata);
        }
        if (skip_metadata) {
#ifndef BP_MINI_QUIET
            fprintf(stderr, "bpmini: metadata not restored: %s\n", entry.name);
#endif
            partial = 1;
        }
        out = NULL;
        if (!skip_metadata && open_fn != NULL) {
            out = open_fn(&entry, user_data);
        }
        if (!skip_metadata && entry.entry_type != BP_TYPE_DIR && out == NULL)
            partial = 1;

        if (bp_u64_is_zero(entry.original_size) && checksum != 0) {
            if (out && status_fn) (void)status_fn(&entry, out, BP_ERR_CHECKSUM, user_data);
            else if (out && done_fn) done_fn(&entry, out, user_data);
            free(metadata);
            free(entry.name); mini_bufs_free(&bufs); return BP_ERR_CHECKSUM;
        }

        /* Decompress: io is now positioned at the first block by
         * mini_read_inline_meta, so pass io directly */
        if (entry.entry_type != BP_TYPE_DIR &&
            entry.entry_type != BP_TYPE_SYMLINK &&
            !bp_u64_is_zero(entry.original_size)) {
            rc = mini_extract_data(io, portable ? &metadata->io : out,
                                   entry.original_size, block_size,
                                   method, checksum, &bufs, dummy_version);
            if (rc != BP_OK) {
                if (out && status_fn) (void)status_fn(&entry, out, rc, user_data);
                else if (out && done_fn) done_fn(&entry, out, user_data);
                free(metadata);
                free(entry.name);
                mini_bufs_free(&bufs);
                return rc;
            }
        }

        if (portable) {
            rc = bp_metadata_parse(metadata->data, metadata->length, &parsed);
            free(metadata);
            if (rc == BP_ERR_CORRUPT) {
                free(entry.name); mini_bufs_free(&bufs); return rc;
            }
        }

        if (out && status_fn) {
            rc = status_fn(&entry, out, BP_OK, user_data);
            if (rc == BP_ERR_PARTIAL) {
                partial = 1;
            } else if (rc != BP_OK) {
                free(entry.name); mini_bufs_free(&bufs); return rc;
            }
        } else if (out && done_fn) {
            done_fn(&entry, out, user_data);
        }

        free(entry.name);
        if (mini_io_seek(io, table_resume, 0) != 0) {
            mini_bufs_free(&bufs); return BP_ERR_IO;
        }
    }

    mini_bufs_free(&bufs);
    return partial ? BP_ERR_PARTIAL : BP_OK;
}

int bp_mini_archive_extract_all(bp_io_t *io, bp_mini_extract_fn open_fn,
                                bp_mini_extract_done_fn done_fn, void *user_data)
{
    return mini_extract_all_impl(io, open_fn, done_fn, NULL, user_data);
}

int bp_mini_archive_extract_all_ex(bp_io_t *io, bp_mini_extract_fn open_fn,
                                   bp_mini_extract_done_status_fn done_fn,
                                   void *user_data)
{
    if (done_fn == NULL) return BP_ERR_INVALID_ARG;
    return mini_extract_all_impl(io, open_fn, NULL, done_fn, user_data);
}

/* ---- bp_mini_archive_test ---------------------------------------------- */

int bp_mini_archive_test(bp_io_t *io, bp_mini_test_fn fn, void *user_data)
{
    bp_uint64  table_offset;
    bp_uint64  table_size;
    bp_uint8   tbl_hdr[BP_TABLE_HDR_V3];
    bp_uint32  entry_count;
    bp_uint32  header_count;
    bp_uint32  i;
    bp_uint16  dummy_version;
    bp_uint32  hdr_sz;
    mini_extract_bufs_t bufs = {NULL, NULL, 0, 0};
    int        rc;
    int        overall = BP_OK;

    rc = mini_read_archive_header(io, &header_count, NULL, NULL, NULL, NULL,
                                  &dummy_version, NULL);
    if (rc != BP_OK) return rc;

    /* v3: verify the header+file-table CRC-32 before trusting the table */
    rc = mini_verify_table_crc(io, dummy_version);
    if (rc != BP_OK) return rc;

    hdr_sz = (dummy_version >= BP_FORMAT_VERSION) ? BP_TABLE_HDR_V3 : BP_TABLE_HDR_V2;

    rc = mini_read_footer(io, &table_offset, &table_size);
    if (rc != BP_OK) return rc;

    rc = mini_io_seek(io, bp_u64_to_off(table_offset), 0);
    if (rc != 0) return BP_ERR_IO;

    rc = mini_io_read(io, tbl_hdr, hdr_sz);
    if (rc != BP_OK) return rc;

    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3) {
        return BP_ERR_MAGIC;
    }
    entry_count = bp_read_le32(tbl_hdr + 4);
    if (entry_count != header_count) return BP_ERR_CORRUPT;

    for (i = 0; i < entry_count; i++) {
        bp_mini_entry_t entry;
        bp_uint8  method;
        bp_uint32 block_size;
        bp_uint32 checksum;
        bp_uint8  checksum_type;
        bp_off_t  table_resume;
        int       result;
        int       portable;
        bp_metadata_sink_t *metadata = NULL;
        bp_portable_metadata_t parsed;

        rc = mini_read_table_record(io, &entry, dummy_version);
        if (rc != BP_OK) {
            mini_bufs_free(&bufs);
            return rc;
        }

        table_resume = mini_io_tell(io);

        method = BP_METHOD_STORED;
        block_size = 0;
        checksum = 0;
        checksum_type = BP_CKSUM_CRC32;

        rc = mini_read_inline_meta(io, entry.offset,
                                   &method, &block_size, &checksum,
                                   &checksum_type, &entry);
        if (rc != BP_OK) {
            free(entry.name);
            mini_bufs_free(&bufs);
            return rc;
        }
        entry.method     = method;
        entry.block_size = block_size;
        entry.checksum   = checksum;

        portable = entry.entry_type == BP_TYPE_META && entry.platform == BP_META_PORTABLE;
        if (portable) {
            metadata = (bp_metadata_sink_t *)malloc(sizeof(*metadata));
            if (metadata == NULL) { free(entry.name); mini_bufs_free(&bufs); return BP_ERR_NOMEM; }
            bp_metadata_sink_init(metadata);
        }
        if (method > BP_METHOD_BOLTLZ) {
            result = BP_ERR_UNSUPPORTED_METHOD;
        } else if (entry.original_size.hi != 0) {
#ifndef BP_MINI_QUIET
            fprintf(stderr,
                    "bpmini: skipping test of %s: too large for mini\n",
                    entry.name);
#endif
            result = BP_ERR_OVERFLOW;
        } else if (portable && entry.original_size.lo > BP_META_MAX_SIZE) {
            result = BP_ERR_CORRUPT;
        } else if (block_size > BP_MINI_MAX_BLOCK_SIZE) {
#ifndef BP_MINI_QUIET
            fprintf(stderr,
                    "bpmini: archive uses %u KB blocks -- re-create with -b %uk for mini\n",
                    (unsigned)(block_size / 1024),
                    (unsigned)(BP_MINI_MAX_BLOCK_SIZE / 1024));
#endif
            result = BP_ERR_OVERFLOW;
        } else {
            result = mini_extract_data(io, portable ? &metadata->io : NULL,
                                       entry.original_size, block_size,
                                       method, checksum, &bufs, dummy_version);
            if (result == BP_OK && portable)
                result = bp_metadata_parse(metadata->data, metadata->length, &parsed);
        }

        free(metadata);

        if (result != BP_OK && overall == BP_OK) overall = result;

        if (fn != NULL) {
            fn(&entry, result, user_data);
        }

        free(entry.name);
        if (mini_io_seek(io, table_resume, 0) != 0) {
            mini_bufs_free(&bufs); return BP_ERR_IO;
        }
    }

    mini_bufs_free(&bufs);
    return overall;
}
