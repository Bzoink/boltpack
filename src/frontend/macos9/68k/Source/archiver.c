/*
 * archiver.c -- archive format read/write for BoltPack
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "archiver.h"
#include "compress.h"
#include <stdlib.h>
#include <string.h>

/* ---- helpers ----------------------------------------------------------- */

static int io_write(bp_io_t *io, const void *buf, bp_uint32 count)
{
    long n = io->write(io->handle, buf, count);
    return (n == (long)count) ? BP_OK : BP_ERR_IO;
}

static int io_read(bp_io_t *io, void *buf, bp_uint32 count)
{
    long n = io->read(io->handle, buf, count);
    return (n == (long)count) ? BP_OK : BP_ERR_TRUNCATED;
}

static int io_seek(bp_io_t *io, long offset, int origin)
{
    return io->seek(io->handle, offset, origin);
}

static long io_tell(bp_io_t *io)
{
    return io->tell(io->handle);
}

static int write_buf(bp_io_t *io, const bp_uint8 *buf, bp_uint32 len)
{
    return io_write(io, buf, len);
}

/* ---- archive creation -------------------------------------------------- */

int bp_archive_create(bp_archive_t *ar, bp_io_t *io,
                      bp_uint32 block_size, bp_uint8 method)
{
    bp_uint8 hdr[BP_HEADER_SIZE];

    if (ar == NULL || io == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    memset(ar, 0, sizeof(*ar));
    ar->io = io;
    ar->block_size = block_size ? block_size : BP_BLOCK_DEFAULT;
    ar->method = method;
    ar->creator_os = BP_OS_UNKNOWN;
    ar->creator_cpu = BP_CPU_UNKNOWN;

    /* detect OS at compile time */
#if defined(__DJGPP__) || defined(__DOS__) || defined(MSDOS)
    ar->creator_os = BP_OS_DOS;
#elif defined(_WIN32) || defined(_WIN64)
    ar->creator_os = BP_OS_WINDOWS;
#elif defined(__linux__)
    ar->creator_os = BP_OS_LINUX;
#elif defined(__FreeBSD__)
    ar->creator_os = BP_OS_FREEBSD;
#elif defined(__NetBSD__)
    ar->creator_os = BP_OS_NETBSD;
#elif defined(__OpenBSD__)
    ar->creator_os = BP_OS_OPENBSD;
#elif defined(__APPLE__)
    ar->creator_os = BP_OS_MACOS;
#elif defined(__amigaos__) || defined(AMIGA)
    ar->creator_os = BP_OS_AMIGA;
#elif defined(__atarist__)
    ar->creator_os = BP_OS_ATARI;
#endif

    /* detect CPU at compile time */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    ar->creator_cpu = BP_CPU_X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    ar->creator_cpu = BP_CPU_X86;
#elif defined(__aarch64__)
    ar->creator_cpu = BP_CPU_ARM64;
#elif defined(__arm__)
    ar->creator_cpu = BP_CPU_ARM32;
#elif defined(__powerpc64__)
    ar->creator_cpu = BP_CPU_PPC64;
#elif defined(__powerpc__)
    ar->creator_cpu = BP_CPU_PPC32;
#elif defined(__m68k__)
    ar->creator_cpu = BP_CPU_M68K;
#endif

    ar->entries_written = 0;
    ar->entry_list = NULL;
    ar->entry_list_cap = 0;

    {
        bp_uint64 z = {0, 0};
        ar->archive_orig_total = z;
        ar->archive_comp_total = z;
        ar->archive_orig_done  = z;
    }

    /* write placeholder header (patched in finalize) */
    memset(hdr, 0, BP_HEADER_SIZE);
    hdr[0] = BP_MAGIC_0;
    hdr[1] = BP_MAGIC_1;
    hdr[2] = BP_MAGIC_2;
    hdr[3] = BP_MAGIC_3;
    bp_write_le16(hdr + 4, BP_FORMAT_VERSION);
    /* flags = 0, file_count = 0 (patched later) */
    hdr[12] = ar->creator_os;
    hdr[13] = ar->creator_cpu;
    hdr[14] = BP_VERSION_MAJOR;
    hdr[15] = BP_VERSION_MINOR;
    /* archive_size = 0 (patched later), reserved = 0 */

    return write_buf(ar->io, hdr, BP_HEADER_SIZE);
}

/* grow entry list */
static int entry_list_grow(bp_archive_t *ar)
{
    bp_uint32 new_cap;
    bp_entry_t *new_list;

    if (ar->entries_written < ar->entry_list_cap) {
        return BP_OK;
    }
    new_cap = ar->entry_list_cap ? ar->entry_list_cap * 2 : 64;
    new_list = (bp_entry_t *)realloc(ar->entry_list,
                                     (size_t)new_cap * sizeof(bp_entry_t));
    if (new_list == NULL) {
        return BP_ERR_NOMEM;
    }
    ar->entry_list = new_list;
    ar->entry_list_cap = new_cap;
    return BP_OK;
}

int bp_archive_add_entry(bp_archive_t *ar, const char *name,
                         bp_uint8 entry_type, bp_uint8 platform,
                         bp_io_t *file_io, bp_uint32 file_size,
                         bp_uint64 mtime)
{
    bp_uint16 name_len;
    bp_uint16 hdr_size;
    bp_uint8 ehdr[BP_ENTRY_FIXED]; /* fixed portion, name written separately */
    bp_entry_t *ent;
    int rc;
    long entry_offset;
    bp_uint32 adler;
    bp_uint64 total_comp;
    bp_uint8 *in_buf = NULL;
    bp_uint8 *out_buf = NULL;
    bp_uint32 out_buf_size;

    if (ar == NULL || name == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    name_len = (bp_uint16)strlen(name);
    if (strlen(name) > BP_MAX_NAME_LEN) {
        return BP_ERR_NAME_TOO_LONG;
    }

    hdr_size = (bp_uint16)(BP_ENTRY_FIXED + name_len);

    entry_offset = io_tell(ar->io);
    if (entry_offset < 0) {
        return BP_ERR_IO;
    }

    /* grow entry list for file table */
    rc = entry_list_grow(ar);
    if (rc != BP_OK) return rc;

    /* write entry header (compressed_size patched after compression) */
    memset(ehdr, 0, BP_ENTRY_FIXED);
    bp_write_le16(ehdr + 0, hdr_size);             /* header_size */
    bp_write_le16(ehdr + 2, name_len);             /* name_len */
    /* name written separately */
    /* original_size at offset 4+N -- written after name */
    /* other fields at their respective offsets after name */

    /* Write header_size and name_len */
    rc = write_buf(ar->io, ehdr, 4);
    if (rc != BP_OK) return rc;

    /* Write name */
    rc = write_buf(ar->io, (const bp_uint8 *)name, name_len);
    if (rc != BP_OK) return rc;

    /* Now write the remaining fixed fields as a block.
     * We build the remaining 38 bytes: original_size(8) + compressed_size(8) +
     * checksum(4) + method(1) + checksum_type(1) + block_size(4) +
     * entry_type(1) + platform(1) + mtime(8) + flags(2) = 38 bytes */
    {
        bp_uint8 fields[38];
        bp_uint64 orig64;
        long fields_pos;

        memset(fields, 0, 38);
        orig64 = bp_u64_from_u32(file_size);
        bp_write_le64(fields + 0, orig64);          /* original_size */
        /* compressed_size at fields+8: filled after compression */
        /* checksum at fields+16: filled after compression */
        fields[20] = (entry_type == BP_TYPE_DIR ||
                      entry_type == BP_TYPE_SYMLINK ||
                      file_size == 0)
                     ? BP_METHOD_STORED : ar->method;
        fields[21] = BP_CKSUM_CRC32;                /* checksum_type */
        if (file_size > 0 &&
            entry_type != BP_TYPE_DIR &&
            entry_type != BP_TYPE_SYMLINK) {
            bp_write_le32(fields + 22, ar->block_size);
        }
        fields[26] = entry_type;
        fields[27] = platform;
        bp_write_le64(fields + 28, mtime);
        /* flags at fields+36: already zero from memset */

        fields_pos = io_tell(ar->io);
        if (fields_pos < 0) return BP_ERR_IO;

        rc = write_buf(ar->io, fields, 38);
        if (rc != BP_OK) return rc;

        /* Now compress and write data blocks */
        total_comp = bp_u64_from_u32(0);
        adler = bp_checksum_seed(BP_CKSUM_CRC32); /* CRC-32 seed (0) */

        if (entry_type == BP_TYPE_SYMLINK && file_io != NULL && file_size > 0) {
            /* symlink: data is the target path, stored uncompressed */
            in_buf = (bp_uint8 *)malloc((size_t)file_size);
            if (in_buf == NULL) return BP_ERR_NOMEM;

            if (file_io->read(file_io->handle, in_buf, file_size) != (long)file_size) {
                free(in_buf);
                return BP_ERR_IO;
            }
            adler = bp_checksum_typed(BP_CKSUM_CRC32, adler, in_buf, file_size);
            total_comp = bp_u64_from_u32(file_size);

            /* no block headers for symlinks - direct data */
            rc = write_buf(ar->io, in_buf, file_size);
            free(in_buf);
            if (rc != BP_OK) return rc;
        } else if (file_size > 0 && file_io != NULL && entry_type != BP_TYPE_DIR) {
            bp_uint32 remaining = file_size;
            bp_uint32 bsize = ar->block_size;
            bp_uint8 actual_method = fields[20];

            /* allocate buffers */
            in_buf = (bp_uint8 *)malloc((size_t)bsize);
            if (in_buf == NULL) return BP_ERR_NOMEM;

            out_buf_size = bsize + bsize / 16 + 64 + 3;
            out_buf = (bp_uint8 *)malloc((size_t)out_buf_size);
            if (out_buf == NULL) { free(in_buf); return BP_ERR_NOMEM; }

            while (remaining > 0) {
                bp_uint32 chunk = (remaining > bsize) ? bsize : remaining;
                bp_uint32 comp_len = 0;
                bp_uint8 blk_hdr[8];
                long n;

                /* read from source */
                n = file_io->read(file_io->handle, in_buf, chunk);
                if (n != (long)chunk) {
                    free(in_buf); free(out_buf);
                    return BP_ERR_IO;
                }

                /* update checksum */
                adler = bp_checksum_typed(BP_CKSUM_CRC32, adler, in_buf, chunk);

                if (actual_method == BP_METHOD_FASTLZ) {
                    rc = bp_compress_block(in_buf, chunk, out_buf, &comp_len);
                    if (rc != BP_OK) {
                        free(in_buf); free(out_buf);
                        return rc;
                    }
                    /* store as-is if compression doesn't help */
                    if (comp_len >= chunk) {
                        comp_len = chunk;
                        memcpy(out_buf, in_buf, (size_t)chunk);
                    }
                } else {
                    /* stored */
                    comp_len = chunk;
                    memcpy(out_buf, in_buf, (size_t)chunk);
                }

                /* write block header */
                bp_write_le32(blk_hdr + 0, comp_len);
                bp_write_le32(blk_hdr + 4, chunk);
                rc = write_buf(ar->io, blk_hdr, 8);
                if (rc != BP_OK) { free(in_buf); free(out_buf); return rc; }

                /* write block data */
                rc = write_buf(ar->io, out_buf, comp_len);
                if (rc != BP_OK) { free(in_buf); free(out_buf); return rc; }

                total_comp = bp_u64_add(total_comp, bp_u64_from_u32(comp_len + 8));
                remaining -= chunk;

                /* progress callback */
                if (ar->progress_fn) {
                    bp_progress_t p;
                    memset(&p, 0, sizeof(p));
                    p.file_index = ar->entries_written;
                    p.file_count = 0; /* unknown during creation */
                    p.file_name = name;
                    p.file_orig_size = orig64;
                    p.file_bytes_done = bp_u64_from_u32(file_size - remaining);
                    p.file_comp_size = total_comp;
                    p.archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                                      p.file_bytes_done);
                    p.archive_comp_total = bp_u64_add(ar->archive_comp_total,
                                                      total_comp);
                    p.archive_orig_done  = bp_u64_add(ar->archive_orig_done,
                                                      p.file_bytes_done);
                    if (ar->progress_fn(&p, ar->progress_data) != 0) {
                        free(in_buf); free(out_buf);
                        return BP_ERR_ABORTED;
                    }
                }
            }

            free(in_buf);
            free(out_buf);
        } else {
            /* empty file or directory: no data to write */
            adler = bp_checksum_seed(BP_CKSUM_CRC32);
        }

        /* patch compressed_size, checksum in the fields we wrote */
        {
            bp_uint8 patch[12]; /* compressed_size(8) + checksum(4) */
            bp_write_le64(patch, total_comp);
            bp_write_le32(patch + 8, adler);

            /* seek back to fields_pos + 8 (skip original_size) */
            rc = io_seek(ar->io, fields_pos + 8, 0); /* SEEK_SET */
            if (rc != 0) return BP_ERR_IO;
            rc = write_buf(ar->io, patch, 12);
            if (rc != BP_OK) return rc;

            /* seek to end */
            rc = io_seek(ar->io, 0, 2); /* SEEK_END */
            if (rc != 0) return BP_ERR_IO;
        }

        /* update archive totals */
        ar->archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                            bp_u64_from_u32(file_size));
        ar->archive_comp_total = bp_u64_add(ar->archive_comp_total, total_comp);
        ar->archive_orig_done  = bp_u64_add(ar->archive_orig_done,
                                            bp_u64_from_u32(file_size));

        /* record entry for file table */
        ent = &ar->entry_list[ar->entries_written];
        memset(ent, 0, sizeof(*ent));
        ent->name = (char *)malloc((size_t)(name_len + 1));
        if (ent->name == NULL) return BP_ERR_NOMEM;
        memcpy(ent->name, name, name_len);
        ent->name[name_len] = '\0';
        ent->name_len = name_len;
        ent->original_size = orig64;
        ent->compressed_size = total_comp;
        ent->checksum = adler;
        ent->method = fields[20];
        ent->checksum_type = BP_CKSUM_CRC32;
        ent->block_size = (file_size > 0 &&
                   entry_type != BP_TYPE_DIR &&
                   entry_type != BP_TYPE_SYMLINK)
                          ? ar->block_size : 0;
        ent->entry_type = entry_type;
        ent->platform = platform;
        ent->mtime = mtime;
        ent->flags = 0;
        ent->header_size = hdr_size;
        ent->offset = bp_u64_from_u32((bp_uint32)entry_offset);

        ar->entries_written++;
    }

    return BP_OK;
}

