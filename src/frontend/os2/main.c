/*
 * main.c -- OS/2 CLI frontend for BoltPack
 *
 * Target:   IBM OS/2 Warp 3/4 (32-bit), ArcaOS, eComStation.
 * Compiler: Open Watcom v2 (os2 target) or os2-gcc (GCC with EMX/kLIBC).
 *
 * Archive reads and in-place append use the standard ANSI C runtime
 * (fopen/fread/fwrite/fseek/ftell). Create/delete stages and extraction
 * staging use OS/2 file handles for exclusive creation and publication.
 * Directory traversal uses the native OS/2 API (DosFindFirst/DosFindNext).
 * OS/2 FAT/HPFS attributes and portable FAT metadata are preserved; HPFS
 * extended attributes such as ".TYPE" are not transferred.
 *
 * Metadata (BP_TYPE_META) payload for BP_OS_OS2 (same as DOS family):
 *   Byte 0 : FAT/HPFS attribute byte (read-only/hidden/system/archive bits)
 *
 * Cross-compilation from Linux:
 *   os2-gcc -ansi -Wall -Wextra -O2                        \
 *       -I src/core                     \
 *       src/core/archiver.c src/core/compress.c            \
 *       src/core/bp_codepage.c                             \
 *       src/frontend/os2/main.c                            \
 *       src/core/boltlz.c                         \
 *       -o build/os2/boltpack.exe
 *   make os2-watcom
 *
 * ANSI C (C89/C90) throughout.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_OS2
#define BP_CREATOR_CPU BP_CPU_X86

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"
#include "../../core/bp_metadata.h"
#include "../mini_shared/dos_time.h"

/* OS/2 Toolkit headers */
#define INCL_DOSFILEMGR
#define INCL_DOSERRORS
#define INCL_DOSPROCESS
#define INCL_DOSNLS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <io.h>
#if defined(__EMX__)
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

/* ---- constants -------------------------------------------------------- */

#define BP_MAX_PATH   260

/* OS/2 uses CP850 for Western Europe */
#define OS2_CP  BP_CP_850

/* FAT/HPFS attribute bits */
#ifndef FILE_READONLY
#  define FILE_READONLY   0x0001
#  define FILE_HIDDEN     0x0002
#  define FILE_SYSTEM     0x0004
#  define FILE_DIRECTORY  0x0010
#  define FILE_ARCHIVED   0x0020
#endif

#define OS2_ATTR_MASK   (FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM | FILE_ARCHIVED)

/* Metadata payload: 1 byte (FAT/HPFS attribute) */
#define OS2_META_PAYLOAD    1
#define OS2_DIR_QUEUE_MAX 256U
#define OS2_DIR_QUEUE_BYTES_MAX 32768UL
#define OS2_NAME_SET_MAX 1024U
#define OS2_NAME_SET_BYTES_MAX 32768UL

/* ---- verbosity -------------------------------------------------------- */

#define VERB_DEFAULT  0
#define VERB_QUIET    1
#define VERB_SILENT   2

static int g_verbosity = VERB_DEFAULT;
static bp_uint32 g_unknown_source_times;

/* ======================================================================
 * bp_io_t adapters for FILE *
 * ====================================================================== */

static long file_read(void *h, void *buf, bp_uint32 n)
{
    return (long)fread(buf, 1, (size_t)n, (FILE *)h);
}

static long file_write(void *h, const void *buf, bp_uint32 n)
{
    return (long)fwrite(buf, 1, (size_t)n, (FILE *)h);
}

static int file_seek(void *h, bp_off_t off, int org)
{
    return fseek((FILE *)h, (long)off, org);
}

static bp_off_t file_tell(void *h)
{
    return (bp_off_t)ftell((FILE *)h);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

typedef struct {
    HFILE handle;
} os2_archive_io_t;

static long os2_archive_read(void *handle, void *buffer, bp_uint32 count)
{
    os2_archive_io_t *file = (os2_archive_io_t *)handle;
    ULONG received = 0;
    APIRET rc = DosRead(file->handle, buffer, (ULONG)count, &received);
    return rc == NO_ERROR ? (long)received : -1L;
}

static long os2_archive_write(void *handle, const void *buffer, bp_uint32 count)
{
    os2_archive_io_t *file = (os2_archive_io_t *)handle;
    ULONG written = 0;
    APIRET rc = DosWrite(file->handle, (PVOID)buffer, (ULONG)count, &written);
    return rc == NO_ERROR ? (long)written : -1L;
}

static int os2_archive_seek(void *handle, bp_off_t offset, int origin)
{
    os2_archive_io_t *file = (os2_archive_io_t *)handle;
    ULONG method;
    ULONG position = 0;
    if (origin == SEEK_SET) method = FILE_BEGIN;
    else if (origin == SEEK_CUR) method = FILE_CURRENT;
    else if (origin == SEEK_END) method = FILE_END;
    else return -1;
    return DosSetFilePtr(file->handle, (LONG)offset, method,
                         &position) == NO_ERROR ? 0 : -1;
}

static bp_off_t os2_archive_tell(void *handle)
{
    os2_archive_io_t *file = (os2_archive_io_t *)handle;
    ULONG position = 0;
    if (DosSetFilePtr(file->handle, 0L, FILE_CURRENT, &position) != NO_ERROR ||
        position > 0x7FFFFFFFUL)
        return (bp_off_t)-1;
    return (bp_off_t)position;
}

static void bp_io_from_os2_handle(bp_io_t *io, os2_archive_io_t *file)
{
    io->read = os2_archive_read;
    io->write = os2_archive_write;
    io->seek = os2_archive_seek;
    io->tell = os2_archive_tell;
    io->handle = file;
}

/* ======================================================================
 * Read-only memory buffer I/O  (for metadata payload supply).
 * ====================================================================== */

typedef struct {
    const unsigned char *buf;
    bp_uint32            size;
    bp_uint32            pos;
} rbuf_ctx_t;

static long rbuf_read_fn(void *h, void *buf, bp_uint32 n)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    if (c->pos >= c->size) return 0;
    if (n > c->size - c->pos) n = c->size - c->pos;
    memcpy(buf, c->buf + c->pos, (size_t)n);
    c->pos += n;
    return (long)n;
}
static long   rbuf_write_fn(void *h, const void *b, bp_uint32 n) { (void)h;(void)b;(void)n; return -1; }
static int    rbuf_seek_fn(void *h, bp_off_t off, int org)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    long np;
    if (org == SEEK_SET)      np = (long)off;
    else if (org == SEEK_CUR) np = (long)c->pos + (long)off;
    else if (org == SEEK_END) np = (long)c->size + (long)off;
    else return -1;
    if (np < 0 || (bp_uint32)np > c->size) return -1;
    c->pos = (bp_uint32)np;
    return 0;
}
static bp_off_t rbuf_tell_fn(void *h) { return (bp_off_t)((rbuf_ctx_t *)h)->pos; }

static void bp_io_from_rbuf(bp_io_t *io, rbuf_ctx_t *ctx)
{
    io->read   = rbuf_read_fn;
    io->write  = rbuf_write_fn;
    io->seek   = rbuf_seek_fn;
    io->tell   = rbuf_tell_fn;
    io->handle = ctx;
}

/* ======================================================================
 * Write-only memory buffer I/O  (for metadata capture on extraction).
 * ====================================================================== */

typedef struct {
    unsigned char *buf;
    bp_uint32      cap;
    bp_uint32      len;
    bp_uint32      pos;
} wbuf_ctx_t;

static long wbuf_read_fn(void *h, void *b, bp_uint32 n) { (void)h;(void)b;(void)n; return -1; }
static long wbuf_write_fn(void *h, const void *data, bp_uint32 n)
{
    wbuf_ctx_t *c = (wbuf_ctx_t *)h;
    if (c->pos > c->cap) return -1;
    if (n > c->cap - c->pos) n = c->cap - c->pos;
    if (!n) return 0;
    memcpy(c->buf + c->pos, data, (size_t)n);
    c->pos += n;
    if (c->pos > c->len) c->len = c->pos;
    return (long)n;
}
static int wbuf_seek_fn(void *h, bp_off_t off, int org)
{
    wbuf_ctx_t *c = (wbuf_ctx_t *)h;
    long np;
    if (org == SEEK_SET)      np = (long)off;
    else if (org == SEEK_CUR) np = (long)c->pos + (long)off;
    else if (org == SEEK_END) np = (long)c->len + (long)off;
    else return -1;
    if (np < 0 || (bp_uint32)np > c->cap) return -1;
    c->pos = (bp_uint32)np;
    return 0;
}
static bp_off_t wbuf_tell_fn(void *h) { return (bp_off_t)((wbuf_ctx_t *)h)->pos; }

typedef struct {
    bp_io_t       io;       /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[BP_META_MAX_SIZE];
} meta_extract_t;

typedef struct os2_dir_restore_s {
    struct os2_dir_restore_s *next;
    char *path;
    bp_uint64 mtime;
    bp_uint16 flags;
    unsigned char native_attr;
    unsigned char fat_valid;
    unsigned char fat_value;
    int native_seen;
    int fat_seen;
} os2_dir_restore_t;

/* ---- error messages --------------------------------------------------- */

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_OK:                    return "success";
    case BP_ERR_IO:                return "I/O error";
    case BP_ERR_NOMEM:             return "out of memory";
    case BP_ERR_MAGIC:             return "not a BoltPack archive (bad magic)";
    case BP_ERR_VERSION:           return "unsupported archive version";
    case BP_ERR_CHECKSUM:          return "checksum mismatch";
    case BP_ERR_CORRUPT:           return "archive is corrupt";
    case BP_ERR_TRUNCATED:         return "unexpected end of file";
    case BP_ERR_OVERFLOW:          return "size exceeds platform limits";
    case BP_ERR_NAME_TOO_LONG:     return "file name too long";
    case BP_ERR_UNSUPPORTED_METHOD:return "unsupported compression method";
    case BP_ERR_COMPRESS:          return "compression failed";
    case BP_ERR_DECOMPRESS:        return "decompression failed";
    case BP_ERR_ABORTED:           return "operation aborted";
    case BP_ERR_NOT_FOUND:         return "entry not found";
    case BP_ERR_INVALID_ARG:       return "invalid argument";
    default:                       return "unknown error";
    }
}

/* ---- speed / size / number formatting --------------------------------- */

