/*
 * test_archive_format.c -- unit tests for archive create / list / test / info
 *
 * Uses a memory-backed bp_io_t to exercise the full archiver API without
 * touching the filesystem.  Tests Stored, BoltLite, boltlz, and Fluff methods.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_runner.h"
#include "bp_types.h"
#include "compress.h"
#include "archiver.h"
#include "mini/archiver.h"
#include "mini/compress.h"
#include "bp_metadata.h"
#include "archive_name.h"
#include "../../src/frontend/mini_shared/dos_time.h"
#include "../../src/frontend/mini_shared/mini_common.h"
#include "../../src/frontend/atari/atari_path.h"
#include "../../src/frontend/cpm86/cpm86_path.h"

/* ======================================================================== */
/* Memory-backed bp_io_t                                                     */
/* ======================================================================== */

#define MEM_IO_CAP (1 * 1024 * 1024)   /* 1 MB */

typedef struct {
    unsigned char *buf;
    long           pos;
    long           size;
    long           cap;
} MemIO;

static long mem_read(void *handle, void *buf, bp_uint32 count)
{
    MemIO *m = (MemIO *)handle;
    long avail = m->size - m->pos;
    long n = ((long)count < avail) ? (long)count : avail;
    if (n <= 0) return 0;
    memcpy(buf, m->buf + m->pos, (size_t)n);
    m->pos += n;
    return n;
}

static long mem_write(void *handle, const void *buf, bp_uint32 count)
{
    MemIO *m = (MemIO *)handle;
    if (m->pos + (long)count > m->cap) return -1;
    memcpy(m->buf + m->pos, buf, (size_t)count);
    m->pos += (long)count;
    if (m->pos > m->size) m->size = m->pos;
    return (long)count;
}

static int mem_seek(void *handle, bp_off_t offset, int origin)
{
    MemIO *m = (MemIO *)handle;
    long newpos;
    if (origin == 0)      newpos = (long)offset;
    else if (origin == 1) newpos = m->pos + (long)offset;
    else                  newpos = m->size + (long)offset;
    if (newpos < 0 || newpos > m->cap) return -1;
    m->pos = newpos;
    return 0;
}

static bp_off_t mem_tell(void *handle)
{
    return (bp_off_t)((MemIO *)handle)->pos;
}

static bp_io_t make_mem_io(MemIO *m)
{
    bp_io_t io;
    io.read   = mem_read;
    io.write  = mem_write;
    io.seek   = mem_seek;
    io.tell   = mem_tell;
    io.handle = m;
    return io;
}

static void mem_rewind(MemIO *m)
{
    m->pos = 0;
}

typedef struct {
    bp_off_t pos;
} SparseIO;

static long sparse_read(void *handle, void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return 0;
}

static long sparse_write(void *handle, const void *buf, bp_uint32 count)
{
    SparseIO *s = (SparseIO *)handle;
    (void)buf;
    s->pos += (bp_off_t)count;
    return (long)count;
}

static int sparse_seek(void *handle, bp_off_t offset, int origin)
{
    SparseIO *s = (SparseIO *)handle;
    if (origin == 0) s->pos = offset;
    else if (origin == 1) s->pos += offset;
    else return -1;
    return s->pos < 0 ? -1 : 0;
}

static bp_off_t sparse_tell(void *handle)
{
    return ((SparseIO *)handle)->pos;
}

static bp_io_t make_sparse_io(SparseIO *s)
{
    bp_io_t io;
    io.read = sparse_read;
    io.write = sparse_write;
    io.seek = sparse_seek;
    io.tell = sparse_tell;
    io.handle = s;
    return io;
}

/* ======================================================================== */
/* String-backed "file" source (for adding entries from memory)              */
/* ======================================================================== */

typedef struct {
    const unsigned char *data;
    bp_uint32            len;
    bp_uint32            pos;
} StringSrc;

static long strsrc_read(void *handle, void *buf, bp_uint32 count)
{
    StringSrc *s = (StringSrc *)handle;
    bp_uint32 avail = s->len - s->pos;
    bp_uint32 n = (count < avail) ? count : avail;
    if (n == 0) return 0;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return (long)n;
}

static long strsrc_write(void *handle, const void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1; /* read-only */
}

static int strsrc_seek(void *handle, bp_off_t offset, int origin)
{
    StringSrc *s = (StringSrc *)handle;
    long newpos;
    if (origin == 0)      newpos = (long)offset;
    else if (origin == 1) newpos = (long)s->pos + (long)offset;
    else                  newpos = (long)s->len  + (long)offset;
    if (newpos < 0 || (bp_uint32)newpos > s->len) return -1;
    s->pos = (bp_uint32)newpos;
    return 0;
}

static bp_off_t strsrc_tell(void *handle)
{
    return (bp_off_t)((StringSrc *)handle)->pos;
}

static bp_io_t make_string_io(StringSrc *s,
                               const unsigned char *data, bp_uint32 len)
{
    bp_io_t io;
    s->data = data;
    s->len  = len;
    s->pos  = 0;
    io.read   = strsrc_read;
    io.write  = strsrc_write;
    io.seek   = strsrc_seek;
    io.tell   = strsrc_tell;
    io.handle = s;
    return io;
}

/* ======================================================================== */
/* List callback state                                                       */
/* ======================================================================== */

#define MAX_LIST_ENTRIES 16

typedef struct {
    bp_entry_t entries[MAX_LIST_ENTRIES];
    int        count;
    char       names[MAX_LIST_ENTRIES][256];
} ListState;

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    ListState *ls = (ListState *)user_data;
    if (ls->count >= MAX_LIST_ENTRIES) return BP_ERR_OVERFLOW;
    ls->entries[ls->count] = *entry;
    /* copy name into stable storage since entry->name is freed after callback */
    if (entry->name_len < 255) {
        memcpy(ls->names[ls->count], entry->name,
               entry->name_len + 1u);
    } else {
        memcpy(ls->names[ls->count], entry->name, 255);
        ls->names[ls->count][255] = '\0';
    }
    ls->entries[ls->count].name = ls->names[ls->count];
    ls->count++;
    return BP_OK;
}

/* ======================================================================== */
/* Test callback state                                                       */
/* ======================================================================== */

typedef struct {
    int all_ok;
    int entry_count;
} TestState;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    TestState *ts = (TestState *)user_data;
    ts->entry_count++;
    if (result != BP_OK) {
        ts->all_ok = 0;
        printf("    [test_cb] entry '%s' failed: %d\n",
               entry->name ? entry->name : "(null)", result);
    }
}

/* ======================================================================== */
/* Create a simple archive with known content                                */
/* Returns 0 on success, non-zero on failure.                                */
/* ======================================================================== */