/* Write bytes while folding them into the running CRC-32 *crc. */
static int write_crc(bp_io_t *io, bp_uint32 *crc,
                     const bp_uint8 *buf, bp_uint32 len)
{
    *crc = bp_crc32(*crc, buf, len);
    return write_buf(io, buf, len);
}

/* Write one v3 file-table record, folding bytes into *crc. */
static int write_table_record(bp_io_t *io, bp_uint32 *crc, const bp_entry_t *e)
{
    bp_uint8 rec_hdr[2];
    bp_uint8 rec_fields[BP_TABLE_REC_TAIL_V3];
    int rc;

    bp_write_le16(rec_hdr, e->name_len);
    rc = write_crc(io, crc, rec_hdr, 2);
    if (rc != BP_OK) return rc;

    rc = write_crc(io, crc, (const bp_uint8 *)e->name, e->name_len);
    if (rc != BP_OK) return rc;

    bp_write_le64(rec_fields + 0, e->original_size);
    bp_write_le64(rec_fields + 8, e->compressed_size);
    rec_fields[16] = e->entry_type;
    rec_fields[17] = e->platform;
    bp_write_le64(rec_fields + 18, e->mtime);
    bp_write_le64(rec_fields + 26, e->offset);
    rec_fields[34] = e->method;                     /* v3: method */
    bp_write_le16(rec_fields + 35, e->header_size); /* v3: header_size */
    bp_write_le32(rec_fields + 37, e->checksum);    /* v3: payload checksum */
    return write_crc(io, crc, rec_fields, BP_TABLE_REC_TAIL_V3);
}

