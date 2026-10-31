/*
 * main.c -- Atari TOS / MiNT CLI frontend for BoltPack
 *
 * Target:   Atari ST/STE/Mega/TT/Falcon, TOS 1.04+
 * Compiler: m68k-atari-mint-gcc (Vincent Rivière's cross-toolchain)
 *           mintlib provides POSIX stdio, opendir/readdir/stat, utime
 *           The resulting binary is statically linked and runs on plain
 *           TOS without a MiNT kernel.
 *
 * File I/O uses stdio.h from mintlib.
 * Directory traversal uses POSIX opendir/readdir/stat from mintlib.
 * TOS file attributes are archived and restored via Fattrib() (tos.h).
 * Available memory is probed with Malloc(-1L) to auto-select block size.
 *
 * Native metadata (BP_TYPE_META) payload for BP_OS_ATARI:
 *   Byte 0 : TOS attribute byte (FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_ARCH)
 *            (only bits 0x27 are stored; FA_LABEL(0x08) and FA_DIR(0x10)
 *             are never written)
 * A separate BP_META_PORTABLE entry stores the same validity/value bits in
 * the shared FAT metadata envelope. Extraction stages files before checksum
 * verification and defers directory timestamps and attributes.
 *
 * Cross-compilation from Linux:
 *   m68k-atari-mint-gcc -ansi -Wall -Wextra -O2               \
 *       -I src/core                         \
 *       src/core/compress.c src/core/archiver.c                \
 *       src/core/bp_codepage.c                                 \
 *       src/frontend/atari/main.c                              \
 *       src/core/boltlz.c                             \
 *       -o build/atari/boltpack.tos
 *
 * ANSI C (C89/C90) throughout.
 *
 * Copyright (C) 2026 BoltPack contributors
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/bp_time.h"
#include "../../core/bp_metadata.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"
#include "atari_path.h"

/* TOS system calls: Fattrib(), Malloc() */
#include <mint/osbind.h>
#include <mint/ostruct.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>

/* mintlib exposes these POSIX interfaces only with feature macros enabled;
 * the frontend is intentionally compiled in strict ANSI mode. */
extern int lstat(const char *path, struct stat *st);
extern int mkstemp(char *template_path);
extern FILE *fdopen(int fd, const char *mode);
extern int fileno(FILE *stream);

/* ---- constants --------------------------------------------------------- */

/* TOS supports paths up to 128 chars officially; mintlib typically allows
 * longer under MiNT.  512 is a safe upper bound. */
#define BP_MAX_PATH  512

/* Atari ST factory ROM encoding (Western European). */
#define ATARI_CP     BP_CP_ATARIST

/* Metadata payload size: 1 byte (TOS attribute flags) */
#define ATARI_META_PAYLOAD  1
#define ATARI_DIR_QUEUE_MAX 256U
#define ATARI_DIR_QUEUE_BYTES_MAX 32768UL

/* TOS attribute bits we care about.  FA_LABEL (0x08) and FA_DIR (0x10)
 * are structural and must never be written back to a plain file. */
#ifndef FA_RDONLY
#  define FA_RDONLY  0x01
#  define FA_HIDDEN  0x02
#  define FA_SYSTEM  0x04
#  define FA_LABEL   0x08
#  define FA_DIR     0x10
#  define FA_ARCH    0x20
#endif

/* FA_ARCH may not be defined in all versions of mint/ostruct.h */
#ifndef FA_ARCH
#  define FA_ARCH    0x20
#endif

#define ATARI_ATTR_MASK  (FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_ARCH)

/* ---- verbosity --------------------------------------------------------- */

#define VERB_DEFAULT 0
#define VERB_QUIET   1
#define VERB_SILENT  2

static int g_verbosity = VERB_DEFAULT;

/* ---- bp_io_t adapters for FILE * --------------------------------------- */

static long file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

/* ---- read-only memory buffer I/O (for sending metadata payload) --------- */

typedef struct {
    const unsigned char *buf;
    bp_uint32            size;
    bp_uint32            pos;
} rbuf_ctx_t;

static long rbuf_read_fn(void *handle, void *buf, bp_uint32 count)
{
    rbuf_ctx_t *ctx = (rbuf_ctx_t *)handle;
    if (ctx->pos >= ctx->size) return 0;
    if (count > ctx->size - ctx->pos) count = ctx->size - ctx->pos;
    memcpy(buf, ctx->buf + ctx->pos, (size_t)count);
    ctx->pos += count;
    return (long)count;
}

static long rbuf_write_fn(void *handle, const void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1; /* read-only */
}

static int rbuf_seek_fn(void *handle, bp_off_t offset, int origin)
{
    rbuf_ctx_t *ctx = (rbuf_ctx_t *)handle;
    long newpos;
    if (origin == SEEK_SET)       newpos = (long)offset;
    else if (origin == SEEK_CUR)  newpos = (long)ctx->pos  + (long)offset;
    else if (origin == SEEK_END)  newpos = (long)ctx->size + (long)offset;
    else return -1;
    if (newpos < 0 || (bp_uint32)newpos > ctx->size) return -1;
    ctx->pos = (bp_uint32)newpos;
    return 0;
}

static bp_off_t rbuf_tell_fn(void *handle)
{
    return (bp_off_t)((rbuf_ctx_t *)handle)->pos;
}

static void bp_io_from_rbuf(bp_io_t *io, rbuf_ctx_t *ctx)
{
    io->read   = rbuf_read_fn;
    io->write  = rbuf_write_fn;
    io->seek   = rbuf_seek_fn;
    io->tell   = rbuf_tell_fn;
    io->handle = ctx;
}

/* ---- write-only memory buffer I/O (for receiving metadata payload) ----- */

typedef struct {
    unsigned char *buf;
    bp_uint32      cap;
    bp_uint32      len;
    bp_uint32      pos;
} wbuf_ctx_t;

static long wbuf_read_fn(void *handle, void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1; /* write-only */
}

static long wbuf_write_fn(void *handle, const void *data, bp_uint32 count)
{
    wbuf_ctx_t *ctx = (wbuf_ctx_t *)handle;
    if (ctx->pos > ctx->cap) return -1;
    if (count > ctx->cap - ctx->pos) count = ctx->cap - ctx->pos;
    if (count == 0) return 0;
    memcpy(ctx->buf + ctx->pos, data, (size_t)count);
    ctx->pos += count;
    if (ctx->pos > ctx->len) ctx->len = ctx->pos;
    return (long)count;
}

static int wbuf_seek_fn(void *handle, bp_off_t offset, int origin)
{
    wbuf_ctx_t *ctx = (wbuf_ctx_t *)handle;
    long newpos;
    if (origin == SEEK_SET)       newpos = (long)offset;
    else if (origin == SEEK_CUR)  newpos = (long)ctx->pos + (long)offset;
    else if (origin == SEEK_END)  newpos = (long)ctx->len + (long)offset;
    else return -1;
    if (newpos < 0 || (bp_uint32)newpos > ctx->cap) return -1;
    ctx->pos = (bp_uint32)newpos;
    return 0;
}

static bp_off_t wbuf_tell_fn(void *handle)
{
    return (bp_off_t)((wbuf_ctx_t *)handle)->pos;
}

/*
 * meta_extract_t: used when extracting a BP_TYPE_META entry.
 * io MUST be the first member so that (meta_extract_t *)out_io is valid
 * (address of first member equals address of struct in C89).
 */
typedef struct {
    bp_io_t       io;       /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[BP_META_MAX_SIZE];
} meta_extract_t;

typedef struct atari_dir_restore_s {
    struct atari_dir_restore_s *next;
    char *path;
    bp_uint64 mtime;
    bp_uint16 flags;
    unsigned char native_attr;
    unsigned char fat_valid;
    unsigned char fat_value;
    int native_seen;
    int fat_seen;
} atari_dir_restore_t;

/* ---- error messages ---------------------------------------------------- */

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
    case BP_ERR_PARTIAL:           return "some attributes were not restored";
    default:                       return "unknown error";
    }
}

/* ---- speed formatting -------------------------------------------------- */

static void format_speed(double bytes_per_sec, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes_per_sec < 1024.0) {
        sprintf(buf, "%.0f bytes/s", bytes_per_sec);
    } else if (bytes_per_sec < 1048576.0) {
        sprintf(buf, "%.2f KB/s", bytes_per_sec / 1024.0);
    } else {
        sprintf(buf, "%.2f MB/s", bytes_per_sec / 1048576.0);
    }
}

/* ---- size formatting --------------------------------------------------- */

static void format_size(bp_uint32 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes < 1024UL) {
        sprintf(buf, "%lu bytes", (unsigned long)bytes);
    } else if (bytes < 1048576UL) {
        sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1073741824UL) {
        sprintf(buf, "%.1f MB", (double)bytes / 1048576.0);
    } else {
        sprintf(buf, "%.2f GB", (double)bytes / 1073741824.0);
    }
}

static void format_size64(bp_uint64 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes.hi == 0) {
        format_size(bytes.lo, buf, bufsize);
    } else {
        double d = (double)bytes.lo + (double)bytes.hi * 4294967296.0;
        if (d < 1048576.0) {
            sprintf(buf, "%.1f KB", d / 1024.0);
        } else if (d < 1073741824.0) {
            sprintf(buf, "%.1f MB", d / 1048576.0);
        } else {
            sprintf(buf, "%.2f GB", d / 1073741824.0);
        }
    }
}

/* ---- number formatting ------------------------------------------------- */