static const unsigned char FILE1_DATA[] = "Hello, BoltPack!";
static const unsigned char FILE2_DATA[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

#define FILE1_LEN ((bp_uint32)(sizeof(FILE1_DATA) - 1))
#define FILE2_LEN ((bp_uint32)(sizeof(FILE2_DATA) - 1))

static int create_test_archive(MemIO *mem, bp_uint8 method)
{
    bp_io_t    io;
    bp_io_t    f1_io;
    bp_io_t    f2_io;
    bp_archive_t ar;
    StringSrc  s1;
    StringSrc  s2;
    int rc;

    memset(mem->buf, 0, (size_t)mem->cap);
    mem->pos  = 0;
    mem->size = 0;

    io = make_mem_io(mem);

    rc = bp_archive_create(&ar, &io, BP_BLOCK_8K, method);
    if (rc != BP_OK) return rc;

    f1_io = make_string_io(&s1, FILE1_DATA, FILE1_LEN);
    rc = bp_archive_add_entry(&ar, "file1.txt",
                               BP_TYPE_DATA, BP_OS_LINUX,
                               &f1_io, bp_u64_from_u32(FILE1_LEN),
                               bp_u64_from_u32(1000000));
    if (rc != BP_OK) { bp_archive_close(&ar); return rc; }

    f2_io = make_string_io(&s2, FILE2_DATA, FILE2_LEN);
    rc = bp_archive_add_entry(&ar, "subdir/all_a.bin",
                               BP_TYPE_DATA, BP_OS_LINUX,
                               &f2_io, bp_u64_from_u32(FILE2_LEN),
                               bp_u64_from_u32(2000000));
    if (rc != BP_OK) { bp_archive_close(&ar); return rc; }

    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    return rc;
}

/* ======================================================================== */
/* Test: archive header magic bytes (direct buffer inspection)               */
/* ======================================================================== */

static void test_header_bytes(void)
{
    MemIO mem;
    int rc;

    UNIT_SECTION("Archive format: header magic bytes");

    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (!mem.buf) {
        printf("  FAIL  malloc failed  [%s:%d]\n", __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    mem.cap = MEM_IO_CAP;

    rc = create_test_archive(&mem, BP_METHOD_STORED);
    UNIT_CHECK(rc == BP_OK, "create archive (Stored) ok");

    if (rc == BP_OK) {
        UNIT_CHECK(mem.buf[0] == 0x42, "magic[0] == 'B'");
        UNIT_CHECK(mem.buf[1] == 0x4F, "magic[1] == 'O'");
        UNIT_CHECK(mem.buf[2] == 0x4C, "magic[2] == 'L'");
        UNIT_CHECK(mem.buf[3] == 0x54, "magic[3] == 'T'");

        /* format version (little-endian uint16) */
        UNIT_CHECK(mem.buf[4] == BP_FORMAT_VERSION && mem.buf[5] == 0x00,
                   "format version == 6 (little-endian)");

        /* file_count (little-endian uint32 at offset 8) must be 2 */
        UNIT_CHECK(mem.buf[8] == 2 && mem.buf[9] == 0 &&
                   mem.buf[10] == 0 && mem.buf[11] == 0,
                   "file_count == 2");
    }

    free(mem.buf);
}

/* This 170-byte archive is laid out from docs/specification-1.0.md sections
 * 2.2, 2.3, 4, and 5, not emitted by bp_archive_create().  It contains one
 * Stored entry named "x" with payload "A".  The four CRC-32 values are the
 * standard IEEE CRC-32 values for the payload, block, table, and header. */
static const unsigned char FORMAT6_GOLDEN_ARCHIVE[] = {
    0x42, 0x4F, 0x4C, 0x54, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x27, 0x66, 0x0B, 0x30, 0x24, 0xCC, 0xE3, 0x16,
    0x2B, 0x00, 0x01, 0x00, 0x78, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x9E, 0xD9,
    0xD3, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x8B, 0x9E, 0xD9, 0xD3, 0x41,
    0x54, 0x42, 0x4C, 0x54, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x78, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2B, 0x00, 0x8B, 0x9E, 0xD9, 0xD3,
    0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x4C, 0x4F, 0x42
};

/* Format 6 §2.3.2's block_size == 0 case: a single unframed Stored blob.
 * This fixture is independently laid out and guards against readers that
 * accidentally require the 12-byte blocked-data header. */
static const unsigned char FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE[] = {
    0x42, 0x4F, 0x4C, 0x54, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x9E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x94, 0xD4, 0x72, 0x31, 0xBE, 0x42, 0xF9, 0x2B, 0x00, 0x01, 0x00,
    0x78, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x9E, 0xD9, 0xD3, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x41, 0x54, 0x42, 0x4C, 0x54, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x78, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2B, 0x00,
    0x8B, 0x9E, 0xD9, 0xD3, 0x00, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x4C,
    0x4F, 0x42
};

/* Single-blob BoltLite case with the independently defined two-byte stream
 * 01 41: one literal flag followed by 'A'. */
static const unsigned char FORMAT6_SINGLE_BOLTLITE_GOLDEN_ARCHIVE[] = {
    0x42, 0x4F, 0x4C, 0x54, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x09, 0x9F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xDA, 0xBE, 0x7C, 0xEE, 0x20, 0x37, 0xEA, 0x6C, 0x2B, 0x00, 0x01, 0x00,
    0x78, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x9E, 0xD9, 0xD3, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x41, 0x54, 0x42, 0x4C, 0x54, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x78,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2B,
    0x00, 0x8B, 0x9E, 0xD9, 0xD3, 0x00, 0x00, 0x4D, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54,
    0x4C, 0x4F, 0x42
};

static void test_independent_format6_golden_archive(void)
{
    MemIO mem;
    bp_io_t io;
    ListState listed;
    TestState tested;
    int rc;

    UNIT_SECTION("Archive format 6: independent golden archive");
    mem.buf = (unsigned char *)FORMAT6_GOLDEN_ARCHIVE;
    mem.pos = 0;
    mem.size = (long)sizeof(FORMAT6_GOLDEN_ARCHIVE);
    mem.cap = mem.size;
    io = make_mem_io(&mem);

    memset(&listed, 0, sizeof(listed));
    rc = bp_archive_list(&io, list_cb, &listed);
    UNIT_CHECK(rc == BP_OK && listed.count == 1,
               "reader accepts independently specified format-6 table");
    if (rc == BP_OK && listed.count == 1) {
        UNIT_CHECK(strcmp(listed.names[0], "x") == 0 &&
                   listed.entries[0].entry_type == BP_TYPE_DATA &&
                   listed.entries[0].method == BP_METHOD_STORED &&
                   listed.entries[0].original_size.lo == 1 &&
                   listed.entries[0].compressed_size.lo == 13 &&
                   listed.entries[0].checksum == 0xD3D99E8BU &&
                   listed.entries[0].flags == 0,
                   "golden record fields decode with unknown timestamp");
    }

    memset(&tested, 0, sizeof(tested));
    tested.all_ok = 1;
    rc = bp_archive_test(&io, test_cb, &tested);
    UNIT_CHECK(rc == BP_OK && tested.all_ok && tested.entry_count == 1,
               "golden Stored payload passes independent CRC fixtures");
}

typedef struct {
    MemIO output;
    bp_io_t io;
    unsigned char bytes[8];
    int done_status;
} GoldenExtractState;

static bp_io_t *golden_extract_open(const bp_entry_t *entry, void *user_data)
{
    GoldenExtractState *state = (GoldenExtractState *)user_data;
    (void)entry;
    return &state->io;
}

static int golden_extract_done(const bp_entry_t *entry, bp_io_t *out,
                               int status, void *user_data)
{
    GoldenExtractState *state = (GoldenExtractState *)user_data;
    (void)entry;
    (void)out;
    state->done_status = status;
    return BP_OK;
}

static void test_format6_single_blob_golden_archive(void)
{
    unsigned char corrupt[sizeof(FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE)];
    MemIO mem;
    bp_io_t io;
    ListState listed;
    TestState tested;
    GoldenExtractState extracted;
    int rc;

    UNIT_SECTION("Archive format 6: unframed single-blob layout");
    mem.buf = (unsigned char *)FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE;
    mem.pos = 0;
    mem.size = (long)sizeof(FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE);
    mem.cap = mem.size;
    io = make_mem_io(&mem);

    memset(&listed, 0, sizeof(listed));
    rc = bp_archive_list(&io, list_cb, &listed);
    UNIT_CHECK(rc == BP_OK && listed.count == 1 &&
               listed.entries[0].block_size == 0 &&
               listed.entries[0].compressed_size.lo == 1,
               "full reader lists the independently specified unframed entry");

    memset(&tested, 0, sizeof(tested));
    tested.all_ok = 1;
    rc = bp_archive_test(&io, test_cb, &tested);
    UNIT_CHECK(rc == BP_OK && tested.all_ok && tested.entry_count == 1,
               "full test verifies the unframed Stored payload");

    memset(&extracted, 0, sizeof(extracted));
    extracted.output.buf = extracted.bytes;
    extracted.output.cap = sizeof(extracted.bytes);
    extracted.io = make_mem_io(&extracted.output);
    extracted.done_status = 99;
    rc = bp_archive_extract_all_ex(&io, golden_extract_open,
                                   golden_extract_done, NULL, &extracted);
    UNIT_CHECK(rc == BP_OK && extracted.done_status == BP_OK &&
               extracted.output.size == 1 && extracted.bytes[0] == 'A',
               "full extraction decodes the unframed single blob");

    memcpy(corrupt, FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE, sizeof(corrupt));
    corrupt[75] ^= 1;
    mem.buf = corrupt;
    mem.pos = 0;
    mem.size = (long)sizeof(corrupt);
    mem.cap = mem.size;
    UNIT_CHECK(bp_archive_test(&io, NULL, NULL) == BP_ERR_CHECKSUM,
               "full test detects corruption in an unframed Stored blob");
    UNIT_CHECK(bp_mini_archive_test(&io, NULL, NULL) == BP_ERR_CHECKSUM,
               "mini test detects corruption in an unframed Stored blob");

    mem.buf = (unsigned char *)FORMAT6_SINGLE_BOLTLITE_GOLDEN_ARCHIVE;
    mem.pos = 0;
    mem.size = (long)sizeof(FORMAT6_SINGLE_BOLTLITE_GOLDEN_ARCHIVE);
    mem.cap = mem.size;
    UNIT_CHECK(bp_archive_test(&io, NULL, NULL) == BP_OK,
               "full test decompresses the unframed BoltLite blob");
    memset(&extracted, 0, sizeof(extracted));
    extracted.output.buf = extracted.bytes;
    extracted.output.cap = sizeof(extracted.bytes);
    extracted.io = make_mem_io(&extracted.output);
    extracted.done_status = 99;
    rc = bp_archive_extract_all_ex(&io, golden_extract_open,
                                   golden_extract_done, NULL, &extracted);
    UNIT_CHECK(rc == BP_OK && extracted.done_status == BP_OK &&
               extracted.output.size == 1 && extracted.bytes[0] == 'A',
               "full extraction decompresses the unframed BoltLite blob");
}

/* ======================================================================== */
/* Test: archive list for a given method                                     */
/* ======================================================================== */

static void test_list_method(const char *label, bp_uint8 method)
{
    MemIO     mem;
    bp_io_t   io;
    ListState ls;
    int       rc;

    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (!mem.buf) {
        printf("  FAIL  %s: malloc failed  [%s:%d]\n", label, __FILE__,
               __LINE__);
        bp_unit_fail_count++;
        return;
    }
    mem.cap = MEM_IO_CAP;

    rc = create_test_archive(&mem, method);
    if (rc != BP_OK) {
        printf("  FAIL  %s: create returned %d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        free(mem.buf);
        return;
    }

    mem_rewind(&mem);
    io = make_mem_io(&mem);
    memset(&ls, 0, sizeof(ls));

    rc = bp_archive_list(&io, list_cb, &ls);
    if (rc != BP_OK) {
        printf("  FAIL  %s: list returned %d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        free(mem.buf);
        return;
    }

    if (ls.count == 2) {
        printf("  PASS  %s: entry count == 2\n", label);
        bp_unit_pass_count++;
    } else {
        printf("  FAIL  %s: entry count == %d (expected 2)  [%s:%d]\n",
               label, ls.count, __FILE__, __LINE__);
        bp_unit_fail_count++;
    }

    if (ls.count >= 1 && strcmp(ls.names[0], "file1.txt") == 0) {
        printf("  PASS  %s: entry[0].name == file1.txt\n", label);
        bp_unit_pass_count++;
    } else if (ls.count >= 1) {
        printf("  FAIL  %s: entry[0].name == '%s'  [%s:%d]\n",
               label, ls.names[0], __FILE__, __LINE__);
        bp_unit_fail_count++;
    }

    if (ls.count >= 1 &&
        ls.entries[0].original_size.lo == FILE1_LEN &&
        ls.entries[0].original_size.hi == 0) {
        printf("  PASS  %s: entry[0].original_size == %lu\n",
               label, (unsigned long)FILE1_LEN);
        bp_unit_pass_count++;
    } else if (ls.count >= 1) {
        printf("  FAIL  %s: entry[0].original_size.lo == %lu  [%s:%d]\n",
               label, (unsigned long)ls.entries[0].original_size.lo,
               __FILE__, __LINE__);
        bp_unit_fail_count++;
    }

    if (ls.count >= 2 && strcmp(ls.names[1], "subdir/all_a.bin") == 0) {
        printf("  PASS  %s: entry[1].name == subdir/all_a.bin\n", label);
        bp_unit_pass_count++;
    }

    free(mem.buf);
}

/* ======================================================================== */
/* Test: integrity check (bp_archive_test) passes for valid archive          */
/* ======================================================================== */

static void test_integrity_method(const char *label, bp_uint8 method)
{
    MemIO      mem;
    bp_io_t    io;
    TestState  ts;
    int        rc;

    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (!mem.buf) {
        printf("  FAIL  %s: malloc failed  [%s:%d]\n", label, __FILE__,
               __LINE__);
        bp_unit_fail_count++;
        return;
    }
    mem.cap = MEM_IO_CAP;

    rc = create_test_archive(&mem, method);
    if (rc != BP_OK) {
        printf("  FAIL  %s: create returned %d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        free(mem.buf);
        return;
    }

    mem_rewind(&mem);
    io = make_mem_io(&mem);
    ts.all_ok      = 1;
    ts.entry_count = 0;

    rc = bp_archive_test(&io, test_cb, &ts);
    if (rc != BP_OK) {
        printf("  FAIL  %s: bp_archive_test returned %d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
    } else if (!ts.all_ok) {
        printf("  FAIL  %s: one or more entries failed integrity check"
               "  [%s:%d]\n", label, __FILE__, __LINE__);
        bp_unit_fail_count++;
    } else {
        printf("  PASS  %s: all %d entries passed integrity check\n",
               label, ts.entry_count);
        bp_unit_pass_count++;
    }

    free(mem.buf);
}

/* ======================================================================== */
/* Test: wrong magic returns BP_ERR_MAGIC                                    */
/* ======================================================================== */

static void test_bad_magic(void)
{
    static unsigned char junk[BP_HEADER_SIZE + 16];
    MemIO   mem;
    bp_io_t io;
    int     rc;

    UNIT_SECTION("Archive format: bad magic");

    memset(junk, 0xAA, sizeof(junk));
    /* Leave magic as 0xAA 0xAA 0xAA 0xAA instead of BOLT */

    mem.buf  = junk;
    mem.pos  = 0;
    mem.size = (long)sizeof(junk);
    mem.cap  = (long)sizeof(junk);
    io = make_mem_io(&mem);

    rc = bp_archive_list(&io, list_cb, NULL);
    UNIT_CHECK(rc == BP_ERR_MAGIC, "bad magic -> BP_ERR_MAGIC from list");

    mem_rewind(&mem);
    rc = bp_archive_test(&io, test_cb, NULL);
    UNIT_CHECK(rc == BP_ERR_MAGIC, "bad magic -> BP_ERR_MAGIC from test");
}

/* ======================================================================== */
/* Test: archive info                                                        */
/* ======================================================================== */

static void test_info(void)
{
    MemIO            mem;
    bp_io_t          io;
    bp_archive_info_t info;
    int              rc;

    UNIT_SECTION("Archive format: bp_archive_info");

    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (!mem.buf) {
        printf("  FAIL  malloc failed  [%s:%d]\n", __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    mem.cap = MEM_IO_CAP;

    rc = create_test_archive(&mem, BP_METHOD_STORED);
    UNIT_CHECK(rc == BP_OK, "create Stored archive for info test");

    if (rc == BP_OK) {
        mem_rewind(&mem);
        io = make_mem_io(&mem);
        memset(&info, 0, sizeof(info));

        rc = bp_archive_info(&io, &info);
        UNIT_CHECK(rc == BP_OK, "bp_archive_info returns BP_OK");
        UNIT_CHECK(info.version == BP_FORMAT_VERSION, "info.version == BP_FORMAT_VERSION");
        UNIT_CHECK(info.file_count == 2, "info.file_count == 2");
        UNIT_CHECK(info.total_original.lo == FILE1_LEN + FILE2_LEN,
                   "info.total_original correct");
    }

    free(mem.buf);
}

/* ======================================================================== */
/* Test: directory entry round-trip                                          */
/* ======================================================================== */

static void test_dir_entry(void)
{
    MemIO        mem;
    bp_io_t      io;
    bp_archive_t ar;
    ListState    ls;
    int          rc;

    UNIT_SECTION("Archive format: directory entry");

    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (!mem.buf) {
        printf("  FAIL  malloc failed  [%s:%d]\n", __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    mem.cap  = MEM_IO_CAP;
    mem.pos  = 0;
    mem.size = 0;
    memset(mem.buf, 0, (size_t)MEM_IO_CAP);

    io = make_mem_io(&mem);
    rc = bp_archive_create(&ar, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    UNIT_CHECK(rc == BP_OK, "create ok for dir test");

    if (rc == BP_OK) {
        rc = bp_archive_add_entry(&ar, "mydir/",
                                   BP_TYPE_DIR, BP_OS_LINUX,
                                   NULL, bp_u64_from_u32(0),
                                   bp_u64_from_u32(5000));
        UNIT_CHECK(rc == BP_OK, "add dir entry ok");

        rc = bp_archive_finalize(&ar);
        UNIT_CHECK(rc == BP_OK, "finalize ok");
        bp_archive_close(&ar);

        mem_rewind(&mem);
        io = make_mem_io(&mem);
        memset(&ls, 0, sizeof(ls));
        rc = bp_archive_list(&io, list_cb, &ls);
        UNIT_CHECK(rc == BP_OK, "list ok after dir entry");
        UNIT_CHECK(ls.count == 1, "entry count == 1");
        if (ls.count == 1) {
            UNIT_CHECK(ls.entries[0].entry_type == BP_TYPE_DIR,
                       "entry_type == BP_TYPE_DIR");
            UNIT_CHECK(strcmp(ls.names[0], "mydir/") == 0,
                       "dir name == mydir/");
        }
    } else {
        bp_archive_close(&ar);
    }

    free(mem.buf);
}

/* ======================================================================== */
/* Test: empty archive (zero entries)                                        */
/* ======================================================================== */

static void test_empty_archive(void)
{
    MemIO        mem;
    bp_io_t      io;
    bp_archive_t ar;
    ListState    ls;
    int          rc;

    UNIT_SECTION("Archive format: empty archive (zero entries)");

    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (!mem.buf) {
        printf("  FAIL  malloc failed  [%s:%d]\n", __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    mem.cap  = MEM_IO_CAP;
    mem.pos  = 0;
    mem.size = 0;
    memset(mem.buf, 0, (size_t)MEM_IO_CAP);

    io = make_mem_io(&mem);
    rc = bp_archive_create(&ar, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    UNIT_CHECK(rc == BP_OK, "create ok");

    if (rc == BP_OK) {
        rc = bp_archive_finalize(&ar);
        UNIT_CHECK(rc == BP_OK, "finalize ok");
        bp_archive_close(&ar);

        mem_rewind(&mem);
        io = make_mem_io(&mem);
        memset(&ls, 0, sizeof(ls));
        rc = bp_archive_list(&io, list_cb, &ls);
        UNIT_CHECK(rc == BP_OK, "list of empty archive ok");
        UNIT_CHECK(ls.count == 0, "entry count == 0");

        mem_rewind(&mem);
        io = make_mem_io(&mem);
        rc = bp_archive_test(&io, test_cb, NULL);
        UNIT_CHECK(rc == BP_OK, "test of empty archive ok");
    } else {
        bp_archive_close(&ar);
    }

    free(mem.buf);
}

/* ======================================================================== */
/* Test: checksum mismatch detected                                          */
/* ======================================================================== */

static void test_checksum_corruption(void)
{
    MemIO     mem;
    bp_io_t   io;
    TestState ts;
    int       rc;
    long      pos;

    UNIT_SECTION("Archive format: checksum mismatch detection");

    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (!mem.buf) {
        printf("  FAIL  malloc failed  [%s:%d]\n", __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    mem.cap = MEM_IO_CAP;

    rc = create_test_archive(&mem, BP_METHOD_STORED);
    UNIT_CHECK(rc == BP_OK, "create Stored archive ok");

    if (rc == BP_OK) {
        /*
         * Flip a byte in the middle of the data section (past the 32-byte
         * archive header and the entry header).  We corrupt one byte at
         * BP_HEADER_SIZE + 150, which should fall inside the compressed data
         * of one of the two entries.
         */
        pos = BP_HEADER_SIZE + 150;
        if (pos < mem.size) {
            mem.buf[pos] ^= 0xFF;

            mem_rewind(&mem);
            io = make_mem_io(&mem);
            ts.all_ok      = 1;
            ts.entry_count = 0;

            rc = bp_archive_test(&io, test_cb, &ts);
            /*
             * Either the overall function returns an error code, or the
             * callback is called with a non-zero result.
             */
            UNIT_CHECK(rc != BP_OK || ts.all_ok == 0,
                       "corrupted archive detected by test (rc or callback)");
        } else {
            printf("  PASS  checksum corruption: archive too small to corrupt"
                   " (skipped)\n");
            bp_unit_pass_count++;
        }
    }

    free(mem.buf);
}

/* ======================================================================== */
/* main                                                                      */
/* ======================================================================== */

static void test_previous_versions_rejected(void)
{
    MemIO mem;
    bp_io_t io;
    bp_archive_info_t info;
    bp_mini_archive_info_t mini_info;
    bp_uint16 version;
    int rc;
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.cap = MEM_IO_CAP;
    UNIT_SECTION("Archive format: version 6 only");
    for (version = 1; version < BP_FORMAT_VERSION; version++) {
        rc = create_test_archive(&mem, BP_METHOD_BOLTLZ);
        if (rc != BP_OK) { UNIT_CHECK(0, "prepare version-rejection fixture"); break; }
        bp_write_le16(mem.buf + 4, version);
        bp_write_le32(mem.buf + BP_HEADER_CRC_OFF,
                      bp_crc32(0, mem.buf, BP_HEADER_CRC_OFF));
        io = make_mem_io(&mem);
        UNIT_CHECK(bp_archive_list(&io, list_cb, NULL) == BP_ERR_VERSION,
                   "list rejects previous version with valid header CRC");
        UNIT_CHECK(bp_archive_info(&io, &info) == BP_ERR_VERSION,
                   "info rejects previous version with valid header CRC");
        UNIT_CHECK(bp_archive_test(&io, test_cb, NULL) == BP_ERR_VERSION,
                   "test rejects previous version with valid header CRC");
        UNIT_CHECK(bp_mini_archive_info(&io, &mini_info) == BP_ERR_VERSION,
                   "mini info rejects previous version with valid header CRC");
        UNIT_CHECK(bp_mini_archive_test(&io, NULL, NULL) == BP_ERR_VERSION,
                   "mini test rejects previous version with valid header CRC");
    }
    free(mem.buf);
}

static void test_format6_flags_and_names(void)
{
    MemIO mem;
    bp_io_t io;
    bp_archive_t ar;
    ListState ls;
    bp_uint64 zero = bp_u64_from_u32(0);
    bp_uint64 tbl_off, tbl_size;
    bp_uint32 flags_off;
    int rc;

    UNIT_SECTION("Archive format 6: timestamp validity and names");
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.pos = mem.size = 0;
    mem.cap = MEM_IO_CAP;
    io = make_mem_io(&mem);
    rc = bp_archive_create(&ar, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    UNIT_CHECK(rc == BP_OK, "create format 6 archive");
    if (rc != BP_OK) { free(mem.buf); return; }
    rc = bp_archive_add_entry_ex(&ar, "known/", BP_TYPE_DIR, BP_OS_LINUX,
                                 NULL, zero, zero, BP_ENTRY_FLAG_MTIME_VALID);
    UNIT_CHECK(rc == BP_OK, "known Unix epoch zero is accepted");
    rc = bp_archive_add_entry_ex(&ar, "unknown/", BP_TYPE_DIR, BP_OS_LINUX,
                                 NULL, zero, zero, 0);
    UNIT_CHECK(rc == BP_OK, "unknown timestamp is accepted");
    rc = bp_archive_add_entry_ex(&ar, "bad/", BP_TYPE_DIR, BP_OS_LINUX,
                                 NULL, zero, bp_u64_from_u32(1), 0);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "unknown time must be encoded as zero");
    rc = bp_archive_add_entry(&ar, "\xC0\xAF", BP_TYPE_DIR, BP_OS_LINUX,
                              NULL, zero, zero);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "overlong UTF-8 name rejected");
    rc = bp_archive_add_entry(&ar, "\xED\xA0\x80", BP_TYPE_DIR, BP_OS_LINUX,
                              NULL, zero, zero);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "UTF-8 surrogate name rejected");
    rc = bp_archive_add_entry(&ar, "../outside", BP_TYPE_DATA, BP_OS_LINUX,
                              NULL, zero, zero);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "parent traversal name rejected");
    rc = bp_archive_add_entry(&ar, "a/./b", BP_TYPE_DATA, BP_OS_LINUX,
                              NULL, zero, zero);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "dot path component rejected");
    rc = bp_archive_add_entry(&ar, "a//b", BP_TYPE_DATA, BP_OS_LINUX,
                              NULL, zero, zero);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "empty path component rejected");
    rc = bp_archive_add_entry(&ar, "file/", BP_TYPE_DATA, BP_OS_LINUX,
                              NULL, zero, zero);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "file name cannot alias a directory path");
    rc = bp_archive_add_entry(&ar, "dir", BP_TYPE_DIR, BP_OS_LINUX,
                              NULL, zero, zero);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "directory name requires trailing slash");
    UNIT_CHECK(!bp_archive_name_valid("a//b", 4) &&
               !bp_archive_name_valid("a/./b", 5) &&
               bp_archive_name_valid("dir/", 4),
               "counted names reject aliases but retain directory spelling");
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK, "finalize timestamp fixture");
    memset(&ls, 0, sizeof(ls));
    rc = bp_archive_list(&io, list_cb, &ls);
    UNIT_CHECK(rc == BP_OK && ls.count == 2, "list timestamp fixture");
    UNIT_CHECK((ls.entries[0].flags & BP_ENTRY_FLAG_MTIME_VALID) != 0 &&
               (ls.entries[1].flags & BP_ENTRY_FLAG_MTIME_VALID) == 0,
               "table distinguishes epoch zero from unknown time");

    tbl_off = bp_read_le64(mem.buf + mem.size - BP_FOOTER_SIZE);
    tbl_size = bp_read_le64(mem.buf + mem.size - BP_FOOTER_SIZE + 8);
    flags_off = tbl_off.lo + BP_TABLE_HDR_V3 + 2U + 6U + 41U;
    UNIT_CHECK(mem.buf[flags_off] == 0 && mem.buf[flags_off + 1] == 0x40,
               "format 6 table contains the expected little-endian flag bytes");
    bp_write_le16(mem.buf + flags_off, 0);
    bp_write_le32(mem.buf + BP_HEADER_TABLE_CRC_OFF,
                  bp_crc32(0, mem.buf + tbl_off.lo, tbl_size.lo));
    bp_write_le32(mem.buf + BP_HEADER_CRC_OFF,
                  bp_crc32(0, mem.buf, BP_HEADER_CRC_OFF));
    memset(&ls, 0, sizeof(ls));
    UNIT_CHECK(bp_archive_list(&io, list_cb, &ls) == BP_ERR_CORRUPT,
               "table/header flag mismatch is rejected even with valid CRCs");
    bp_write_le16(mem.buf + flags_off, BP_ENTRY_FLAG_MTIME_VALID);
    memcpy(mem.buf + BP_HEADER_SIZE + 4U, "a/./b/", 6U);
    memcpy(mem.buf + tbl_off.lo + BP_TABLE_HDR_V3 + 2U, "a/./b/", 6U);
    bp_write_le32(mem.buf + BP_HEADER_TABLE_CRC_OFF,
                  bp_crc32(0, mem.buf + tbl_off.lo, tbl_size.lo));
    bp_write_le32(mem.buf + BP_HEADER_CRC_OFF,
                  bp_crc32(0, mem.buf, BP_HEADER_CRC_OFF));
    memset(&ls, 0, sizeof(ls));
    UNIT_CHECK(bp_archive_list(&io, list_cb, &ls) == BP_ERR_CORRUPT,
               "reader rejects noncanonical paths before listing or extraction");
    free(mem.buf);
}

static void test_format6_name_length_boundary(void)
{
    MemIO mem;
    bp_io_t io;
    bp_archive_t full;
    bp_mini_archive_t mini;
    bp_uint64 zero = bp_u64_from_u32(0);
    char *name;
    int rc, full_open, mini_open;

    UNIT_SECTION("Archive format 6: maximum encoded name length");
    name = (char *)malloc((size_t)BP_MAX_NAME_LEN + 2U);
    mem.buf = (unsigned char *)malloc((size_t)MEM_IO_CAP);
    if (name == NULL || mem.buf == NULL) exit(2);
    memset(name, 'a', BP_MAX_NAME_LEN);
    name[BP_MAX_NAME_LEN - 1U] = '/';
    name[BP_MAX_NAME_LEN] = '\0';

    mem.pos = mem.size = 0;
    mem.cap = MEM_IO_CAP;
    io = make_mem_io(&mem);
    rc = bp_archive_create(&full, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    full_open = rc == BP_OK;
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&full, name, BP_TYPE_DIR, BP_OS_LINUX,
                                     NULL, zero, zero, 0);
    UNIT_CHECK(rc == BP_OK,
               "full writer accepts a 65,493-byte name that exactly fills the header size");
    if (rc == BP_OK) rc = bp_archive_finalize(&full);
    if (full_open) bp_archive_close(&full);
    UNIT_CHECK(rc == BP_OK && bp_archive_test(&io, NULL, NULL) == BP_OK,
               "full reader verifies the maximum-length name record");

    mem.pos = mem.size = 0;
    io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    mini_open = rc == BP_OK;
    if (rc == BP_OK)
        rc = bp_mini_archive_add_entry_ex(&mini, name, BP_TYPE_DIR,
                                          BP_OS_DOS, NULL, zero, zero, 0);
    UNIT_CHECK(rc == BP_OK,
               "mini writer accepts the maximum 65,493-byte name");
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    if (mini_open) bp_mini_archive_close(&mini);
    UNIT_CHECK(rc == BP_OK && bp_mini_archive_test(&io, NULL, NULL) == BP_OK,
               "mini reader verifies the maximum-length name record");

    name[BP_MAX_NAME_LEN - 1U] = 'a';
    name[BP_MAX_NAME_LEN] = '/';
    name[BP_MAX_NAME_LEN + 1U] = '\0';
    mem.pos = mem.size = 0;
    io = make_mem_io(&mem);
    rc = bp_archive_create(&full, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    full_open = rc == BP_OK;
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&full, name, BP_TYPE_DIR, BP_OS_LINUX,
                                     NULL, zero, zero, 0);
    UNIT_CHECK(rc == BP_ERR_NAME_TOO_LONG,
               "full writer rejects a name one byte beyond the header limit");
    if (full_open) bp_archive_close(&full);

    mem.pos = mem.size = 0;
    io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    mini_open = rc == BP_OK;
    if (rc == BP_OK)
        rc = bp_mini_archive_add_entry_ex(&mini, name, BP_TYPE_DIR,
                                          BP_OS_DOS, NULL, zero, zero, 0);
    UNIT_CHECK(rc == BP_ERR_NAME_TOO_LONG,
               "mini writer rejects a name one byte beyond the header limit");
    if (mini_open) bp_mini_archive_close(&mini);
    free(name);
    free(mem.buf);
}

typedef struct {
    MemIO output;
    bp_io_t io;
    bp_uint16 flags;
    int count;
    int done_status;
    int report_partial;
    int metadata_count;
    int directory_count;
    int directory_result;
} MiniInteropState;

static int mini_interop_list(const bp_mini_entry_t *entry, void *user_data)
{
    MiniInteropState *s = (MiniInteropState *)user_data;
    s->flags = entry->flags;
    s->count++;
    return BP_OK;
}

static bp_io_t *mini_interop_open(const bp_mini_entry_t *entry, void *user_data)
{
    MiniInteropState *s = (MiniInteropState *)user_data;
    s->flags = entry->flags;
    s->count++;
    return &s->io;
}

static int mini_interop_done(const bp_mini_entry_t *entry, bp_io_t *out,
                             int status, void *user_data)
{
    MiniInteropState *s = (MiniInteropState *)user_data;
    (void)entry;
    (void)out;
    s->done_status = status;
    return s->report_partial ? BP_ERR_PARTIAL : BP_OK;
}

static void test_mini_independent_format6_golden_archive(void)
{
    MemIO mem;
    bp_io_t io;
    MiniInteropState state;
    unsigned char output[8];
    int rc;

    UNIT_SECTION("Mini core: independent format-6 golden archive");
    mem.buf = (unsigned char *)FORMAT6_GOLDEN_ARCHIVE;
    mem.pos = 0;
    mem.size = (long)sizeof(FORMAT6_GOLDEN_ARCHIVE);
    mem.cap = mem.size;
    io = make_mem_io(&mem);

    memset(&state, 0, sizeof(state));
    state.output.buf = output;
    state.output.cap = sizeof(output);
    state.io = make_mem_io(&state.output);
    rc = bp_mini_archive_list(&io, mini_interop_list, &state);
    UNIT_CHECK(rc == BP_OK && state.count == 1 && state.flags == 0,
               "mini list accepts independently specified format-6 table");
    UNIT_CHECK(bp_mini_archive_test(&io, NULL, NULL) == BP_OK,
               "mini test verifies independent Stored payload and CRCs");

    state.count = 0;
    state.done_status = 99;
    rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                        mini_interop_done, &state);
    UNIT_CHECK(rc == BP_OK && state.done_status == BP_OK &&
               state.output.size == 1 && output[0] == 'A',
               "mini extracts and checks independent Stored payload");

    mem.buf = (unsigned char *)FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE;
    mem.pos = 0;
    mem.size = (long)sizeof(FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE);
    mem.cap = mem.size;
    state.count = 0;
    state.flags = 0xFFFF;
    rc = bp_mini_archive_list(&io, mini_interop_list, &state);
    UNIT_CHECK(rc == BP_OK && state.count == 1 && state.flags == 0,
               "mini lists the independent unframed single-blob entry");
    UNIT_CHECK(bp_mini_archive_test(&io, NULL, NULL) == BP_OK,
               "mini test verifies the unframed Stored payload");

    state.output.pos = 0;
    state.output.size = 0;
    state.count = 0;
    state.done_status = 99;
    rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                        mini_interop_done, &state);
    UNIT_CHECK(rc == BP_OK && state.done_status == BP_OK &&
               state.output.size == 1 && output[0] == 'A',
               "mini extraction decodes the unframed single blob");

    mem.buf = (unsigned char *)FORMAT6_SINGLE_BOLTLITE_GOLDEN_ARCHIVE;
    mem.pos = 0;
    mem.size = (long)sizeof(FORMAT6_SINGLE_BOLTLITE_GOLDEN_ARCHIVE);
    mem.cap = mem.size;
    UNIT_CHECK(bp_mini_archive_test(&io, NULL, NULL) == BP_OK,
               "mini test decompresses the unframed BoltLite blob");
    state.output.pos = 0;
    state.output.size = 0;
    state.count = 0;
    state.done_status = 99;
    rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                        mini_interop_done, &state);
    UNIT_CHECK(rc == BP_OK && state.done_status == BP_OK &&
               state.output.size == 1 && output[0] == 'A',
               "mini extraction decompresses the unframed BoltLite blob");
}

static void test_mini_single_blob_resource_preflight(void)
{
    unsigned char archive[sizeof(FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE)];
    unsigned char output[8];
    MemIO mem;
    bp_io_t io;
    MiniInteropState state;
    bp_uint64 oversized = bp_u64_from_u32(BP_MINI_MAX_BLOCK_SIZE + 1U);
    bp_uint64 table_offset;
    bp_uint64 table_size;
    bp_uint32 table_start;
    int rc;

    UNIT_SECTION("Mini core: single-blob resource preflight");
    memcpy(archive, FORMAT6_SINGLE_BLOB_GOLDEN_ARCHIVE, sizeof(archive));
    bp_write_le64(archive + 37, oversized);
    bp_write_le64(archive + 84, oversized);
    bp_write_le64(archive + 95, oversized);
    table_offset = bp_read_le64(archive + sizeof(archive) - BP_FOOTER_SIZE);
    table_size = bp_read_le64(archive + sizeof(archive) - BP_FOOTER_SIZE + 8);
    table_start = bp_u64_to_u32(table_offset);
    bp_write_le32(archive + BP_HEADER_TABLE_CRC_OFF,
                  bp_crc32(0, archive + table_start, bp_u64_to_u32(table_size)));
    bp_write_le32(archive + BP_HEADER_CRC_OFF,
                  bp_crc32(0, archive, BP_HEADER_CRC_OFF));

    mem.buf = archive;
    mem.pos = 0;
    mem.size = (long)sizeof(archive);
    mem.cap = mem.size;
    io = make_mem_io(&mem);
    memset(&state, 0, sizeof(state));
    state.output.buf = output;
    state.output.cap = sizeof(output);
    state.io = make_mem_io(&state.output);
    rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                        mini_interop_done, &state);
    UNIT_CHECK(rc == BP_ERR_OVERFLOW && state.count == 0,
               "oversized single blob is rejected before opening output");
}

static int mini_metadata_test(const bp_mini_entry_t *entry,
                              const bp_portable_metadata_t *metadata, void *user_data)
{
    MiniInteropState *s = (MiniInteropState *)user_data;
    (void)entry;
    if (!metadata->has_fat || metadata->fat_valid != 0x27 || metadata->fat_value != 0x21)
        return BP_ERR_CORRUPT;
    s->metadata_count++;
    return BP_OK;
}

static bp_io_t *mini_skip_test(const bp_mini_entry_t *entry, void *user_data)
{
    (void)entry; (void)user_data;
    return NULL;
}

static int mini_directory_test(const bp_mini_entry_t *entry, void *user_data)
{
    MiniInteropState *s = (MiniInteropState *)user_data;
    (void)entry;
    s->directory_count++;
    return s->directory_result;
}

static void test_mini_directory_outcomes(void)
{
    MemIO mem;
    bp_io_t io, source;
    bp_archive_t ar;
    StringSrc input;
    MiniInteropState state;
    bp_uint8 metadata[BP_META_FAT_SIZE];
    bp_uint64 zero = bp_u64_from_u32(0);
    int rc;
    UNIT_SECTION("Mini directory metadata outcomes");
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.pos = mem.size = 0; mem.cap = MEM_IO_CAP; io = make_mem_io(&mem);
    rc = bp_metadata_fat_encode(metadata, sizeof(metadata), BP_TYPE_DIR, 0x27, 0x21);
    if (rc == BP_OK) rc = bp_archive_create(&ar, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    if (rc == BP_OK) rc = bp_archive_add_entry_ex(&ar, "DIR/", BP_TYPE_DIR, BP_OS_DOS,
                                                   NULL, zero, zero, 0);
    source = make_string_io(&input, metadata, sizeof(metadata));
    if (rc == BP_OK) rc = bp_archive_add_entry_ex(&ar, "DIR/", BP_TYPE_META, BP_META_PORTABLE,
                                                   &source, bp_u64_from_u32(sizeof(metadata)), zero, 0);
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK, "directory metadata fixture written");
    memset(&state, 0, sizeof(state));
    rc = bp_mini_archive_extract_all_with_directories(&io, mini_interop_open,
              mini_interop_done, mini_directory_test, mini_metadata_test, &state);
    UNIT_CHECK(rc == BP_OK && state.directory_count == 1 && state.metadata_count == 1,
               "verified metadata is delivered for a successful directory");
    memset(&state, 0, sizeof(state)); state.directory_result = BP_ERR_PARTIAL;
    rc = bp_mini_archive_extract_all_with_directories(&io, mini_interop_open,
              mini_interop_done, mini_directory_test, mini_metadata_test, &state);
    UNIT_CHECK(rc == BP_ERR_PARTIAL && state.directory_count == 1 && state.metadata_count == 0,
               "skipped directory never receives metadata");
    memset(&state, 0, sizeof(state)); state.directory_result = BP_ERR_IO;
    rc = bp_mini_archive_extract_all_with_directories(&io, mini_interop_open,
              mini_interop_done, mini_directory_test, mini_metadata_test, &state);
    UNIT_CHECK(rc == BP_ERR_IO && state.directory_count == 1 && state.metadata_count == 0,
               "directory creation failure aborts before metadata");
    free(mem.buf);
}

typedef struct {
    char order[128];
    int count;
    int fat_seen;
} MiniDirectoryQueueState;

static int mini_directory_queue_apply(const bp_mini_directory_t *directory, void *user_data)
{
    MiniDirectoryQueueState *state = (MiniDirectoryQueueState *)user_data;
    if (state->count) strcat(state->order, "|");
    strcat(state->order, directory->path);
    state->count++;
    if (directory->fat_valid == 0x27 && directory->fat_value == 0x21) state->fat_seen++;
    return strcmp(directory->path, "parent/child") == 0 ? BP_ERR_PARTIAL : BP_OK;
}

static void test_mini_directory_queue(void)
{
    bp_mini_directory_queue_t queue;
    bp_mini_entry_t entry;
    MiniDirectoryQueueState state;
    int rc;
    UNIT_SECTION("Mini deferred directory metadata");
    memset(&queue, 0, sizeof(queue)); memset(&entry, 0, sizeof(entry));
    memset(&state, 0, sizeof(state));
    rc = bp_mini_directory_add(&queue, "parent", &entry);
    if (rc == BP_OK) rc = bp_mini_directory_add(&queue, "parent/child", &entry);
    if (rc == BP_OK) rc = bp_mini_directory_add(&queue, "sibling", &entry);
    if (rc == BP_OK) rc = bp_mini_directory_set_fat(&queue, "parent/child", 0x27, 0x21);
    UNIT_CHECK(rc == BP_OK && queue.count == 3, "directory metadata is queued");
    UNIT_CHECK(bp_mini_directory_set_fat(&queue, "missing", 0x27, 0x21) == BP_ERR_CORRUPT,
               "metadata cannot attach to a missing directory");
    rc = bp_mini_directory_finish(&queue, 1, mini_directory_queue_apply, &state);
    UNIT_CHECK(rc == BP_ERR_PARTIAL && state.count == 3 && state.fat_seen == 1 &&
               strcmp(state.order, "parent/child|sibling|parent") == 0 && queue.count == 0,
               "children are finalized before parents and partial results do not stop cleanup");
    rc = bp_mini_directory_add(&queue, "discarded", &entry);
    if (rc == BP_OK) rc = bp_mini_directory_finish(&queue, 0, NULL, &state);
    UNIT_CHECK(rc == BP_OK && queue.count == 0 && state.count == 3,
               "failed extraction discards deferred metadata without applying it");
}

static void test_format6_full_mini_interop(void)
{
    MemIO mem;
    bp_io_t io, source;
    bp_archive_t full;
    bp_mini_archive_t mini;
    StringSrc input;
    MiniInteropState state;
    ListState full_list;
    bp_uint8 output[256];
    bp_uint64 zero = bp_u64_from_u32(0);
    int rc;

    UNIT_SECTION("Archive format 6: full and mini interoperability");
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.pos = mem.size = 0; mem.cap = MEM_IO_CAP;
    io = make_mem_io(&mem);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    rc = bp_archive_create(&full, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&full, "FILE.TXT", BP_TYPE_DATA,
                                     BP_OS_LINUX, &source,
                                     bp_u64_from_u32(FILE1_LEN), zero,
                                     BP_ENTRY_FLAG_MTIME_VALID);
    if (rc == BP_OK) rc = bp_archive_finalize(&full);
    bp_archive_close(&full);
    UNIT_CHECK(rc == BP_OK, "full writer creates mini-compatible archive");

    memset(&state, 0, sizeof(state));
    state.output.buf = output; state.output.cap = sizeof(output);
    state.io = make_mem_io(&state.output);
    rc = bp_mini_archive_list(&io, mini_interop_list, &state);
    UNIT_CHECK(rc == BP_OK && state.count == 1 &&
               (state.flags & BP_ENTRY_FLAG_MTIME_VALID) != 0,
               "mini list receives known-zero timestamp flag");
    state.count = 0; state.done_status = 99;
    rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                         mini_interop_done, &state);
    UNIT_CHECK(rc == BP_OK && state.done_status == BP_OK &&
               state.output.size == (long)FILE1_LEN &&
               memcmp(output, FILE1_DATA, FILE1_LEN) == 0,
               "mini extracts full-writer bytes and verifies checksum");

    /* The first stored block begins after the inline header and block header. */
    mem.buf[BP_HEADER_SIZE + BP_ENTRY_FIXED + 8 + BP_BLOCK_HDR_V3] ^= 1;
    state.output.pos = state.output.size = 0; state.done_status = 99;
    rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                         mini_interop_done, &state);
    UNIT_CHECK(rc == BP_ERR_CHECKSUM && state.done_status == BP_ERR_CHECKSUM,
               "mini reports corrupt payload and gives cleanup the failure status");
    UNIT_CHECK(bp_mini_archive_test(&io, NULL, NULL) == BP_ERR_CHECKSUM,
               "mini test returns failure for a bad block without a callback");
    UNIT_CHECK(bp_archive_test(&io, NULL, NULL) == BP_ERR_CHECKSUM,
               "full test returns failure for a bad block without a callback");

    mem.pos = mem.size = 0;
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_BOLTLITE);
    if (rc == BP_OK)
        rc = bp_mini_archive_add_entry_ex(&mini, "MINI.TXT", BP_TYPE_DATA,
                                           BP_OS_CPM86, &source,
                                           bp_u64_from_u32(FILE1_LEN), zero, 0);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK)
        rc = bp_mini_archive_add_entry_ex(&mini, "SECOND.TXT", BP_TYPE_DATA,
                                           BP_OS_CPM86, &source,
                                           bp_u64_from_u32(FILE1_LEN), zero, 0);
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    bp_mini_archive_close(&mini);
    UNIT_CHECK(rc == BP_OK, "mini writer creates full-compatible archive");
    {
        unsigned first = BP_HEADER_SIZE;
        unsigned name_len = bp_read_le16(mem.buf + first + 2);
        bp_uint64 packed_size = bp_read_le64(mem.buf + first + 4 + name_len + 8);
        unsigned second = first + bp_read_le16(mem.buf + first) + (unsigned)packed_size.lo;
        unsigned flag_offset = second + bp_read_le16(mem.buf + second) - 2;
        mem.buf[flag_offset] ^= 1;
        memset(&state, 0, sizeof(state));
        state.output.buf = output; state.output.cap = sizeof(output);
        state.io = make_mem_io(&state.output);
        rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                             mini_interop_done, &state);
        UNIT_CHECK(rc == BP_ERR_CORRUPT && state.count == 0,
                   "mini preflights all entry headers before opening output");
        mem.buf[flag_offset] ^= 1;
    }
    memset(&full_list, 0, sizeof(full_list));
    rc = bp_archive_list(&io, list_cb, &full_list);
    UNIT_CHECK(rc == BP_OK && full_list.count == 2 &&
               !(full_list.entries[0].flags & BP_ENTRY_FLAG_MTIME_VALID),
               "full list receives mini's unknown-time flag");
    rc = bp_archive_test(&io, NULL, NULL);
    UNIT_CHECK(rc == BP_OK, "full reader verifies mini-writer bytes");
    memset(&state, 0, sizeof(state));
    state.output.buf = output; state.output.cap = sizeof(output);
    state.io = make_mem_io(&state.output);
    state.report_partial = 1;
    rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                         mini_interop_done, &state);
    UNIT_CHECK(rc == BP_ERR_PARTIAL && state.count == 2 &&
               state.output.size == (long)(2 * FILE1_LEN),
               "mini continues extracting after metadata loss and reports partial");
    free(mem.buf);
}