int bp_archive_finalize(bp_archive_t *ar)
{
    int rc;
    long table_start;
    long table_end;
    long archive_end;
    bp_uint32 i;
    bp_uint32 table_crc;
    bp_uint8 buf[32];

    if (ar == NULL) return BP_ERR_INVALID_ARG;

    table_start = io_tell(ar->io);
    if (table_start < 0) return BP_ERR_IO;

    table_crc = 0;

    /* -- write file table (v3) ------------------------------------------- */

    /* table header: magic(4) + entry_count(4) */
    buf[0] = BP_TABLE_MAGIC_0;
    buf[1] = BP_TABLE_MAGIC_1;
    buf[2] = BP_TABLE_MAGIC_2;
    buf[3] = BP_TABLE_MAGIC_3;
    bp_write_le32(buf + 4, ar->entries_written);
    rc = write_crc(ar->io, &table_crc, buf, 8);
    if (rc != BP_OK) return rc;

    /* table records (v3: carry method + header_size + checksum) */
    for (i = 0; i < ar->entries_written; i++) {
        rc = write_table_record(ar->io, &table_crc, &ar->entry_list[i]);
        if (rc != BP_OK) return rc;
    }

    table_end = io_tell(ar->io);
    if (table_end < 0) return BP_ERR_IO;

    /* -- write footer ----------------------------------------------------- */
    {
        bp_uint8 footer[BP_FOOTER_SIZE];
        bp_uint64 tbl_off = bp_u64_from_u32((bp_uint32)table_start);
        bp_uint64 tbl_sz  = bp_u64_from_u32((bp_uint32)(table_end - table_start));

        bp_write_le64(footer + 0, tbl_off);
        bp_write_le64(footer + 8, tbl_sz);
        footer[16] = BP_FOOTER_MAGIC_0;
        footer[17] = BP_FOOTER_MAGIC_1;
        footer[18] = BP_FOOTER_MAGIC_2;
        footer[19] = BP_FOOTER_MAGIC_3;

        rc = write_buf(ar->io, footer, BP_FOOTER_SIZE);
        if (rc != BP_OK) return rc;
    }

    archive_end = io_tell(ar->io);
    if (archive_end < 0) return BP_ERR_IO;

    /* -- patch header: file_count + creator info + archive_size ---------- */
    rc = io_seek(ar->io, 0, 0); /* SEEK_SET */
    if (rc != 0) return BP_ERR_IO;
    {
        bp_uint8 hdr[BP_HEADER_SIZE];
        /* re-read to preserve other fields */
        rc = io_read(ar->io, hdr, BP_HEADER_SIZE);
        if (rc != BP_OK) return rc;

        bp_write_le16(hdr + 4, BP_FORMAT_VERSION); /* upgrade to v3 */
        bp_write_le32(hdr + 8, ar->entries_written);
        hdr[12] = ar->creator_os;
        hdr[13] = ar->creator_cpu;
        bp_write_le64(hdr + 16, bp_u64_from_u32((bp_uint32)archive_end));
        bp_write_le32(hdr + BP_HEADER_TABLE_CRC_OFF, table_crc);
        bp_write_le32(hdr + BP_HEADER_CRC_OFF,
                      bp_crc32(0, hdr, BP_HEADER_CRC_OFF));

        rc = io_seek(ar->io, 0, 0);
        if (rc != 0) return BP_ERR_IO;
        rc = write_buf(ar->io, hdr, BP_HEADER_SIZE);
        if (rc != BP_OK) return rc;
    }

    return BP_OK;
}