static void format_number(bp_uint32 n, char *buf, size_t bufsize)
{
    char tmp[32];
    int  len, i, j, commas;

    sprintf(tmp, "%lu", (unsigned long)n);
    len    = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > bufsize) {
        strcpy(buf, tmp);
        return;
    }
    j = len + commas;
    buf[j] = '\0';
    for (i = len - 1; i >= 0; i--) {
        buf[--j] = tmp[i];
        if (i > 0 && (len - i) % 3 == 0) buf[--j] = ',';
    }
}

/* ---- progress callback ------------------------------------------------- */

static int progress_cb(const bp_progress_t *p, void *user_data)
{
    (void)p;
    (void)user_data;
    return 0;
}

/* ---- path utilities ---------------------------------------------------- */

static int ends_with_sep(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && (s[len - 1] == '/' || s[len - 1] == '\\');
}

static int path_join(char *buf, size_t bufsize, const char *dir,
                     const char *name)
{
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    int add_sep = !ends_with_sep(dir);
    size_t pos = dir_len;
    if (bufsize == 0 || dir_len >= bufsize) return -1;
    if (add_sep) {
        if (pos >= bufsize - 1) return -1;
        buf[pos++] = '/';
    }
    if (name_len >= bufsize - pos) return -1;
    memcpy(buf, dir, dir_len);
    if (add_sep) buf[dir_len] = '/';
    memcpy(buf + pos, name, name_len);
    buf[pos + name_len] = '\0';
    return 0;
}

/*
 * Strip optional drive prefix ("C:") and leading path separators so that
 * "C:\folder\file.txt" becomes "folder/file.txt".
 * The caller should then run normalise_seps() on the result.
 */
static const char *clean_path(const char *path)
{
    /* strip drive letter + colon, e.g. "C:" */
    if (path[0] != '\0' && path[1] == ':') {
        path += 2;
    }
    /* strip leading separators */
    while (*path == '/' || *path == '\\') {
        path++;
    }
    /* strip "./" or ".\" */
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) {
        path += 2;
    }
    return path;
}

/* Convert backslashes to forward slashes in-place (for archive names) */
static void normalise_seps(char *path)
{
    char *p;
    for (p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* ---- mkdir -p ---------------------------------------------------------- */

static int atari_mkdir(const char *path)
{
    return mkdir(path, 0755);
}

static int ensure_directory_component(const char *path)
{
    struct stat st;
    size_t len = strlen(path);
    if (len == 2 && path[1] == ':') return 0;
    if (lstat(path, &st) == 0) {
#ifdef S_ISLNK
        if (S_ISLNK(st.st_mode)) return -1;
#endif
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (errno != ENOENT) return -1;
    if (atari_mkdir(path) == 0) return 0;
    if (errno != EEXIST || lstat(path, &st) != 0) return -1;
#ifdef S_ISLNK
    if (S_ISLNK(st.st_mode)) return -1;
#endif
    return S_ISDIR(st.st_mode) ? 0 : -1;
}

static int mkdirs(const char *path)
{
    char  tmp[BP_MAX_PATH];
    char *p;
    size_t len;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[--len] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (ensure_directory_component(tmp) != 0) {
                *p = saved;
                return -1;
            }
            *p = saved;
        }
    }
    return ensure_directory_component(tmp);
}

static int mkdirs_for_file(const char *path)
{
    char  tmp[BP_MAX_PATH];
    char *slash;
    char *bslash;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    slash  = strrchr(tmp, '/');
    bslash = strrchr(tmp, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) slash = bslash;

    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        if (mkdirs(tmp) != 0) return -1;
    }
    return 0;
}

/* ---- parse block size -------------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (strcmp(s, "8k")   == 0 || strcmp(s, "8K")   == 0) return BP_BLOCK_8K;
    if (strcmp(s, "16k")  == 0 || strcmp(s, "16K")  == 0) return BP_BLOCK_16K;
    if (strcmp(s, "32k")  == 0 || strcmp(s, "32K")  == 0) return BP_BLOCK_32K;
    if (strcmp(s, "64k")  == 0 || strcmp(s, "64K")  == 0) return BP_BLOCK_64K;
    if (strcmp(s, "256k") == 0 || strcmp(s, "256K") == 0) return BP_BLOCK_256K;
    return 0;
}

/* ---- Atari attribute helpers ------------------------------------------- */

static int atari_gemdos_path(const char *path, char *out, size_t capacity)
{
    size_t i, length = strlen(path);
    if (length >= capacity) return -1;
    for (i = 0; i < length; i++) out[i] = path[i] == '/' ? '\\' : path[i];
    while (length > 1 && out[length - 1] == '\\' &&
           !(length == 3 && out[1] == ':')) length--;
    out[length] = '\0';
    return 0;
}

/*
 * Read TOS file attributes.  Fattrib(path, 0, 0) returns the current value.
 */
static int atari_get_attrib(const char *path, unsigned char *attr)
{
    char native_path[BP_MAX_PATH];
    long result;
    if (atari_gemdos_path(path, native_path, sizeof(native_path)) != 0) return -1;
    result = Fattrib(native_path, 0, 0);
    if (result < 0 || attr == NULL) return -1;
    *attr = (unsigned char)((int)result & ATARI_ATTR_MASK);
    return 0;
}

/*
 * Write only the supplied attribute bits and report target API failures.
 */
static int atari_apply_attrib(const char *path, unsigned char valid,
                              unsigned char value)
{
    char native_path[BP_MAX_PATH];
    long current;
    struct stat st;
    unsigned char desired;
    long result;
    if (atari_gemdos_path(path, native_path, sizeof(native_path)) != 0) return -1;
    current = Fattrib(native_path, 0, 0);
    if (current < 0) {
        /* TOS 1.62 returns EFILNF for a directory with no optional FAT
         * attributes.  A successful stat distinguishes that default case. */
        if (stat(native_path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
        current = 0;
        if ((value & valid & ATARI_ATTR_MASK) == 0) return 0;
    }
    valid = (unsigned char)(valid & ATARI_ATTR_MASK);
    desired = (unsigned char)(((unsigned char)current & (unsigned char)~valid) |
                              (value & valid));
    result = Fattrib(native_path, 1, (int)(desired & ATARI_ATTR_MASK));
    return result < 0 ? -1 : 0;
}

static void atari_stat_mtime(const struct stat *st, bp_uint64 *mtime,
                             bp_uint16 *flags)
{
    unsigned long seconds;
    mtime->lo = 0;
    mtime->hi = 0;
    *flags = 0;
    if (st->st_mtime < (time_t)0) return;
    seconds = (unsigned long)st->st_mtime;
    if ((time_t)seconds != st->st_mtime) return;
    *mtime = bp_u64_from_ulong(seconds);
    *flags = BP_ENTRY_FLAG_MTIME_VALID;
}

/*
 * Auto-detect block size based on the size of the largest free contiguous
 * memory block reported by Malloc(-1L).
 * Reserves ~64 KB overhead for the C stack, heap, and decompressor buffers.
 */
static bp_uint32 atari_auto_block_size(void)
{
    long avail = (long)Malloc(-1L);
    if      (avail >= 1024L * 1024L)       return BP_BLOCK_256K;
    else if (avail >= 512L  * 1024L)       return BP_BLOCK_64K;
    else if (avail >= 256L  * 1024L)       return BP_BLOCK_32K;
    else if (avail >= 128L  * 1024L)       return BP_BLOCK_16K;
    return BP_BLOCK_8K;
}

/* ---- recursive add ----------------------------------------------------- */

/*
 * Add a BP_TYPE_META entry containing the single-byte TOS attribute value.
 * The entry shares the same archive name as the preceding data/dir entry.
 */
static int add_meta_entry(bp_archive_t *ar, const char *arcname,
                          unsigned char attr)
{
    unsigned char meta_buf[ATARI_META_PAYLOAD];
    bp_io_t       meta_io;
    rbuf_ctx_t    rctx;

    meta_buf[0] = attr & ATARI_ATTR_MASK;

    rctx.buf  = meta_buf;
    rctx.size = ATARI_META_PAYLOAD;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry_ex(ar, arcname, BP_TYPE_META, BP_OS_ATARI,
                                &meta_io, bp_u64_from_u32((bp_uint32)ATARI_META_PAYLOAD),
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
static int atari_archive_stat_matches(const struct stat *expected,
                                     const struct stat *current);

/* Hash a pathname with the same CRC-32 used for archive payloads.  The
 * expected length comes from the initial stat so a concurrent size change
 * cannot be hidden by a matching CRC over a shorter/longer stream. */
static int atari_path_crc32(const char *path, unsigned long expected_size,
                            bp_uint32 *out_crc)
{
    FILE *fp;
    unsigned char buffer[1024];
    unsigned long total;
    size_t count;
    bp_uint32 crc;

    fp = fopen(path, "rb");
    if (fp == NULL) return -1;
    total = 0;
    crc = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), fp)) != 0) {
        if ((unsigned long)count > expected_size - total) {
            (void)fclose(fp);
            return -1;
        }
        total += (unsigned long)count;
        crc = bp_crc32(crc, buffer, (bp_uint32)count);
    }
    {
        int failed = ferror(fp) || total != expected_size;
        if (fclose(fp) != 0) failed = 1;
        if (failed) return -1;
    }
    *out_crc = crc;
    return 0;
}

#ifdef BP_TEST_PAUSE_AFTER_SOURCE_STAT
static void atari_test_pause_after_source_stat(void)
{
    FILE *fp = fopen("C:\\STATDONE.TXT", "wb");
    if (fp == NULL) return;
    (void)fputs("source stat captured\n", fp);
    (void)fclose(fp);
    for (;;) {
        fp = fopen("C:\\CONTINUE.TXT", "rb");
        if (fp != NULL) {
            (void)fclose(fp);
            break;
        }
    }
}
#endif