static void test_mini_archive_size_limit(void)
{
    static const unsigned char byte[1] = { 'x' };
    MemIO mem;
    bp_io_t io, source;
    bp_mini_archive_t mini;
    StringSrc input;
    MiniInteropState state;
    bp_uint64 zero;
    bp_uint64 oversized;
    int rc;

    UNIT_SECTION("Mini interchange archive-size limit");
    zero = bp_u64_from_u32(0);
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.pos = mem.size = 0; mem.cap = MEM_IO_CAP; io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, 0, BP_METHOD_STORED);
    UNIT_CHECK(rc == BP_OK && mini.block_size == BP_BLOCK_16K,
               "mini profile defaults to 16 KiB blocks");
    bp_mini_archive_close(&mini);
    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    mem.pos = (long)BP_MINI_MAX_ARCHIVE_SIZE - 1L;
    source = make_string_io(&input, byte, 1);
    if (rc == BP_OK)
        rc = bp_mini_archive_add_entry_ex(&mini, "FILE.TXT", BP_TYPE_DATA,
                                           BP_OS_CPM86, &source,
                                           bp_u64_from_u32(1), zero, 0);
    UNIT_CHECK(rc == BP_ERR_OVERFLOW,
               "mini writer refuses an entry that crosses the 2 GiB profile limit");
    bp_mini_archive_close(&mini);

    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    mem.pos = (long)BP_MINI_MAX_ARCHIVE_SIZE - 10L;
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    UNIT_CHECK(rc == BP_ERR_OVERFLOW,
               "mini finalizer includes table and footer in the profile limit");
    bp_mini_archive_close(&mini);

    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    source = make_string_io(&input, byte, 1);
    if (rc == BP_OK)
        rc = bp_mini_archive_add_entry_ex(&mini, "FILE.TXT", BP_TYPE_DATA,
                                           BP_OS_CPM86, &source,
                                           bp_u64_from_u32(1), zero, 0);
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    bp_mini_archive_close(&mini);
    UNIT_CHECK(rc == BP_OK, "mini size-limit reader fixture is valid");
    oversized.hi = 0;
    oversized.lo = (bp_uint32)BP_MINI_MAX_ARCHIVE_SIZE + 1U;
    bp_write_le64(mem.buf + 16, oversized);
    bp_write_le32(mem.buf + BP_HEADER_CRC_OFF,
                  bp_crc32(0, mem.buf, BP_HEADER_CRC_OFF));
    memset(&state, 0, sizeof(state));
    rc = bp_mini_archive_list(&io, mini_interop_list, &state);
    UNIT_CHECK(rc == BP_ERR_OVERFLOW,
               "mini reader rejects archive headers above the profile limit");
    free(mem.buf);
}