void bp_archive_close(bp_archive_t *ar)
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

/* ---- reading: shared helpers ------------------------------------------- */

/* Read footer to find file table offset */
static int read_footer(bp_io_t *io, bp_uint64 *table_offset, bp_uint64 *table_size)
{
    bp_uint8 buf[BP_FOOTER_SIZE];
    int rc;

    /* seek to end - 20 */
    rc = io_seek(io, -BP_FOOTER_SIZE, 2); /* SEEK_END */
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, buf, BP_FOOTER_SIZE);
    if (rc != BP_OK) return rc;

    /* check footer magic */
    if (buf[16] != BP_FOOTER_MAGIC_0 || buf[17] != BP_FOOTER_MAGIC_1 ||
        buf[18] != BP_FOOTER_MAGIC_2 || buf[19] != BP_FOOTER_MAGIC_3) {
        return BP_ERR_MAGIC;
    }

    *table_offset = bp_read_le64(buf);
    *table_size = bp_read_le64(buf + 8);
    return BP_OK;
}

/* Read archive header and validate.  Accepts versions 1..3.  out_version
 * (may be NULL) receives the on-disk version so callers can parse the
 * version-specific file-table record layout.  For v3 the header CRC-32 is
 * verified. */
static int read_header(bp_io_t *io, bp_uint32 *file_count,
                       bp_uint16 *out_version)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    bp_uint16 version;
    int rc;

    rc = io_seek(io, 0, 0); /* SEEK_SET */
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    if (hdr[0] != BP_MAGIC_0 || hdr[1] != BP_MAGIC_1 ||
        hdr[2] != BP_MAGIC_2 || hdr[3] != BP_MAGIC_3) {
        return BP_ERR_MAGIC;
    }

    version = bp_read_le16(hdr + 4);
    if (version == 0 || version > BP_FORMAT_VERSION) {
        return BP_ERR_VERSION;
    }

    if (version == BP_FORMAT_VERSION) {
        bp_uint32 stored = bp_read_le32(hdr + BP_HEADER_CRC_OFF);
        bp_uint32 calc   = bp_crc32(0, hdr, BP_HEADER_CRC_OFF);
        if (stored != calc) return BP_ERR_CORRUPT;
    }

    *file_count = bp_read_le32(hdr + 8);
    if (out_version != NULL) *out_version = version;
    return BP_OK;
}