#ifdef BP_TEST_PAUSE_AFTER_SOURCE_OPEN
static void atari_test_pause_after_source_open(void)
{
    FILE *fp = fopen("C:\\OPENED.TXT", "wb");
    if (fp == NULL) return;
    (void)fputs("source opened\n", fp);
    (void)fclose(fp);
    for (;;) {
        fp = fopen("C:\\CONT2.TXT", "rb");
        if (fp != NULL) {
            (void)fclose(fp);
            break;
        }
    }
}
#endif

#ifdef BP_ATARI_TEST_PAUSE_BEFORE_ROOT_CHECK
static void atari_test_pause_before_root_check(void)
{
    static int used = 0;
    FILE *fp;
    if (used) return;
    used = 1;
    fp = fopen("C:\\ROOTCHK.TXT", "wb");
    if (fp == NULL) return;
    (void)fclose(fp);
    for (;;) {
        fp = fopen("C:\\CNTROOT.TXT", "rb");
        if (fp != NULL) {
            (void)fclose(fp);
            break;
        }
    }
}
#endif

#ifdef BP_ATARI_TEST_PAUSE_AFTER_EXTRACT_STAGE
static void atari_test_pause_after_extract_stage(void)
{
    static int used = 0;
    FILE *fp;
    if (used) return;
    used = 1;
    fp = fopen("C:\\STAGEOK.TXT", "wb");
    if (fp == NULL) return;
    (void)fclose(fp);
    for (;;) {
        fp = fopen("C:\\CNTSTG.TXT", "rb");
        if (fp != NULL) {
            (void)fclose(fp);
            break;
        }
    }
}
#endif

static int add_directory(bp_archive_t *ar, const char *dir_path,
                         const char *arc_prefix)
{
    DIR           *d;
    struct dirent *ent;
    int            rc;
    char           child_path[BP_MAX_PATH];
    char           child_arc[BP_MAX_PATH];

    d = opendir(dir_path);
    if (d == NULL) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", dir_path);
        return BP_ERR_IO;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (path_join(child_path, sizeof(child_path), dir_path, ent->d_name) != 0 ||
            path_join(child_arc, sizeof(child_arc), arc_prefix, ent->d_name) != 0) {
            closedir(d);
            return BP_ERR_NAME_TOO_LONG;
        }
        rc = add_path(ar, child_path, child_arc);
        if (rc != BP_OK) { closedir(d); return rc; }
    }
    closedir(d);
    return BP_OK;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct stat st;
    unsigned char attr;
    int         rc;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }
    if (S_ISDIR(st.st_mode)) {
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 2];
        bp_uint64 mtime;
        bp_uint16 flags;

        if (atari_get_attrib(path, &attr) != 0) {
            if (!S_ISDIR(st.st_mode)) {
                fprintf(stderr, "boltpack: cannot read TOS attributes: %s\n", path);
                return BP_ERR_IO;
            }
            /* Default directories have no optional FAT attributes on TOS. */
            attr = 0;
        }

        if (!ends_with_sep(arcname))
            sprintf(dir_arc, "%s/", arcname);
        else
            strcpy(dir_arc, arcname);

        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", dir_arc);

        if (bp_local_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8),
                             ATARI_CP) != 0) return BP_ERR_INVALID_ARG;

        atari_stat_mtime(&st, &mtime, &flags);
        rc = bp_archive_add_entry_ex(ar, dir_arc_utf8, BP_TYPE_DIR,
                                     BP_OS_ATARI, NULL, bp_u64_from_u32(0),
                                     mtime, flags);
        if (rc != BP_OK) return rc;

        rc = add_meta_entry(ar, dir_arc_utf8, attr);
        if (rc != BP_OK) return rc;
        rc = add_portable_meta_entry(ar, dir_arc_utf8, BP_TYPE_DIR, attr);
        if (rc != BP_OK) return rc;

        return add_directory(ar, path, dir_arc);

    } else if (S_ISREG(st.st_mode)) {
        FILE      *fp;
        bp_io_t    fio;
        struct stat opened_st, final_st, path_st;
        bp_uint64  file_size;
        bp_uint64  mtime;
        bp_uint32  source_crc, after_crc;
        bp_uint16  flags;
        char       arcname_utf8[BP_MAX_PATH * 2];

        if (st.st_size < 0) return BP_ERR_OVERFLOW;
        file_size = bp_u64_from_ulong((unsigned long)st.st_size);
        if (atari_get_attrib(path, &attr) != 0) {
            fprintf(stderr, "boltpack: cannot read TOS attributes: %s\n", path);
            return BP_ERR_IO;
        }
        atari_stat_mtime(&st, &mtime, &flags);

        if (atari_path_crc32(path, (unsigned long)st.st_size,
                             &source_crc) != 0) {
            fprintf(stderr, "boltpack: cannot checksum source: %s\n", path);
            return BP_ERR_IO;
        }

#ifdef BP_TEST_PAUSE_AFTER_SOURCE_STAT
        atari_test_pause_after_source_stat();
#endif
        fp = fopen(path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", path);
            return BP_ERR_IO;
        }
        if (fstat(fileno(fp), &opened_st) != 0 ||
            !atari_archive_stat_matches(&st, &opened_st)) {
            (void)fclose(fp);
            fprintf(stderr, "boltpack: source changed while opening: %s\n",
                    path);
            return BP_ERR_IO;
        }
#ifdef BP_TEST_PAUSE_AFTER_SOURCE_OPEN
        atari_test_pause_after_source_open();
#endif
        bp_io_from_file(&fio, fp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("compressing: %s", arcname);
            fflush(stdout);
        }

        if (bp_local_to_utf8(arcname, arcname_utf8, sizeof(arcname_utf8),
                             ATARI_CP) != 0) {
            fclose(fp);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DATA,
                                     BP_OS_ATARI, &fio, file_size, mtime, flags);
        if (rc == BP_OK &&
            (ar->entry_list[ar->entries_written - 1].checksum_type !=
                 BP_CKSUM_CRC32 ||
             ar->entry_list[ar->entries_written - 1].checksum != source_crc)) {
            fprintf(stderr, "boltpack: source content changed while reading: %s\n",
                    path);
            rc = BP_ERR_IO;
        }
        if (fstat(fileno(fp), &final_st) != 0 ||
            !atari_archive_stat_matches(&opened_st, &final_st) ||
            stat(path, &path_st) != 0 ||
            !atari_archive_stat_matches(&opened_st, &path_st)) {
            if (rc == BP_OK) {
                fprintf(stderr, "boltpack: source changed while reading: %s\n",
                        path);
                rc = BP_ERR_IO;
            }
        }
        fclose(fp);
        if (rc != BP_OK) return rc;
        if (atari_path_crc32(path, (unsigned long)st.st_size,
                             &after_crc) != 0 || after_crc != source_crc) {
            fprintf(stderr, "boltpack: source content changed while reading: %s\n",
                    path);
            return BP_ERR_IO;
        }

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e    = &ar->entry_list[ar->entries_written - 1];
            bp_uint32   comp = bp_u64_to_u32(e->compressed_size);
            bp_uint32   fsz  = bp_u64_to_u32(file_size);
            char        sizebuf[32];
            if (comp == 0 || e->method == BP_METHOD_STORED || comp >= fsz) {
                format_size(fsz, sizebuf, sizeof(sizebuf));
                printf("  %s (stored)\n", sizebuf);
            } else {
                double pct = (double)comp / (double)fsz * 100.0;
                format_size(comp, sizebuf, sizeof(sizebuf));
                printf("  %s (%.0f%%)\n", sizebuf, pct);
            }
        }

        /* Preserve TOS attribute flags */
        rc = add_meta_entry(ar, arcname_utf8, attr);
        if (rc != BP_OK) return rc;
        return add_portable_meta_entry(ar, arcname_utf8, BP_TYPE_DATA, attr);
    }

    /* skip other file types (devices, etc.) */
    return BP_OK;
}

/* ---- list callback ----------------------------------------------------- */

typedef struct {
    bp_uint32 file_count;
    bp_uint32 dir_count;
    bp_uint64 total_orig;
    bp_uint64 total_comp;
} list_ctx_t;

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    list_ctx_t *ctx = (list_ctx_t *)user_data;
    const char *type_str;
    char        orig_buf[32], comp_buf[32], disp_name[BP_MAX_PATH];

    switch (entry->entry_type) {
    case BP_TYPE_DATA:    type_str = "DATA"; break;
    case BP_TYPE_RSRC:    type_str = "RSRC"; break;
    case BP_TYPE_META:    type_str = "META"; break;
    case BP_TYPE_SYMLINK: type_str = "LINK"; break;
    case BP_TYPE_DIR:     type_str = "DIR "; break;
    default:              type_str = "??? "; break;
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
    } else if (entry->entry_type == BP_TYPE_DATA ||
               entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->file_count++;
    }
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);

    if (bp_utf8_to_local(entry->name, disp_name, sizeof(disp_name), ATARI_CP) != 0) strcpy(disp_name, "<unrepresentable>");

    format_number(bp_u64_to_u32(entry->original_size),  orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_u32(entry->compressed_size), comp_buf, sizeof(comp_buf));

    printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, disp_name);
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct {
    const char *out_dir;
    struct stat out_root_stat;
    int         out_root_stat_valid;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
    int         mtime_errors;
    int         skip_errors;
    int         metadata_errors;
    int         subject_ok;
    int         subject_type;
    int         subject_native_seen;
    char        subject[BP_MAX_PATH];
    atari_dir_restore_t *directories;
    atari_dir_restore_t *subject_directory;
    unsigned int directory_count;
    unsigned long directory_path_bytes;
    bp_atari_name_set_t projected_names;
} extract_ctx_t;