static void format_speed(double bps, char *buf, size_t sz)
{
    (void)sz;
    if (bps < 1024.0)         sprintf(buf, "%.0f bytes/s", bps);
    else if (bps < 1048576.0) sprintf(buf, "%.2f KB/s", bps / 1024.0);
    else                      sprintf(buf, "%.2f MB/s", bps / 1048576.0);
}

static void format_size(bp_uint32 bytes, char *buf, size_t sz)
{
    (void)sz;
    if (bytes < 1024UL)         sprintf(buf, "%lu bytes", (unsigned long)bytes);
    else if (bytes < 1048576UL) sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1073741824UL) sprintf(buf, "%.1f MB", (double)bytes / 1048576.0);
    else                        sprintf(buf, "%.2f GB", (double)bytes / 1073741824.0);
}

static void format_size64(bp_uint64 bytes, char *buf, size_t sz)
{
    (void)sz;
    if (bytes.hi == 0) { format_size(bytes.lo, buf, sz); return; }
    { double d = (double)bytes.lo + (double)bytes.hi * 4294967296.0;
      if (d < 1048576.0)     sprintf(buf, "%.1f KB", d / 1024.0);
      else if (d < 1073741824.0) sprintf(buf, "%.1f MB", d / 1048576.0);
      else                   sprintf(buf, "%.2f GB", d / 1073741824.0); }
}

static void format_number(bp_uint32 n, char *buf, size_t sz)
{
    char tmp[32]; int len, i, j, commas;
    sprintf(tmp, "%lu", (unsigned long)n);
    len    = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > sz) { strcpy(buf, tmp); return; }
    j = len + commas; buf[j] = '\0';
    for (i = len - 1; i >= 0; i--) {
        buf[--j] = tmp[i];
        if (i > 0 && (len - i) % 3 == 0) buf[--j] = ',';
    }
}

/* ---- progress callback ------------------------------------------------ */

static int progress_cb(const bp_progress_t *p, void *ud)
{
    (void)p; (void)ud; return 0;
}

/* ---- path utilities --------------------------------------------------- */

static int ends_with_sep(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\');
}

static int path_join(char *buf, size_t sz, const char *dir, const char *name)
{
    size_t d = strlen(dir), n = strlen(name), pos = d;
    const char *slash = strrchr(dir, '/');
    const char *backslash = strrchr(dir, '\\');
    char separator_char = backslash != NULL && (slash == NULL || backslash > slash) ? '\\' : '/';
    int separator = d != 0 && !ends_with_sep(dir);
    if (d >= sz) return -1;
    if (separator) {
        if (pos >= sz - 1) return -1;
        pos++;
    }
    if (n >= sz - pos) return -1;
    memcpy(buf, dir, d);
    if (separator) buf[d] = separator_char;
    memcpy(buf + pos, name, n + 1U);
    return 0;
}

static const char *clean_path(const char *p)
{
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
    return p;
}

static void normalise_seps(char *p)
{
    for (; *p; p++) if (*p == '\\') *p = '/';
}

/* ---- parse block size ------------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (!strcmp(s,"8k")   || !strcmp(s,"8K"))   return BP_BLOCK_8K;
    if (!strcmp(s,"16k")  || !strcmp(s,"16K"))  return BP_BLOCK_16K;
    if (!strcmp(s,"32k")  || !strcmp(s,"32K"))  return BP_BLOCK_32K;
    if (!strcmp(s,"64k")  || !strcmp(s,"64K"))  return BP_BLOCK_64K;
    if (!strcmp(s,"256k") || !strcmp(s,"256K")) return BP_BLOCK_256K;
    return 0;
}

/* Convert OS/2's local FAT date/time with an explicit offset. */
static int os2_mtime(FDATE d, FTIME t, bp_uint64 *mtime)
{
    long offset;
    unsigned date, time_value;
    mtime->lo = 0;
    mtime->hi = 0;
    if (bp_dos_timezone(&offset) != 0) return -1;
    date = ((unsigned)d.year << 9) | ((unsigned)d.month << 5) |
           (unsigned)d.day;
    time_value = ((unsigned)t.hours << 11) | ((unsigned)t.minutes << 5) |
                 (unsigned)t.twosecs;
    return bp_dos_local_to_utc(date, time_value, offset, mtime);
}

/* Set OS/2 file attribute via DosSetPathInfo */
static int os2_apply_attr(const char *path, unsigned char valid,
                          unsigned char value)
{
    FILESTATUS3 fs;
    if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs)) != NO_ERROR)
        return BP_ERR_IO;
    valid = (unsigned char)(valid & OS2_ATTR_MASK);
    fs.attrFile = (ULONG)((fs.attrFile & ~(ULONG)valid) | (ULONG)(value & valid));
    return DosSetPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs), DSPI_WRTTHRU)
        == NO_ERROR ? BP_OK : BP_ERR_IO;
}

static int os2_restore_mtime(const char *path, bp_uint64 value)
{
    FILESTATUS3 fs;
    long offset;
    unsigned date, time_value;
    int converted;
    if (bp_dos_timezone(&offset) != 0) return BP_ERR_PARTIAL;
    converted = bp_dos_utc_to_local(value, offset, &date, &time_value);
    if (converted < 0) return BP_ERR_OVERFLOW;
    if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs)) != NO_ERROR)
        return BP_ERR_IO;
    fs.fdateLastWrite.year    = (USHORT)((date >> 9) & 127U);
    fs.fdateLastWrite.month   = (USHORT)((date >> 5) & 15U);
    fs.fdateLastWrite.day     = (USHORT)(date & 31U);
    fs.ftimeLastWrite.hours   = (USHORT)((time_value >> 11) & 31U);
    fs.ftimeLastWrite.minutes = (USHORT)((time_value >> 5) & 63U);
    fs.ftimeLastWrite.twosecs = (USHORT)(time_value & 31U);
    if (DosSetPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs), DSPI_WRTTHRU)
        != NO_ERROR) return BP_ERR_IO;
    return converted > 0 ? BP_ERR_PARTIAL : BP_OK;
}

/* ---- recursive add ---------------------------------------------------- */

static int add_meta_entry(bp_archive_t *ar, const char *arcname,
                          unsigned char attr)
{
    unsigned char meta_buf[OS2_META_PAYLOAD];
    bp_io_t       meta_io;
    rbuf_ctx_t    rctx;

    meta_buf[0] = attr & OS2_ATTR_MASK;
    rctx.buf  = meta_buf;
    rctx.size = OS2_META_PAYLOAD;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry_ex(ar, arcname, BP_TYPE_META, BP_OS_OS2,
                                &meta_io,
                                bp_u64_from_u32(OS2_META_PAYLOAD),
                                bp_u64_from_u32(0), 0);
}

static int add_portable_meta_entry(bp_archive_t *ar, const char *arcname,
                                   bp_uint8 subject_type,
                                   unsigned char attr)
{
    unsigned char meta_buf[BP_META_FAT_SIZE];
    bp_io_t meta_io;
    rbuf_ctx_t rctx;
    int rc;
    rc = bp_metadata_fat_encode(meta_buf, sizeof(meta_buf), subject_type,
                                BP_META_FAT_MASK,
                                (bp_uint8)(attr & BP_META_FAT_MASK));
    if (rc != BP_OK) return rc;
    rctx.buf = meta_buf;
    rctx.size = sizeof(meta_buf);
    rctx.pos = 0;
    bp_io_from_rbuf(&meta_io, &rctx);
    return bp_archive_add_entry_ex(ar, arcname, BP_TYPE_META, BP_META_PORTABLE,
                                   &meta_io,
                                   bp_u64_from_u32((bp_uint32)sizeof(meta_buf)),
                                   bp_u64_from_u32(0), 0);
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int add_directory_contents(bp_archive_t *ar, const char *dir_path,
                                  const char *arc_prefix)
{
    HDIR          hDir = HDIR_CREATE;
    FILEFINDBUF3  fb;
    ULONG         cFound = 1;
    APIRET        rc;
    char          pattern[BP_MAX_PATH];
    char          child_path[BP_MAX_PATH];
    char          child_arc[BP_MAX_PATH];
    int           brc;

    sprintf(pattern, "%s\\*", dir_path);

    rc = DosFindFirst((PSZ)pattern, &hDir,
                      FILE_NORMAL | FILE_DIRECTORY | FILE_HIDDEN | FILE_SYSTEM,
                      &fb, sizeof(fb), &cFound, FIL_STANDARD);
    while (rc == NO_ERROR) {
        if (strcmp(fb.achName, ".") != 0 && strcmp(fb.achName, "..") != 0) {
            if (path_join(child_path, sizeof(child_path), dir_path, fb.achName) != 0 ||
                path_join(child_arc, sizeof(child_arc), arc_prefix, fb.achName) != 0) {
                DosFindClose(hDir);
                return BP_ERR_NAME_TOO_LONG;
            }
            brc = add_path(ar, child_path, child_arc);
            if (brc != BP_OK) { DosFindClose(hDir); return brc; }
        }
        cFound = 1;
        rc = DosFindNext(hDir, &fb, sizeof(fb), &cFound);
    }
    DosFindClose(hDir);
    return BP_OK;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    FILESTATUS3 fs;
    APIRET      apc;
    int         rc;

    apc = DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs));
    if (apc != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (fs.attrFile & FILE_DIRECTORY) {
        /* Directory */
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 2];
        bp_uint64 mtime;
        bp_uint16 time_flags = 0;

        if (!ends_with_sep(arcname)) sprintf(dir_arc, "%s/", arcname);
        else                         strcpy(dir_arc, arcname);

        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);

        if (bp_local_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8), OS2_CP) != 0)
            return BP_ERR_INVALID_ARG;

        if (os2_mtime(fs.fdateLastWrite, fs.ftimeLastWrite, &mtime) == 0)
            time_flags = BP_ENTRY_FLAG_MTIME_VALID;
        else {
            g_unknown_source_times++;
            mtime.lo = 0; mtime.hi = 0;
        }
        rc = bp_archive_add_entry_ex(ar, dir_arc_utf8, BP_TYPE_DIR, BP_OS_OS2,
                                     NULL, bp_u64_from_u32(0), mtime, time_flags);
        if (rc != BP_OK) return rc;

        rc = add_meta_entry(ar, dir_arc_utf8, (unsigned char)fs.attrFile);
        if (rc != BP_OK) return rc;
        rc = add_portable_meta_entry(ar, dir_arc_utf8, BP_TYPE_DIR,
                                     (unsigned char)fs.attrFile);
        if (rc != BP_OK) return rc;

        return add_directory_contents(ar, path, dir_arc);
    }

    /* Regular file */
    {
        FILE     *fp;
        bp_io_t   fio;
        bp_uint64 fsize = bp_u64_from_ulong(fs.cbFile);
        bp_uint64 mtime;
        bp_uint16 time_flags = 0;
        char      arcname_utf8[BP_MAX_PATH * 2];

        if (os2_mtime(fs.fdateLastWrite, fs.ftimeLastWrite, &mtime) == 0)
            time_flags = BP_ENTRY_FLAG_MTIME_VALID;
        else {
            g_unknown_source_times++;
            mtime.lo = 0; mtime.hi = 0;
        }

        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "boltpack: cannot open: %s\n", path);
            return BP_ERR_IO;
        }
        bp_io_from_file(&fio, fp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("compressing: %s", arcname); fflush(stdout);
        }

        if (bp_local_to_utf8(arcname, arcname_utf8, sizeof(arcname_utf8), OS2_CP) != 0) {
            fclose(fp);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DATA, BP_OS_OS2,
                                     &fio, fsize, mtime, time_flags);
        fclose(fp);
        if (rc != BP_OK) return rc;

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e = &ar->entry_list[ar->entries_written - 1];
            bp_uint32 comp = bp_u64_to_u32(e->compressed_size);
            bp_uint32 fsz  = bp_u64_to_u32(fsize);
            char sbuf[32];
            if (comp == 0 || e->method == BP_METHOD_STORED || comp >= fsz) {
                format_size(fsz, sbuf, sizeof(sbuf)); printf("  %s (stored)\n", sbuf);
            } else {
                format_size(comp, sbuf, sizeof(sbuf));
                printf("  %s (%.0f%%)\n", sbuf, (double)comp / (double)fsz * 100.0);
            }
        }

        rc = add_meta_entry(ar, arcname_utf8, (unsigned char)fs.attrFile);
        if (rc != BP_OK) return rc;
        return add_portable_meta_entry(ar, arcname_utf8, BP_TYPE_DATA,
                                       (unsigned char)fs.attrFile);
    }
}