/* Read a single entry from the file table.
 * version selects the record tail (v2: 34 bytes; v3: 41 with method +
 * header_size + checksum).  Caller must free entry->name. */
static int read_table_entry(bp_io_t *io, bp_entry_t *entry, bp_uint16 version)
{
    bp_uint8 buf[BP_TABLE_REC_TAIL_V3];
    bp_uint32 tail;
    int rc;

    memset(entry, 0, sizeof(*entry));
    tail = (version >= BP_FORMAT_VERSION) ? BP_TABLE_REC_TAIL_V3
                                          : BP_TABLE_REC_TAIL_V2;

    /* name_len (2 bytes) */
    rc = io_read(io, buf, 2);
    if (rc != BP_OK) return rc;
    entry->name_len = bp_read_le16(buf);

    /* name */
    entry->name = (char *)malloc((size_t)(entry->name_len + 1));
    if (entry->name == NULL) return BP_ERR_NOMEM;
    rc = io_read(io, entry->name, entry->name_len);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }
    entry->name[entry->name_len] = '\0';

    rc = io_read(io, buf, tail);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }

    entry->original_size  = bp_read_le64(buf + 0);
    entry->compressed_size = bp_read_le64(buf + 8);
    entry->entry_type     = buf[16];
    entry->platform       = buf[17];
    entry->mtime          = bp_read_le64(buf + 18);
    entry->offset         = bp_read_le64(buf + 26);
    if (tail >= BP_TABLE_REC_TAIL_V3) {
        entry->method      = buf[34];
        entry->header_size = bp_read_le16(buf + 35);
        entry->checksum    = bp_read_le32(buf + 37);
    }

    return BP_OK;
}

/* For v3 archives, verify the header's file-table CRC-32 against the actual
 * table bytes.  A no-op (BP_OK) for v1/v2.  io position left undefined. */
static int verify_table_crc(bp_io_t *io, bp_uint16 version)
{
    bp_uint8  hdr[BP_HEADER_SIZE];
    bp_uint64 tbl_off, tbl_sz;
    bp_uint32 stored, calc, rem;
    bp_uint8  cbuf[256];
    int rc;

    if (version < BP_FORMAT_VERSION) return BP_OK;

    rc = io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;
    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;
    stored = bp_read_le32(hdr + BP_HEADER_TABLE_CRC_OFF);

    rc = read_footer(io, &tbl_off, &tbl_sz);
    if (rc != BP_OK) return rc;

    rc = io_seek(io, (long)bp_u64_to_u32(tbl_off), 0);
    if (rc != 0) return BP_ERR_IO;

    /* Table size fits in 32 bits on this platform (32-bit file offsets). */
    calc = 0;
    rem = bp_u64_to_u32(tbl_sz);
    while (rem > 0) {
        bp_uint32 chunk = (rem > (bp_uint32)sizeof(cbuf))
                          ? (bp_uint32)sizeof(cbuf) : rem;
        rc = io_read(io, cbuf, chunk);
        if (rc != BP_OK) return rc;
        calc = bp_crc32(calc, cbuf, chunk);
        rem -= chunk;
    }
    if (calc != stored) return BP_ERR_CORRUPT;
    return BP_OK;
}

/* Read entry header from archive (at entry offset).
 * Fills entry fields from the on-disk header. */