typedef struct {
    bp_io_t io;
    int fd;
    char temporary[BP_MAX_PATH];
    char target[BP_MAX_PATH];
    int preserve_target_attributes;
    unsigned char target_attributes;
} atari_staged_output_t;

static int atari_archive_stat_matches(const struct stat *expected,
                                      const struct stat *current);
static int atari_archive_target_unchanged(const char *path,
                                          const struct stat *snapshot,
                                          int existed);
static int atari_output_root_unchanged(const extract_ctx_t *ctx);

static long staged_file_write(void *handle, const void *buf, bp_uint32 count)
{
    atari_staged_output_t *out = (atari_staged_output_t *)handle;
    long written = (long)write(out->fd, buf, (size_t)count);
    return written < 0 ? -1 : written;
}

static int atari_sibling_name(const char *target, const char *base,
                              char *out, size_t out_size)
{
    const char *slash = strrchr(target, '/');
    const char *bslash = strrchr(target, '\\');
    const char *last = slash;
    size_t prefix;
    size_t base_len = strlen(base);
    if (bslash != NULL && (last == NULL || bslash > last)) last = bslash;
    prefix = last != NULL ? (size_t)(last - target + 1) : 0;
    if (prefix >= out_size || base_len >= out_size - prefix) return -1;
    memcpy(out, target, prefix);
    memcpy(out + prefix, base, base_len + 1);
    return 0;
}

static int atari_make_temp_template(const char *target, char *out,
                                    size_t out_size)
{
    /* BPXXXXXX is an eight-character TOS basename. */
    return atari_sibling_name(target, "BPXXXXXX", out, out_size);
}

static int atari_rename_noreplace(const char *from, const char *to)
{
    char old_path[BP_MAX_PATH];
    char new_path[BP_MAX_PATH];
    char *p;
    if (strlen(from) >= sizeof(old_path) || strlen(to) >= sizeof(new_path))
        return -1;
    strcpy(old_path, from);
    strcpy(new_path, to);
    for (p = old_path; *p; p++) if (*p == '/') *p = '\\';
    for (p = new_path; *p; p++) if (*p == '/') *p = '\\';
    return Frename(0, old_path, new_path) == 0 ? 0 : -1;
}

static int atari_backup_existing(const char *target, char *backup,
                                 size_t backup_size)
{
    static unsigned long sequence = 0;
    unsigned int attempt;
    char base[9];
    for (attempt = 0; attempt < 64U; attempt++) {
        unsigned long id = (((unsigned long)clock() << 4) + sequence++) &
                           0x0FFFFFFFUL;
        sprintf(base, "B%07lX", id);
        if (atari_sibling_name(target, base, backup, backup_size) != 0)
            return -1;
        if (atari_rename_noreplace(target, backup) == 0) return 0;
    }
    return -1;
}

static int atari_commit_staged_checked(atari_staged_output_t *out, int force,
                                       const struct stat *snapshot,
                                       int expected_exists)
{
    struct stat st;
    struct stat backup_stat;
    char backup[BP_MAX_PATH];
    int has_backup = 0;
    int target_exists;
    if (snapshot != NULL &&
        !atari_archive_target_unchanged(out->target, snapshot,
                                        expected_exists))
        return BP_ERR_PARTIAL;
    if (snapshot != NULL && out->preserve_target_attributes) {
        unsigned char current_attributes;
        if (atari_get_attrib(out->target, &current_attributes) != 0 ||
            (current_attributes & ATARI_ATTR_MASK) != out->target_attributes)
            return BP_ERR_PARTIAL;
    }
    if (lstat(out->target, &st) == 0) target_exists = 1;
    else if (errno == ENOENT) target_exists = 0;
    else return BP_ERR_IO;
    if (snapshot != NULL && target_exists != expected_exists)
        return BP_ERR_PARTIAL;
    if (target_exists && !force) return BP_ERR_PARTIAL;
    backup[0] = '\0';
    if (target_exists) {
        if (atari_backup_existing(out->target, backup, sizeof(backup)) != 0)
            return BP_ERR_IO;
        has_backup = 1;
        if (snapshot != NULL &&
            (lstat(backup, &backup_stat) != 0 ||
             !atari_archive_stat_matches(snapshot, &backup_stat))) {
            if (atari_rename_noreplace(backup, out->target) != 0)
                fprintf(stderr,
                        "boltpack: changed archive target remains at recovery path: %s\n",
                        backup);
            return BP_ERR_PARTIAL;
        }
        if (snapshot != NULL && out->preserve_target_attributes) {
            unsigned char backup_attributes;
            if (atari_get_attrib(backup, &backup_attributes) != 0 ||
                (backup_attributes & ATARI_ATTR_MASK) != out->target_attributes) {
                if (atari_rename_noreplace(backup, out->target) != 0)
                    fprintf(stderr,
                            "boltpack: changed archive attributes remain at recovery path: %s\n",
                            backup);
                return BP_ERR_PARTIAL;
            }
        }
    }
    if (atari_rename_noreplace(out->temporary, out->target) != 0) {
        if (has_backup && atari_rename_noreplace(backup, out->target) != 0) {
            fprintf(stderr,
                    "boltpack: original file remains at recovery path: %s\n",
                    backup);
        }
        return BP_ERR_IO;
    }
    out->temporary[0] = '\0';
    if (has_backup && unlink(backup) != 0) {
        fprintf(stderr, "boltpack: cannot remove replaced-file backup: %s\n",
                backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

static int atari_commit_staged(atari_staged_output_t *out, int force)
{
    return atari_commit_staged_checked(out, force, NULL, 0);
}

static void atari_discard_staged(atari_staged_output_t *out)
{
    if (out->fd >= 0) {
        (void)close(out->fd);
        out->fd = -1;
    }
    if (out->temporary[0] != '\0') {
        (void)unlink(out->temporary);
        out->temporary[0] = '\0';
    }
}

static int atari_archive_target_state(const char *path, int allow_missing,
                                     struct stat *snapshot, int *exists,
                                     int *is_read_only)
{
    *is_read_only = 0;
    if (lstat(path, snapshot) == 0) {
        unsigned char attributes;
        if (!S_ISREG(snapshot->st_mode)) return BP_ERR_IO;
        if (atari_get_attrib(path, &attributes) != 0)
            return BP_ERR_IO;
        if ((attributes & FA_RDONLY) != 0) {
            *is_read_only = 1;
            return BP_ERR_IO;
        }
        *exists = 1;
        return BP_OK;
    }
    if (allow_missing && errno == ENOENT) {
        memset(snapshot, 0, sizeof(*snapshot));
        *exists = 0;
        return BP_OK;
    }
    return BP_ERR_IO;
}

static int atari_archive_stat_matches(const struct stat *expected,
                                      const struct stat *current)
{
    return S_ISREG(current->st_mode) &&
           /* TOS exposes different device identifiers for pathname stats
            * and file-handle stats on FAT12 volumes.  It also synthesizes
            * inode numbers independently for those queries. */
           current->st_size == expected->st_size &&
           current->st_mtime == expected->st_mtime &&
           current->st_ctime == expected->st_ctime;
}

static int atari_output_root_unchanged(const extract_ctx_t *ctx)
{
    struct stat current;
    if (ctx->out_dir == NULL || !ctx->out_root_stat_valid) return 1;
    if (lstat(ctx->out_dir, &current) != 0 || !S_ISDIR(current.st_mode))
        return 0;
    /* GEMDOS synthesizes a new inode value on every pathname query. */
    return current.st_dev == ctx->out_root_stat.st_dev &&
           current.st_size == ctx->out_root_stat.st_size &&
           current.st_mtime == ctx->out_root_stat.st_mtime &&
           current.st_ctime == ctx->out_root_stat.st_ctime;
}

static int atari_output_root_refresh(extract_ctx_t *ctx)
{
    struct stat current;
    if (ctx->out_dir == NULL || !ctx->out_root_stat_valid) return 1;
    if (lstat(ctx->out_dir, &current) != 0 || !S_ISDIR(current.st_mode))
        return 0;
    ctx->out_root_stat = current;
    return 1;
}

static int atari_archive_target_unchanged(const char *path,
                                          const struct stat *snapshot,
                                          int existed)
{
    struct stat current;
    if (lstat(path, &current) == 0) {
        return existed && atari_archive_stat_matches(snapshot, &current);
    }
    return !existed && errno == ENOENT;
}

static int atari_open_archive_stage(const char *target,
                                    atari_staged_output_t *stage,
                                    FILE **stream)
{
    struct stat target_stat;
    unsigned char target_attributes;
    int fd;
    memset(stage, 0, sizeof(*stage));
    stage->fd = -1;
    *stream = NULL;
    if (strlen(target) >= sizeof(stage->target) ||
        atari_make_temp_template(target, stage->temporary,
                                 sizeof(stage->temporary)) != 0)
        return BP_ERR_NAME_TOO_LONG;
    strcpy(stage->target, target);
    if (lstat(target, &target_stat) == 0) {
        if (!S_ISREG(target_stat.st_mode) ||
            atari_get_attrib(target, &target_attributes) != 0 ||
            (target_attributes & FA_RDONLY) != 0) {
            return BP_ERR_IO;
        }
        stage->target_attributes = (unsigned char)(target_attributes &
                                                   ATARI_ATTR_MASK);
        stage->preserve_target_attributes = 1;
    } else if (errno != ENOENT) {
        return BP_ERR_IO;
    }
    fd = mkstemp(stage->temporary);
    if (fd < 0) return BP_ERR_IO;
    stage->fd = fd;
    *stream = fdopen(fd, "w+b");
    if (*stream == NULL) {
        atari_discard_staged(stage);
        return BP_ERR_IO;
    }
    return BP_OK;
}

static int atari_finish_archive_stage(FILE **stream,
                                     atari_staged_output_t *stage,
                                     const struct stat *snapshot,
                                     int existed)
{
    int rc = BP_OK;
    if (fflush(*stream) != 0) rc = BP_ERR_IO;
    if (fclose(*stream) != 0) rc = BP_ERR_IO;
    *stream = NULL;
    stage->fd = -1;
    if (rc != BP_OK) return rc;
    if (stage->preserve_target_attributes &&
        atari_apply_attrib(stage->temporary,
                           (unsigned char)(ATARI_ATTR_MASK & ~FA_RDONLY),
                           stage->target_attributes) != 0)
        return BP_ERR_IO;
    if (!atari_archive_target_unchanged(stage->target, snapshot, existed))
        return BP_ERR_PARTIAL;
    return atari_commit_staged_checked(stage, 1, snapshot, existed);
}

static int atari_copy_archive_stream(FILE *source, FILE *destination)
{
    static unsigned char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), source)) != 0) {
        if (fwrite(buffer, 1, count, destination) != count)
            return BP_ERR_IO;
    }
    if (ferror(source) || fflush(destination) != 0) return BP_ERR_IO;
    return BP_OK;
}