static void test_dos_time_interop(void)
{
    bp_uint64 utc;
    unsigned date, time;
    int rc;
    UNIT_SECTION("DOS local time conversion");
    date = (20U << 9) | (2U << 5) | 29U;
    time = (1U << 11) | (30U << 5);
    rc = bp_dos_local_to_utc(date, time, 60L, &utc);
    UNIT_CHECK(rc == 0 && utc.hi == 0 && utc.lo == 951784200UL,
               "leap-day local time converts using offset east of UTC");
    rc = bp_dos_utc_to_local(utc, 60L, &date, &time);
    UNIT_CHECK(rc == 0 && date == ((20U << 9) | (2U << 5) | 29U) &&
               time == ((1U << 11) | (30U << 5)),
               "local time round-trips with the same fixed offset");
    rc = bp_dos_local_to_utc((21U << 9) | (2U << 5) | 29U, 0, 0, &utc);
    UNIT_CHECK(rc != 0, "invalid leap day rejected");
    rc = bp_dos_local_to_utc((127U << 9) | (12U << 5) | 31U,
                             (23U << 11) | (59U << 5) | 29U, 0, &utc);
    UNIT_CHECK(rc == 0 && utc.hi == 1,
               "2107 DOS time does not wrap at 32 bits");
    rc = bp_dos_utc_to_local(utc, 0, &date, &time);
    UNIT_CHECK(rc == 0 && date == ((127U << 9) | (12U << 5) | 31U),
               "2107 DOS time round-trips");
    utc = bp_u64_from_u32(951784201UL);
    rc = bp_dos_utc_to_local(utc, 60, &date, &time);
    UNIT_CHECK(rc == 1, "odd seconds report FAT timestamp rounding");
    utc = bp_u64_from_u32(0);
    UNIT_CHECK(bp_dos_utc_to_local(utc, 0, &date, &time) < 0,
               "pre-1980 time is not clamped");
}