static int read_entry_header(bp_io_t *io, bp_entry_t *entry)
{
    bp_uint8 buf[38];
    bp_uint8 pre[4];
    int rc;

    memset(entry, 0, sizeof(*entry));

    /* header_size(2) + name_len(2) */
    rc = io_read(io, pre, 4);
    if (rc != BP_OK) return rc;

    entry->header_size = bp_read_le16(pre);
    entry->name_len    = bp_read_le16(pre + 2);

    entry->name = (char *)malloc((size_t)(entry->name_len + 1));
    if (entry->name == NULL) return BP_ERR_NOMEM;
    rc = io_read(io, entry->name, entry->name_len);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }
    entry->name[entry->name_len] = '\0';

    /* remaining 38 bytes of fixed fields */
    rc = io_read(io, buf, 38);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }

    entry->original_size   = bp_read_le64(buf + 0);
    entry->compressed_size = bp_read_le64(buf + 8);
    entry->checksum        = bp_read_le32(buf + 16);
    entry->method          = buf[20];
    entry->checksum_type   = buf[21];
    entry->block_size      = bp_read_le32(buf + 22);
    entry->entry_type      = buf[26];
    entry->platform        = buf[27];
    entry->mtime           = bp_read_le64(buf + 28);
    entry->flags           = bp_read_le16(buf + 36);

    /* skip any unknown trailing header bytes (forward compatibility) */
    {
        bp_uint16 expected = (bp_uint16)(BP_ENTRY_FIXED + entry->name_len);
        if (entry->header_size > expected) {
            bp_uint16 skip = entry->header_size - expected;
            rc = io_seek(io, (long)skip, 1); /* SEEK_CUR */
            if (rc != 0) {
                free(entry->name); entry->name = NULL;
                return BP_ERR_IO;
            }
        }
    }

    return BP_OK;
}

/* ---- listing ----------------------------------------------------------- */

int bp_archive_list(bp_io_t *io, bp_list_fn fn, void *user_data)
{
    bp_uint64 table_offset, table_size;
    bp_uint32 file_count, table_count;
    bp_uint8 tbl_hdr[8];
    bp_uint32 i;
    bp_uint16 version;
    int rc;

    if (io == NULL || fn == NULL) return BP_ERR_INVALID_ARG;

    rc = read_header(io, &file_count, &version);
    if (rc != BP_OK) return rc;

    rc = read_footer(io, &table_offset, &table_size);
    if (rc != BP_OK) return rc;

    /* seek to file table */
    rc = io_seek(io, (long)bp_u64_to_u32(table_offset), 0);
    if (rc != 0) return BP_ERR_IO;

    /* read table header */
    rc = io_read(io, tbl_hdr, 8);
    if (rc != BP_OK) return rc;

    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3) {
        return BP_ERR_CORRUPT;
    }

    table_count = bp_read_le32(tbl_hdr + 4);
    if (table_count != file_count) {
        return BP_ERR_CORRUPT;
    }

    for (i = 0; i < table_count; i++) {
        bp_entry_t entry;
        rc = read_table_entry(io, &entry, version);
        if (rc != BP_OK) return rc;

        rc = fn(&entry, user_data);
        free(entry.name);
        if (rc != 0) return BP_ERR_ABORTED;
    }

    return BP_OK;
}

/* ---- extraction -------------------------------------------------------- */

/* Pre-scan the file table to sum all original_size values. */
static bp_uint64 scan_archive_orig_total(bp_io_t *io, bp_uint16 version)
{
    bp_uint64 total, table_offset, table_size;
    bp_uint8 tbl_hdr[8];
    bp_uint32 count, i;

    total.lo = 0; total.hi = 0;
    if (read_footer(io, &table_offset, &table_size) != BP_OK) return total;
    if (io_seek(io, (long)bp_u64_to_u32(table_offset), 0) != 0) return total;
    if (io_read(io, tbl_hdr, 8) != BP_OK) return total;
    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3)
        return total;
    count = bp_read_le32(tbl_hdr + 4);
    for (i = 0; i < count; i++) {
        bp_entry_t e;
        if (read_table_entry(io, &e, version) != BP_OK) break;
        total = bp_u64_add(total, e.original_size);
        free(e.name);
    }
    return total;
}