static atari_dir_restore_t *queue_directory_restore(extract_ctx_t *ctx,
                                                     const char *path,
                                                     const bp_entry_t *entry)
{
    atari_dir_restore_t *node;
    size_t path_len = strlen(path) + 1;
    if (ctx->directory_count >= ATARI_DIR_QUEUE_MAX ||
        path_len > ATARI_DIR_QUEUE_BYTES_MAX - ctx->directory_path_bytes) {
        ctx->metadata_errors++;
        return NULL;
    }
    node = (atari_dir_restore_t *)calloc(1, sizeof(*node));
    if (node == NULL) {
        ctx->metadata_errors++;
        return NULL;
    }
    node->path = (char *)malloc(path_len);
    if (node->path == NULL) {
        free(node);
        ctx->metadata_errors++;
        return NULL;
    }
    memcpy(node->path, path, path_len);
    node->mtime = entry->mtime;
    node->flags = entry->flags;
    node->next = ctx->directories;
    ctx->directories = node;
    ctx->directory_count++;
    ctx->directory_path_bytes += (unsigned long)path_len;
    return node;
}

static int path_depth(const char *path)
{
    int depth = 0;
    for (; *path; path++)
        if (*path == '/' || *path == '\\') depth++;
    return depth;
}

/* Finalize deepest directories first so parent timestamps and protection
 * flags cannot prevent creation or restoration of their children. */
static void finish_directory_restores(extract_ctx_t *ctx)
{
    while (ctx->directories != NULL) {
        atari_dir_restore_t **best_link = &ctx->directories;
        atari_dir_restore_t **link;
        int best_depth = -1;
        atari_dir_restore_t *node;
        for (link = &ctx->directories; *link != NULL; link = &(*link)->next) {
            int depth = path_depth((*link)->path);
            if (depth > best_depth) {
                best_depth = depth;
                best_link = link;
            }
        }
        node = *best_link;
        *best_link = node->next;
        if (!atari_output_root_unchanged(ctx)) {
            fprintf(stderr,
                    "boltpack: output root changed; directory metadata was not restored: %s\n",
                    node->path);
            ctx->metadata_errors++;
            free(node->path);
            free(node);
            continue;
        }
        if (node->flags & BP_ENTRY_FLAG_MTIME_VALID) {
            char native_path[BP_MAX_PATH];
            if (atari_gemdos_path(node->path, native_path, sizeof(native_path)) != 0 ||
                bp_restore_mtime(native_path, node->mtime) != BP_OK) {
                fprintf(stderr, "boltpack: cannot restore directory timestamp: %s\n",
                        node->path);
                ctx->mtime_errors++;
            }
        }
        if (node->native_seen) {
            if (atari_apply_attrib(node->path, ATARI_ATTR_MASK,
                                   node->native_attr) != 0) {
                fprintf(stderr, "boltpack: cannot restore TOS attributes: %s\n",
                        node->path);
                ctx->metadata_errors++;
            }
        } else if (node->fat_seen && node->fat_valid != 0) {
            if (atari_apply_attrib(node->path, node->fat_valid,
                                   node->fat_value) != 0) {
                fprintf(stderr, "boltpack: cannot restore portable FAT attributes: %s\n",
                        node->path);
                ctx->metadata_errors++;
            }
        }
        if (ctx->directory_count != 0) ctx->directory_count--;
        if (ctx->directory_path_bytes >= (unsigned long)strlen(node->path) + 1)
            ctx->directory_path_bytes -= (unsigned long)strlen(node->path) + 1;
        free(node->path);
        free(node);
    }
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t  *ctx = (extract_ctx_t *)user_data;
    char            path[BP_MAX_PATH];
    char            local_name[BP_MAX_PATH];
    char            projected_name[BP_MAX_PATH];
    atari_staged_output_t *staged;
    meta_extract_t *mex;
    bp_uint32       meta_sz;
    unsigned int    projected_length;
    int             project_rc;

#ifdef BP_ATARI_TEST_PAUSE_BEFORE_ROOT_CHECK
    atari_test_pause_before_root_check();
#endif
    if (!ctx->dry_run && !atari_output_root_unchanged(ctx)) {
        fprintf(stderr, "boltpack: output root changed during extraction\n");
        ctx->skip_errors++;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR ||
        entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->subject_ok = 0;
        ctx->subject_type = entry->entry_type;
        ctx->subject_native_seen = 0;
        ctx->subject_directory = NULL;
    } else if (entry->entry_type != BP_TYPE_META &&
               entry->entry_type != BP_TYPE_RSRC) {
        ctx->subject_ok = 0;
    }
    if (!bp_name_is_safe(entry->name)) {
        fprintf(stderr, "boltpack: refusing unsafe archive path\n");
        ctx->skip_errors++;
        return NULL;
    }

    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                         ATARI_CP) != 0) {
        fprintf(stderr, "boltpack: filename is not representable on Atari: %s\n",
                entry->name);
        ctx->skip_errors++;
        return NULL;
    }

    if (ctx->out_dir) {
        if (path_join(path, sizeof(path), ctx->out_dir, local_name) != 0) {
            ctx->skip_errors++;
            return NULL;
        }
    } else {
        strcpy(path, local_name);
    }

    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR ||
        entry->entry_type == BP_TYPE_SYMLINK) {
        size_t gemdos_path_length = strlen(path);
        if (entry->entry_type == BP_TYPE_DIR && gemdos_path_length > 0 &&
            (path[gemdos_path_length - 1] == '/' ||
             path[gemdos_path_length - 1] == '\\')) gemdos_path_length--;
        if (gemdos_path_length > BP_ATARI_GEMDOS_PATH_MAX) {
            fprintf(stderr, "boltpack: Atari GEMDOS path exceeds 125 bytes: %s\n",
                    local_name);
            ctx->skip_errors++;
            return NULL;
        }
        project_rc = bp_atari_project_name(local_name,
                                           entry->entry_type == BP_TYPE_DIR,
                                           projected_name, sizeof(projected_name),
                                           &projected_length);
        if (project_rc != BP_OK) {
            fprintf(stderr, "boltpack: name is outside the Atari 8.3 profile: %s\n",
                    local_name);
            ctx->skip_errors++;
            return NULL;
        }
        project_rc = bp_atari_name_set_add(&ctx->projected_names,
                                           projected_name, projected_length);
        if (project_rc != BP_OK) {
            if (project_rc == BP_ERR_CORRUPT)
                fprintf(stderr,
                        "boltpack: Atari destination name collision: %s\n",
                        local_name);
            else
                fprintf(stderr,
                        "boltpack: Atari name index limit or allocation failure: %s\n",
                        local_name);
            ctx->skip_errors++;
            return NULL;
        }
    }

    /* --- Directory ---------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", local_name);
        if (ctx->dry_run && strlen(entry->name) < sizeof(ctx->subject)) {
            strcpy(ctx->subject, entry->name);
            ctx->subject_ok = 1;
            return NULL;
        }
        if (!ctx->dry_run && mkdirs(path) == 0 &&
            atari_output_root_refresh(ctx) &&
            strlen(entry->name) < sizeof(ctx->subject)) {
            atari_dir_restore_t *directory =
                queue_directory_restore(ctx, path, entry);
            if (directory != NULL) {
                strcpy(ctx->subject, entry->name);
                ctx->subject_directory = directory;
                ctx->subject_ok = 1;
            } else {
                ctx->skip_errors++;
            }
        } else if (!ctx->dry_run) {
            ctx->skip_errors++;
        }
        return NULL;
    }

    /* --- Atari metadata: capture into buffer for apply in done() ------ */
    if (entry->entry_type == BP_TYPE_META) {
        if (!ctx->subject_ok || strcmp(entry->name, ctx->subject) != 0) {
            ctx->subject_ok = 0;
            ctx->metadata_errors++;
            return NULL;
        }
        meta_sz = bp_u64_to_u32(entry->original_size);
        if (entry->platform == BP_OS_ATARI && meta_sz != ATARI_META_PAYLOAD) {
            ctx->metadata_errors++;
            return NULL;
        }
        if (entry->platform == BP_META_PORTABLE &&
            (meta_sz < 8U || meta_sz > BP_META_MAX_SIZE)) {
            ctx->metadata_errors++;
            return NULL;
        }
        if (entry->platform != BP_OS_ATARI && entry->platform != BP_META_PORTABLE) {
            ctx->metadata_errors++;
            return NULL;
        }

        mex = (meta_extract_t *)malloc(sizeof(meta_extract_t));
        if (!mex) {
            ctx->metadata_errors++;
            return NULL;
        }

        mex->wctx.buf = mex->buf;
        mex->wctx.cap = (bp_uint32)sizeof(mex->buf);
        mex->wctx.len = 0;
        mex->wctx.pos = 0;

        mex->io.read   = wbuf_read_fn;
        mex->io.write  = wbuf_write_fn;
        mex->io.seek   = wbuf_seek_fn;
        mex->io.tell   = wbuf_tell_fn;
        mex->io.handle = &mex->wctx;

        return &mex->io; /* io is first field; cast back is valid in C89 */
    }

    /* --- Resource fork (skip) ----------------------------------------- */
    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity == VERB_DEFAULT) {
            if (entry->entry_type == BP_TYPE_RSRC)
                printf("   skipping: %s [resource fork - not supported on TOS]\n",
                       local_name);
            else
                printf("   skipping: %s [symbolic links are not supported on TOS]\n",
                       local_name);
        }
        ctx->skip_errors++;
        return NULL;
    }

    if (entry->entry_type != BP_TYPE_DATA) {
        fprintf(stderr, "boltpack: unsupported entry type for %s\n", local_name);
        ctx->skip_errors++;
        return NULL;
    }

    /* --- Regular file ------------------------------------------------- */
    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT)
        printf(" extracting: %s\n", local_name);

    if (ctx->dry_run) {
        if (strlen(entry->name) < sizeof(ctx->subject)) {
            strcpy(ctx->subject, entry->name);
            ctx->subject_ok = 1;
        }
        return bp_archive_dry_run_stream();
    }

    if (!ctx->force) {
        struct stat st_buf;
        if (lstat(path, &st_buf) == 0) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n", path);
            ctx->skip_errors++;
            return NULL;
        }
        if (errno != ENOENT) {
            fprintf(stderr, "boltpack: cannot inspect destination: %s\n", path);
            ctx->skip_errors++;
            return NULL;
        }
    }

    if (mkdirs_for_file(path) != 0) {
        fprintf(stderr, "boltpack: cannot create parent directories for: %s\n",
                path);
        ctx->skip_errors++;
        return NULL;
    }
    staged = (atari_staged_output_t *)calloc(1, sizeof(*staged));
    if (staged == NULL) {
        ctx->skip_errors++;
        return NULL;
    }
    if (strlen(path) >= sizeof(staged->target) ||
        atari_make_temp_template(path, staged->temporary,
                                 sizeof(staged->temporary)) != 0) {
        free(staged);
        ctx->skip_errors++;
        return NULL;
    }
    strcpy(staged->target, path);
    staged->fd = mkstemp(staged->temporary);
    if (staged->fd < 0) {
        fprintf(stderr, "boltpack: cannot create a staging file for: %s\n", path);
        free(staged);
        ctx->skip_errors++;
        return NULL;
    }
    (void)atari_output_root_refresh(ctx);