/* ---- list callback ---------------------------------------------------- */

typedef struct {
    bp_uint32 file_count;
    bp_uint32 dir_count;
    bp_uint64 total_orig;
    bp_uint64 total_comp;
} list_ctx_t;

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    list_ctx_t *ctx = (list_ctx_t *)user_data;
    const char *type;
    char ob[32], cb[32], dn[BP_MAX_PATH];

    switch (entry->entry_type) {
    case BP_TYPE_DATA:    type = "DATA"; break;
    case BP_TYPE_RSRC:    type = "RSRC"; break;
    case BP_TYPE_META:    type = "META"; break;
    case BP_TYPE_SYMLINK: type = "LINK"; break;
    case BP_TYPE_DIR:     type = "DIR "; break;
    default:              type = "??? "; break;
    }
    if (entry->entry_type == BP_TYPE_DIR) ctx->dir_count++;
    else if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_SYMLINK)
        ctx->file_count++;
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);
    if (bp_utf8_to_local(entry->name, dn, sizeof(dn), OS2_CP) != 0) strcpy(dn, "<unrepresentable>");
    format_number(bp_u64_to_u32(entry->original_size),  ob, sizeof(ob));
    format_number(bp_u64_to_u32(entry->compressed_size), cb, sizeof(cb));
    printf("%10s %10s  %s  %s\n", ob, cb, type, dn);
    return 0;
}

/* ---- extract callbacks ------------------------------------------------ */

typedef struct os2_name_node_s {
    struct os2_name_node_s *next;
    char *key;
    unsigned int length;
} os2_name_node_t;

typedef struct {
    os2_name_node_t *head;
    unsigned int count;
    unsigned long bytes;
} os2_name_set_t;

typedef struct {
    const char *out_dir;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
    bp_uint32   mtime_errors;
    bp_uint32   skip_errors;
    bp_uint32   metadata_errors;
    int         subject_ok;
    int         subject_type;
    int         subject_native_seen;
    char        subject[BP_MAX_PATH];
    os2_dir_restore_t *directories;
    os2_dir_restore_t *subject_directory;
    unsigned int directory_count;
    unsigned long directory_path_bytes;
} extract_ctx_t;

typedef struct {
    bp_io_t io;
    HFILE handle;
    int committed;
    char temporary[BP_MAX_PATH];
    char target[BP_MAX_PATH];
} os2_staged_output_t;

typedef struct {
    os2_name_set_t names;
    const char *out_dir;
    int fat_profile;
    int error;
} os2_preflight_t;

static int os2_path_join(char *out, size_t size, const char *dir,
                         const char *name)
{
    size_t d = strlen(dir), n = strlen(name), pos = d;
    int separator = d != 0 && dir[d - 1] != '/' && dir[d - 1] != '\\';
    if (pos >= size || n >= size - pos) return -1;
    if (separator) {
        if (pos + 1 >= size) return -1;
        out[pos++] = '\\';
    }
    if (n >= size - pos) return -1;
    memcpy(out, dir, d);
    if (separator) out[d] = '\\';
    memcpy(out + pos, name, n + 1);
    return 0;
}

static int os2_ensure_directory(const char *path)
{
    FILESTATUS3 fs;
#if defined(__EMX__)
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
        return 0;
    }
    if (errno != ENOENT) return -1;
#endif
    if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs)) == NO_ERROR)
        return (fs.attrFile & FILE_DIRECTORY) ? 0 : -1;
    if (DosCreateDir((PSZ)path, NULL) == NO_ERROR) return 0;
#if defined(__EMX__)
    if (lstat(path, &st) == 0)
        return !S_ISLNK(st.st_mode) && S_ISDIR(st.st_mode) ? 0 : -1;
#endif
    if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs)) != NO_ERROR)
        return -1;
    return (fs.attrFile & FILE_DIRECTORY) ? 0 : -1;
}

static int os2_mkdirs_checked(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *p;
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(tmp)) return -1;
    memcpy(tmp, path, length + 1);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (!(strlen(tmp) == 2 && tmp[1] == ':') &&
                os2_ensure_directory(tmp) != 0) {
                *p = saved;
                return -1;
            }
            *p = saved;
        }
    }
    while (length > 1 && (tmp[length - 1] == '/' || tmp[length - 1] == '\\'))
        tmp[--length] = '\0';
    if (length == 2 && tmp[1] == ':') return 0;
    return os2_ensure_directory(tmp);
}

static int os2_mkdirs_for_file_checked(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *slash, *backslash;
    size_t length = strlen(path);
    if (length >= sizeof(tmp)) return -1;
    memcpy(tmp, path, length + 1U);
    slash = strrchr(tmp, '/');
    backslash = strrchr(tmp, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL || slash == tmp) return 0;
    *slash = '\0';
    return os2_mkdirs_checked(tmp);
}