static void test_atari_name_projection(void)
{
    bp_atari_name_set_t set;
    char key[256];
    char name[16];
    unsigned int key_length;
    unsigned long i;
    int rc;

    UNIT_SECTION("Atari GEMDOS name projection and collisions");
    rc = bp_atari_project_name("foo/bar.txt", 0, key, sizeof(key),
                               &key_length);
    UNIT_CHECK(rc == BP_OK && strcmp(key, "FOO\\BAR.TXT") == 0 &&
               key_length == strlen("FOO\\BAR.TXT"),
               "path key folds case and uses GEMDOS separators");
    rc = bp_atari_project_name("FOO.TXT/", 1, key, sizeof(key),
                               &key_length);
    UNIT_CHECK(rc == BP_OK && strcmp(key, "FOO.TXT") == 0,
               "directory trailing slash is not part of destination key");
    UNIT_CHECK(bp_atari_project_name("123456789", 0, key, sizeof(key),
                                     &key_length) == BP_ERR_INVALID_ARG,
               "component longer than eight-character basename is refused");
    UNIT_CHECK(bp_atari_project_name("FILE.abcd", 0, key, sizeof(key),
                                     &key_length) == BP_ERR_INVALID_ARG,
               "extension longer than three characters is refused");
    UNIT_CHECK(bp_atari_project_name("FILE.", 0, key, sizeof(key),
                                     &key_length) == BP_ERR_INVALID_ARG,
               "trailing dot alias is refused");
    UNIT_CHECK(bp_atari_project_name("DIR//FILE", 0, key, sizeof(key),
                                     &key_length) == BP_ERR_INVALID_ARG,
               "empty path components are refused");
    UNIT_CHECK(bp_atari_project_name("caf\xE9.txt", 0, key, sizeof(key),
                                     &key_length) == BP_ERR_INVALID_ARG,
               "non-ASCII bytes outside base GEMDOS profile are refused");

    bp_atari_name_set_init(&set);
    rc = bp_atari_project_name("dir/file.txt", 0, key, sizeof(key),
                               &key_length);
    if (rc == BP_OK) rc = bp_atari_name_set_add(&set, key, key_length);
    UNIT_CHECK(rc == BP_OK, "first projected archive name is indexed");
    rc = bp_atari_project_name("DIR/FILE.TXT", 0, key, sizeof(key),
                               &key_length);
    if (rc == BP_OK) rc = bp_atari_name_set_add(&set, key, key_length);
    UNIT_CHECK(rc == BP_ERR_CORRUPT,
               "case-insensitive destination collision is rejected");
    bp_atari_name_set_free(&set);

    bp_atari_name_set_init(&set);
    rc = BP_OK;
    for (i = 0; i < BP_ATARI_PATH_MAX_ENTRIES && rc == BP_OK; i++) {
        sprintf(name, "F%07lX", i);
        rc = bp_atari_project_name(name, 0, key, sizeof(key), &key_length);
        if (rc == BP_OK) rc = bp_atari_name_set_add(&set, key, key_length);
    }
    UNIT_CHECK(rc == BP_OK && set.count == BP_ATARI_PATH_MAX_ENTRIES,
               "bounded index accepts its documented entry capacity");
    rc = bp_atari_project_name("ZZZZZZZZ", 0, key, sizeof(key), &key_length);
    if (rc == BP_OK) rc = bp_atari_name_set_add(&set, key, key_length);
    UNIT_CHECK(rc == BP_ERR_OVERFLOW,
               "index capacity is refused before another path is accepted");
    bp_atari_name_set_free(&set);
}