#ifdef BP_ATARI_TEST_PAUSE_AFTER_EXTRACT_STAGE
    atari_test_pause_after_extract_stage();
#endif
    staged->io.read = NULL;
    staged->io.write = staged_file_write;
    staged->io.seek = NULL;
    staged->io.tell = NULL;
    staged->io.handle = staged;
    return &staged->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;

    /* --- Native and portable metadata ------------------------------- */
    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;
        if (status == BP_OK && ctx->subject_ok && !ctx->dry_run &&
            entry->platform == BP_OS_ATARI &&
            mex->wctx.len == ATARI_META_PAYLOAD) {
            char path[BP_MAX_PATH];
            char local_name[BP_MAX_PATH];
            if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                                 ATARI_CP) != 0) {
                ctx->metadata_errors++;
                free(mex);
                return BP_ERR_PARTIAL;
            }
            if (ctx->out_dir) {
                if (path_join(path, sizeof(path), ctx->out_dir, local_name) != 0) {
                    ctx->metadata_errors++;
                    free(mex);
                    return BP_ERR_PARTIAL;
                }
            } else {
                strcpy(path, local_name);
            }
            ctx->subject_native_seen = 1;
            if (ctx->subject_type == BP_TYPE_DIR) {
                if (ctx->subject_directory != NULL) {
                    ctx->subject_directory->native_seen = 1;
                    ctx->subject_directory->native_attr = mex->buf[0];
                }
            } else if (atari_apply_attrib(path, ATARI_ATTR_MASK,
                                          mex->buf[0]) != 0) {
                fprintf(stderr, "boltpack: cannot restore TOS attributes: %s\n",
                        path);
                ctx->metadata_errors++;
            }
        } else if (status == BP_OK && ctx->subject_ok && !ctx->dry_run &&
                   entry->platform == BP_META_PORTABLE) {
            bp_portable_metadata_t parsed;
            int parse_rc = bp_metadata_parse(mex->buf, (unsigned)mex->wctx.len,
                                             &parsed);
            if (parse_rc == BP_ERR_CORRUPT) {
                free(mex);
                return parse_rc;
            }
            if (!bp_metadata_matches_subject(&parsed, ctx->subject_type)) {
                fprintf(stderr,
                        "boltpack: portable metadata subject type does not match: %s\n",
                        entry->name);
                ctx->metadata_errors++;
                free(mex);
                return BP_ERR_PARTIAL;
            }
            if (parsed.has_fat && !ctx->subject_native_seen) {
                if (ctx->subject_type == BP_TYPE_DIR) {
                    if (ctx->subject_directory != NULL) {
                        ctx->subject_directory->fat_seen = 1;
                        ctx->subject_directory->fat_valid = parsed.fat_valid;
                        ctx->subject_directory->fat_value = parsed.fat_value;
                    }
                } else {
                    char path[BP_MAX_PATH];
                    char local_name[BP_MAX_PATH];
                    if (bp_utf8_to_local(entry->name, local_name,
                                         sizeof(local_name), ATARI_CP) != 0) {
                        ctx->metadata_errors++;
                        free(mex);
                        return BP_ERR_PARTIAL;
                    }
                    if (ctx->out_dir) {
                        if (path_join(path, sizeof(path), ctx->out_dir,
                                      local_name) != 0) {
                            ctx->metadata_errors++;
                            free(mex);
                            return BP_ERR_PARTIAL;
                        }
                    } else {
                        strcpy(path, local_name);
                    }
                    if (parsed.fat_valid != 0 &&
                        atari_apply_attrib(path, parsed.fat_valid,
                                           parsed.fat_value) != 0) {
                        fprintf(stderr,
                                "boltpack: cannot restore portable FAT attributes: %s\n",
                                path);
                        ctx->metadata_errors++;
                    }
                }
            }
            if (parse_rc == BP_ERR_PARTIAL) {
                free(mex);
                return BP_ERR_PARTIAL;
            }
        }
        free(mex);
        return BP_OK;
    }

    /* --- Regular file: verify, timestamp, and commit staged output ------ */
    {
        atari_staged_output_t *staged = (atari_staged_output_t *)out->handle;
        int close_rc = close(staged->fd);
        int commit_rc;
        staged->fd = -1;
        if (close_rc != 0) {
            atari_discard_staged(staged);
            free(staged);
            return status == BP_OK ? BP_ERR_IO : BP_OK;
        }
        if (status != BP_OK) {
            atari_discard_staged(staged);
            free(staged);
            return BP_OK;
        }
        if (!atari_output_root_unchanged(ctx)) {
            fprintf(stderr,
                    "boltpack: output root changed; staged file left in its original directory: %s\n",
                    staged->target);
            ctx->skip_errors++;
            free(staged);
            return BP_ERR_PARTIAL;
        }
        if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
            if (bp_restore_mtime(staged->temporary, entry->mtime) != BP_OK) {
                fprintf(stderr, "boltpack: cannot restore timestamp for: %s\n",
                        staged->target);
                ctx->mtime_errors++;
            }
        }
        commit_rc = atari_commit_staged(staged, ctx->force);
        if (commit_rc != BP_OK && staged->temporary[0] != '\0') {
            fprintf(stderr, "boltpack: cannot commit extracted file: %s\n",
                    staged->target);
            ctx->skip_errors++;
            atari_discard_staged(staged);
            free(staged);
            return commit_rc;
        }
        if (!atari_output_root_refresh(ctx)) {
            ctx->skip_errors++;
            free(staged);
            return BP_ERR_PARTIAL;
        }
        if (strlen(entry->name) < sizeof(ctx->subject)) {
            strcpy(ctx->subject, entry->name);
            ctx->subject_ok = 1;
            ctx->subject_type = entry->entry_type;
            ctx->subject_native_seen = 0;
        } else {
            ctx->subject_ok = 0;
            ctx->metadata_errors++;
        }
        free(staged);
        return commit_rc;
    }
}

/* ---- test callback ----------------------------------------------------- */

typedef struct {
    bp_uint32 ok_count;
    bp_uint32 fail_count;
} test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;

    if (g_verbosity < VERB_SILENT) {
        char disp_name[BP_MAX_PATH];
        if (bp_utf8_to_local(entry->name, disp_name, sizeof(disp_name), ATARI_CP) != 0) strcpy(disp_name, "<unrepresentable>");
        printf("  testing: %s", disp_name);
        if (result == BP_OK) {
            printf(" OK\n");
        } else {
            printf(" FAILED -- %s\n", bp_strerror(result));
        }
    }
    if (result == BP_OK) ctx->ok_count++;
    else                  ctx->fail_count++;
}