static int os2_check_local_name(char *local, int is_directory,
                                int fat_profile)
{
    size_t length = strlen(local), i, start = 0;
    if (is_directory && length > 0 && local[length - 1] == '/') local[--length] = '\0';
    if (length == 0 || length >= BP_MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    for (i = 0; i <= length; i++) {
        unsigned char c = (unsigned char)local[i];
        if (i == length || c == '/') {
            size_t component = i - start, j, base = 0, ext = 0;
            char short_base[9];
            int in_ext = 0;
            memset(short_base, 0, sizeof(short_base));
            if (component == 0) return BP_ERR_INVALID_ARG;
            if ((component == 1 && local[start] == '.') ||
                (component == 2 && local[start] == '.' && local[start + 1] == '.'))
                return BP_ERR_INVALID_ARG;
            if (local[i - 1] == '.' || local[i - 1] == ' ')
                return BP_ERR_INVALID_ARG;
            for (j = start; j < i; j++) {
                c = (unsigned char)local[j];
                if (c < 0x20U || c == ':' || c == '\\' || c == '*' ||
                    c == '?' || c == '<' || c == '>' || c == '"' || c == '|')
                    return BP_ERR_INVALID_ARG;
                if (fat_profile && (c == ' ' || c == '+' || c == ',' ||
                    c == ';' || c == '=' || c == '[' || c == ']'))
                    return BP_ERR_INVALID_ARG;
                if (c == '.') {
                    if (in_ext) return BP_ERR_INVALID_ARG;
                    in_ext = 1;
                } else if (in_ext) ext++;
                else {
                    if (base < sizeof(short_base) - 1U)
                        short_base[base] = (char)(c >= 'a' && c <= 'z' ?
                                                  c - 'a' + 'A' : c);
                    base++;
                }
            }
            if (fat_profile) {
                int reserved = 0;
                if (base == 0 || base > 8U || ext > 3U) return BP_ERR_NAME_TOO_LONG;
                if ((base == 3U && (!memcmp(short_base, "CON", 3) ||
                                    !memcmp(short_base, "PRN", 3) ||
                                    !memcmp(short_base, "AUX", 3) ||
                                    !memcmp(short_base, "NUL", 3))) ||
                    (base == 4U && short_base[3] >= '1' && short_base[3] <= '9' &&
                     (!memcmp(short_base, "COM", 3) ||
                      !memcmp(short_base, "LPT", 3)))) reserved = 1;
                if (reserved) return BP_ERR_INVALID_ARG;
            }
            start = i + 1;
        }
    }
    return BP_OK;
}

static int os2_query_fat_profile(const char *out_dir, int *is_fat)
{
    char drive[3];
    char buffer[512];
    PFSQBUFFER2 query = (PFSQBUFFER2)buffer;
    char *fsd;
    ULONG length = sizeof(buffer), disk = 0, map = 0;
    APIRET rc;
    if (out_dir != NULL && out_dir[0] == '\\' && out_dir[1] == '\\') return -1;
    if (out_dir != NULL && out_dir[0] != '\0' && out_dir[1] == ':') {
        drive[0] = out_dir[0]; drive[1] = ':'; drive[2] = '\0';
    } else {
        rc = DosQueryCurrentDisk(&disk, &map);
        if (rc != NO_ERROR || disk < 1 || disk > 26) return -1;
        drive[0] = (char)('A' + disk - 1); drive[1] = ':'; drive[2] = '\0';
    }
    memset(buffer, 0, sizeof(buffer));
    rc = DosQueryFSAttach((PSZ)drive, 0, FSAIL_QUERYNAME, query, &length);
    if (rc != NO_ERROR) return -1;
    fsd = (char *)(query->szName + query->cbName + 1);
    *is_fat = strncmp(fsd, "FAT", 3) == 0;
    return 0;
}

static int os2_name_set_add(os2_name_set_t *set, char *key, unsigned int length)
{
    os2_name_node_t *node;
    if (set->count >= OS2_NAME_SET_MAX ||
        (unsigned long)length + 1UL > OS2_NAME_SET_BYTES_MAX - set->bytes)
        return BP_ERR_OVERFLOW;
    for (node = set->head; node != NULL; node = node->next)
        if (node->length == length && memcmp(node->key, key, length) == 0)
            return BP_ERR_CORRUPT;
    node = (os2_name_node_t *)malloc(sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->key = (char *)malloc((size_t)length + 1U);
    if (node->key == NULL) { free(node); return BP_ERR_NOMEM; }
    memcpy(node->key, key, length + 1U);
    node->length = length;
    node->next = set->head;
    set->head = node;
    set->count++;
    set->bytes += (unsigned long)length + 1UL;
    return BP_OK;
}

static void os2_name_set_free(os2_name_set_t *set)
{
    os2_name_node_t *node = set->head;
    while (node != NULL) {
        os2_name_node_t *next = node->next;
        free(node->key); free(node); node = next;
    }
    set->head = NULL; set->count = 0; set->bytes = 0;
}

static int os2_preflight_entry(const bp_entry_t *entry, void *user_data)
{
    os2_preflight_t *preflight = (os2_preflight_t *)user_data;
    char local[BP_MAX_PATH];
    char full_path[BP_MAX_PATH];
    COUNTRYCODE country;
    size_t length, i;
    unsigned int key_length;
    int rc;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    if (!bp_name_is_safe(entry->name) ||
        bp_utf8_to_local(entry->name, local, sizeof(local), OS2_CP) != 0) {
        preflight->error = BP_ERR_PARTIAL;
        return 1;
    }
    rc = os2_check_local_name(local, entry->entry_type == BP_TYPE_DIR,
                              preflight->fat_profile);
    if (rc != BP_OK) { preflight->error = rc; return 1; }
    if (preflight->out_dir != NULL &&
        os2_path_join(full_path, sizeof(full_path), preflight->out_dir,
                      local) != 0) {
        preflight->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }
    length = strlen(local);
    for (i = 0; i < length; i++) if (local[i] == '/') local[i] = '\\';
    country.country = 0;
    country.codepage = OS2_CP;
    if (DosMapCase((ULONG)length, &country, local) != NO_ERROR) {
        preflight->error = BP_ERR_PARTIAL;
        return 1;
    }
    key_length = (unsigned int)length;
    rc = os2_name_set_add(&preflight->names, local, key_length);
    if (rc != BP_OK) { preflight->error = rc; return 1; }
    return 0;
}

static int os2_make_path(const extract_ctx_t *ctx, const char *archive_name,
                         int is_directory, char *local, char *path)
{
    size_t out_len;
    if (!bp_name_is_safe(archive_name) ||
        bp_utf8_to_local(archive_name, local, BP_MAX_PATH, OS2_CP) != 0)
        return BP_ERR_PARTIAL;
    if (os2_check_local_name(local, is_directory, 0) != BP_OK)
        return BP_ERR_INVALID_ARG;
    if (ctx->out_dir == NULL) {
        if (strlen(local) >= BP_MAX_PATH) return BP_ERR_NAME_TOO_LONG;
        strcpy(path, local);
        return BP_OK;
    }
    out_len = strlen(ctx->out_dir) + strlen(local) + 2U;
    if (out_len > BP_MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    return os2_path_join(path, BP_MAX_PATH, ctx->out_dir, local) == 0 ?
           BP_OK : BP_ERR_NAME_TOO_LONG;
}

static int os2_make_temp_path(const char *target, char *temporary, size_t size)
{
    static unsigned long sequence = 0;
    const char *slash = strrchr(target, '\\');
    const char *forward = strrchr(target, '/');
    size_t prefix;
    unsigned int attempt;
    if (forward != NULL && (slash == NULL || forward > slash)) slash = forward;
    prefix = slash != NULL ? (size_t)(slash - target + 1) : 0;
    if (prefix + 13U >= size) return -1;
    for (attempt = 0; attempt < 128U; attempt++) {
        HFILE handle;
        ULONG action = 0;
        ULONG id = sequence++ & 0xFFFFFFUL;
        APIRET rc;
        memcpy(temporary, target, prefix);
        sprintf(temporary + prefix, "BP%06lX.TMP", id);
        rc = DosOpen((PSZ)temporary, &handle, &action, 0, FILE_NORMAL,
                     OPEN_ACTION_CREATE_IF_NEW | OPEN_ACTION_FAIL_IF_EXISTS,
                     OPEN_ACCESS_WRITEONLY | OPEN_SHARE_DENYWRITE |
                     OPEN_FLAGS_SEQUENTIAL, NULL);
        if (rc == NO_ERROR) return (int)handle;
    }
    return -1;
}

static long os2_staged_write(void *handle, const void *data, bp_uint32 count)
{
    os2_staged_output_t *output = (os2_staged_output_t *)handle;
    ULONG written = 0;
    APIRET rc = DosWrite(output->handle, (PVOID)data, (ULONG)count, &written);
    return rc == NO_ERROR ? (long)written : -1L;
}

static void os2_discard_staged(os2_staged_output_t *output)
{
    if (output->handle != (HFILE)-1) {
        (void)DosClose(output->handle);
        output->handle = (HFILE)-1;
    }
    if (output->temporary[0] != '\0') {
        (void)DosDelete((PSZ)output->temporary);
        output->temporary[0] = '\0';
    }
}

static int os2_restore_backup(const char *backup, const char *target)
{
    FILESTATUS3 fs;
    if (DosQueryPathInfo((PSZ)target, FIL_STANDARD, &fs, sizeof(fs)) == NO_ERROR)
        return -1;
    return DosMove((PSZ)backup, (PSZ)target) == NO_ERROR ? 0 : -1;
}

static int os2_publish_archive(const char *temporary, const char *target)
{
    static unsigned long sequence = 0;
    FILESTATUS3 status;
    char backup[BP_MAX_PATH];
    const char *slash = strrchr(target, '\\');
    const char *forward = strrchr(target, '/');
    size_t prefix;
    unsigned int attempt;
    int has_backup = 0;
    APIRET rc;

    if (forward != NULL && (slash == NULL || forward > slash)) slash = forward;
    prefix = slash != NULL ? (size_t)(slash - target + 1) : 0;
    if (prefix + 13U >= sizeof(backup)) return BP_ERR_NAME_TOO_LONG;

    rc = DosQueryPathInfo((PSZ)target, FIL_STANDARD, &status, sizeof(status));
    if (rc == NO_ERROR) {
        if (status.attrFile & FILE_DIRECTORY) return BP_ERR_IO;
    } else if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) {
        return DosMove((PSZ)temporary, (PSZ)target) == NO_ERROR ?
               BP_OK : BP_ERR_IO;
    } else {
        return BP_ERR_IO;
    }

    for (attempt = 0; attempt < 128U; attempt++) {
        ULONG id = sequence++ & 0xFFFFFFUL;
        memcpy(backup, target, prefix);
        sprintf(backup + prefix, "BO%06lX.BAK", id);
        rc = DosQueryPathInfo((PSZ)backup, FIL_STANDARD, &status,
                              sizeof(status));
        if (rc == NO_ERROR) continue;
        if (rc != ERROR_FILE_NOT_FOUND && rc != ERROR_PATH_NOT_FOUND)
            return BP_ERR_IO;
        rc = DosMove((PSZ)target, (PSZ)backup);
        if (rc == NO_ERROR) { has_backup = 1; break; }
    }
    if (!has_backup) return BP_ERR_IO;

    rc = DosMove((PSZ)temporary, (PSZ)target);
    if (rc != NO_ERROR) {
        if (os2_restore_backup(backup, target) != 0)
            fprintf(stderr,
                    "boltpack: original archive remains at recovery path: %s\n",
                    backup);
        return BP_ERR_IO;
    }
    if (DosDelete((PSZ)backup) != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot remove replaced-archive backup: %s\n",
                backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

static int os2_archive_status_same(const FILESTATUS3 *left,
                                   const FILESTATUS3 *right)
{
    return left->cbFile == right->cbFile &&
           left->cbFileAlloc == right->cbFileAlloc &&
           left->attrFile == right->attrFile &&
           left->fdateCreation.year == right->fdateCreation.year &&
           left->fdateCreation.month == right->fdateCreation.month &&
           left->fdateCreation.day == right->fdateCreation.day &&
           left->ftimeCreation.hours == right->ftimeCreation.hours &&
           left->ftimeCreation.minutes == right->ftimeCreation.minutes &&
           left->ftimeCreation.twosecs == right->ftimeCreation.twosecs &&
           left->fdateLastWrite.year == right->fdateLastWrite.year &&
           left->fdateLastWrite.month == right->fdateLastWrite.month &&
           left->fdateLastWrite.day == right->fdateLastWrite.day &&
           left->ftimeLastWrite.hours == right->ftimeLastWrite.hours &&
           left->ftimeLastWrite.minutes == right->ftimeLastWrite.minutes &&
           left->ftimeLastWrite.twosecs == right->ftimeLastWrite.twosecs;
}

static int os2_copy_archive_attributes(const char *temporary,
                                       const FILESTATUS3 *original)
{
    FILESTATUS3 staged;

    if (DosQueryPathInfo((PSZ)temporary, FIL_STANDARD, &staged,
                         sizeof(staged)) != NO_ERROR)
        return -1;
    staged.attrFile = (staged.attrFile & ~(ULONG)OS2_ATTR_MASK) |
                      (original->attrFile & (ULONG)OS2_ATTR_MASK);
    staged.fdateCreation = original->fdateCreation;
    staged.ftimeCreation = original->ftimeCreation;
    staged.fdateLastAccess = original->fdateLastAccess;
    staged.ftimeLastAccess = original->ftimeLastAccess;
    return DosSetPathInfo((PSZ)temporary, FIL_STANDARD, &staged,
                          sizeof(staged), DSPI_WRTTHRU) == NO_ERROR ? 0 : -1;
}

static int os2_commit_staged(os2_staged_output_t *output, int force)
{
    static unsigned long sequence = 0;
    FILESTATUS3 fs;
    char backup[BP_MAX_PATH];
    int has_backup = 0;
    unsigned int attempt;
    APIRET rc;
    HFILE source = (HFILE)-1, destination = (HFILE)-1;
    ULONG action = 0;
    unsigned char buffer[8192];
    int copy_error = 0;
    if (DosQueryPathInfo((PSZ)output->target, FIL_STANDARD, &fs, sizeof(fs))
        == NO_ERROR) {
        if (fs.attrFile & FILE_DIRECTORY) return BP_ERR_PARTIAL;
        if (!force) return BP_ERR_PARTIAL;
        for (attempt = 0; attempt < 128U; attempt++) {
            const char *slash = strrchr(output->target, '\\');
            const char *forward = strrchr(output->target, '/');
            size_t prefix;
            if (forward != NULL && (slash == NULL || forward > slash)) slash = forward;
            prefix = slash != NULL ? (size_t)(slash - output->target + 1) : 0;
            if (prefix + 13U >= sizeof(backup)) return BP_ERR_NAME_TOO_LONG;
            memcpy(backup, output->target, prefix);
            sprintf(backup + prefix, "BO%06lX.BAK", sequence++ & 0xFFFFFFUL);
            if (DosQueryPathInfo((PSZ)backup, FIL_STANDARD, &fs, sizeof(fs)) == NO_ERROR)
                continue;
            rc = DosMove((PSZ)output->target, (PSZ)backup);
            if (rc == NO_ERROR) { has_backup = 1; break; }
        }
        if (!has_backup) return BP_ERR_IO;
    }

    /* Reserve the final name without replacing a file created by another
     * process after preflight. Payload bytes have already passed the archive
     * checksum before this copy begins. */
    rc = DosOpen((PSZ)output->target, &destination, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_CREATE_IF_NEW | OPEN_ACTION_FAIL_IF_EXISTS,
                 OPEN_ACCESS_WRITEONLY | OPEN_SHARE_DENYREADWRITE |
                 OPEN_FLAGS_SEQUENTIAL, NULL);
    if (rc != NO_ERROR) {
        if (has_backup && os2_restore_backup(backup, output->target) != 0)
            fprintf(stderr, "boltpack: original file remains at recovery path: %s\n", backup);
        return force ? BP_ERR_IO : BP_ERR_PARTIAL;
    }
    rc = DosOpen((PSZ)output->temporary, &source, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                 OPEN_ACCESS_READONLY | OPEN_SHARE_DENYNONE |
                 OPEN_FLAGS_SEQUENTIAL, NULL);
    if (rc != NO_ERROR) copy_error = 1;
    while (!copy_error) {
        ULONG read_count = 0, written_total = 0;
        rc = DosRead(source, buffer, sizeof(buffer), &read_count);
        if (rc != NO_ERROR) { copy_error = 1; break; }
        if (read_count == 0) break;
        while (written_total < read_count) {
            ULONG written = 0;
            rc = DosWrite(destination, buffer + written_total,
                          read_count - written_total, &written);
            if (rc != NO_ERROR || written == 0) { copy_error = 1; break; }
            written_total += written;
        }
    }
    if (!copy_error && DosResetBuffer(destination) != NO_ERROR) copy_error = 1;
    if (source != (HFILE)-1 && DosClose(source) != NO_ERROR) copy_error = 1;
    if (DosClose(destination) != NO_ERROR) copy_error = 1;
    if (copy_error) {
        (void)DosDelete((PSZ)output->target);
        if (has_backup && os2_restore_backup(backup, output->target) != 0)
            fprintf(stderr, "boltpack: original file remains at recovery path: %s\n", backup);
        return BP_ERR_IO;
    }
    output->committed = 1;
    if (DosDelete((PSZ)output->temporary) != NO_ERROR) {
        return BP_ERR_PARTIAL;
    }
    output->temporary[0] = '\0';
    if (has_backup && DosDelete((PSZ)backup) != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot remove replaced-file backup: %s\n", backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

static os2_dir_restore_t *os2_queue_directory(extract_ctx_t *ctx,
                                               const char *path,
                                               const bp_entry_t *entry)
{
    os2_dir_restore_t *node;
    size_t length = strlen(path) + 1U;
    if (ctx->directory_count >= OS2_DIR_QUEUE_MAX ||
        length > OS2_DIR_QUEUE_BYTES_MAX - ctx->directory_path_bytes) {
        ctx->metadata_errors++;
        return NULL;
    }
    node = (os2_dir_restore_t *)calloc(1, sizeof(*node));
    if (node == NULL) { ctx->metadata_errors++; return NULL; }
    node->path = (char *)malloc(length);
    if (node->path == NULL) { free(node); ctx->metadata_errors++; return NULL; }
    memcpy(node->path, path, length);
    node->mtime = entry->mtime;
    node->flags = entry->flags;
    node->next = ctx->directories;
    ctx->directories = node;
    ctx->directory_count++;
    ctx->directory_path_bytes += (unsigned long)length;
    return node;
}

static int os2_path_depth(const char *path)
{
    int depth = 0;
    for (; *path; path++) if (*path == '/' || *path == '\\') depth++;
    return depth;
}

static void os2_finish_directories(extract_ctx_t *ctx)
{
    while (ctx->directories != NULL) {
        os2_dir_restore_t **best = &ctx->directories, **link;
        os2_dir_restore_t *node;
        int depth = -1;
        for (link = &ctx->directories; *link != NULL; link = &(*link)->next) {
            int d = os2_path_depth((*link)->path);
            if (d > depth) { depth = d; best = link; }
        }
        node = *best; *best = node->next;
        if (!ctx->dry_run && (node->flags & BP_ENTRY_FLAG_MTIME_VALID)) {
            int rc = os2_restore_mtime(node->path, node->mtime);
            if (rc != BP_OK) ctx->mtime_errors++;
        }
        if (!ctx->dry_run && node->native_seen) {
            if (os2_apply_attr(node->path, OS2_ATTR_MASK, node->native_attr) != BP_OK)
                ctx->metadata_errors++;
        } else if (!ctx->dry_run && node->fat_seen && node->fat_valid != 0) {
            if (os2_apply_attr(node->path, node->fat_valid, node->fat_value) != BP_OK)
                ctx->metadata_errors++;
        }
        if (ctx->directory_count != 0) ctx->directory_count--;
        if (ctx->directory_path_bytes >= (unsigned long)strlen(node->path) + 1UL)
            ctx->directory_path_bytes -= (unsigned long)strlen(node->path) + 1UL;
        free(node->path); free(node);
    }
}

static int os2_preflight_archive(FILE *fp, const char *out_dir)
{
    bp_io_t io;
    os2_preflight_t preflight;
    int rc;
    memset(&preflight, 0, sizeof(preflight));
    preflight.out_dir = out_dir;
    if (os2_query_fat_profile(out_dir, &preflight.fat_profile) != 0)
        return BP_ERR_PARTIAL;
    bp_io_from_file(&io, fp);
    rc = bp_archive_list(&io, os2_preflight_entry, &preflight);
    os2_name_set_free(&preflight.names);
    if (preflight.error != 0) return preflight.error;
    return rc;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH], local_name[BP_MAX_PATH];
    meta_extract_t *mex;
    bp_uint32 meta_sz;
    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR ||
        entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->subject_ok = 0; ctx->subject_type = entry->entry_type;
        ctx->subject_native_seen = 0; ctx->subject_directory = NULL;
    }
    if (entry->entry_type == BP_TYPE_META) {
        if (!ctx->subject_ok || strcmp(entry->name, ctx->subject) != 0) {
            ctx->metadata_errors++;
            return NULL;
        }
        meta_sz = bp_u64_to_u32(entry->original_size);
        if ((entry->platform == BP_OS_OS2 && meta_sz != OS2_META_PAYLOAD) ||
            (entry->platform == BP_META_PORTABLE &&
             (meta_sz < 8U || meta_sz > BP_META_MAX_SIZE)) ||
            (entry->platform != BP_OS_OS2 && entry->platform != BP_META_PORTABLE)) {
            ctx->metadata_errors++;
            return NULL;
        }
        mex = (meta_extract_t *)malloc(sizeof(*mex));
        if (mex == NULL) { ctx->metadata_errors++; return NULL; }
        mex->wctx.buf = mex->buf; mex->wctx.cap = sizeof(mex->buf);
        mex->wctx.len = 0; mex->wctx.pos = 0;
        mex->io.read = wbuf_read_fn; mex->io.write = wbuf_write_fn;
        mex->io.seek = wbuf_seek_fn; mex->io.tell = wbuf_tell_fn;
        mex->io.handle = &mex->wctx;
        return &mex->io;
    }
    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->skip_errors++;
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [not supported on OS/2]\n", entry->name);
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_DIR) {
        os2_dir_restore_t *directory = NULL;
        ctx->dir_count++;
        if (os2_make_path(ctx, entry->name, 1, local_name, path) != BP_OK) {
            ctx->skip_errors++; return NULL;
        }
        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", local_name);
        if (!ctx->dry_run) {
            if (os2_mkdirs_checked(path) != 0) { ctx->skip_errors++; return NULL; }
            directory = os2_queue_directory(ctx, path, entry);
            if (directory == NULL) { ctx->skip_errors++; return NULL; }
        }
        if (strlen(entry->name) < sizeof(ctx->subject)) {
            strcpy(ctx->subject, entry->name); ctx->subject_ok = 1;
            ctx->subject_directory = directory;
        }
        return NULL;
    }
    if (entry->entry_type != BP_TYPE_DATA) { ctx->skip_errors++; return NULL; }
    ctx->file_count++;
    if (os2_make_path(ctx, entry->name, 0, local_name, path) != BP_OK) {
        ctx->skip_errors++; return NULL;
    }
    if (g_verbosity == VERB_DEFAULT) printf(" extracting: %s\n", local_name);
    if (ctx->dry_run) {
        if (strlen(entry->name) < sizeof(ctx->subject)) {
            strcpy(ctx->subject, entry->name); ctx->subject_ok = 1;
        }
        return bp_archive_dry_run_stream();
    }
    {
        FILESTATUS3 fs;
        os2_staged_output_t *output;
        int handle;
        if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs)) == NO_ERROR) {
            if (!ctx->force || (fs.attrFile & FILE_DIRECTORY)) {
                ctx->skip_errors++; return NULL;
            }
        }
        if (os2_mkdirs_for_file_checked(path) != 0) {
            ctx->skip_errors++; return NULL;
        }
        output = (os2_staged_output_t *)calloc(1, sizeof(*output));
        if (output == NULL) { ctx->skip_errors++; return NULL; }
        if (strlen(path) >= sizeof(output->target)) { free(output); ctx->skip_errors++; return NULL; }
        strcpy(output->target, path);
        output->handle = (HFILE)-1;
        handle = os2_make_temp_path(path, output->temporary, sizeof(output->temporary));
        if (handle < 0) { free(output); ctx->skip_errors++; return NULL; }
        output->handle = (HFILE)handle;
        output->io.read = NULL; output->io.write = os2_staged_write;
        output->io.seek = NULL; output->io.tell = NULL; output->io.handle = output;
        return &output->io;
    }
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;
        char path[BP_MAX_PATH], local_name[BP_MAX_PATH];
        int rc = BP_OK;
        if (status != BP_OK) { free(mex); return BP_OK; }
        if (!ctx->subject_ok || strcmp(entry->name, ctx->subject) != 0 ||
            os2_make_path(ctx, entry->name, ctx->subject_type == BP_TYPE_DIR,
                          local_name, path) != BP_OK) {
            ctx->metadata_errors++; free(mex); return BP_ERR_PARTIAL;
        }
        if (entry->platform == BP_OS_OS2 && mex->wctx.len == OS2_META_PAYLOAD) {
            ctx->subject_native_seen = 1;
            if (!ctx->dry_run && ctx->subject_type == BP_TYPE_DIR &&
                ctx->subject_directory != NULL) {
                ctx->subject_directory->native_seen = 1;
                ctx->subject_directory->native_attr = mex->buf[0];
            } else if (!ctx->dry_run && ctx->subject_type == BP_TYPE_DATA &&
                       os2_apply_attr(path, OS2_ATTR_MASK, mex->buf[0]) != BP_OK) {
                ctx->metadata_errors++;
            }
        } else if (entry->platform == BP_META_PORTABLE) {
            bp_portable_metadata_t parsed;
            rc = bp_metadata_parse(mex->buf, (unsigned)mex->wctx.len, &parsed);
            if (rc == BP_ERR_CORRUPT) { free(mex); return rc; }
            if (parsed.has_fat && !ctx->subject_native_seen) {
                if (!ctx->dry_run && ctx->subject_type == BP_TYPE_DIR &&
                    ctx->subject_directory != NULL) {
                    ctx->subject_directory->fat_seen = 1;
                    ctx->subject_directory->fat_valid = parsed.fat_valid;
                    ctx->subject_directory->fat_value = parsed.fat_value;
                } else if (!ctx->dry_run && ctx->subject_type == BP_TYPE_DATA &&
                           parsed.fat_valid != 0 &&
                           os2_apply_attr(path, parsed.fat_valid,
                                          parsed.fat_value) != BP_OK) {
                    ctx->metadata_errors++;
                }
            }
            if (rc == BP_ERR_PARTIAL) {
                ctx->metadata_errors++; free(mex); return BP_ERR_PARTIAL;
            }
        } else {
            ctx->metadata_errors++; rc = BP_ERR_PARTIAL;
        }
        free(mex);
        return rc;
    }
    {
        os2_staged_output_t *output = (os2_staged_output_t *)out->handle;
        int rc = BP_OK;
        if (DosClose(output->handle) != NO_ERROR) rc = BP_ERR_IO;
        output->handle = (HFILE)-1;
        if (status != BP_OK || rc != BP_OK) {
            if (status == BP_OK && rc != BP_OK) ctx->skip_errors++;
            os2_discard_staged(output); free(output); return BP_OK;
        }
        rc = os2_commit_staged(output, ctx->force);
        if (!output->committed) {
            ctx->skip_errors++;
            os2_discard_staged(output); free(output);
            return rc == BP_ERR_PARTIAL ? BP_ERR_PARTIAL : BP_OK;
        }
        if (rc != BP_OK) ctx->skip_errors++;
        if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
            int time_rc = os2_restore_mtime(output->target, entry->mtime);
            if (time_rc != BP_OK) ctx->mtime_errors++;
        }
        if (strlen(entry->name) < sizeof(ctx->subject)) {
            strcpy(ctx->subject, entry->name); ctx->subject_ok = 1;
            ctx->subject_type = entry->entry_type; ctx->subject_native_seen = 0;
        } else { ctx->subject_ok = 0; ctx->metadata_errors++; }
        os2_discard_staged(output);
        free(output);
        if (rc == BP_ERR_PARTIAL) return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

