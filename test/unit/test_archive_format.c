/*
 * test_archive_format.c -- unit tests for archive create / list / test / info
 *
 * Uses a memory-backed bp_io_t to exercise the full archiver API without
 * touching the filesystem.  Tests Stored, LZSS, fastlz, and Fluff methods.
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
        UNIT_CHECK(mem.buf[4] == 0x03 && mem.buf[5] == 0x00,
                   "format version == 3 (little-endian)");

        /* file_count (little-endian uint32 at offset 8) must be 2 */
        UNIT_CHECK(mem.buf[8] == 2 && mem.buf[9] == 0 &&
                   mem.buf[10] == 0 && mem.buf[11] == 0,
                   "file_count == 2");
    }

    free(mem.buf);
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
        UNIT_CHECK(info.version == 3, "info.version == 3");
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

    UNIT_SECTION("Archive format: Stored method");
    test_list_method("list (Stored)", BP_METHOD_STORED);
    test_integrity_method("test (Stored)", BP_METHOD_STORED);

    UNIT_SECTION("Archive format: LZSS method");
    test_list_method("list (LZSS)", BP_METHOD_LZSS);
    test_integrity_method("test (LZSS)", BP_METHOD_LZSS);

   UNIT_SECTION("Archive format: fastlz method");
   test_list_method("list (fastlz)", BP_METHOD_FASTLZ);
   test_integrity_method("test (fastlz)", BP_METHOD_FASTLZ);

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