/* ---- usage ------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver (Atari TOS)\n"
        "%s\n\n"
        "Usage: boltpack.tos <command> [options] archive.blt [files...]\n\n"
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
        "  -b SIZE   Block size: 8k, 16k, 32k, 64k, 256k (auto-detected)\n"
        "  --interchange-profile-1  Mini-compatible limits for create/add\n"
        "  -0        Store only (no compression)\n"
        "  -1        Use BoltLite compression\n"
        "  -F        Use fluff LZ77+Huffman compression (better ratio than boltlz)\n"
        "  -q        Quiet (summary only)\n"
        "  -qq       Silent (errors only)\n"
        "  -o PATH   Output directory for extraction\n"
        "  -f        Force overwrite existing files\n"
        "  -n        Dry run\n");
    fprintf(stderr,
        "\nFile paths: drive prefixes (e.g. C:) are stripped from archive\n"
        "names.  Atari ST filenames are stored as UTF-8.\n"
        "TOS file attributes (read-only, hidden, system, archive) are\n"
        "preserved in each archive entry.\n");
}

/* ---- os / cpu name helpers (for cmd_info) ------------------------------ */

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
    case BP_OS_DOS:       return "DOS";
    case BP_OS_WINDOWS:   return "Windows";
    case BP_OS_FREEBSD:   return "FreeBSD";
    case BP_OS_NETBSD:    return "NetBSD";
    case BP_OS_OPENBSD:   return "OpenBSD";
    case BP_OS_SOLARIS:   return "Solaris";
    case BP_OS_IRIX:      return "IRIX";
    case BP_OS_HPUX:      return "HP-UX";
    case BP_OS_AIX:       return "AIX";
    case BP_OS_ATARI:     return "Atari ST";
    case BP_OS_RISCOS:    return "RISC OS";
    case BP_OS_OS2:       return "OS/2";
    case BP_OS_BEOS:      return "BeOS";
    default:              return "Unknown";
    }
}

static const char *cpu_name(bp_uint8 cpu)
{
    switch (cpu) {
    case BP_CPU_UNKNOWN:  return "Unknown";
    case BP_CPU_X86:      return "x86";
    case BP_CPU_X86_64:   return "x86-64";
    case BP_CPU_PPC32:    return "PowerPC";
    case BP_CPU_PPC64:    return "PowerPC 64";
    case BP_CPU_M68K:     return "Motorola 68k";
    case BP_CPU_ARM32:    return "ARM";
    case BP_CPU_ARM64:    return "ARM64";
    case BP_CPU_MIPS32:   return "MIPS";
    case BP_CPU_MIPS64:   return "MIPS64";
    default:              return "Unknown";
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

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int interchange_profile)
{
    const char  *archive_name;
    FILE        *fp = NULL;
    bp_io_t      aio;
    bp_archive_t ar;
    atari_staged_output_t stage;
    struct stat  snapshot;
    int          existed;
    int          is_read_only;
    int          archive_open = 0;
    int          rc, i;
    clock_t      start;
    double       elapsed;
    bp_uint64    total_orig = {0, 0};
    bp_uint64    total_comp = {0, 0};
    bp_uint32    file_count = 0;
    bp_uint32    dir_count = 0;
    bp_uint32    j;

    if (argc < 2) {
        fprintf(stderr, "boltpack: create requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    rc = atari_archive_target_state(archive_name, 1, &snapshot, &existed,
                                    &is_read_only);
    if (rc != BP_OK) {
        fprintf(stderr, is_read_only
                ? "boltpack: archive is read-only: %s\n"
                : "boltpack: archive target must be a regular file: %s\n",
                archive_name);
        return 1;
    }
    if (block_size == 0) block_size = atari_auto_block_size();

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    rc = atari_open_archive_stage(archive_name, &stage, &fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot create archive staging file: %s\n",
                bp_strerror(rc));
        return 1;
    }
    bp_io_from_file(&aio, fp);

    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto create_discard;
    }
    archive_open = 1;
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;

    start = clock();

    for (i = 1; i < argc; i++) {
        const char *path    = argv[i];
        const char *stripped = clean_path(path);
        char        arcbuf[BP_MAX_PATH];

        strcpy(arcbuf, stripped);
        normalise_seps(arcbuf);

        rc = add_path(&ar, path, arcbuf);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            goto create_discard;
        }
    }

    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        goto create_discard;
    }

    for (j = 0; j < ar.entries_written; j++) {
        if (ar.entry_list[j].entry_type == BP_TYPE_DIR) dir_count++;
        else if (ar.entry_list[j].entry_type == BP_TYPE_DATA) file_count++;
        total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
        total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
    }
    elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;
    bp_archive_close(&ar);
    archive_open = 0;
    rc = atari_finish_archive_stage(&fp, &stage, &snapshot, existed);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish archive: %s\n",
                bp_strerror(rc));
        atari_discard_staged(&stage);
        return 1;
    }

    if (g_verbosity < VERB_SILENT) {
        char      numbuf[32], speedbuf[64];

        format_size64(total_orig, numbuf, sizeof(numbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)dir_count,  dir_count  == 1 ? "y" : "ies",
               numbuf);
        if (!bp_u64_is_zero(total_orig) && !bp_u64_is_zero(total_comp)) {
            char compbuf[32];
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
            format_size64(total_comp, compbuf, sizeof(compbuf));
            printf(", %.0f%% (%s)", pct, compbuf);
        }
        if (elapsed > 0.0) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            printf(", %.2fs", elapsed);
            format_speed(orig_d / elapsed, speedbuf, sizeof(speedbuf));
            printf(", %s", speedbuf);
        }
        printf("\n");
    }

    return 0;

create_discard:
    if (archive_open) bp_archive_close(&ar);
    if (fp != NULL) {
        (void)fclose(fp);
        fp = NULL;
        stage.fd = -1;
    }
    atari_discard_staged(&stage);
    return 1;
}

static int cmd_list(const char *archive_name)
{
    FILE       *fp;
    bp_io_t     aio;
    list_ctx_t  ctx;
    int         rc;

    fp = fopen(archive_name, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    printf("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");

    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_list(&aio, list_cb, &ctx);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }
    {
        char origbuf[32], compbuf[32];
        format_size64(ctx.total_orig, origbuf, sizeof(origbuf));
        format_size64(ctx.total_comp, compbuf, sizeof(compbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)ctx.file_count,
               ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,
               ctx.dir_count  == 1 ? "y" : "ies",
               origbuf);
        if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
            double orig_d = (double)ctx.total_orig.lo + (double)ctx.total_orig.hi * 4294967296.0;
            double comp_d = (double)ctx.total_comp.lo + (double)ctx.total_comp.hi * 4294967296.0;
            double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
            printf(", %.0f%% (%s)", pct, compbuf);
        }
        printf("\n");
    }
    return 0;
}

typedef struct {
    const char *out_dir;
    bp_atari_tree_t names;
} atari_preflight_t;

static int atari_preflight_entry(const bp_entry_t *entry, void *user_data)
{
    atari_preflight_t *ctx = (atari_preflight_t *)user_data;
    char local[BP_MAX_PATH], projected[BP_MAX_PATH], path[BP_MAX_PATH];
    unsigned int length;
    size_t path_length;
    int rc;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    if (!bp_name_is_safe(entry->name) ||
        bp_utf8_to_local(entry->name, local, sizeof(local), ATARI_CP) != 0) {
        fprintf(stderr, "boltpack: unsafe or unrepresentable Atari name: %s\n", entry->name);
        return 1;
    }
    if (ctx->out_dir) {
        if (path_join(path, sizeof(path), ctx->out_dir, local) != 0) return 1;
    } else strcpy(path, local);
    path_length = strlen(path);
    if (entry->entry_type == BP_TYPE_DIR && path_length &&
        (path[path_length - 1] == '/' || path[path_length - 1] == '\\')) path_length--;
    if (path_length > BP_ATARI_GEMDOS_PATH_MAX ||
        bp_atari_project_name(local, entry->entry_type == BP_TYPE_DIR,
                              projected, sizeof(projected), &length) != BP_OK) {
        fprintf(stderr, "boltpack: name is outside the Atari 8.3 profile: %s\n", local);
        return 1;
    }
    rc = bp_atari_tree_add(&ctx->names, local, projected, length,
                           entry->entry_type == BP_TYPE_DIR);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: Atari destination name collision or index limit: %s\n", local);
        return 1;
    }
    return 0;
}

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE          *fp;
    bp_io_t        aio;
    extract_ctx_t  ctx;
    int            rc;
    clock_t        start;
    double         elapsed;
    atari_preflight_t preflight;

    fp = fopen(archive_name, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp); return 1;
    }
    bp_io_from_file(&aio, fp);
    memset(&preflight, 0, sizeof(preflight));
    preflight.out_dir = out_dir;
    rc = bp_archive_list(&aio, atari_preflight_entry, &preflight);
    bp_atari_tree_free(&preflight.names);
    if (rc != BP_OK || fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "boltpack: archive name preflight failed\n");
        fclose(fp);
        return 1;
    }
    memset(&ctx, 0, sizeof(ctx));
    if (out_dir && !dry_run) {
        if (mkdirs(out_dir) != 0 || lstat(out_dir, &ctx.out_root_stat) != 0 ||
            !S_ISDIR(ctx.out_root_stat.st_mode)) {
            fprintf(stderr, "boltpack: cannot safely inspect output root: %s\n",
                    out_dir);
            fclose(fp);
            return 1;
        }
        ctx.out_root_stat_valid = 1;
    }
    ctx.out_dir = out_dir;
    ctx.force   = force;
    ctx.dry_run = dry_run;

    start = clock();
    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done,
                                g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                &ctx);
    finish_directory_restores(&ctx);
    bp_atari_name_set_free(&ctx.projected_names);
    fclose(fp);

    if (rc == BP_OK &&
        (ctx.mtime_errors || ctx.skip_errors || ctx.metadata_errors))
        rc = BP_ERR_PARTIAL;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }

    elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;
    if (g_verbosity < VERB_SILENT) {
        printf("%lu file%s, %lu director%s",
               (unsigned long)ctx.file_count,
               ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,
               ctx.dir_count  == 1 ? "y" : "ies");
        if (elapsed > 0.0) printf(", %.2fs", elapsed);
        printf("\n");
    }
    return 0;
}