/* ---- test callback ---------------------------------------------------- */

typedef struct { bp_uint32 ok; bp_uint32 fail; } test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    if (g_verbosity < VERB_SILENT) {
        char dn[BP_MAX_PATH];
        if (bp_utf8_to_local(entry->name, dn, sizeof(dn), OS2_CP) != 0)
            strncpy(dn, entry->name, sizeof(dn) - 1);
        printf("  testing: %s %s\n", dn,
               result == BP_OK ? "OK" : bp_strerror(result));
    }
    if (result == BP_OK) ctx->ok++; else ctx->fail++;
}

/* ---- usage ------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver (OS/2)\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.blt [files...]\n\n"
        "Commands:\n"
        "  c   Create archive from files\n"
        "  a   Add files to existing archive\n"
        "  x   Extract archive\n"
        "  l   List archive contents\n"
        "  t   Test archive integrity\n"
        "  i   Show archive information\n"
        "  d   Delete files from archive\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr,
        "\nOptions:\n"
        "  -b SIZE   Block size: 8k, 16k, 32k, 64k, 256k (default: 32k)\n"
        "  --interchange-profile-1  Mini-compatible limits for create/add\n"
        "  -0        Store only (no compression)\n"
        "  -F        Use fluff LZ77+Huffman compression (better ratio than boltlz)\n"
        "  -q        Quiet (summary only)\n"
        "  -qq       Silent (errors only)\n"
        "  -o PATH   Output directory for extraction\n"
        "  -f        Force overwrite existing files\n"
        "  -n        Dry run\n");
}

/* ---- os_name / cpu_name ----------------------------------------------- */