/* Decompress/copy all data blocks for one entry */
static int extract_entry_data(bp_io_t *io, bp_entry_t *entry,
                              bp_io_t *out,
                              bp_progress_fn progress_fn,
                              void *user_data,
                              bp_uint32 file_index,
                              bp_uint32 file_count,
                              bp_uint64 archive_orig_total,
                              bp_uint64 archive_orig_done,
                              bp_uint32 *out_checksum)
{
    bp_uint8 ck_type = (entry->checksum_type == BP_CKSUM_CRC32)
                       ? BP_CKSUM_CRC32 : BP_CKSUM_ADLER32;
    bp_uint32 adler = bp_checksum_seed(ck_type);
    bp_uint64 remaining = entry->original_size;
    bp_uint8 *in_buf = NULL;
    bp_uint8 *dec_buf = NULL;
    int rc;

    if (bp_u64_is_zero(remaining)) {
        *out_checksum = bp_checksum_seed(ck_type);
        return BP_OK;
    }

    /* Symlink payloads are raw in fixed archives, but older archives wrote
     * them through the normal block path. */
    if (entry->entry_type == BP_TYPE_SYMLINK &&
        bp_u64_cmp(entry->compressed_size, entry->original_size) == 0) {
        bp_uint32 sz = bp_u64_to_u32(entry->original_size);
        in_buf = (bp_uint8 *)malloc((size_t)sz);
        if (in_buf == NULL) return BP_ERR_NOMEM;
        rc = io_read(io, in_buf, sz);
        if (rc != BP_OK) { free(in_buf); return rc; }
        adler = bp_checksum_typed(ck_type, adler, in_buf, sz);
        if (out) {
            rc = io_write(out, in_buf, sz);
            if (rc != BP_OK) { free(in_buf); return rc; }
        }
        free(in_buf);
        *out_checksum = adler;
        return BP_OK;
    }

    /* block-based decompression */
    {
        bp_uint32 bsize = entry->block_size;
        bp_uint32 alloc_in;

        if (bsize == 0) {
            /* single blob mode */
            bsize = bp_u64_to_u32(entry->original_size);
        }

        /* worst-case compressed block can be slightly larger than original */
        alloc_in = bsize + bsize / 16 + 64 + 3;
        in_buf = (bp_uint8 *)malloc((size_t)alloc_in);
        if (in_buf == NULL) return BP_ERR_NOMEM;

        dec_buf = (bp_uint8 *)malloc((size_t)bsize);
        if (dec_buf == NULL) { free(in_buf); return BP_ERR_NOMEM; }

        while (!bp_u64_is_zero(remaining)) {
            bp_uint8 blk_hdr[8];
            bp_uint32 comp_sz, orig_sz;
            bp_uint32 dec_len;

            rc = io_read(io, blk_hdr, 8);
            if (rc != BP_OK) { free(in_buf); free(dec_buf); return rc; }

            comp_sz = bp_read_le32(blk_hdr);
            orig_sz = bp_read_le32(blk_hdr + 4);

            if (orig_sz > bsize || comp_sz > alloc_in) {
                free(in_buf); free(dec_buf);
                return BP_ERR_CORRUPT;
            }

            rc = io_read(io, in_buf, comp_sz);
            if (rc != BP_OK) { free(in_buf); free(dec_buf); return rc; }

            if (comp_sz == orig_sz) {
                /* stored block */
                memcpy(dec_buf, in_buf, (size_t)orig_sz);
            } else {
                /* compressed block */
                dec_len = orig_sz;
                rc = bp_decompress_block(in_buf, comp_sz, dec_buf, &dec_len);
                if (rc != BP_OK) { free(in_buf); free(dec_buf); return rc; }
                if (dec_len != orig_sz) {
                    free(in_buf); free(dec_buf);
                    return BP_ERR_CORRUPT;
                }
            }

            adler = bp_checksum_typed(ck_type, adler, dec_buf, orig_sz);

            if (out) {
                rc = io_write(out, dec_buf, orig_sz);
                if (rc != BP_OK) { free(in_buf); free(dec_buf); return rc; }
            }

            {
                bp_uint64 block64 = bp_u64_from_u32(orig_sz);
                /* subtract: remaining = remaining - block64 */
                /* simple: since orig_sz fits in 32 bits and remaining >= orig_sz */
                remaining.lo -= orig_sz;
                if (remaining.lo > 0xFFFFFFFFUL - orig_sz) {
                    remaining.hi--;
                }
                (void)block64;
            }

            if (progress_fn) {
                bp_progress_t p;
                bp_uint64 done;
                memset(&p, 0, sizeof(p));
                done = bp_u64_from_u32(
                    bp_u64_to_u32(entry->original_size) -
                    bp_u64_to_u32(remaining));
                p.file_index        = file_index;
                p.file_count        = file_count;
                p.file_name         = entry->name;
                p.file_orig_size    = entry->original_size;
                p.file_bytes_done   = done;
                p.archive_orig_total = archive_orig_total;
                p.archive_orig_done  = bp_u64_add(archive_orig_done, done);
                if (progress_fn(&p, user_data) != 0) {
                    free(in_buf); free(dec_buf);
                    return BP_ERR_ABORTED;
                }
            }
        }

        free(in_buf);
        free(dec_buf);
    }

    *out_checksum = adler;
    return BP_OK;
}

int bp_archive_extract_all(bp_io_t *io,
                           bp_extract_fn open_fn,
                           bp_extract_done_fn done_fn,
                           bp_progress_fn progress_fn,
                           void *user_data)
{
    bp_uint32 file_count;
    bp_uint32 i;
    bp_uint16 version;
    int rc;
    bp_uint64 archive_orig_total;
    bp_uint64 archive_orig_done;

    if (io == NULL || open_fn == NULL) return BP_ERR_INVALID_ARG;

    rc = read_header(io, &file_count, &version);
    if (rc != BP_OK) return rc;

    /* pre-scan file table for overall progress total */
    archive_orig_total = scan_archive_orig_total(io, version);
    archive_orig_done.lo = 0;
    archive_orig_done.hi = 0;

    /* position after header */
    rc = io_seek(io, BP_HEADER_SIZE, 0);
    if (rc != 0) return BP_ERR_IO;

    for (i = 0; i < file_count; i++) {
        bp_entry_t entry;
        bp_io_t *out;
        bp_uint32 checksum;

        rc = read_entry_header(io, &entry);
        if (rc != BP_OK) return rc;

        out = open_fn(&entry, user_data);

        rc = extract_entry_data(io, &entry, out, progress_fn, user_data,
                                i, file_count,
                                archive_orig_total, archive_orig_done,
                                &checksum);
        if (rc != BP_OK) {
            free(entry.name);
            return rc;
        }

        /* verify checksum */
        if (checksum != entry.checksum) {
            if (done_fn && out) done_fn(&entry, out, user_data);
            free(entry.name);
            return BP_ERR_CHECKSUM;
        }

        if (done_fn && out) {
            done_fn(&entry, out, user_data);
        }

        archive_orig_done = bp_u64_add(archive_orig_done, entry.original_size);
        free(entry.name);
    }

    return BP_OK;
}