static int cmd_test(const char *archive_name)
{
    FILE       *fp;
    bp_io_t     aio;
    test_ctx_t  ctx;
    int         rc;

    fp = fopen(archive_name, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp); return 1;
    }

    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_test(&aio, test_cb, &ctx);
    fclose(fp);

    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }
    if (g_verbosity < VERB_SILENT) {
        printf("\n%lu file%s OK",
               (unsigned long)ctx.ok_count,
               ctx.ok_count == 1 ? "" : "s");
        if (ctx.fail_count > 0)
            printf(", %lu file%s FAILED",
                   (unsigned long)ctx.fail_count,
                   ctx.fail_count == 1 ? "" : "s");
        printf("\n");
    }
    return ctx.fail_count > 0 ? 1 : 0;
}

static int cmd_info(const char *archive_name)
{
    FILE             *fp;
    bp_io_t           aio;
    bp_archive_info_t info;
    int               rc;

    fp = fopen(archive_name, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_info(&aio, &info);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }
    {
        bp_uint32 file_count = info.entry_count - info.dir_count;
        bp_uint64 total_orig = info.total_original;
        bp_uint64 total_comp = info.total_compressed;
        bp_uint64 arc_size   = info.archive_size;
        char      origbuf[32], compbuf[32], arcbuf[32];

        printf("Archive:        %s\n", archive_name);
        printf("Format version: %u\n", (unsigned)info.version);
        printf("Created by:     BoltPack %u.%u\n",
               (unsigned)info.creator_ver_major,
               (unsigned)info.creator_ver_minor);
        printf("Creator OS:     %s\n", os_name(info.creator_os));
        printf("Creator CPU:    %s\n", cpu_name(info.creator_cpu));
        printf("Method:         %s\n", format_methods(info.method_counts));
        format_size64(arc_size, arcbuf, sizeof(arcbuf));
        printf("Archive size:   %s\n", arcbuf);
        printf("Entries:        %lu (%lu file%s, %lu director%s)\n",
               (unsigned long)info.entry_count,
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)info.dir_count,
               info.dir_count == 1 ? "y" : "ies");
        format_size64(total_orig, origbuf, sizeof(origbuf));
        format_size64(total_comp, compbuf, sizeof(compbuf));
        printf("Original size:  %s\n", origbuf);
        printf("Compressed:     %s", compbuf);
        if (!bp_u64_is_zero(total_orig)) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            printf(" (%.0f%%)", (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0);
        }
        printf("\n");
    }
    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    const char  *archive_name;
    const char **del_names;
    int          del_count;
    FILE        *src_fp = NULL, *dst_fp = NULL;
    bp_io_t      src_io, dst_io;
    atari_staged_output_t stage;
    struct stat  snapshot;
    struct stat  opened;
    int          existed;
    int          is_read_only;
    bp_uint32    deleted = 0;
    int          rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }
    archive_name = argv[0];
    del_names    = (const char **)&argv[1];
    del_count    = argc - 1;

    rc = atari_archive_target_state(archive_name, 0, &snapshot, &existed,
                                    &is_read_only);
    if (rc != BP_OK) {
        fprintf(stderr, is_read_only
                ? "boltpack: archive is read-only: %s\n"
                : "boltpack: archive must be a regular file: %s\n",
                archive_name);
        return 1;
    }
    src_fp = fopen(archive_name, "rb");
    if (!src_fp || fstat(fileno(src_fp), &opened) != 0 ||
        !atari_archive_stat_matches(&snapshot, &opened)) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        if (src_fp != NULL) fclose(src_fp);
        return 1;
    }
    rc = atari_open_archive_stage(archive_name, &stage, &dst_fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot create archive staging file: %s\n",
                bp_strerror(rc));
        fclose(src_fp);
        return 1;
    }

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);

    rc = bp_archive_delete(&src_io, &dst_io, del_names,
                           (bp_uint32)del_count, &deleted);
    if (fclose(src_fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    src_fp = NULL;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        goto delete_discard;
    }
    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        goto delete_discard;
    }
    rc = atari_finish_archive_stage(&dst_fp, &stage, &snapshot, existed);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish deleted archive: %s\n",
                bp_strerror(rc));
        atari_discard_staged(&stage);
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    return 0;

delete_discard:
    if (src_fp != NULL) fclose(src_fp);
    if (dst_fp != NULL) {
        fclose(dst_fp);
        stage.fd = -1;
    }
    atari_discard_staged(&stage);
    return 1;
}

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int interchange_profile)
{
    const char  *archive_name;
    FILE        *src_fp = NULL;
    FILE        *dst_fp = NULL;
    bp_io_t      dst_io;
    bp_archive_t ar;
    atari_staged_output_t stage;
    struct stat  snapshot;
    struct stat  opened;
    int          existed;
    int          is_read_only;
    int          archive_open = 0;
    int          rc, i;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    rc = atari_archive_target_state(archive_name, 0, &snapshot, &existed,
                                    &is_read_only);
    if (rc != BP_OK) {
        fprintf(stderr, is_read_only
                ? "boltpack: archive is read-only: %s\n"
                : "boltpack: archive must be a regular file: %s\n",
                archive_name);
        return 1;
    }
    if (block_size == 0) block_size = atari_auto_block_size();

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    src_fp = fopen(archive_name, "rb");
    if (!src_fp || fstat(fileno(src_fp), &opened) != 0 ||
        !atari_archive_stat_matches(&snapshot, &opened)) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        if (src_fp != NULL) fclose(src_fp);
        return 1;
    }
    rc = atari_open_archive_stage(archive_name, &stage, &dst_fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot create archive staging file: %s\n",
                bp_strerror(rc));
        fclose(src_fp);
        return 1;
    }
    rc = atari_copy_archive_stream(src_fp, dst_fp);
    if (fclose(src_fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    src_fp = NULL;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot copy existing archive: %s\n",
                bp_strerror(rc));
        goto add_discard;
    }
    bp_io_from_file(&dst_io, dst_fp);
    rc = bp_archive_open_append(&ar, &dst_io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto add_discard;
    }
    archive_open = 1;
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            goto add_discard;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }

    for (i = 1; i < argc; i++) {
        const char *path     = argv[i];
        const char *stripped = clean_path(path);
        char        arcbuf[BP_MAX_PATH];

        strcpy(arcbuf, stripped);
        normalise_seps(arcbuf);

        rc = add_path(&ar, path, arcbuf);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            goto add_discard;
        }
    }

    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    archive_open = 0;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        goto add_discard;
    }
    rc = atari_finish_archive_stage(&dst_fp, &stage, &snapshot, existed);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish updated archive: %s\n",
                bp_strerror(rc));
        atari_discard_staged(&stage);
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    return 0;

add_discard:
    if (archive_open) bp_archive_close(&ar);
    if (src_fp != NULL) fclose(src_fp);
    if (dst_fp != NULL) {
        fclose(dst_fp);
        stage.fd = -1;
    }
    atari_discard_staged(&stage);
    return 1;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char       command;
    bp_uint32  block_size = 0; /* 0 = auto-detect via Malloc(-1L) */
    bp_uint8   method    = BP_METHOD_BOLTLZ;
    const char *out_dir  = NULL;
    int        force     = 0;
    int        dry_run   = 0;
    int        interchange_profile = 0;
    int        codec_given = 0;
    int        i;

    if (argc < 3) { usage(); return 1; }

    if (strlen(argv[1]) != 1) {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]);
        usage(); return 1;
    }
    command = argv[1][0];

    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-b") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: -b requires an argument\n");
                return 1;
            }
            block_size = parse_block_size(argv[i + 1]);
            if (block_size == 0) {
                fprintf(stderr, "boltpack: invalid block size: %s\n",
                        argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--interchange-profile-1") == 0) {
            interchange_profile = 1; i++;
        } else if (strcmp(argv[i], "-0") == 0) {
            method = BP_METHOD_STORED; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-1") == 0) {
            method = BP_METHOD_BOLTLITE; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-F") == 0) {
            method = BP_METHOD_FLUFF; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-qq") == 0) {
            g_verbosity = VERB_SILENT; i++;
        } else if (strcmp(argv[i], "-q") == 0) {
            g_verbosity = VERB_QUIET; i++;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: -o requires an argument\n");
                return 1;
            }
            out_dir = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "-f") == 0) {
            force = 1; i++;
        } else if (strcmp(argv[i], "-n") == 0) {
            dry_run = 1; i++;
        } else {
            fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]);
            usage(); return 1;
        }
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
    case 'l':
        if (i >= argc) {
            fprintf(stderr, "boltpack: list requires archive name\n"); return 1;
        }
        return cmd_list(argv[i]);
    case 'x':
        if (i >= argc) {
            fprintf(stderr, "boltpack: extract requires archive name\n"); return 1;
        }
        return cmd_extract(argv[i], out_dir, force, dry_run);
    case 't':
        if (i >= argc) {
            fprintf(stderr, "boltpack: test requires archive name\n"); return 1;
        }
        return cmd_test(argv[i]);
    case 'i':
        if (i >= argc) {
            fprintf(stderr, "boltpack: info requires archive name\n"); return 1;
        }
        return cmd_info(argv[i]);
    case 'd': return cmd_delete(argc - i, argv + i);
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage(); return 1;
    }
}