static void test_cpm86_name_projection(void)
{
    char projected[BP_CPM86_NAME_SIZE];
    char *colliding[2];
    char *distinct[2];
    int ok;

    UNIT_SECTION("CP/M-86 flat 8.3 name projection");
    ok = bp_cpm86_project_name("A:report.txt", projected);
    UNIT_CHECK(ok && strcmp(projected, "REPORT.TXT") == 0,
               "drive prefix and ASCII case normalize without truncation");
    UNIT_CHECK(bp_cpm86_project_name("ABCDEFGH.123", projected) &&
               strcmp(projected, "ABCDEFGH.123") == 0,
               "exact 8.3 boundary is accepted");
    UNIT_CHECK(!bp_cpm86_project_name("TOOLONGNAME.TXT", projected),
               "overlong basename is refused instead of truncated");
    UNIT_CHECK(!bp_cpm86_project_name("FILE.TOOO", projected),
               "overlong extension is refused instead of truncated");
    UNIT_CHECK(!bp_cpm86_project_name("DIR/FILE.TXT", projected),
               "directory paths are refused on the flat filesystem");
    UNIT_CHECK(!bp_cpm86_project_name("FILE?.TXT", projected),
               "wildcard path characters are refused");
    colliding[0] = "report.txt";
    colliding[1] = "REPORT.TXT";
    UNIT_CHECK(!bp_cpm86_names_unique(colliding, 2U),
               "case-folded 8.3 aliases fail creation preflight");
    distinct[0] = "ONE.TXT";
    distinct[1] = "TWO.TXT";
    UNIT_CHECK(bp_cpm86_names_unique(distinct, 2U),
               "distinct valid 8.3 sources pass creation preflight");
    UNIT_CHECK(!bp_cpm86_create_names_safe("A:REPORT.TXT", colliding, 1U),
               "archive target aliasing a source is rejected before truncation");
    UNIT_CHECK(bp_cpm86_create_names_safe("PACK.BLT", distinct, 2U),
               "valid archive and source names pass complete preflight");
}

static void test_mini_projected_name_index(void)
{
    bp_mini_projected_index_t index;
    char cp437_lower[] = {(char)0x82, '.', 'T', 'X', 'T', '\0'};
    char cp437_upper[] = {(char)0x90, '.', 'T', 'X', 'T', '\0'};

    UNIT_SECTION("Mini case-insensitive projected-name preflight");
    UNIT_CHECK(bp_mini_projected_index_init(&index) == BP_OK,
               "bounded projected-name index initializes");
    UNIT_CHECK(bp_mini_projected_index_add(&index, "Dir/One.txt", 0) == BP_OK,
               "first file and directory prefixes are indexed");
    UNIT_CHECK(bp_mini_projected_index_add(&index, "DIR/Two.txt", 0) == BP_ERR_PARTIAL,
               "case-folded parent-directory collision is refused");
    bp_mini_projected_index_free(&index);

    UNIT_CHECK(bp_mini_projected_index_init(&index) == BP_OK,
               "projected-name index can be reused");
    UNIT_CHECK(bp_mini_projected_index_add(&index, "DIR/One.txt", 0) == BP_OK &&
               bp_mini_projected_index_add(&index, "DIR/Two.txt", 0) == BP_OK,
               "same-spelling parent directories may be shared");
    UNIT_CHECK(bp_mini_projected_index_add(&index, "dir/Three.txt", 0) == BP_ERR_PARTIAL,
               "case-only aliases cannot merge directory trees");
    bp_mini_projected_index_free(&index);

    UNIT_CHECK(bp_mini_projected_index_init(&index) == BP_OK,
               "projected-name index initializes for codepage aliases");
    UNIT_CHECK(bp_mini_projected_index_add(&index, cp437_lower, 0) == BP_OK &&
               bp_mini_projected_index_add(&index, cp437_upper, 0) == BP_ERR_PARTIAL,
               "CP437 lowercase and uppercase names collide");
    bp_mini_projected_index_free(&index);

    UNIT_CHECK(bp_mini_projected_index_init(&index) == BP_OK,
               "projected-name index initializes for prefix type check");
    UNIT_CHECK(bp_mini_projected_index_add(&index, "NAME", 0) == BP_OK &&
               bp_mini_projected_index_add(&index, "NAME/CHILD.TXT", 0) == BP_ERR_PARTIAL,
               "a file cannot also be a projected parent directory");
    bp_mini_projected_index_free(&index);
}

static void test_portable_metadata(void)
{
    static const bp_uint8 golden[] = {
        0x42, 0x50, 0x4d, 0x54, 1, 0, 1, 0, 1, 0, 2, 0, 0x27, 0x21
    };
    bp_uint8 bytes[32];
    bp_portable_metadata_t m;
    int rc;
    UNIT_SECTION("Portable metadata envelope");
    rc = bp_metadata_fat_encode(bytes, sizeof(bytes), BP_TYPE_DATA, 0x27, 0x21);
    UNIT_CHECK(rc == BP_OK && memcmp(bytes, golden, sizeof(golden)) == 0,
               "encoder agrees with independently specified FAT bytes");
    rc = bp_metadata_parse(golden, sizeof(golden), &m);
    UNIT_CHECK(rc == BP_OK && m.supported_version && m.has_fat &&
               m.fat_valid == 0x27 && m.fat_value == 0x21,
               "decoder preserves FAT validity and value masks");
    memcpy(bytes, golden, sizeof(golden)); bytes[13] = 0x08;
    UNIT_CHECK(bp_metadata_parse(bytes, sizeof(golden), &m) == BP_ERR_CORRUPT,
               "FAT value outside validity mask rejected");
    memcpy(bytes, golden, sizeof(golden)); bytes[6] = 2;
    memcpy(bytes + sizeof(golden), golden + 8, 6);
    UNIT_CHECK(bp_metadata_parse(bytes, 20, &m) == BP_ERR_CORRUPT,
               "duplicate FAT record rejected");
    memcpy(bytes, golden, sizeof(golden)); bytes[10] = 3;
    UNIT_CHECK(bp_metadata_parse(bytes, sizeof(golden), &m) == BP_ERR_CORRUPT,
               "truncated TLV rejected");
    memcpy(bytes, golden, sizeof(golden)); bytes[8] = 9;
    UNIT_CHECK(bp_metadata_parse(bytes, sizeof(golden), &m) == BP_ERR_PARTIAL &&
               m.has_unknown, "unknown well-framed TLV reports partial");
    memcpy(bytes, golden, sizeof(golden)); bytes[4] = 2;
    UNIT_CHECK(bp_metadata_parse(bytes, sizeof(golden), &m) == BP_ERR_PARTIAL &&
               !m.supported_version, "unknown schema version is not interpreted");
    memcpy(bytes, golden, sizeof(golden)); bytes[14] = 0;
    UNIT_CHECK(bp_metadata_parse(bytes, 15, &m) == BP_ERR_CORRUPT,
               "trailing envelope bytes rejected");
}

static bp_io_t *full_metadata_test_open(const bp_entry_t *entry, void *user_data)
{
    MiniInteropState *s = (MiniInteropState *)user_data;
    (void)entry;
    s->count++;
    return &s->io;
}

static int full_metadata_test_done(const bp_entry_t *entry, bp_io_t *out,
                                   int status, void *user_data)
{
    MiniInteropState *s = (MiniInteropState *)user_data;
    (void)entry; (void)out;
    if (status == BP_OK && s->report_partial) return BP_ERR_PARTIAL;
    return status;
}