/* ---- test -------------------------------------------------------------- */

int bp_archive_test(bp_io_t *io, bp_test_fn fn, void *user_data)
{
    bp_uint32 file_count;
    bp_uint32 i;
    bp_uint16 version;
    int rc;

    if (io == NULL) return BP_ERR_INVALID_ARG;

    rc = read_header(io, &file_count, &version);
    if (rc != BP_OK) return rc;

    /* v3: verify the header+file-table CRC-32 before trusting the archive */
    rc = verify_table_crc(io, version);
    if (rc != BP_OK) return rc;

    rc = io_seek(io, BP_HEADER_SIZE, 0);
    if (rc != 0) return BP_ERR_IO;

    for (i = 0; i < file_count; i++) {
        bp_entry_t entry;
        bp_uint32 checksum;
        int entry_rc;

        rc = read_entry_header(io, &entry);
        if (rc != BP_OK) return rc;

        {
            bp_uint64 z;
            z.lo = 0; z.hi = 0;
            rc = extract_entry_data(io, &entry, NULL, NULL, NULL,
                                    i, 0, z, z, &checksum);
        }
        if (rc != BP_OK) {
            entry_rc = rc;
        } else if (checksum != entry.checksum) {
            entry_rc = BP_ERR_CHECKSUM;
        } else {
            entry_rc = BP_OK;
        }

        if (fn) {
            fn(&entry, entry_rc, user_data);
        }

        free(entry.name);

        if (entry_rc != BP_OK && entry_rc != BP_ERR_CHECKSUM) {
            return entry_rc;
        }
    }

    /* verify footer */
    {
        bp_uint64 table_offset, table_size;
        rc = read_footer(io, &table_offset, &table_size);
        if (rc != BP_OK) return rc;
    }

    return BP_OK;
}

/* ---- info -------------------------------------------------------------- */

int bp_archive_info(bp_io_t *io, bp_archive_info_t *info)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    bp_uint16 version;
    bp_uint64 table_offset, table_size;
    bp_uint32 file_count, table_count, i;
    bp_uint8 tbl_hdr[8];
    int rc;

    if (io == NULL || info == NULL) return BP_ERR_INVALID_ARG;

    memset(info, 0, sizeof(*info));

    /* read archive header */
    rc = io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    if (hdr[0] != BP_MAGIC_0 || hdr[1] != BP_MAGIC_1 ||
        hdr[2] != BP_MAGIC_2 || hdr[3] != BP_MAGIC_3) {
        return BP_ERR_MAGIC;
    }

    version = bp_read_le16(hdr + 4);
    if (version == 0 || version > BP_FORMAT_VERSION) {
        return BP_ERR_VERSION;
    }

    info->version          = version;
    info->flags            = bp_read_le16(hdr + 6);
    info->file_count       = bp_read_le32(hdr + 8);
    info->creator_os       = hdr[12];
    info->creator_cpu      = hdr[13];
    info->creator_ver_major = hdr[14];
    info->creator_ver_minor = hdr[15];
    info->archive_size     = bp_read_le64(hdr + 16);

    file_count = info->file_count;

    /* read footer to find file table */
    rc = read_footer(io, &table_offset, &table_size);
    if (rc != BP_OK) return rc;

    rc = io_seek(io, (long)bp_u64_to_u32(table_offset), 0);
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, tbl_hdr, 8);
    if (rc != BP_OK) return rc;

    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3) {
        return BP_ERR_CORRUPT;
    }

    table_count = bp_read_le32(tbl_hdr + 4);
    if (table_count != file_count) {
        return BP_ERR_CORRUPT;
    }

    /* scan file table to accumulate totals */
    {
        bp_uint64 tot_orig = {0, 0};
        bp_uint64 tot_comp = {0, 0};

        for (i = 0; i < table_count; i++) {
            bp_entry_t entry;
            rc = read_table_entry(io, &entry, version);
            if (rc != BP_OK) return rc;

            if (entry.entry_type == BP_TYPE_DIR) {
                info->dir_count++;
            }
            info->entry_count++;
            tot_orig = bp_u64_add(tot_orig, entry.original_size);
            tot_comp = bp_u64_add(tot_comp, entry.compressed_size);

            free(entry.name);
        }

        info->total_original   = tot_orig;
        info->total_compressed = tot_comp;
    }

    return BP_OK;
}