static const char *os_name(bp_uint8 os)
{
    switch (os) {
    case BP_OS_UNKNOWN:   return "Unknown";
    case BP_OS_LINUX:     return "Linux";
    case BP_OS_MACOS9:    return "Mac OS 9";
    case BP_OS_MACOSX:    return "Mac OS X";
    case BP_OS_AMIGA_68K: return "AmigaOS (68k)";
    case BP_OS_AMIGA_PPC: return "AmigaOS (PPC)";
    case BP_OS_MORPHOS:   return "MorphOS";
    case BP_OS_AROS:      return "AROS";
    case BP_OS_DOS:       return "MS-DOS";
    case BP_OS_WINDOWS:   return "Windows";
    case BP_OS_FREEBSD:   return "FreeBSD";
    case BP_OS_NETBSD:    return "NetBSD";
    case BP_OS_OPENBSD:   return "OpenBSD";
    case BP_OS_SOLARIS:   return "Solaris";
    case BP_OS_IRIX:      return "IRIX";
    case BP_OS_HPUX:      return "HP-UX";
    case BP_OS_AIX:       return "AIX";
    case BP_OS_ATARI:     return "Atari TOS/MiNT";
    case BP_OS_RISCOS:    return "RISC OS";
    case BP_OS_OS2:       return "OS/2";
    case BP_OS_BEOS:      return "BeOS/Haiku";
    case BP_OS_OPENVMS:   return "OpenVMS";
    case BP_OS_ZOS:       return "z/OS";
    case BP_OS_WASM:      return "WebAssembly";
    case BP_OS_NEXTSTEP:  return "NeXTSTEP/OPENSTEP";
    case BP_OS_QNX:       return "QNX Neutrino";
    case BP_OS_PLAN9:     return "Plan 9";
    case BP_OS_TRU64:     return "Tru64 UNIX";
    case BP_OS_SCO:       return "SCO OpenServer";
    case BP_OS_CPM86:     return "CP/M-86";
    case BP_OS_QL:        return "Sinclair QL";
    case BP_OS_WIN16:     return "Windows 3.x (Win16)";
    case BP_OS_HURD:      return "GNU Hurd";
    case BP_OS_X68K:      return "Sharp X68000";
    default:              return "Unknown";
    }
}

static const char *cpu_name(bp_uint8 cpu)
{
    switch (cpu) {
    case BP_CPU_UNKNOWN: return "Unknown";
    case BP_CPU_X86:     return "x86";
    case BP_CPU_X86_64:  return "x86-64";
    case BP_CPU_PPC32:   return "PowerPC";
    case BP_CPU_PPC64:   return "PowerPC 64";
    case BP_CPU_M68K:    return "Motorola 68k";
    case BP_CPU_ARM32:   return "ARM";
    case BP_CPU_ARM64:   return "ARM64";
    case BP_CPU_MIPS32:  return "MIPS";
    case BP_CPU_MIPS64:  return "MIPS64";
    case BP_CPU_SPARC:   return "SPARC";
    case BP_CPU_ALPHA:   return "Alpha";
    case BP_CPU_PARISC:  return "PA-RISC";
    case BP_CPU_RISCV32: return "RISC-V 32";
    case BP_CPU_RISCV64: return "RISC-V 64";
    case BP_CPU_SH:      return "SuperH";
    case BP_CPU_6502:    return "6502";
    case BP_CPU_Z80:     return "Z80";
    case BP_CPU_8086:    return "8086/80286+";
    case BP_CPU_E2K:     return "Elbrus (e2k)";
    case BP_CPU_ZARCH:   return "z/Architecture";
    case BP_CPU_SPARC64: return "SPARC64";
    case BP_CPU_IA64:    return "Itanium (IA-64)";
    default:             return "Unknown";
    }
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[4] = {"Stored", "BoltLite", "boltlz", "Fluff"};
    int pos = 0;
    int first = 1;
    int i;
    buf[0] = '\0';
    for (i = 0; i < 4; i++) {
        if (counts[i] == 0) continue;
        if (!first) { buf[pos++] = ','; buf[pos++] = ' '; }
        pos += sprintf(buf + pos, "%s (%lu)", names[i], (unsigned long)counts[i]);
        first = 0;
    }
    if (first) strcpy(buf, "none");
    return buf;
}