static void test_portable_metadata_archive(void)
{
    MemIO mem;
    bp_io_t io, source;
    bp_archive_t ar;
    StringSrc input;
    MiniInteropState state;
    bp_uint8 bytes[BP_META_FAT_SIZE], output[256];
    int malformed, rc;
    UNIT_SECTION("Portable metadata archive validation");
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    for (malformed = 0; malformed < 6; malformed++) {
        mem.pos = mem.size = 0; mem.cap = MEM_IO_CAP;
        io = make_mem_io(&mem);
        bp_metadata_fat_encode(bytes, sizeof(bytes), BP_TYPE_DATA, 0x27, 0x21);
        if (malformed == 1) bytes[10] = 3;
        if (malformed == 3) bytes[5] = BP_TYPE_DIR;
        rc = bp_archive_create(&ar, &io, BP_BLOCK_8K, BP_METHOD_STORED);
        source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
        if (rc == BP_OK)
            rc = bp_archive_add_entry(&ar, "FILE.TXT", BP_TYPE_DATA,
                                      BP_OS_LINUX, &source,
                                      bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
        source = make_string_io(&input, bytes, sizeof(bytes));
        if (rc == BP_OK)
            rc = bp_archive_add_entry_ex(&ar, "FILE.TXT", BP_TYPE_META,
                                         BP_META_PORTABLE, &source,
                                         bp_u64_from_u32(sizeof(bytes)), bp_u64_from_u32(0), 0);
        if (malformed == 4 && rc == BP_OK) {
            source = make_string_io(&input, bytes, sizeof(bytes));
            rc = bp_archive_add_entry_ex(&ar, "FILE.TXT", BP_TYPE_META,
                                         BP_OS_DOS, &source,
                                         bp_u64_from_u32(sizeof(bytes)), bp_u64_from_u32(0), 0);
        }
        if (malformed == 5 && rc == BP_OK) {
            source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
            rc = bp_archive_add_entry(&ar, "DUPE.TXT", BP_TYPE_DATA,
                                      BP_OS_LINUX, &source,
                                      bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
        }
        if (rc == BP_OK) rc = bp_archive_finalize(&ar);
        bp_archive_close(&ar);
        UNIT_CHECK(rc == BP_OK, "portable metadata fixture written");
        if ((malformed == 2 || malformed == 4 || malformed == 5) && rc == BP_OK) {
            long pos;
            int replacements = 0;
            bp_uint64 table_off = bp_read_le64(mem.buf + mem.size - BP_FOOTER_SIZE);
            bp_uint64 table_size = bp_read_le64(mem.buf + mem.size - BP_FOOTER_SIZE + 8);
            unsigned first = (unsigned)table_off.lo + BP_TABLE_HDR_V3;
            unsigned second = first + 2U + bp_read_le16(mem.buf + first) + BP_TABLE_REC_TAIL_V6;
            if (malformed == 2) {
                unsigned n = bp_read_le16(mem.buf + second);
                unsigned tail = second + 2U + n;
                bp_uint64 inline_offset = bp_read_le64(mem.buf + tail + 26);
                memcpy(mem.buf + second + 2, "SOME.TXT", 8);
                memcpy(mem.buf + inline_offset.lo + 4, "SOME.TXT", 8);
                replacements = 2;
            } else if (malformed == 4) {
                unsigned third = second + 2U + bp_read_le16(mem.buf + second) + BP_TABLE_REC_TAIL_V6;
                unsigned n = bp_read_le16(mem.buf + third);
                unsigned tail = third + 2U + n;
                bp_uint64 inline_offset = bp_read_le64(mem.buf + tail + 26);
                mem.buf[tail + 17] = BP_META_PORTABLE;
                mem.buf[inline_offset.lo + 4 + n + 27] = BP_META_PORTABLE;
                replacements = 2;
            } else {
                for (pos = 0; pos + 8 <= mem.size; pos++) {
                    if (memcmp(mem.buf + pos, "DUPE.TXT", 8) == 0) {
                        memcpy(mem.buf + pos, "FILE.TXT", 8);
                        replacements++;
                    }
                }
            }
            bp_write_le32(mem.buf + BP_HEADER_TABLE_CRC_OFF,
                          bp_crc32(0, mem.buf + table_off.lo, table_size.lo));
            bp_write_le32(mem.buf + BP_HEADER_CRC_OFF,
                          bp_crc32(0, mem.buf, BP_HEADER_CRC_OFF));
            UNIT_CHECK(replacements == 2, "independent duplicate fixture mutates header and table");
        }
        rc = bp_archive_test(&io, NULL, NULL);
        UNIT_CHECK(rc == (malformed ? BP_ERR_CORRUPT : BP_OK),
                   "full archive test validates portable metadata framing");
        rc = bp_mini_archive_test(&io, NULL, NULL);
        UNIT_CHECK(rc == (malformed ? BP_ERR_CORRUPT : BP_OK),
                   "mini archive test validates portable metadata framing");
        memset(&state, 0, sizeof(state));
        state.output.buf = output; state.output.cap = sizeof(output);
        state.io = make_mem_io(&state.output);
        rc = bp_archive_extract_all_ex(&io, full_metadata_test_open,
                                        full_metadata_test_done, NULL, &state);
        UNIT_CHECK(rc == (malformed ? BP_ERR_CORRUPT : BP_OK) &&
                   state.count == ((malformed == 2 || malformed == 4 || malformed == 5) ? 0 : (malformed ? 1 : 2)),
                   "full extraction validates metadata before opening its output");
        if (!malformed) {
            memset(&state, 0, sizeof(state));
            state.output.buf = output; state.output.cap = sizeof(output);
            state.io = make_mem_io(&state.output);
            state.report_partial = 1;
            rc = bp_archive_extract_all_ex(&io, full_metadata_test_open,
                                            full_metadata_test_done, NULL, &state);
            UNIT_CHECK(rc == BP_ERR_PARTIAL && state.count == 2,
                       "full extraction continues after incomplete metadata");
        }
        memset(&state, 0, sizeof(state));
        state.output.buf = output; state.output.cap = sizeof(output);
        state.io = make_mem_io(&state.output);
        rc = bp_mini_archive_extract_all_ex(&io, mini_interop_open,
                                             mini_interop_done, &state);
        UNIT_CHECK(rc == (malformed ? BP_ERR_CORRUPT : BP_ERR_PARTIAL),
                   "mini extraction rejects malformed or reports skipped metadata");
        if (!malformed) {
            MemIO copied, deleted_mem;
            bp_io_t copied_io, deleted_io;
            bp_archive_t copied_ar;
            bp_uint32 deleted_count = 0;
            const char *delete_names[1];
            ListState listed;
            memset(&state, 0, sizeof(state));
            state.output.buf = output; state.output.cap = sizeof(output);
            state.io = make_mem_io(&state.output);
            rc = bp_mini_archive_extract_all_with_metadata(&io, mini_interop_open,
                                    mini_interop_done, mini_metadata_test, &state);
            UNIT_CHECK(rc == BP_OK && state.metadata_count == 1,
                       "mini delivers validated FAT metadata after successful file extraction");
            memset(&state, 0, sizeof(state));
            rc = bp_mini_archive_extract_all_with_metadata(&io, mini_skip_test,
                                    mini_interop_done, mini_metadata_test, &state);
            UNIT_CHECK(rc == BP_ERR_PARTIAL && state.metadata_count == 0,
                       "mini does not apply metadata when the subject was skipped");

            copied.buf = (unsigned char *)malloc(MEM_IO_CAP);
            deleted_mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
            if (!copied.buf || !deleted_mem.buf) exit(2);
            copied.pos = copied.size = 0; copied.cap = MEM_IO_CAP;
            deleted_mem.pos = deleted_mem.size = 0; deleted_mem.cap = MEM_IO_CAP;
            copied_io = make_mem_io(&copied);
            deleted_io = make_mem_io(&deleted_mem);
            rc = bp_archive_create(&copied_ar, &copied_io, BP_BLOCK_8K, BP_METHOD_STORED);
            if (rc == BP_OK) rc = bp_archive_copy_from(&copied_ar, &io);
            source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
            if (rc == BP_OK)
                rc = bp_archive_add_entry(&copied_ar, "NEXT.TXT", BP_TYPE_DATA,
                                          BP_OS_LINUX, &source,
                                          bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
            if (rc == BP_OK) rc = bp_archive_finalize(&copied_ar);
            bp_archive_close(&copied_ar);
            if (rc == BP_OK) rc = bp_archive_test(&copied_io, NULL, NULL);
            memset(&listed, 0, sizeof(listed));
            if (rc == BP_OK) rc = bp_archive_list(&copied_io, list_cb, &listed);
            UNIT_CHECK(rc == BP_OK && listed.count == 3 &&
                       listed.entries[1].entry_type == BP_TYPE_META &&
                       (listed.entries[0].flags & BP_ENTRY_FLAG_MTIME_VALID),
                       "archive copy and append preserve metadata group and flags");
            delete_names[0] = "FILE.TXT";
            rc = bp_archive_delete(&copied_io, &deleted_io, delete_names, 1, &deleted_count);
            if (rc == BP_OK) rc = bp_archive_test(&deleted_io, NULL, NULL);
            memset(&listed, 0, sizeof(listed));
            if (rc == BP_OK) rc = bp_archive_list(&deleted_io, list_cb, &listed);
            UNIT_CHECK(rc == BP_OK && deleted_count == 2 && listed.count == 1 &&
                       strcmp(listed.entries[0].name, "NEXT.TXT") == 0,
                       "deletion removes both subject and its metadata");
            free(copied.buf); free(deleted_mem.buf);
        }
    }
    free(mem.buf);
}

static void test_duplicate_writer_names(void)
{
    MemIO mem;
    bp_io_t io, source;
    bp_archive_t full;
    bp_mini_archive_t mini;
    StringSrc input;
    int rc;
    UNIT_SECTION("Duplicate primary names");
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.pos = mem.size = 0; mem.cap = MEM_IO_CAP; io = make_mem_io(&mem);
    rc = bp_archive_create(&full, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_archive_add_entry(&full, "SAME", BP_TYPE_DATA, BP_OS_LINUX,
                                               &source, bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_archive_add_entry(&full, "SAME", BP_TYPE_DATA, BP_OS_LINUX,
                                               &source, bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
    if (rc == BP_OK) rc = bp_archive_finalize(&full);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "full writer refuses duplicate primary names");
    bp_archive_close(&full);

    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_mini_archive_add_entry(&mini, "SAME", BP_TYPE_DATA, BP_OS_DOS,
                                                     &source, bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_mini_archive_add_entry(&mini, "SAME", BP_TYPE_DATA, BP_OS_DOS,
                                                     &source, bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "mini writer refuses duplicate primary names");
    bp_mini_archive_close(&mini);

    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_archive_create(&full, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_archive_add_entry_ex(&full, "ORPHAN", BP_TYPE_META,
                              BP_META_PORTABLE, &source, bp_u64_from_u32(FILE1_LEN),
                              bp_u64_from_u32(0), 0);
    if (rc == BP_OK) rc = bp_archive_finalize(&full);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "full writer refuses orphan portable metadata");
    bp_archive_close(&full);

    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_mini_archive_add_entry_ex(&mini, "ORPHAN", BP_TYPE_META,
                              BP_META_PORTABLE, &source, bp_u64_from_u32(FILE1_LEN),
                              bp_u64_from_u32(0), 0);
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "mini writer refuses orphan portable metadata");
    bp_mini_archive_close(&mini);

    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_archive_create(&full, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_archive_add_entry(&full, "SAME", BP_TYPE_DATA, BP_OS_LINUX,
                              &source, bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
    if (rc == BP_OK) rc = bp_archive_add_entry(&full, "SAME/", BP_TYPE_DIR, BP_OS_LINUX,
                              NULL, bp_u64_from_u32(0), bp_u64_from_u32(0));
    if (rc == BP_OK) rc = bp_archive_finalize(&full);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "full writer rejects directory trailing-slash alias");
    bp_archive_close(&full);

    mem.pos = mem.size = 0; io = make_mem_io(&mem);
    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) rc = bp_mini_archive_add_entry(&mini, "SAME", BP_TYPE_DATA, BP_OS_DOS,
                              &source, bp_u64_from_u32(FILE1_LEN), bp_u64_from_u32(0));
    if (rc == BP_OK) rc = bp_mini_archive_add_entry(&mini, "SAME/", BP_TYPE_DIR, BP_OS_DOS,
                              NULL, bp_u64_from_u32(0), bp_u64_from_u32(0));
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "mini writer rejects directory trailing-slash alias");
    bp_mini_archive_close(&mini);
    free(mem.buf);
}

static void test_mini_local_directory_names(void)
{
    MemIO mem;
    bp_io_t io, source;
    bp_mini_archive_t mini;
    StringSrc input;
    ListState listed;
    int rc;

    UNIT_SECTION("Mini local directory path spelling");
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.pos = mem.size = 0;
    mem.cap = MEM_IO_CAP;
    io = make_mem_io(&mem);

    rc = bp_mini_archive_create(&mini, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    if (rc == BP_OK)
        rc = bp_mini_add_local_entry(&mini, "folder", BP_TYPE_DIR,
                                     BP_OS_DOS, NULL,
                                     bp_u64_from_u32(0),
                                     bp_u64_from_u32(0), 0);
    UNIT_CHECK(rc == BP_OK,
               "mini local-entry helper adds a directory without caller slash");
    if (rc == BP_OK)
        rc = bp_mini_add_fat_metadata(&mini, "folder", BP_TYPE_DIR,
                                      BP_META_FAT_MASK, 0x02);
    UNIT_CHECK(rc == BP_OK,
               "directory portable metadata uses the canonical subject path");
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK)
        rc = bp_mini_add_local_entry(&mini, "folder/FILE.TXT", BP_TYPE_DATA,
                                     BP_OS_DOS, &source,
                                     bp_u64_from_u32(FILE1_LEN),
                                     bp_u64_from_u32(0), 0);
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&mini);
    bp_mini_archive_close(&mini);
    UNIT_CHECK(rc == BP_OK,
               "mini archive with directory, metadata, and child finalizes");

    memset(&listed, 0, sizeof(listed));
    if (rc == BP_OK) rc = bp_archive_list(&io, list_cb, &listed);
    UNIT_CHECK(rc == BP_OK && listed.count == 3 &&
               strcmp(listed.names[0], "folder/") == 0 &&
               listed.entries[1].entry_type == BP_TYPE_META &&
               strcmp(listed.names[1], "folder/") == 0 &&
               strcmp(listed.names[2], "folder/FILE.TXT") == 0,
               "directory and portable metadata share the canonical slash path");
    if (rc == BP_OK)
        UNIT_CHECK(bp_archive_test(&io, NULL, NULL) == BP_OK &&
                   bp_mini_archive_test(&io, NULL, NULL) == BP_OK,
                   "full and mini readers verify the local-helper archive");
    free(mem.buf);
}

static void test_full_interchange_profile(void)
{
    MemIO mem;
    MemIO copied;
    bp_io_t io, source;
    bp_io_t sparse_io;
    bp_io_t copied_io;
    bp_archive_t ar;
    bp_archive_t copied_ar;
    StringSrc input;
    SparseIO sparse;
    ListState list;
    bp_uint64 zero;
    bp_uint64 too_large;
    unsigned char *saved;
    long saved_size;
    bp_uint32 i;
    bp_off_t before_extra;
    char directory_name[16];
    int limit_rc;
    int finalize_rc;
    int rc;

    UNIT_SECTION("Full writer interchange profile");
    mem.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!mem.buf) exit(2);
    mem.pos = mem.size = 0; mem.cap = MEM_IO_CAP;
    io = make_mem_io(&mem);
    zero = bp_u64_from_u32(0);

    rc = bp_archive_create_profile(&ar, &io, BP_PROFILE_INTERCHANGE_1);
    UNIT_CHECK(rc == BP_OK && ar.method == BP_METHOD_BOLTLITE &&
               ar.block_size == BP_BLOCK_16K,
               "full profile selects BoltLite with 16 KiB blocks");
    if (rc == BP_OK) {
        rc = bp_archive_add_entry_ex(&ar, "folder/", BP_TYPE_DIR,
                                     BP_OS_LINUX, NULL, zero, zero, 0);
    }
    source = make_string_io(&input, FILE2_DATA, FILE2_LEN);
    if (rc == BP_OK) {
        rc = bp_archive_add_entry_ex(&ar, "folder/data.bin", BP_TYPE_DATA,
                                     BP_OS_LINUX, &source,
                                     bp_u64_from_u32(FILE2_LEN), zero, 0);
    }
    too_large.hi = 1;
    too_large.lo = 0;
    source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
    if (rc == BP_OK) {
        int size_rc;
        size_rc = bp_archive_add_entry_ex(&ar, "oversized.bin", BP_TYPE_DATA,
                                          BP_OS_LINUX, &source, too_large,
                                          zero, 0);
        UNIT_CHECK(size_rc == BP_ERR_OVERFLOW,
                   "profile rejects entries above the 32-bit size limit before reading");
    }
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK, "full profile writes a valid bounded archive");

    mem_rewind(&mem);
    memset(&list, 0, sizeof(list));
    rc = bp_archive_list(&io, list_cb, &list);
    UNIT_CHECK(rc == BP_OK && list.count == 2 &&
               list.entries[1].method <= BP_METHOD_BOLTLITE &&
               bp_read_le32(mem.buf + (size_t)list.entries[1].offset.lo +
                            4U + list.entries[1].name_len + 22U) == BP_BLOCK_16K,
               "full profile output uses mini-compatible entry methods and block sizes");
    mem_rewind(&mem);
    rc = bp_mini_archive_test(&io, NULL, NULL);
    UNIT_CHECK(rc == BP_OK,
               "mini reader verifies an archive emitted by the full profile writer");

    copied.buf = (unsigned char *)malloc(MEM_IO_CAP);
    if (!copied.buf) exit(2);
    copied.pos = copied.size = 0; copied.cap = MEM_IO_CAP;
    copied_io = make_mem_io(&copied);
    rc = bp_archive_create_profile(&copied_ar, &copied_io,
                                   BP_PROFILE_INTERCHANGE_1);
    mem_rewind(&mem);
    if (rc == BP_OK) rc = bp_archive_copy_from(&copied_ar, &io);
    if (rc == BP_OK) rc = bp_archive_finalize(&copied_ar);
    bp_archive_close(&copied_ar);
    mem_rewind(&copied);
    if (rc == BP_OK) rc = bp_mini_archive_test(&copied_io, NULL, NULL);
    UNIT_CHECK(rc == BP_OK,
               "profile copy preserves mini compatibility and validates source entries");
    free(copied.buf);

    rc = bp_archive_open_append(&ar, &io, BP_BLOCK_64K, BP_METHOD_BOLTLZ);
    if (rc == BP_OK) rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
    if (rc == BP_OK) {
        source = make_string_io(&input, FILE1_DATA, FILE1_LEN);
        rc = bp_archive_add_entry_ex(&ar, "added.txt", BP_TYPE_DATA,
                                     BP_OS_LINUX, &source,
                                     bp_u64_from_u32(FILE1_LEN), zero, 0);
    }
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK, "profile mode validates and preserves a conforming archive during append");

    mem_rewind(&mem);
    rc = bp_archive_test(&io, NULL, NULL);
    UNIT_CHECK(rc == BP_OK, "profile archive remains readable after append");

    mem.pos = mem.size = 0;
    rc = create_test_archive(&mem, BP_METHOD_BOLTLZ);
    saved_size = mem.size;
    saved = (unsigned char *)malloc((size_t)saved_size);
    if (!saved) exit(2);
    memcpy(saved, mem.buf, (size_t)saved_size);
    mem_rewind(&mem);
    rc = bp_archive_open_append(&ar, &io, BP_BLOCK_16K, BP_METHOD_BOLTLITE);
    if (rc == BP_OK) rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
    UNIT_CHECK(rc == BP_ERR_UNSUPPORTED_METHOD,
               "profile append refuses an existing archive with an extended codec");
    if (rc != BP_ERR_UNSUPPORTED_METHOD) bp_archive_close(&ar);
    UNIT_CHECK(mem.size == saved_size &&
               memcmp(mem.buf, saved, (size_t)saved_size) == 0,
               "refused profile append leaves the archive bytes unchanged");
    free(saved);

    sparse.pos = 0;
    sparse_io = make_sparse_io(&sparse);
    rc = bp_archive_create_profile(&ar, &sparse_io,
                                   BP_PROFILE_INTERCHANGE_1);
    sparse.pos = (bp_off_t)BP_PROFILE1_MAX_ARCHIVE - 10;
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    UNIT_CHECK(rc == BP_ERR_OVERFLOW,
               "profile finalizer includes file-table and footer overhead in the size cap");
    bp_archive_close(&ar);

    sparse.pos = 0;
    rc = bp_archive_create_profile(&ar, &sparse_io,
                                   BP_PROFILE_INTERCHANGE_1);
    sparse.pos = (bp_off_t)BP_PROFILE1_MAX_ARCHIVE - 40;
    if (rc == BP_OK) {
        rc = bp_archive_add_entry_ex(&ar, "d/", BP_TYPE_DIR, BP_OS_LINUX,
                                     NULL, zero, zero, 0);
    }
    UNIT_CHECK(rc == BP_ERR_OVERFLOW &&
               sparse.pos == (bp_off_t)BP_PROFILE1_MAX_ARCHIVE - 40,
               "profile writer rejects an entry whose serialized record crosses the size cap");
    bp_archive_close(&ar);

    sparse.pos = 0;
    rc = bp_archive_create_profile(&ar, &sparse_io,
                                   BP_PROFILE_INTERCHANGE_1);
    sparse.pos = (bp_off_t)BP_PROFILE1_MAX_ARCHIVE - 130;
    source = make_string_io(&input, FILE1_DATA, 1);
    if (rc == BP_OK) {
        rc = bp_archive_add_entry_ex(&ar, "x", BP_TYPE_DATA, BP_OS_LINUX,
                                     &source, bp_u64_from_u32(1), zero, 0);
    }
    UNIT_CHECK(rc == BP_ERR_OVERFLOW &&
               sparse.pos == (bp_off_t)BP_PROFILE1_MAX_ARCHIVE - 130 +
                             BP_ENTRY_FIXED + 1,
               "profile writer refuses the block before it crosses the serialized-size cap");
    bp_archive_close(&ar);

    sparse.pos = 0;
    rc = bp_archive_create_profile(&ar, &sparse_io,
                                   BP_PROFILE_INTERCHANGE_1);
    for (i = 0; rc == BP_OK && i < BP_PROFILE1_MAX_ENTRIES; i++) {
        sprintf(directory_name, "d%05lu/", (unsigned long)i);
        rc = bp_archive_add_entry_ex(&ar, directory_name, BP_TYPE_DIR,
                                     BP_OS_LINUX, NULL, zero, zero, 0);
    }
    UNIT_CHECK(rc == BP_OK && ar.entries_written == BP_PROFILE1_MAX_ENTRIES,
               "profile writer accepts exactly 65,535 physical entries");
    before_extra = sparse.pos;
    limit_rc = bp_archive_add_entry_ex(&ar, "d65535/", BP_TYPE_DIR,
                                      BP_OS_LINUX, NULL, zero, zero, 0);
    UNIT_CHECK(limit_rc == BP_ERR_OVERFLOW && sparse.pos == before_extra,
               "profile writer refuses entry 65,536 before writing it");
    finalize_rc = rc == BP_OK ? bp_archive_finalize(&ar) : rc;
    UNIT_CHECK(finalize_rc == BP_OK,
               "maximum-entry profile archive finalizes within serialized-size limit");
    bp_archive_close(&ar);
    free(mem.buf);
}