static int cmd_info(const char *archive_name)
{
    FILE *fp; bp_io_t aio; bp_archive_info_t info; int rc;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_info(&aio, &info);
    fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    {
        bp_uint32 fc = info.entry_count - info.dir_count;
        char ob[32], cb[32], ab[32];
        format_size64(info.total_original,   ob, sizeof(ob));
        format_size64(info.total_compressed, cb, sizeof(cb));
        format_size64(info.archive_size,     ab, sizeof(ab));
        printf("Archive:        %s\n",  archive_name);
        printf("Format version: %u\n",  (unsigned)info.version);
        printf("Created by:     BoltPack %u.%u\n",
               (unsigned)info.creator_ver_major, (unsigned)info.creator_ver_minor);
        printf("Creator OS:     %s\n",  os_name(info.creator_os));
        printf("Creator CPU:    %s\n",  cpu_name(info.creator_cpu));
        printf("Method:         %s\n", format_methods(info.method_counts));
        printf("Archive size:   %s\n",  ab);
        printf("Entries:        %lu (%lu file%s, %lu director%s)\n",
               (unsigned long)info.entry_count,
               (unsigned long)fc, fc == 1 ? "" : "s",
               (unsigned long)info.dir_count, info.dir_count == 1 ? "y" : "ies");
        printf("Original size:  %s\n",  ob);
        printf("Compressed:     %s", cb);
        if (!bp_u64_is_zero(info.total_original)) {
            double od = (double)info.total_original.lo
                      + (double)info.total_original.hi * 4294967296.0;
            double cd = (double)info.total_compressed.lo
                      + (double)info.total_compressed.hi * 4294967296.0;
            printf(" (%.0f%%)", od > 0.0 ? cd / od * 100.0 : 0.0);
        }
        printf("\n");
    }
    return 0;
}

/* ---- cmd_list --------------------------------------------------------- */

static int cmd_list(const char *archive_name)
{
    FILE *fp; bp_io_t aio; list_ctx_t ctx; int rc;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    printf("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");
    rc = bp_archive_list(&aio, list_cb, &ctx);
    fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    {
        char ob[32], cb[32];
        format_size64(ctx.total_orig, ob, sizeof(ob));
        format_size64(ctx.total_comp, cb, sizeof(cb));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)ctx.file_count, ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,  ctx.dir_count  == 1 ? "y" : "ies", ob);
        if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
            double od = (double)ctx.total_orig.lo + (double)ctx.total_orig.hi * 4294967296.0;
            double cd = (double)ctx.total_comp.lo + (double)ctx.total_comp.hi * 4294967296.0;
            printf(", %.0f%% (%s)", od > 0.0 ? cd / od * 100.0 : 0.0, cb);
        }
        printf("\n");
    }
    return 0;
}

/* ---- cmd_create ------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int interchange_profile)
{
    bp_io_t aio;
    bp_archive_t ar;
    os2_archive_io_t stage_file;
    int stage_handle;
    int rc, i;
    clock_t start;
    char temporary[BP_MAX_PATH];
    bp_uint64 total_orig, total_comp;
    bp_uint32 file_count, dir_count;

    if (argc < 2) { fprintf(stderr, "boltpack: create requires archive and files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    g_unknown_source_times = 0;
    temporary[0] = '\0';
    stage_handle = os2_make_temp_path(argv[0], temporary, sizeof(temporary));
    if (stage_handle < 0) {
        fprintf(stderr, "boltpack: cannot create archive staging file\n");
        return 1;
    }
    stage_file.handle = (HFILE)stage_handle;
    bp_io_from_os2_handle(&aio, &stage_file);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto create_discard;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    start = clock();
    for (i = 1; i < argc; i++) {
        char buf[BP_MAX_PATH]; strcpy(buf, clean_path(argv[i])); normalise_seps(buf);
        rc = add_path(&ar, argv[i], buf);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", argv[i], bp_strerror(rc));
            bp_archive_close(&ar);
            goto create_discard;
        }
    }
    rc = bp_archive_finalize(&ar);
    total_orig.lo = total_orig.hi = 0;
    total_comp.lo = total_comp.hi = 0;
    file_count = dir_count = 0;
    if (rc == BP_OK) {
        bp_uint32 j;
        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) dir_count++;
            else file_count++;
            total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
            total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
        }
    }
    bp_archive_close(&ar);
    if (rc == BP_OK && DosResetBuffer(stage_file.handle) != NO_ERROR)
        rc = BP_ERR_IO;
    if (DosClose(stage_file.handle) != NO_ERROR && rc == BP_OK)
        rc = BP_ERR_IO;
    stage_file.handle = (HFILE)-1;
    stage_handle = -1;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize or close failed: %s\n",
                bp_strerror(rc));
        goto create_discard;
    }
    rc = os2_publish_archive(temporary, argv[0]);
    if (rc != BP_OK && rc != BP_ERR_PARTIAL) {
        fprintf(stderr, "boltpack: cannot publish archive: %s\n",
                bp_strerror(rc));
        goto create_discard;
    }
    if (g_unknown_source_times != 0) {
        fprintf(stderr, "boltpack: %lu timestamp(s) were stored as unknown; set BP_TZ_OFFSET (minutes east of UTC)\n",
                (unsigned long)g_unknown_source_times);
    }
    if (g_verbosity < VERB_SILENT) {
        double elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;
        char nb[32], sb[64];
        format_size64(total_orig, nb, sizeof(nb));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)dir_count, dir_count == 1 ? "y" : "ies", nb);
        if (!bp_u64_is_zero(total_orig) && !bp_u64_is_zero(total_comp)) {
            double od = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double cd = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            char cb[32]; format_size64(total_comp, cb, sizeof(cb));
            printf(", %.0f%% (%s)", od > 0.0 ? cd / od * 100.0 : 0.0, cb);
        }
        if (elapsed > 0.0) {
            double od = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            printf(", %.2fs", elapsed);
            format_speed(od / elapsed, sb, sizeof(sb));
            printf(", %s", sb);
        }
        printf("\n");
    }
    return rc == BP_ERR_PARTIAL || g_unknown_source_times != 0 ? 1 : 0;

create_discard:
    if (stage_file.handle != (HFILE)-1) {
        (void)DosClose(stage_file.handle);
        stage_file.handle = (HFILE)-1;
    }
    if (temporary[0] != '\0') (void)DosDelete((PSZ)temporary);
    return 1;
}

/* ---- cmd_add ---------------------------------------------------------- */

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int interchange_profile)
{
    os2_archive_io_t archive_file;
    bp_io_t aio;
    bp_archive_t ar;
    FILESTATUS3 original_status, opened_status, current_status;
    HFILE source_handle, stage_handle;
    ULONG action, read_count, written, written_total;
    unsigned char buffer[8192];
    char temporary[BP_MAX_PATH];
    int temp_handle, rc, i, archive_open, result;

    if (argc < 2) { fprintf(stderr, "boltpack: add requires archive and files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    g_unknown_source_times = 0;
    source_handle = (HFILE)-1;
    stage_handle = (HFILE)-1;
    temporary[0] = '\0';
    archive_open = 0;
    result = 1;

    rc = DosQueryPathInfo((PSZ)argv[0], FIL_STANDARD, &original_status,
                          sizeof(original_status));
    if (rc != NO_ERROR || (original_status.attrFile & FILE_DIRECTORY)) {
        fprintf(stderr, "boltpack: cannot use archive target: %s\n", argv[0]);
        goto add_cleanup_os2;
    }
    if (original_status.attrFile & FILE_READONLY) {
        fprintf(stderr, "boltpack: archive is read-only: %s\n", argv[0]);
        goto add_cleanup_os2;
    }
    rc = DosOpen((PSZ)argv[0], &source_handle, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                 OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYWRITE |
                 OPEN_FLAGS_SEQUENTIAL, NULL);
    if (rc != NO_ERROR) {
        source_handle = (HFILE)-1;
        fprintf(stderr, "boltpack: cannot open archive for update: %s\n",
                argv[0]);
        goto add_cleanup_os2;
    }
    if (DosQueryFileInfo(source_handle, FIL_STANDARD, &opened_status,
                         sizeof(opened_status)) != NO_ERROR ||
        !os2_archive_status_same(&original_status, &opened_status)) {
        fprintf(stderr, "boltpack: archive changed while opening: %s\n",
                argv[0]);
        goto add_cleanup_os2;
    }

    temp_handle = os2_make_temp_path(argv[0], temporary, sizeof(temporary));
    if (temp_handle < 0) {
        stage_handle = (HFILE)-1;
        fprintf(stderr, "boltpack: cannot create archive staging file\n");
        goto add_cleanup_os2;
    }
    stage_handle = (HFILE)temp_handle;
    for (;;) {
        if (DosRead(source_handle, buffer, sizeof(buffer), &read_count) !=
            NO_ERROR) {
            fprintf(stderr, "boltpack: cannot read original archive\n");
            goto add_cleanup_os2;
        }
        if (read_count == 0) break;
        written_total = 0;
        while (written_total < read_count) {
            rc = DosWrite(stage_handle, buffer + written_total,
                          read_count - written_total, &written);
            if (rc != NO_ERROR || written == 0) {
                fprintf(stderr, "boltpack: cannot copy archive to staging file\n");
                goto add_cleanup_os2;
            }
            written_total += written;
        }
    }
    if (DosResetBuffer(stage_handle) != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot flush archive staging file\n");
        goto add_cleanup_os2;
    }
    if (DosClose(stage_handle) != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot close archive staging file\n");
        goto add_cleanup_os2;
    }
    stage_handle = (HFILE)-1;

    rc = DosOpen((PSZ)temporary, &stage_handle, &action, 0, FILE_NORMAL,
                 OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                 OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYWRITE |
                 OPEN_FLAGS_SEQUENTIAL, NULL);
    if (rc != NO_ERROR) {
        stage_handle = (HFILE)-1;
        fprintf(stderr, "boltpack: cannot reopen archive staging file\n");
        goto add_cleanup_os2;
    }
    archive_file.handle = stage_handle;
    bp_io_from_os2_handle(&aio, &archive_file);
    rc = bp_archive_open_append(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto add_cleanup_os2;
    }
    archive_open = 1;
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            goto add_cleanup_os2;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
    for (i = 1; i < argc; i++) {
        char arcname[BP_MAX_PATH];
        strcpy(arcname, clean_path(argv[i]));
        normalise_seps(arcname);
        rc = add_path(&ar, argv[i], arcname);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", argv[i], bp_strerror(rc));
            goto add_cleanup_os2;
        }
    }
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    archive_open = 0;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize: %s\n", bp_strerror(rc));
        goto add_cleanup_os2;
    }
    if (DosQueryFileInfo(source_handle, FIL_STANDARD, &opened_status,
                         sizeof(opened_status)) != NO_ERROR ||
        DosQueryPathInfo((PSZ)argv[0], FIL_STANDARD, &current_status,
                         sizeof(current_status)) != NO_ERROR ||
        !os2_archive_status_same(&original_status, &opened_status) ||
        !os2_archive_status_same(&original_status, &current_status)) {
        fprintf(stderr, "boltpack: archive changed during append: %s\n",
                argv[0]);
        goto add_cleanup_os2;
    }
    if (DosResetBuffer(archive_file.handle) != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot flush appended archive stage\n");
        goto add_cleanup_os2;
    }
    if (DosClose(archive_file.handle) != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot close appended archive stage\n");
        goto add_cleanup_os2;
    }
    archive_file.handle = (HFILE)-1;
    stage_handle = (HFILE)-1;
    if (os2_copy_archive_attributes(temporary, &original_status) != 0) {
        fprintf(stderr, "boltpack: cannot preserve archive attributes\n");
        goto add_cleanup_os2;
    }
    if (DosClose(source_handle) != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot close original archive\n");
        goto add_cleanup_os2;
    }
    source_handle = (HFILE)-1;

    rc = os2_publish_archive(temporary, argv[0]);
    if (rc != BP_OK && rc != BP_ERR_PARTIAL) {
        fprintf(stderr, "boltpack: cannot publish appended archive: %s\n",
                bp_strerror(rc));
        goto add_cleanup_os2;
    }
    temporary[0] = '\0';
    if (g_unknown_source_times != 0) {
        fprintf(stderr, "boltpack: %lu timestamp(s) were stored as unknown; set BP_TZ_OFFSET (minutes east of UTC)\n",
                (unsigned long)g_unknown_source_times);
    }
    if (g_verbosity < VERB_SILENT)
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    result = rc == BP_ERR_PARTIAL || g_unknown_source_times != 0 ? 1 : 0;

add_cleanup_os2:
    if (archive_open) bp_archive_close(&ar);
    if (stage_handle != (HFILE)-1) (void)DosClose(stage_handle);
    if (source_handle != (HFILE)-1) (void)DosClose(source_handle);
    if (temporary[0] != '\0') (void)DosDelete((PSZ)temporary);
    return result;
}

/* ---- cmd_extract ------------------------------------------------------ */

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE *fp; bp_io_t aio; extract_ctx_t ctx; int rc;
    clock_t start; double elapsed;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    rc = os2_preflight_archive(fp, out_dir);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: destination name preflight failed: %s\n",
                bp_strerror(rc));
        fclose(fp);
        return 1;
    }
    if (out_dir && !dry_run && os2_mkdirs_checked(out_dir) != 0) {
        fprintf(stderr, "boltpack: cannot create output directory: %s\n", out_dir);
        fclose(fp);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir; ctx.force = force; ctx.dry_run = dry_run;
    start = clock();
    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done,
                                g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                &ctx);
    os2_finish_directories(&ctx);
    fclose(fp);
    if (rc != BP_OK && rc != BP_ERR_PARTIAL) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }
    if (ctx.skip_errors || ctx.metadata_errors || ctx.mtime_errors || rc == BP_ERR_PARTIAL) {
        fprintf(stderr, "boltpack: partial extraction (%lu skipped, %lu metadata, %lu timestamps)\n",
                (unsigned long)ctx.skip_errors, (unsigned long)ctx.metadata_errors,
                (unsigned long)ctx.mtime_errors);
        return 1;
    }
    elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;
    if (g_verbosity < VERB_SILENT) {
        printf("%lu file%s, %lu director%s",
               (unsigned long)ctx.file_count, ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,  ctx.dir_count  == 1 ? "y" : "ies");
        if (elapsed > 0.0) printf(", %.2fs", elapsed);
        printf("\n");
    }
    return 0;
}

/* ---- cmd_test --------------------------------------------------------- */

static int cmd_test(const char *archive_name)
{
    FILE *fp; bp_io_t aio; test_ctx_t ctx; int rc;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_test(&aio, test_cb, &ctx);
    fclose(fp);
    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1;
    }
    if (g_verbosity < VERB_SILENT) {
        printf("\n%lu file%s OK", (unsigned long)ctx.ok, ctx.ok == 1 ? "" : "s");
        if (ctx.fail) printf(", %lu file%s FAILED",
                             (unsigned long)ctx.fail, ctx.fail == 1 ? "" : "s");
        printf("\n");
    }
    return ctx.fail > 0 ? 1 : 0;
}

/* ---- cmd_delete ------------------------------------------------------- */

static int cmd_delete(int argc, char **argv)
{
    const char **del;
    int del_count;
    FILE *sf;
    bp_io_t sio, dio;
    os2_archive_io_t stage_file;
    char temporary[BP_MAX_PATH];
    int stage_handle;
    bp_uint32 deleted = 0;
    int rc;

    if (argc < 2) { fprintf(stderr, "boltpack: delete requires archive and files\n"); return 1; }
    del = (const char **)&argv[1]; del_count = argc - 1;

    sf = fopen(argv[0], "rb");
    if (!sf) { fprintf(stderr, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    temporary[0] = '\0';
    stage_handle = os2_make_temp_path(argv[0], temporary, sizeof(temporary));
    if (stage_handle < 0) {
        fprintf(stderr, "boltpack: cannot create archive staging file\n");
        fclose(sf);
        return 1;
    }
    stage_file.handle = (HFILE)stage_handle;
    bp_io_from_file(&sio, sf);
    bp_io_from_os2_handle(&dio, &stage_file);
    rc = bp_archive_delete(&sio, &dio, del, (bp_uint32)del_count, &deleted);
    if (fclose(sf) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        goto delete_discard;
    }
    if (!deleted) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        goto delete_discard;
    }
    if (DosResetBuffer(stage_file.handle) != NO_ERROR) rc = BP_ERR_IO;
    if (DosClose(stage_file.handle) != NO_ERROR && rc == BP_OK)
        rc = BP_ERR_IO;
    stage_file.handle = (HFILE)-1;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot close deleted archive stage\n");
        goto delete_discard;
    }
    rc = os2_publish_archive(temporary, argv[0]);
    if (rc != BP_OK && rc != BP_ERR_PARTIAL) {
        fprintf(stderr, "boltpack: cannot replace archive: %s\n",
                bp_strerror(rc));
        goto delete_discard;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n", (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    return rc == BP_ERR_PARTIAL ? 1 : 0;

delete_discard:
    if (stage_file.handle != (HFILE)-1) {
        (void)DosClose(stage_file.handle);
        stage_file.handle = (HFILE)-1;
    }
    if (temporary[0] != '\0') (void)DosDelete((PSZ)temporary);
    return 1;
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char       command;
    bp_uint32  block_size = 0;
    bp_uint8   method     = BP_METHOD_BOLTLZ;
    const char *out_dir   = NULL;
    int        force = 0, dry_run = 0, i;
    int        interchange_profile = 0;
    int        codec_given = 0;

    if (argc < 3) { usage(); return 1; }
    if (strlen(argv[1]) != 1) {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]); usage(); return 1;
    }
    command = argv[1][0];
    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-b")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -b needs argument\n"); return 1; }
            block_size = parse_block_size(argv[i++]);
            if (!block_size) { fprintf(stderr, "boltpack: invalid block size\n"); return 1; }
        } else if (!strcmp(argv[i], "--interchange-profile-1")) {
            interchange_profile = 1; i++;
        } else if (!strcmp(argv[i], "-0"))  {
            method = BP_METHOD_STORED; codec_given = 1; i++;
        } else if (!strcmp(argv[i], "-1"))  {
            method = BP_METHOD_BOLTLITE; codec_given = 1; i++;
        } else if (!strcmp(argv[i], "-F"))  {
            method = BP_METHOD_FLUFF; codec_given = 1; i++;
        }
        else if (!strcmp(argv[i], "-qq")) { g_verbosity = VERB_SILENT;  i++; }
        else if (!strcmp(argv[i], "-q"))  { g_verbosity = VERB_QUIET;   i++; }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -o needs argument\n"); return 1; }
            out_dir = argv[i++];
        } else if (!strcmp(argv[i], "-f")) { force   = 1; i++; }
        else if (!strcmp(argv[i], "-n"))   { dry_run = 1; i++; }
        else { fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]); usage(); return 1; }
    }

    if (interchange_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED)) {
            fprintf(stderr,
                    "boltpack: Interchange Profile 1 requires create/add, BoltLite or Stored, and 16 KiB blocks\n");
            return 1;
        }
        block_size = BP_BLOCK_16K;
        if (method != BP_METHOD_STORED) method = BP_METHOD_BOLTLITE;
    }

    if (method == BP_METHOD_FLUFF && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method,
                                interchange_profile);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method,
                             interchange_profile);
    case 'l': if (i >= argc) { fprintf(stderr, "boltpack: list needs archive\n"); return 1; }
              return cmd_list(argv[i]);
    case 'x': if (i >= argc) { fprintf(stderr, "boltpack: extract needs archive\n"); return 1; }
              return cmd_extract(argv[i], out_dir, force, dry_run);
    case 't': if (i >= argc) { fprintf(stderr, "boltpack: test needs archive\n"); return 1; }
              return cmd_test(argv[i]);
    case 'i': if (i >= argc) { fprintf(stderr, "boltpack: info needs archive\n"); return 1; }
              return cmd_info(argv[i]);
    case 'd': return cmd_delete(argc - i, argv + i);
    default:  fprintf(stderr, "boltpack: unknown command: %c\n", command); usage(); return 1;
    }
}