int main(void)
{
    int rc;

    printf("BoltPack unit tests: archive format\n");

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "bp_compress_init() failed: %d\n", rc);
        return 1;
    }

    test_header_bytes();
    test_independent_format6_golden_archive();
    test_format6_single_blob_golden_archive();
    test_previous_versions_rejected();
    test_format6_flags_and_names();
    test_format6_name_length_boundary();
    test_format6_full_mini_interop();
    test_mini_independent_format6_golden_archive();
    test_mini_single_blob_resource_preflight();
    test_mini_archive_size_limit();
    test_mini_directory_outcomes();
    test_mini_directory_queue();
    test_dos_time_interop();
    test_atari_name_projection();
    test_cpm86_name_projection();
    test_mini_projected_name_index();
    test_portable_metadata();
    test_portable_metadata_archive();
    test_mini_local_directory_names();
    test_duplicate_writer_names();
    test_full_interchange_profile();

    UNIT_SECTION("Archive format: Stored method");
    test_list_method("list (Stored)", BP_METHOD_STORED);
    test_integrity_method("test (Stored)", BP_METHOD_STORED);

    UNIT_SECTION("Archive format: BoltLite method");
    test_list_method("list (BoltLite)", BP_METHOD_BOLTLITE);
    test_integrity_method("test (BoltLite)", BP_METHOD_BOLTLITE);

   UNIT_SECTION("Archive format: boltlz method");
   test_list_method("list (boltlz)", BP_METHOD_BOLTLZ);
   test_integrity_method("test (boltlz)", BP_METHOD_BOLTLZ);

    UNIT_SECTION("Archive format: Fluff method");
    test_list_method("list (Fluff)", BP_METHOD_FLUFF);
    test_integrity_method("test (Fluff)", BP_METHOD_FLUFF);

    test_bad_magic();
    test_info();
    test_dir_entry();
    test_empty_archive();
    test_checksum_corruption();

    return UNIT_REPORT();
}
