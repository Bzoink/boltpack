/*
 * main.c -- macOS / Mac OS X CLI frontend for BoltPack
 *
 * Target:   macOS 10.6 Snow Leopard and later (ARM64 primary, x86_64 secondary)
 * Compiler: Apple Clang (Xcode) or arm64-apple-macosx-gcc cross-compiler
 *
 * POSIX I/O + directory recursion are identical to linux/main.c.
 * This frontend additionally:
 *
 *   On CREATE:
 *   - Adds a BP_TYPE_META entry (platform=BP_OS_MACOSX, Unix-family 2-byte
 *     mode) immediately after each BP_TYPE_DATA or BP_TYPE_DIR entry.
 *   - If the file has a non-empty resource fork, adds a BP_TYPE_RSRC entry
 *     immediately after the BP_TYPE_DATA entry (before the META entry).
 *   - If the file has Finder info (Type, Creator, flags), adds a second
 *     BP_TYPE_META entry (platform=BP_OS_MACOS9, Classic Mac format) after
 *     the Unix-mode META entry.
 *
 *   On EXTRACT:
 *   - BP_TYPE_RSRC: restores resource fork via setxattr(XATTR_RESOURCEFORK).
 *   - BP_TYPE_META platform=BP_OS_MACOSX: restores Unix mode via chmod().
 *   - BP_TYPE_META platform=BP_OS_MACOS9: restores Finder info via
 *     setxattr("com.apple.FinderInfo").
 *
 * Metadata formats (from format.md):
 *
 *   Unix-family mode meta (platform = BP_OS_MACOSX = 0x03):
 *     Offset  Size  Field
 *     0       2     mode  Unix permissions (low 9 bits of mode_t)
 *
 *   Classic Mac meta (platform = BP_OS_MACOS9 = 0x02):
 *     Offset  Size  Field
 *     0       4     type         Finder Type code
 *     4       4     creator      Finder Creator code
 *     8       2     finder_flags Finder flags (uint16 LE)
 *     10      4     crdate       Creation date (Mac epoch = since 1904-01-01)
 *     14      4     mdate        Modification date (Mac epoch)
 *     Total: 18 bytes
 *
 * For Intel Mac builds: -DBP_CREATOR_CPU=BP_CPU_X86_64
 *
 * Cross-compilation from Linux:
 *   arm64-apple-macosx11-clang -ansi -pedantic -Wall -Wextra -O2 \
 *       -I src/core                           \
 *       src/core/compress.c src/core/archiver.c                  \
 *       src/core/bp_codepage.c                                   \
 *       src/frontend/macosx/main.c                               \
 *       src/core/boltlz.c                               \
 *       -o boltpack
 *
 * ANSI C (C89/C90) throughout (with POSIX + Apple extensions).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _POSIX_C_SOURCE 200112L

#define BP_CREATOR_OS  BP_OS_MACOSX
#ifndef BP_CREATOR_CPU
#define BP_CREATOR_CPU BP_CPU_ARM64
#endif

#include "../../core/archiver.h"
#include "../../core/bp_time.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"
#ifdef BOLTPACK_ZSTD
#include "../../core/zstd_wrap.h"
#endif
#ifdef BOLTPACK_NET
#include "../common/bp_net_cli.h"
#endif

#include <sys/xattr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <utime.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

/* Some C library headers hide this POSIX declaration under strict C89 mode. */
extern int mkstemp(char *template_path);

/* ---- macOS metadata sizes --------------------------------------------- */

/* Resource fork: limit per-file to 16 MB (in practice much smaller) */
#define MACOS_RSRC_MAX    (16 * 1024 * 1024)

/* Unix-family mode meta: 2 bytes */
#define MACOS_MODE_META_SIZE  2

/* Classic Mac meta: Type(4) + Creator(4) + flags(2) + crdate(4) + mdate(4) */
#define MACOS_CLASSIC_META_SIZE  18

/* Buffer large enough for Classic Mac meta */
#define MACOS_META_BUF_SIZE      (MACOS_CLASSIC_META_SIZE + 4)

/* macOS Finder info xattr name and size */
#define APPLE_FINDERINFO_XATTR  "com.apple.FinderInfo"
#define APPLE_FINDERINFO_SIZE   32

/* Mac epoch: seconds between 1904-01-01 and 1970-01-01 */
#define MAC_EPOCH_DELTA  2082844800UL

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

/* Core diagnostic callback: print archiver log messages to stderr. */
static void cli_log_stderr(const char *msg, void *user_data)
{
    (void)user_data;
    fprintf(stderr, "%s\n", msg);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseeko((FILE *)handle, (off_t)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftello((FILE *)handle);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

/* Create an exclusive temporary archive beside target so rename stays on the
 * same filesystem.  mkstemp starts at mode 0600; callers restore the archive's
 * previous mode, or the normal create mode filtered by the process umask. */
static int make_archive_stage(const char *target, char *stage, size_t cap)
{
    static const char suffix[] = ".bp_tmp.XXXXXX";
    size_t n = strlen(target);
    if (n + sizeof(suffix) > cap) { errno = ENAMETOOLONG; return -1; }
    memcpy(stage, target, n);
    memcpy(stage + n, suffix, sizeof(suffix));
    return mkstemp(stage);
}

static int archive_mode_for_stage(const char *target, mode_t *mode,
                                  struct stat *snapshot, int must_exist)
{
    if (lstat(target, snapshot) == 0) {
        if (!S_ISREG(snapshot->st_mode)) { errno = EINVAL; return -1; }
        *mode = snapshot->st_mode & 07777;
        return 0;
    }
    if (errno != ENOENT || must_exist) return -1;
    {
        mode_t mask = umask(0);
        (void)umask(mask);
        *mode = (mode_t)(0666 & ~mask);
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return 0;
}

static int archive_target_unchanged(const char *target,
                                    const struct stat *snapshot)
{
    struct stat now;
    if (lstat(target, &now) != 0) return 0;
    return S_ISREG(now.st_mode) && now.st_dev == snapshot->st_dev &&
           now.st_ino == snapshot->st_ino && now.st_size == snapshot->st_size &&
           now.st_mtime == snapshot->st_mtime &&
           now.st_ctime == snapshot->st_ctime;
}

static int copy_archive_stream(FILE *src, FILE *dst)
{
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) != 0) {
        if (fwrite(buf, 1, n, dst) != n) return -1;
    }
    return ferror(src) ? -1 : 0;
}

/* ---- read-only memory buffer I/O (for metadata payloads) -------------- */

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
    return -1;
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

/* ---- write-only memory buffer I/O (for receiving metadata on extract) - */

typedef struct {
    unsigned char *buf;
    bp_uint32      cap;
    bp_uint32      len;
    bp_uint32      pos;
} wbuf_ctx_t;

static long wbuf_read_fn(void *handle, void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1;
}

static long wbuf_write_fn(void *handle, const void *data, bp_uint32 count)
{
    wbuf_ctx_t *ctx = (wbuf_ctx_t *)handle;
    if (ctx->pos + count > ctx->cap) count = ctx->cap - ctx->pos;
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
 * meta_extract_t: captures metadata payload bytes during extraction.
 * io MUST be first so that (meta_extract_t *)out_io is valid.
 */
#define META_EXTRACT_BUF  (MACOS_CLASSIC_META_SIZE + 4)

typedef struct {
    bp_io_t       io;                      /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[META_EXTRACT_BUF];
} meta_extract_t;

/*
 * rsrc_extract_t: captures resource fork bytes during extraction.
 * io MUST be first.
 * Resource forks are variable-length so we use a malloc'd buffer.
 */
typedef struct {
    bp_io_t        io;                     /* MUST be first */
    unsigned char *buf;
    bp_uint32      cap;
    bp_uint32      len;
    bp_uint32      pos;
} rsrc_extract_t;

static long rsrc_write_fn(void *handle, const void *data, bp_uint32 count)
{
    rsrc_extract_t *ctx = (rsrc_extract_t *)handle;
    unsigned char  *newbuf;
    bp_uint32       need;

    if (ctx->pos + count > ctx->cap) {
        need = ctx->pos + count;
        if (need < ctx->cap * 2) need = ctx->cap * 2;
        newbuf = (unsigned char *)realloc(ctx->buf, (size_t)need);
        if (!newbuf) return -1;
        ctx->buf = newbuf;
        ctx->cap = need;
    }
    memcpy(ctx->buf + ctx->pos, data, (size_t)count);
    ctx->pos += count;
    if (ctx->pos > ctx->len) ctx->len = ctx->pos;
    return (long)count;
}

static long rsrc_read_fn(void *handle, void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1;
}

static int rsrc_seek_fn(void *handle, bp_off_t offset, int origin)
{
    rsrc_extract_t *ctx = (rsrc_extract_t *)handle;
    long newpos;
    if (origin == SEEK_SET)       newpos = (long)offset;
    else if (origin == SEEK_CUR)  newpos = (long)ctx->pos + (long)offset;
    else if (origin == SEEK_END)  newpos = (long)ctx->len + (long)offset;
    else return -1;
    if (newpos < 0) return -1;
    ctx->pos = (bp_uint32)newpos;
    return 0;
}

static bp_off_t rsrc_tell_fn(void *handle)
{
    return (bp_off_t)((rsrc_extract_t *)handle)->pos;
}

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
    case BP_ERR_PARTIAL:           return "partial preservation";
    default:                       return "unknown error";
    }
}

/* ---- size / speed / number formatting --------------------------------- */

static void format_speed(double bytes_per_sec, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes_per_sec < 1024.0)
        sprintf(buf, "%.0f bytes/s", bytes_per_sec);
    else if (bytes_per_sec < 1048576.0)
        sprintf(buf, "%.2f KB/s", bytes_per_sec / 1024.0);
    else
        sprintf(buf, "%.2f MB/s", bytes_per_sec / 1048576.0);
}

static void format_size(bp_uint32 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes < 1024UL)
        sprintf(buf, "%lu bytes", (unsigned long)bytes);
    else if (bytes < 1048576UL)
        sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1073741824UL)
        sprintf(buf, "%.1f MB", (double)bytes / 1048576.0);
    else
        sprintf(buf, "%.2f GB", (double)bytes / 1073741824.0);
}

static void format_size64(bp_uint64 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes.hi == 0) {
        format_size(bytes.lo, buf, bufsize);
    } else {
        double d = (double)bytes.lo + (double)bytes.hi * 4294967296.0;
        if (d < 1048576.0)         sprintf(buf, "%.1f KB", d / 1024.0);
        else if (d < 1073741824.0) sprintf(buf, "%.1f MB", d / 1048576.0);
        else                       sprintf(buf, "%.2f GB", d / 1073741824.0);
    }
}

static void format_number(bp_uint32 n, char *buf, size_t bufsize)
{
    char tmp[32];
    int len, i, j, commas;
    sprintf(tmp, "%lu", (unsigned long)n);
    len = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > bufsize) { strcpy(buf, tmp); return; }
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
    (void)p; (void)user_data;
    return 0;
}

/* ---- path utilities ---------------------------------------------------- */

static int ends_with_slash(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && s[len - 1] == '/';
}

static int path_join(char *buf, size_t bufsize, const char *dir,
                     const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    int slash = !ends_with_slash(dir);
    if (dlen + (size_t)slash + nlen + 1 > bufsize)
        return BP_ERR_NAME_TOO_LONG;
    memcpy(buf, dir, dlen);
    if (slash) buf[dlen++] = '/';
    memcpy(buf + dlen, name, nlen + 1);
    return BP_OK;
}

static const char *clean_path(const char *path)
{
    while (path[0] == '.' && path[1] == '/') path += 2;
    return path;
}

/* ---- mkdir -p ---------------------------------------------------------- */

static int mkdirs(const char *path)
{
    char *tmp = (char *)malloc(strlen(path) + 1);
    char *p;
    if (tmp == NULL) return -1;
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
    free(tmp);
    return 0;
}

static int mkdirs_for_file(const char *path)
{
    char *tmp = (char *)malloc(strlen(path) + 1);
    char *slash;
    if (tmp == NULL) return -1;
    strcpy(tmp, path);
    slash = strrchr(tmp, '/');
    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        if (mkdirs(tmp) != 0) { free(tmp); return -1; }
    }
    free(tmp);
    return 0;
}

/* ---- block size parser ------------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (strcmp(s, "8k") == 0  || strcmp(s, "8K") == 0)   return BP_BLOCK_8K;
    if (strcmp(s, "16k") == 0 || strcmp(s, "16K") == 0)  return BP_BLOCK_16K;
    if (strcmp(s, "32k") == 0 || strcmp(s, "32K") == 0)  return BP_BLOCK_32K;
    if (strcmp(s, "64k") == 0 || strcmp(s, "64K") == 0)  return BP_BLOCK_64K;
    if (strcmp(s, "256k") == 0|| strcmp(s, "256K") == 0) return BP_BLOCK_256K;
    return 0;
}

static char parse_command_name(const char *s)
{
    if (s == NULL || s[0] == '\0') return '\0';
    if (s[1] == '\0') return s[0];
    if (strcmp(s, "create") == 0) return 'c';
    if (strcmp(s, "add") == 0) return 'a';
    if (strcmp(s, "extract") == 0) return 'x';
    if (strcmp(s, "list") == 0) return 'l';
    if (strcmp(s, "test") == 0) return 't';
    if (strcmp(s, "info") == 0) return 'i';
    if (strcmp(s, "delete") == 0) return 'd';
#ifdef BOLTPACK_NET
    {
        char nc = bp_net_cli_command(s);
        if (nc) return nc;
    }
#endif
    return '\0';
}

/* ---- macOS metadata helpers ------------------------------------------- */

/*
 * macos_encode_mode_meta: encode Unix-family 2-byte mode payload.
 */
static void macos_encode_mode_meta(unsigned char *buf, bp_uint16 mode)
{
    buf[0] = (unsigned char)(mode & 0xFF);
    buf[1] = (unsigned char)((mode >> 8) & 0xFF);
}

/*
 * macos_encode_classic_meta: encode Classic Mac 18-byte metadata payload.
 *   buf[0..3]   = type code (4 bytes, big-endian as text, stored as-is)
 *   buf[4..7]   = creator code
 *   buf[8..9]   = finder_flags (uint16 LE)
 *   buf[10..13] = crdate (uint32 LE, Mac epoch)
 *   buf[14..17] = mdate  (uint32 LE, Mac epoch)
 */
static void macos_encode_classic_meta(unsigned char *buf,
                                      const unsigned char *fi_raw,
                                      bp_uint32 crdate_mac,
                                      bp_uint32 mdate_mac)
{
    /* Copy Type (4 bytes at fi_raw[0..3]) */
    buf[0] = fi_raw[0]; buf[1] = fi_raw[1];
    buf[2] = fi_raw[2]; buf[3] = fi_raw[3];
    /* Copy Creator (4 bytes at fi_raw[4..7]) */
    buf[4] = fi_raw[4]; buf[5] = fi_raw[5];
    buf[6] = fi_raw[6]; buf[7] = fi_raw[7];
    /* Finder flags (2 bytes at fi_raw[8..9] — already uint16 big-endian;
       store as little-endian per format.md) */
    {
        unsigned int flags = ((unsigned int)fi_raw[8] << 8) | fi_raw[9];
        buf[8] = (unsigned char)(flags & 0xFF);
        buf[9] = (unsigned char)((flags >> 8) & 0xFF);
    }
    /* crdate uint32 LE */
    buf[10] = (unsigned char)(crdate_mac & 0xFF);
    buf[11] = (unsigned char)((crdate_mac >>  8) & 0xFF);
    buf[12] = (unsigned char)((crdate_mac >> 16) & 0xFF);
    buf[13] = (unsigned char)((crdate_mac >> 24) & 0xFF);
    /* mdate uint32 LE */
    buf[14] = (unsigned char)(mdate_mac & 0xFF);
    buf[15] = (unsigned char)((mdate_mac >>  8) & 0xFF);
    buf[16] = (unsigned char)((mdate_mac >> 16) & 0xFF);
    buf[17] = (unsigned char)((mdate_mac >> 24) & 0xFF);
}

/*
 * macos_decode_mode_meta: extract Unix mode from 2-byte buffer.
 */
static bp_uint16 macos_decode_mode_meta(const unsigned char *buf, int len)
{
    if (len < 2) return 0;
    return (bp_uint16)((unsigned int)buf[0] | ((unsigned int)buf[1] << 8));
}

/*
 * macos_decode_classic_meta: extract Finder info from 18-byte buffer.
 * fi_out must be at least APPLE_FINDERINFO_SIZE (32) bytes; unused bytes
 * are zeroed so setxattr() doesn't leave garbage.
 */
static int macos_decode_classic_meta(const unsigned char *buf, int len,
                                     unsigned char *fi_out)
{
    unsigned int flags;
    bp_uint32 crdate_mac, mdate_mac;
    if (len < MACOS_CLASSIC_META_SIZE) return -1;
    memset(fi_out, 0, APPLE_FINDERINFO_SIZE);
    /* Type at fi_out[0..3] */
    fi_out[0] = buf[0]; fi_out[1] = buf[1];
    fi_out[2] = buf[2]; fi_out[3] = buf[3];
    /* Creator at fi_out[4..7] */
    fi_out[4] = buf[4]; fi_out[5] = buf[5];
    fi_out[6] = buf[6]; fi_out[7] = buf[7];
    /* Flags at fi_out[8..9] — stored LE, Finder wants big-endian */
    flags = (unsigned int)buf[8] | ((unsigned int)buf[9] << 8);
    fi_out[8] = (unsigned char)((flags >> 8) & 0xFF);
    fi_out[9] = (unsigned char)(flags & 0xFF);
    /* Dates not written back into FinderInfo xattr — Finder manages them */
    (void)crdate_mac; (void)mdate_mac;
    return 0;
}

/*
 * macos_has_meaningful_finder_info: return non-zero if the raw 32-byte
 * FinderInfo xattr contains non-trivial data worth preserving (non-zero
 * type, creator, or Finder flags).
 */
static int macos_has_meaningful_finder_info(const unsigned char *fi_raw)
{
    int i;
    /* Type code [0..3], creator [4..7], and flags [8..9]. */
    for (i = 0; i < 10; i++) {
        if (fi_raw[i] != 0) return 1;
    }
    return 0;
}

static bp_uint32 macos_unix_to_mac_time(time_t unix_time)
{
    unsigned long seconds;
    if (unix_time < (time_t)0) {
        fprintf(stderr,
                "boltpack: warning: pre-1970 Classic Mac date cannot be stored\n");
        return 0;
    }
    seconds = (unsigned long)unix_time;
    if (seconds > 0xFFFFFFFFUL - MAC_EPOCH_DELTA) {
        fprintf(stderr,
                "boltpack: warning: Classic Mac date is outside its 32-bit range\n");
        return 0;
    }
    return (bp_uint32)(seconds + MAC_EPOCH_DELTA);
}

static bp_uint32 macos_stat_creation_time(const struct stat *st)
{
#if defined(st_birthtime)
    return macos_unix_to_mac_time(st->st_birthtime);
#elif defined(st_birthtimespec)
    return macos_unix_to_mac_time(st->st_birthtimespec.tv_sec);
#else
    return macos_unix_to_mac_time(st->st_mtime);
#endif
}

/* Format 6 stores unsigned UTC seconds. Negative and host-unrepresentable
 * values are encoded as unknown rather than narrowed or wrapped. */
static bp_uint64 macos_archive_mtime(time_t value, bp_uint16 *flags)
{
    bp_off_t seconds;
    if (value < (time_t)0) {
        fprintf(stderr,
                "boltpack: warning: pre-1970 modification time cannot be stored\n");
        return bp_u64_from_u32(0);
    }
    seconds = (bp_off_t)value;
    if (seconds < 0 || (time_t)seconds != value) {
        fprintf(stderr,
                "boltpack: warning: modification time is outside the archive range\n");
        return bp_u64_from_u32(0);
    }
    *flags |= BP_ENTRY_FLAG_MTIME_VALID;
    return bp_off_to_u64(seconds);
}

/* ---- add meta / rsrc entries after a data entry ----------------------- */

static int add_mode_meta_entry(bp_archive_t *ar, const char *arcname,
                               bp_uint16 mode)
{
    unsigned char meta_buf[MACOS_MODE_META_SIZE];
    bp_io_t    meta_io;
    rbuf_ctx_t rctx;

    macos_encode_mode_meta(meta_buf, mode);

    rctx.buf  = meta_buf;
    rctx.size = MACOS_MODE_META_SIZE;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry_ex(ar, arcname, BP_TYPE_META, BP_OS_MACOSX,
                                &meta_io,
                                bp_u64_from_u32(MACOS_MODE_META_SIZE),
                                bp_u64_from_u32(0), 0);
}

static int add_classic_meta_entry(bp_archive_t *ar, const char *arcname,
                                  const unsigned char *fi_raw,
                                  bp_uint32 crdate_mac, bp_uint32 mdate_mac)
{
    unsigned char meta_buf[MACOS_CLASSIC_META_SIZE];
    bp_io_t    meta_io;
    rbuf_ctx_t rctx;

    macos_encode_classic_meta(meta_buf, fi_raw, crdate_mac, mdate_mac);

    rctx.buf  = meta_buf;
    rctx.size = MACOS_CLASSIC_META_SIZE;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry_ex(ar, arcname, BP_TYPE_META, BP_OS_MACOS9,
                                &meta_io,
                                bp_u64_from_u32(MACOS_CLASSIC_META_SIZE),
                                bp_u64_from_u32(0), 0);
}

/* ---- recursive add ----------------------------------------------------- */

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int add_directory(bp_archive_t *ar, const char *dir_path,
                         const char *arc_prefix)
{
    DIR *d;
    struct dirent *ent;
    int rc;
    char child_path[4096];
    char child_arc[4096];

    d = opendir(dir_path);
    if (d == NULL) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", dir_path);
        return BP_ERR_IO;
    }

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        rc = path_join(child_path, sizeof(child_path), dir_path, ent->d_name);
        if (rc == BP_OK)
            rc = path_join(child_arc, sizeof(child_arc), arc_prefix, ent->d_name);
        if (rc != BP_OK) { closedir(d); return rc; }
        rc = add_path(ar, child_path, child_arc);
        if (rc != BP_OK) { closedir(d); return rc; }
    }
    closedir(d);
    return BP_OK;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct stat st;
    bp_uint16 flags;
    bp_uint64 mtime;
    int rc;

    if (lstat(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        char dir_arc[4096];
        if (!ends_with_slash(arcname))
            snprintf(dir_arc, sizeof(dir_arc), "%s/", arcname);
        else
            snprintf(dir_arc, sizeof(dir_arc), "%s", arcname);

        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", dir_arc);

        flags = 0;
        mtime = macos_archive_mtime(st.st_mtime, &flags);
        rc = bp_archive_add_entry_ex(ar, dir_arc, BP_TYPE_DIR, BP_CREATOR_OS,
                                     NULL, bp_u64_from_u32(0), mtime, flags);
        if (rc != BP_OK) return rc;

        /* Store Unix mode for directories too */
        rc = add_mode_meta_entry(ar, dir_arc, (bp_uint16)(st.st_mode & 0x1FF));
        if (rc != BP_OK) return rc;

        return add_directory(ar, path, dir_arc);

    } else if (S_ISREG(st.st_mode)) {
        FILE      *fp;
        bp_io_t    fio;
        bp_uint64  file_size = bp_off_to_u64((bp_off_t)st.st_size);

        /* -- Data fork ------------------------------------------------- */
        fp = fopen(path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", path);
            return BP_ERR_IO;
        }
        bp_io_from_file(&fio, fp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("compressing: %s", arcname);
            fflush(stdout);
        }

        flags = 0;
        mtime = macos_archive_mtime(st.st_mtime, &flags);
        rc = bp_archive_add_entry_ex(ar, arcname, BP_TYPE_DATA, BP_CREATOR_OS,
                                     &fio, file_size, mtime, flags);
        if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            return rc;
        }

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e = &ar->entry_list[ar->entries_written - 1];
            bp_uint64 comp = e->compressed_size;
            if (bp_u64_is_zero(comp) || e->method == BP_METHOD_STORED
                || bp_u64_gte(comp, file_size)) {
                char sizebuf[32];
                format_size64(file_size, sizebuf, sizeof(sizebuf));
                printf("  %s (stored)\n", sizebuf);
            } else {
                double orig = (double)file_size.lo + (double)file_size.hi * 4294967296.0;
                double cmp  = (double)comp.lo      + (double)comp.hi      * 4294967296.0;
                double pct  = (orig > 0.0) ? (cmp / orig * 100.0) : 0.0;
                char sizebuf[32];
                format_size64(comp, sizebuf, sizeof(sizebuf));
                printf("  %s (%.0f%%)\n", sizebuf, pct);
            }
        }

        /* -- Resource fork --------------------------------------------- */
#ifdef __APPLE__
        {
            ssize_t rsrc_sz = getxattr(path, XATTR_RESOURCEFORK_NAME,
                                       NULL, 0, 0, XATTR_NOFOLLOW);
            if (rsrc_sz > 0 && rsrc_sz <= MACOS_RSRC_MAX) {
                unsigned char *rsrc_buf = (unsigned char *)malloc((size_t)rsrc_sz);
                ssize_t got;
                if (rsrc_buf == NULL) return BP_ERR_NOMEM;
                got = getxattr(path, XATTR_RESOURCEFORK_NAME,
                               rsrc_buf, (size_t)rsrc_sz,
                               0, XATTR_NOFOLLOW);
                if (got != rsrc_sz) {
                    free(rsrc_buf);
                    fprintf(stderr,
                            "boltpack: cannot read resource fork completely: %s\n",
                            path);
                    return BP_ERR_IO;
                }
                {
                    bp_io_t rsrc_io;
                    rbuf_ctx_t rctx;
                    rctx.buf = rsrc_buf;
                    rctx.size = (bp_uint32)rsrc_sz;
                    rctx.pos = 0;
                    bp_io_from_rbuf(&rsrc_io, &rctx);
                    rc = bp_archive_add_entry_ex(ar, arcname,
                            BP_TYPE_RSRC, BP_CREATOR_OS, &rsrc_io,
                            bp_u64_from_u32((bp_uint32)rsrc_sz),
                            bp_u64_from_u32(0), 0);
                    if (rc != BP_OK) {
                        free(rsrc_buf);
                        return rc;
                    }
                }
                if (g_verbosity == VERB_DEFAULT) {
                    char sizebuf[32];
                    format_size((bp_uint32)rsrc_sz, sizebuf, sizeof(sizebuf));
                    printf("   rsrc fork: %s  %s\n", arcname, sizebuf);
                }
                free(rsrc_buf);
            } else if (rsrc_sz > MACOS_RSRC_MAX) {
                fprintf(stderr,
                        "boltpack: warning: resource fork exceeds %u-byte capture limit: %s\n",
                        (unsigned int)MACOS_RSRC_MAX, path);
            } else if (rsrc_sz < 0 && errno != ENOATTR && errno != ENOTSUP) {
                fprintf(stderr,
                        "boltpack: warning: cannot inspect resource fork for %s\n",
                        path);
            }
        }
#endif /* __APPLE__ */

        /* -- Unix mode meta -------------------------------------------- */
        rc = add_mode_meta_entry(ar, arcname,
                                 (bp_uint16)(st.st_mode & 0x1FF));
        if (rc != BP_OK) return rc;

        /* -- Finder info (Classic Mac meta) ----------------------------- */
#ifdef __APPLE__
        {
            unsigned char fi_raw[APPLE_FINDERINFO_SIZE];
            ssize_t fi_sz = getxattr(path, APPLE_FINDERINFO_XATTR,
                                     fi_raw, sizeof(fi_raw), 0, XATTR_NOFOLLOW);
            if (fi_sz == APPLE_FINDERINFO_SIZE
                && macos_has_meaningful_finder_info(fi_raw)) {
                bp_uint32 crdate_mac, mdate_mac;
                crdate_mac = macos_stat_creation_time(&st);
                mdate_mac  = macos_unix_to_mac_time(st.st_mtime);
                rc = add_classic_meta_entry(ar, arcname, fi_raw,
                                            crdate_mac, mdate_mac);
                if (rc != BP_OK) return rc;
            } else if (fi_sz < 0 && errno != ENOATTR && errno != ENOTSUP) {
                fprintf(stderr,
                        "boltpack: warning: cannot inspect Finder info for %s\n",
                        path);
            } else if (fi_sz > 0 && fi_sz != APPLE_FINDERINFO_SIZE) {
                fprintf(stderr,
                        "boltpack: warning: unsupported Finder info size for %s\n",
                        path);
            }
        }
#endif /* __APPLE__ */
        return BP_OK;

    } else if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t tlen;
        bp_io_t dummy_io;
        FILE *tmp;

        tlen = readlink(path, target, sizeof(target) - 1);
        if (tlen < 0) {
            fprintf(stderr, "boltpack: cannot read symlink: %s\n", path);
            return BP_ERR_IO;
        }
        target[tlen] = '\0';

        tmp = tmpfile();
        if (tmp == NULL) return BP_ERR_IO;
        fwrite(target, 1, (size_t)tlen, tmp);
        rewind(tmp);
        bp_io_from_file(&dummy_io, tmp);

        if (g_verbosity == VERB_DEFAULT)
            printf("    linking: %s -> %s\n", arcname, target);

        flags = 0;
        mtime = macos_archive_mtime(st.st_mtime, &flags);
        rc = bp_archive_add_entry_ex(ar, arcname, BP_TYPE_SYMLINK,
                                     BP_CREATOR_OS, &dummy_io,
                                     bp_u64_from_u32((bp_uint32)tlen),
                                     mtime, flags);
        if (fclose(tmp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        if (rc != BP_OK)
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
        return rc;
    }

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
    char orig_buf[32], comp_buf[32];

    switch (entry->entry_type) {
    case BP_TYPE_DATA:    type_str = "DATA"; break;
    case BP_TYPE_RSRC:    type_str = "RSRC"; break;
    case BP_TYPE_META:    type_str = "META"; break;
    case BP_TYPE_SYMLINK: type_str = "LINK"; break;
    case BP_TYPE_DIR:     type_str = "DIR "; break;
    default:              type_str = "??? "; break;
    }

    if (entry->entry_type == BP_TYPE_DIR)
        ctx->dir_count++;
    else if (entry->entry_type == BP_TYPE_DATA ||
             entry->entry_type == BP_TYPE_SYMLINK)
        ctx->file_count++;
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);

    format_number(bp_u64_to_u32(entry->original_size),  orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_u32(entry->compressed_size), comp_buf, sizeof(comp_buf));
    printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, entry->name);
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct {
    char       *path;
    bp_uint16   mode;
    int         mode_valid;
    bp_uint64   mtime;
    int         mtime_valid;
} mac_dir_restore_t;

typedef struct {
    bp_io_t     io;               /* MUST be first */
    char        path[4096];
    char        temporary[4096];
    int         staged;
} mac_extract_output_t;

typedef struct {
    const char *out_dir;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
    bp_uint32   unsafe_count;  /* entries skipped for unsafe names/targets */
    bp_uint32   partial_count;
    int         open_error;
    bp_uint8    subject_type;
    int         subject_succeeded;
    char        subject_name[4096];
    int         pending_file_mode_valid;
    bp_uint16   pending_file_mode;
    char        pending_file_path[4096];
    mac_dir_restore_t *directories;
    bp_uint32   directory_count;
    int         slash_warned;  /* printed the leading-'/' warning once */
} extract_ctx_t;

#define MACOS_DIR_QUEUE_MAX 256
#define MACOS_NAME_PREFLIGHT_MAX 65535

typedef struct {
    char *key;
    bp_uint8 type;
} mac_name_key_t;

typedef struct {
    mac_name_key_t *items;
    bp_uint32 count;
    bp_uint32 physical_count;
    int error;
    const char *out_dir;
} mac_name_preflight_t;

static int extract_ctx_init(extract_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->directories = (mac_dir_restore_t *)calloc(
        MACOS_DIR_QUEUE_MAX, sizeof(mac_dir_restore_t));
    return ctx->directories != NULL ? BP_OK : BP_ERR_NOMEM;
}

static int extract_path_join(char *path, size_t cap, const char *dir,
                             const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    int slash = dlen != 0 && dir[dlen - 1] != '/';
    if (dlen + (size_t)slash + nlen + 1 > cap) return BP_ERR_NAME_TOO_LONG;
    memcpy(path, dir, dlen);
    if (slash) path[dlen++] = '/';
    memcpy(path + dlen, name, nlen + 1);
    return BP_OK;
}

/* macOS's default volumes ignore case and canonical Unicode differences.
 * Normalize and fold the archive path before collision checks; this is
 * deliberately conservative on case-sensitive volumes too. */
static char *extract_name_key(const char *name)
{
#ifdef __APPLE__
    CFStringRef source;
    CFMutableStringRef folded;
    CFIndex length, max_bytes;
    char *key;
    source = CFStringCreateWithCString(kCFAllocatorDefault, name,
                                       kCFStringEncodingUTF8);
    if (source == NULL) return NULL;
    folded = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
    CFRelease(source);
    if (folded == NULL) return NULL;
    CFStringNormalize(folded, kCFStringNormalizationFormD);
    CFStringFold(folded, kCFCompareCaseInsensitive, NULL);
    CFStringNormalize(folded, kCFStringNormalizationFormD);
    length = CFStringGetLength(folded);
    max_bytes = CFStringGetMaximumSizeForEncoding(length,
                                                   kCFStringEncodingUTF8);
    if (max_bytes < 0 || max_bytes > 16384) {
        CFRelease(folded);
        return NULL;
    }
    key = (char *)malloc((size_t)max_bytes + 1);
    if (key == NULL || !CFStringGetCString(folded, key, max_bytes + 1,
                                           kCFStringEncodingUTF8)) {
        free(key);
        CFRelease(folded);
        return NULL;
    }
    CFRelease(folded);
    return key;
#else
    char *key = (char *)malloc(strlen(name) + 1);
    size_t i;
    if (key == NULL) return NULL;
    strcpy(key, name);
    for (i = 0; key[i] != '\0'; i++)
        if (key[i] >= 'A' && key[i] <= 'Z')
            key[i] = (char)(key[i] - 'A' + 'a');
    return key;
#endif
}

static int mac_name_key_compare(const void *left, const void *right)
{
    const mac_name_key_t *a = (const mac_name_key_t *)left;
    const mac_name_key_t *b = (const mac_name_key_t *)right;
    return strcmp(a->key, b->key);
}

static int mac_name_key_find(const mac_name_key_t *items, bp_uint32 count,
                             const char *key)
{
    bp_uint32 lo = 0, hi = count;
    while (lo < hi) {
        bp_uint32 mid = lo + (hi - lo) / 2;
        int cmp = strcmp(items[mid].key, key);
        if (cmp == 0) return (int)mid;
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

static int mac_name_preflight_cb(const bp_entry_t *entry, void *user_data)
{
    mac_name_preflight_t *pre = (mac_name_preflight_t *)user_data;
    const char *name;
    size_t n, component = 0;
    char full_path[4096];
    char *key;
    mac_name_key_t *item;

    pre->physical_count++;
    if (pre->physical_count > MACOS_NAME_PREFLIGHT_MAX) {
        pre->error = BP_ERR_OVERFLOW;
        return 1;
    }
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    name = entry->name;
    while (*name == '/') name++;
    if (!bp_name_is_safe(name)) { pre->error = BP_ERR_INVALID_ARG; return 1; }
    n = strlen(name);
    if (n == 0 || n >= sizeof(full_path)) {
        pre->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }
    if (pre->out_dir != NULL) {
        if (extract_path_join(full_path, sizeof(full_path), pre->out_dir,
                              name) != BP_OK) {
            pre->error = BP_ERR_NAME_TOO_LONG;
            return 1;
        }
    } else strcpy(full_path, name);
#ifdef PATH_MAX
    if (strlen(full_path) >= PATH_MAX) {
        pre->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }
#endif
    {
        size_t i;
        for (i = 0; i <= n; i++) {
            if (name[i] == '/' || name[i] == '\0') {
#ifdef NAME_MAX
                if (component > NAME_MAX) {
                    pre->error = BP_ERR_NAME_TOO_LONG;
                    return 1;
                }
#endif
                component = 0;
            } else component++;
        }
    }
    if (pre->count >= MACOS_NAME_PREFLIGHT_MAX) {
        pre->error = BP_ERR_OVERFLOW;
        return 1;
    }
    key = extract_name_key(name);
    if (key == NULL) { pre->error = BP_ERR_NOMEM; return 1; }
    if (entry->entry_type == BP_TYPE_DIR) {
        size_t key_len = strlen(key);
        if (key_len != 0 && key[key_len - 1] == '/') key[key_len - 1] = '\0';
    }
    item = &pre->items[pre->count++];
    item->key = key;
    item->type = entry->entry_type;
    return 0;
}

static int extract_preflight(bp_io_t *aio, const char *out_dir)
{
    mac_name_preflight_t pre;
    bp_uint32 i;
    int rc;
    memset(&pre, 0, sizeof(pre));
    pre.out_dir = out_dir;
    pre.items = (mac_name_key_t *)calloc(MACOS_NAME_PREFLIGHT_MAX,
                                         sizeof(mac_name_key_t));
    if (pre.items == NULL) return BP_ERR_NOMEM;
    rc = bp_archive_list(aio, mac_name_preflight_cb, &pre);
    if (pre.error != 0) rc = pre.error;
    if (rc == BP_OK) {
        qsort(pre.items, pre.count, sizeof(mac_name_key_t),
              mac_name_key_compare);
        for (i = 1; i < pre.count; i++) {
            if (strcmp(pre.items[i - 1].key, pre.items[i].key) == 0) {
                fprintf(stderr,
                        "boltpack: destination name collision after case/Unicode mapping\n");
                rc = BP_ERR_INVALID_ARG;
                break;
            }
        }
        for (i = 0; rc == BP_OK && i < pre.count; i++) {
            char prefix[4096];
            char *slash;
            size_t n = strlen(pre.items[i].key);
            if (n >= sizeof(prefix)) { rc = BP_ERR_NAME_TOO_LONG; break; }
            strcpy(prefix, pre.items[i].key);
            for (slash = strchr(prefix, '/'); slash != NULL;
                 slash = strchr(slash + 1, '/')) {
                int parent;
                *slash = '\0';
                parent = mac_name_key_find(pre.items, pre.count, prefix);
                *slash = '/';
                if (parent >= 0 && pre.items[parent].type != BP_TYPE_DIR) {
                    fprintf(stderr,
                            "boltpack: file/directory path conflict after macOS name mapping\n");
                    rc = BP_ERR_INVALID_ARG;
                    break;
                }
            }
        }
    }
    for (i = 0; i < pre.count; i++) free(pre.items[i].key);
    free(pre.items);
    if (aio->seek(aio->handle, 0, SEEK_SET) != 0 && rc == BP_OK)
        rc = BP_ERR_IO;
    return rc;
}

/* Path-based equivalent of openat(O_NOFOLLOW): reject every existing
 * symlink/non-directory component before creating or using the path. */
static int extract_safe_mkdirs(const char *path)
{
    char *copy;
    char *p;
    struct stat st;
    copy = (char *)malloc(strlen(path) + 1);
    if (copy == NULL) return -1;
    strcpy(copy, path);
    for (p = copy + (copy[0] == '/' ? 1 : 0); ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            if (p != copy && p[-1] != '/') {
                *p = '\0';
                if (lstat(copy, &st) == 0) {
                    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                        free(copy);
                        return -1;
                    }
                } else if (errno == ENOENT) {
                    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                        free(copy);
                        return -1;
                    }
                    if (lstat(copy, &st) != 0 || !S_ISDIR(st.st_mode) ||
                        S_ISLNK(st.st_mode)) {
                        free(copy);
                        return -1;
                    }
                } else {
                    free(copy);
                    return -1;
                }
                *p = saved;
            }
            if (saved == '\0') break;
        }
    }
    free(copy);
    return 0;
}

static int extract_safe_parent(const char *path)
{
    char *copy;
    char *slash;
    int rc = 0;
    copy = (char *)malloc(strlen(path) + 1);
    if (copy == NULL) return -1;
    strcpy(copy, path);
    slash = strrchr(copy, '/');
    if (slash != NULL) {
        if (slash == copy) slash[1] = '\0';
        else *slash = '\0';
        rc = extract_safe_mkdirs(copy);
    }
    free(copy);
    return rc;
}

static int extract_make_stage(const char *path, char *temporary, size_t cap)
{
    const char *slash = strrchr(path, '/');
    int n;
    if (slash == NULL) n = snprintf(temporary, cap, ".boltpack-XXXXXX");
    else if (slash == path)
        n = snprintf(temporary, cap, "/.boltpack-XXXXXX");
    else
        n = snprintf(temporary, cap, "%.*s/.boltpack-XXXXXX",
                     (int)(slash - path), path);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return -1; }
    return mkstemp(temporary);
}

static int extract_restore_link_mtime(const char *path, bp_uint64 value)
{
#ifdef __APPLE__
    struct stat st;
    struct timeval times[2];
    time_t t;
    int rc = bp_mtime_to_time_t(value, &t);
    if (rc != BP_OK) return rc;
    if (lstat(path, &st) != 0) return BP_ERR_IO;
    times[0].tv_sec = st.st_atime;
    times[0].tv_usec = 0;
    times[1].tv_sec = t;
    times[1].tv_usec = 0;
    return lutimes(path, times) == 0 ? BP_OK : BP_ERR_IO;
#else
    (void)path;
    (void)value;
    return BP_ERR_PARTIAL;
#endif
}

static int extract_publish_symlink(mac_extract_output_t *output,
                                   const char *target, int force,
                                   const bp_entry_t *entry,
                                   extract_ctx_t *ctx)
{
    int fd, rc = BP_OK;
    fd = extract_make_stage(output->path, output->temporary,
                            sizeof(output->temporary));
    if (fd < 0) return BP_ERR_IO;
    if (close(fd) != 0 || unlink(output->temporary) != 0 ||
        symlink(target, output->temporary) != 0)
        return BP_ERR_IO;
    if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
        if (extract_restore_link_mtime(output->temporary, entry->mtime) != BP_OK) {
            ctx->partial_count++;
            rc = BP_ERR_PARTIAL;
        }
    }
    if (force) {
        if (rename(output->temporary, output->path) != 0) rc = BP_ERR_IO;
    } else if (link(output->temporary, output->path) != 0 ||
               unlink(output->temporary) != 0) {
        rc = BP_ERR_IO;
    }
    return rc;
}

static int extract_queue_directory(extract_ctx_t *ctx, const char *path,
                                   const bp_entry_t *entry)
{
    mac_dir_restore_t *d;
    if (ctx->directory_count >= MACOS_DIR_QUEUE_MAX) {
        ctx->partial_count++;
        return BP_ERR_PARTIAL;
    }
    d = &ctx->directories[ctx->directory_count];
    d->path = (char *)malloc(strlen(path) + 1);
    if (d->path == NULL) return BP_ERR_NOMEM;
    strcpy(d->path, path);
    d->mtime = entry->mtime;
    d->mtime_valid = (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0;
    ctx->directory_count++;
    return BP_OK;
}

static mac_dir_restore_t *extract_find_directory(extract_ctx_t *ctx,
                                                  const char *path)
{
    bp_uint32 i;
    for (i = 0; i < ctx->directory_count; i++)
        if (strcmp(ctx->directories[i].path, path) == 0)
            return &ctx->directories[i];
    return NULL;
}

static void extract_finish_directories(extract_ctx_t *ctx)
{
    bp_uint32 done;
    for (done = 0; done < ctx->directory_count; done++) {
        bp_uint32 i, best = done;
        size_t best_len = strlen(ctx->directories[best].path);
        for (i = done + 1; i < ctx->directory_count; i++) {
            size_t len = strlen(ctx->directories[i].path);
            if (len > best_len) { best = i; best_len = len; }
        }
        if (best != done) {
            mac_dir_restore_t swap = ctx->directories[done];
            ctx->directories[done] = ctx->directories[best];
            ctx->directories[best] = swap;
        }
        if (ctx->directories[done].mode_valid &&
            chmod(ctx->directories[done].path,
                  (mode_t)(ctx->directories[done].mode & 0777)) != 0)
            ctx->partial_count++;
        if (ctx->directories[done].mtime_valid &&
            bp_restore_mtime(ctx->directories[done].path,
                             ctx->directories[done].mtime) != BP_OK)
            ctx->partial_count++;
    }
}

static void extract_finish_file_mode(extract_ctx_t *ctx)
{
    if (ctx->pending_file_mode_valid) {
        if (chmod(ctx->pending_file_path,
                  (mode_t)(ctx->pending_file_mode & 0777)) != 0)
            ctx->partial_count++;
        ctx->pending_file_mode_valid = 0;
    }
}

static void extract_ctx_destroy(extract_ctx_t *ctx)
{
    bp_uint32 i;
    if (ctx->directories != NULL) {
        for (i = 0; i < ctx->directory_count; i++)
            free(ctx->directories[i].path);
        free(ctx->directories);
        ctx->directories = NULL;
    }
}

/* Strip leading '/' from an entry name (tar-style), warning once. */
static const char *extract_entry_name(const bp_entry_t *entry,
                                      extract_ctx_t *ctx)
{
    const char *name = entry->name;

    while (*name == '/') name++;
    if (name != entry->name && !ctx->slash_warned) {
        fprintf(stderr,
                "boltpack: removing leading '/' from member names\n");
        ctx->slash_warned = 1;
    }
    return name;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[4096];
    FILE *fp;
    mac_extract_output_t *output;
    meta_extract_t *mex;
    rsrc_extract_t *rex;
    const char *name;

    /* A hostile archive must not choose where we write: refuse ".."
     * components, backslashes, and drive letters outright. */
    name = extract_entry_name(entry, ctx);
    if (!bp_name_is_safe(name)) {
        fprintf(stderr, "boltpack: skipping entry with unsafe name: %s\n",
                entry->name);
        ctx->unsafe_count++;
        ctx->partial_count++;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_DATA ||
        entry->entry_type == BP_TYPE_DIR ||
        entry->entry_type == BP_TYPE_SYMLINK) {
        size_t n = strlen(name);
        extract_finish_file_mode(ctx);
        if (n >= sizeof(ctx->subject_name)) {
            ctx->open_error = BP_ERR_NAME_TOO_LONG;
            return NULL;
        }
        memcpy(ctx->subject_name, name, n + 1);
        ctx->subject_type = entry->entry_type;
        ctx->subject_succeeded = 0;
    }

    if (ctx->out_dir) {
        if (extract_path_join(path, sizeof(path), ctx->out_dir, name) != BP_OK) {
            ctx->open_error = BP_ERR_NAME_TOO_LONG;
            return NULL;
        }
    } else {
        if (strlen(name) >= sizeof(path)) {
            ctx->open_error = BP_ERR_NAME_TOO_LONG;
            return NULL;
        }
        strcpy(path, name);
    }

    /* --- Directory ---------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", entry->name);
        if (!ctx->dry_run) {
            int r;
            if (extract_safe_mkdirs(path) != 0) {
                ctx->open_error = BP_ERR_IO;
                return NULL;
            }
            r = extract_queue_directory(ctx, path, entry);
            if (r == BP_ERR_NOMEM) ctx->open_error = r;
            else if (r == BP_OK) ctx->subject_succeeded = 1;
        }
        return NULL;
    }

    /* --- Metadata ----------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_OS_MACOSX &&
            entry->platform != BP_OS_MACOS9) {
            if (!ctx->dry_run) ctx->partial_count++;
            return NULL;
        }

        if (entry->original_size.hi != 0 || entry->original_size.lo == 0 ||
            entry->original_size.lo > META_EXTRACT_BUF) {
            if (!ctx->dry_run) ctx->partial_count++;
            return NULL;
        }
        if (ctx->dry_run) return bp_archive_dry_run_stream();

        mex = (meta_extract_t *)malloc(sizeof(meta_extract_t));
        if (!mex) { ctx->open_error = BP_ERR_NOMEM; return NULL; }

        mex->wctx.buf = mex->buf;
        mex->wctx.cap = sizeof(mex->buf);
        mex->wctx.len = 0;
        mex->wctx.pos = 0;

        mex->io.read   = wbuf_read_fn;
        mex->io.write  = wbuf_write_fn;
        mex->io.seek   = wbuf_seek_fn;
        mex->io.tell   = wbuf_tell_fn;
        mex->io.handle = &mex->wctx;
        return &mex->io;
    }

    /* --- Resource fork ------------------------------------------------ */
    if (entry->entry_type == BP_TYPE_RSRC) {
        bp_uint32 rsrc_sz;

        if (ctx->dry_run) return bp_archive_dry_run_stream();

        if (entry->original_size.hi != 0 || entry->original_size.lo == 0 ||
            entry->original_size.lo > MACOS_RSRC_MAX) {
            ctx->partial_count++;
            return NULL;
        }
        rsrc_sz = entry->original_size.lo;

        rex = (rsrc_extract_t *)malloc(sizeof(rsrc_extract_t));
        if (!rex) { ctx->open_error = BP_ERR_NOMEM; return NULL; }

        rex->buf = (unsigned char *)malloc(rsrc_sz);
        if (!rex->buf) { free(rex); ctx->open_error = BP_ERR_NOMEM; return NULL; }
        rex->cap = rsrc_sz;
        rex->len = 0;
        rex->pos = 0;

        rex->io.read   = rsrc_read_fn;
        rex->io.write  = rsrc_write_fn;
        rex->io.seek   = rsrc_seek_fn;
        rex->io.tell   = rsrc_tell_fn;
        rex->io.handle = rex;
        return &rex->io;
    }

    /* --- Symlink ------------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->file_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf("    linking: %s\n", entry->name);
        if (ctx->dry_run) return bp_archive_dry_run_stream();
        if (!ctx->force) {
            struct stat st_buf;
            if (lstat(path, &st_buf) == 0) {
                ctx->partial_count++;
                return NULL;
            }
            if (errno != ENOENT) { ctx->open_error = BP_ERR_IO; return NULL; }
        }
        if (extract_safe_parent(path) != 0) {
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
        fp = tmpfile();
        if (fp == NULL) { ctx->open_error = BP_ERR_IO; return NULL; }
        output = (mac_extract_output_t *)calloc(1, sizeof(*output));
        if (output == NULL) {
            fclose(fp); ctx->open_error = BP_ERR_NOMEM; return NULL;
        }
        strcpy(output->path, path);
        bp_io_from_file(&output->io, fp);
        return &output->io;
    }

    /* --- Regular file ------------------------------------------------- */
    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT)
        printf(" extracting: %s\n", entry->name);
    if (ctx->dry_run) return bp_archive_dry_run_stream();

    if (!ctx->force) {
        struct stat st_buf;
        if (lstat(path, &st_buf) == 0) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n", path);
            ctx->partial_count++;
            return NULL;
        } else if (errno != ENOENT) {
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
    }
    if (extract_safe_parent(path) != 0) {
        fprintf(stderr, "boltpack: unsafe or inaccessible parent: %s\n", path);
        ctx->open_error = BP_ERR_IO;
        return NULL;
    }
    output = (mac_extract_output_t *)calloc(1, sizeof(*output));
    if (output == NULL) { ctx->open_error = BP_ERR_NOMEM; return NULL; }
    strcpy(output->path, path);
    {
        int fd = extract_make_stage(path, output->temporary,
                                    sizeof(output->temporary));
        if (fd < 0) {
            free(output); ctx->open_error = BP_ERR_IO; return NULL;
        }
        fp = fdopen(fd, "wb");
        if (fp == NULL) {
            close(fd); unlink(output->temporary); free(output);
            ctx->open_error = BP_ERR_IO; return NULL;
        }
    }
    output->staged = 1;
    bp_io_from_file(&output->io, fp);
    return &output->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    const char *name = extract_entry_name(entry, ctx);
    int rc = BP_OK;

    if (status != BP_OK) {
        if (entry->entry_type == BP_TYPE_META) free((meta_extract_t *)out);
        else if (entry->entry_type == BP_TYPE_RSRC) {
            rsrc_extract_t *rex = (rsrc_extract_t *)out;
            free(rex->buf);
            free(rex);
        } else {
            mac_extract_output_t *output = (mac_extract_output_t *)out;
            fclose((FILE *)out->handle);
            if (output->staged) unlink(output->temporary);
            free(output);
        }
        ctx->subject_succeeded = 0;
        return BP_OK;
    }

    /* --- macOS / Classic Mac metadata --------------------------------- */
    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;
        char path[4096];

        if (!ctx->subject_succeeded || strcmp(name, ctx->subject_name) != 0) {
            ctx->partial_count++;
            free(mex);
            return BP_ERR_PARTIAL;
        }
        if (ctx->out_dir) rc = extract_path_join(path, sizeof(path), ctx->out_dir, name);
        else if (strlen(name) < sizeof(path)) strcpy(path, name);
        else rc = BP_ERR_NAME_TOO_LONG;
        if (rc != BP_OK) {
            ctx->partial_count++;
            free(mex);
            return BP_ERR_PARTIAL;
        }

        if (entry->platform == BP_OS_MACOSX) {
            bp_uint16 mode;
            if (mex->wctx.len != MACOS_MODE_META_SIZE) rc = BP_ERR_CORRUPT;
            else {
                mode = macos_decode_mode_meta(mex->wctx.buf,
                                              (int)mex->wctx.len);
                if (ctx->subject_type == BP_TYPE_DIR) {
                    mac_dir_restore_t *d = extract_find_directory(ctx, path);
                    if (d == NULL) rc = BP_ERR_PARTIAL;
                    else { d->mode = mode; d->mode_valid = 1; }
                } else if (ctx->subject_type == BP_TYPE_DATA) {
                    strcpy(ctx->pending_file_path, path);
                    ctx->pending_file_mode = mode;
                    ctx->pending_file_mode_valid = 1;
                } else {
                    rc = BP_ERR_PARTIAL;
                }
            }
        } else if (entry->platform == BP_OS_MACOS9) {
            /* Classic Mac 18-byte meta -> Finder info xattr. */
#ifdef __APPLE__
            unsigned char fi_out[APPLE_FINDERINFO_SIZE];
            if (macos_decode_classic_meta(mex->wctx.buf,
                                          (int)mex->wctx.len,
                                          fi_out) != 0 ||
                setxattr(path, APPLE_FINDERINFO_XATTR, fi_out,
                         APPLE_FINDERINFO_SIZE, 0, XATTR_NOFOLLOW) != 0)
                rc = BP_ERR_PARTIAL;
            else rc = BP_ERR_PARTIAL; /* Classic dates are not restored. */
#else
            rc = BP_ERR_PARTIAL;
#endif /* __APPLE__ */
        }
        if (rc == BP_ERR_PARTIAL) ctx->partial_count++;
        free(mex);
        return rc;
    }

    /* --- Resource fork ------------------------------------------------ */
    if (entry->entry_type == BP_TYPE_RSRC) {
        rsrc_extract_t *rex = (rsrc_extract_t *)out;

        char path[4096];
        if (!ctx->subject_succeeded || strcmp(name, ctx->subject_name) != 0) {
            rc = BP_ERR_PARTIAL;
        } else if (ctx->out_dir) {
            rc = extract_path_join(path, sizeof(path), ctx->out_dir, name);
        } else if (strlen(name) < sizeof(path)) {
            strcpy(path, name);
            rc = BP_OK;
        } else rc = BP_ERR_NAME_TOO_LONG;
        if (rc == BP_OK && rex->len > 0) {
#ifdef __APPLE__
            if (setxattr(path, XATTR_RESOURCEFORK_NAME, rex->buf,
                         (size_t)rex->len, 0, XATTR_NOFOLLOW) != 0)
                rc = BP_ERR_PARTIAL;
#else
            rc = BP_ERR_PARTIAL;
#endif /* __APPLE__ */
        }
        free(rex->buf);
        free(rex);
        if (rc == BP_ERR_PARTIAL || rc == BP_ERR_NAME_TOO_LONG)
            ctx->partial_count++;
        return rc == BP_OK ? BP_OK : BP_ERR_PARTIAL;
    }

    /* --- Symlink ------------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        FILE *fp = (FILE *)out->handle;
        mac_extract_output_t *output = (mac_extract_output_t *)out;
        char  target[4096];
        long  len;

        len = fseek(fp, 0, SEEK_END) == 0 ? ftell(fp) : -1;
        if (len > 0 && len < (long)sizeof(target)) {
            rewind(fp);
            if (fread(target, 1, (size_t)len, fp) == (size_t)len) {
                if (memchr(target, '\0', (size_t)len) != NULL) {
                    rc = BP_ERR_CORRUPT;
                } else {
                    target[len] = '\0';
                    /* Refuse targets that point outside the extraction root. */
                    if (!bp_symlink_target_is_safe(name, target)) {
                        fprintf(stderr,
                                "boltpack: skipping symlink with unsafe target: "
                                "%s -> %s\n", entry->name, target);
                        ctx->unsafe_count++;
                        ctx->partial_count++;
                        rc = BP_ERR_PARTIAL;
                    } else {
                        if (fclose(fp) != 0) rc = BP_ERR_IO;
                        fp = NULL;
                        if (rc == BP_OK)
                            rc = extract_publish_symlink(output, target,
                                                         ctx->force, entry, ctx);
                    }
                }
            } else rc = BP_ERR_IO;
        } else rc = BP_ERR_CORRUPT;
        if (fp != NULL && fclose(fp) != 0 &&
            (rc == BP_OK || rc == BP_ERR_PARTIAL)) rc = BP_ERR_IO;
        if (rc == BP_OK || rc == BP_ERR_PARTIAL)
            ctx->subject_succeeded = 1;
        if (output->temporary[0] != '\0' &&
            (rc == BP_ERR_IO || rc == BP_ERR_CORRUPT)) unlink(output->temporary);
        free(output);
        return rc;
    }

    /* --- Regular file: close, set mtime ------------------------------- */
    {
        FILE *fp = (FILE *)out->handle;
        mac_extract_output_t *output = (mac_extract_output_t *)out;
        mode_t mask;

        if (fflush(fp) != 0) rc = BP_ERR_IO;
        mask = umask(0);
        (void)umask(mask);
        if (fchmod(fileno(fp), (mode_t)(0666 & ~mask)) != 0 &&
            (rc == BP_OK || rc == BP_ERR_PARTIAL))
            rc = BP_ERR_IO;
        if (fclose(fp) != 0 &&
            (rc == BP_OK || rc == BP_ERR_PARTIAL)) rc = BP_ERR_IO;
        if ((entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
            bp_restore_mtime(output->temporary, entry->mtime) != BP_OK) {
            fprintf(stderr, "boltpack: cannot restore timestamp: %s\n",
                    output->path);
            ctx->partial_count++;
            if (rc == BP_OK) rc = BP_ERR_PARTIAL;
        }
        if (rc == BP_OK || rc == BP_ERR_PARTIAL) {
            int commit_rc = 0;
            if (ctx->force) commit_rc = rename(output->temporary, output->path);
            else {
                commit_rc = link(output->temporary, output->path);
                if (commit_rc == 0 && unlink(output->temporary) != 0)
                    commit_rc = -1;
            }
            if (commit_rc != 0) {
                fprintf(stderr, "boltpack: cannot publish extracted file: %s\n",
                        output->path);
                unlink(output->temporary);
                rc = BP_ERR_IO;
            } else {
                ctx->subject_succeeded = 1;
            }
        } else {
            unlink(output->temporary);
        }
        free(output);
    }
    return rc;
}

static int extract_run(bp_io_t *aio, extract_ctx_t *ctx,
                       bp_progress_fn progress_fn)
{
    int rc;
    rc = extract_preflight(aio, ctx->out_dir);
    if (rc != BP_OK) return rc;
    if (ctx->out_dir != NULL && !ctx->dry_run &&
        extract_safe_mkdirs(ctx->out_dir) != 0)
        return BP_ERR_IO;
    rc = bp_archive_extract_all_ex(aio, extract_open, extract_done,
                                   progress_fn, ctx);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && ctx->open_error != BP_OK)
        rc = ctx->open_error;
    if (rc == BP_OK || rc == BP_ERR_PARTIAL) {
        extract_finish_file_mode(ctx);
        extract_finish_directories(ctx);
    }
    if (rc == BP_OK && ctx->partial_count != 0) rc = BP_ERR_PARTIAL;
    if (rc == BP_ERR_PARTIAL && ctx->partial_count == 0)
        ctx->partial_count = 1;
    return rc;
}

#ifdef BOLTPACK_NET
/* Extract a verified network-received archive using the macOS callbacks. */
int bp_frontend_extract_io(bp_io_t *aio, const char *out_dir, int force)
{
    extract_ctx_t ctx;
    int rc;
    if (extract_ctx_init(&ctx) != BP_OK) return BP_ERR_NOMEM;
    ctx.out_dir = out_dir;
    ctx.force = force;
    if (out_dir != NULL && extract_safe_mkdirs(out_dir) != 0) {
        extract_ctx_destroy(&ctx);
        return BP_ERR_IO;
    }
    rc = extract_run(aio, &ctx, NULL);
    extract_ctx_destroy(&ctx);
    return rc;
}
#endif

/* ---- test callback ----------------------------------------------------- */

typedef struct {
    bp_uint32 ok_count;
    bp_uint32 fail_count;
} test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    if (g_verbosity < VERB_SILENT) {
        printf("  testing: %s", entry->name);
        printf(result == BP_OK ? " OK\n" : " FAILED -- %s\n",
               bp_strerror(result));
    }
    if (result == BP_OK) ctx->ok_count++;
    else                 ctx->fail_count++;
}

/* ---- os_name / cpu_name ------------------------------------------------ */

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
    case BP_OS_BEOS:      return "BeOS/Haiku";
    case BP_OS_OPENVMS:   return "OpenVMS";
    case BP_OS_ZOS:       return "z/OS";
    case BP_OS_WASM:      return "WebAssembly";
    case BP_OS_NEXTSTEP:  return "NeXTSTEP";
    case BP_OS_QNX:       return "QNX";
    case BP_OS_PLAN9:     return "Plan 9";
    case BP_OS_TRU64:     return "Tru64 UNIX";
    case BP_OS_SCO:       return "SCO UNIX";
    case BP_OS_CPM86:     return "CP/M-86";
    case BP_OS_QL:        return "Sinclair QL";
    case BP_OS_WIN16:     return "Windows 3.x";
    case BP_OS_HURD:      return "GNU Hurd";
    case BP_OS_X68K:      return "Sharp X68000";
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
    case BP_CPU_SPARC:    return "SPARC";
    case BP_CPU_ALPHA:    return "Alpha";
    case BP_CPU_PARISC:   return "PA-RISC";
    case BP_CPU_RISCV32:  return "RISC-V 32";
    case BP_CPU_RISCV64:  return "RISC-V 64";
    case BP_CPU_SH:       return "SuperH";
    case BP_CPU_6502:     return "6502/65C816";
    case BP_CPU_Z80:      return "Z80";
    case BP_CPU_8086:     return "8086";
    case BP_CPU_E2K:      return "Elbrus (e2k)";
    case BP_CPU_ZARCH:    return "z/Architecture";
    case BP_CPU_SPARC64:  return "SPARC64";
    case BP_CPU_IA64:     return "Itanium";
    default:              return "Unknown";
    }
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[5] = {"Stored", "BoltLite", "boltlz", "Fluff", "zstd"};
    int pos = 0;
    int first = 1;
    int i;
    buf[0] = '\0';
    for (i = 0; i < 5; i++) {
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
    FILE *fp;
    bp_io_t aio;
    bp_archive_info_t info;
    int rc;

    fp = fopen(archive_name, "rb");
    if (fp == NULL) {
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
        char origbuf[32], compbuf[32], arcbuf[32];

        format_size64(info.archive_size, arcbuf, sizeof(arcbuf));
        format_size64(info.total_original, origbuf, sizeof(origbuf));
        format_size64(info.total_compressed, compbuf, sizeof(compbuf));

        printf("Archive:        %s\n", archive_name);
        printf("Format version: %u\n", (unsigned)info.version);
        printf("Created by:     BoltPack %u.%u\n",
               (unsigned)info.creator_ver_major,
               (unsigned)info.creator_ver_minor);
        printf("Creator OS:     %s\n", os_name(info.creator_os));
        printf("Creator CPU:    %s\n", cpu_name(info.creator_cpu));
        printf("Method:         %s\n", format_methods(info.method_counts));
        printf("Archive size:   %s\n", arcbuf);
        printf("Entries:        %lu (%lu file%s, %lu director%s)\n",
               (unsigned long)info.entry_count,
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)info.dir_count,
               info.dir_count == 1 ? "y" : "ies");
        printf("Original size:  %s\n", origbuf);
        printf("Compressed:     %s", compbuf);
        if (info.total_original.hi != 0 || info.total_original.lo != 0) {
            double orig_d = (double)info.total_original.lo
                          + (double)info.total_original.hi * 4294967296.0;
            double comp_d = (double)info.total_compressed.lo
                          + (double)info.total_compressed.hi * 4294967296.0;
            printf(" (%.0f%%)", (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0);
        }
        printf("\n");
    }
    return 0;
}

/* ---- cmd_delete -------------------------------------------------------- */

static int cmd_delete(int argc, char **argv)
{
    const char *archive_name;
    const char **del_names;
    int del_count;
    FILE *src_fp, *dst_fp;
    bp_io_t src_io, dst_io;
    char tmp_name[4096];
    struct stat snapshot, opened;
    mode_t stage_mode;
    int stage_fd;
    bp_uint32 deleted = 0;
    int rc, close_rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }
    archive_name = argv[0];
    del_names    = (const char **)&argv[1];
    del_count    = argc - 1;

    if (archive_mode_for_stage(archive_name, &stage_mode, &snapshot, 1) != 0) {
        fprintf(stderr, "boltpack: archive must be a regular file: %s\n",
                archive_name);
        return 1;
    }
    src_fp = fopen(archive_name, "rb");
    if (!src_fp || fstat(fileno(src_fp), &opened) != 0 ||
        opened.st_dev != snapshot.st_dev || opened.st_ino != snapshot.st_ino) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        if (src_fp) fclose(src_fp);
        return 1;
    }
    stage_fd = make_archive_stage(archive_name, tmp_name, sizeof(tmp_name));
    if (stage_fd < 0) {
        fprintf(stderr, "boltpack: cannot create temp file beside archive\n");
        fclose(src_fp);
        return 1;
    }
    dst_fp = fdopen(stage_fd, "w+b");
    if (!dst_fp) {
        close(stage_fd);
        unlink(tmp_name);
        fclose(src_fp);
        fprintf(stderr, "boltpack: cannot open temp file stream\n");
        return 1;
    }

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);
    rc = bp_archive_delete(&src_io, &dst_io, del_names, (bp_uint32)del_count,
                           &deleted);
    close_rc = fclose(src_fp) == 0 ? BP_OK : BP_ERR_IO;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        fclose(dst_fp);
        remove(tmp_name);
        return 1;
    }
    if (close_rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot close source archive\n");
        fclose(dst_fp);
        remove(tmp_name);
        return 1;
    }
    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        fclose(dst_fp);
        remove(tmp_name);
        return 1;
    }
    close_rc = BP_OK;
    if (fchmod(fileno(dst_fp), stage_mode) != 0) close_rc = BP_ERR_IO;
    if (fflush(dst_fp) != 0) close_rc = BP_ERR_IO;
    if (fclose(dst_fp) != 0) close_rc = BP_ERR_IO;
    if (close_rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot finish temporary archive\n");
        remove(tmp_name);
        return 1;
    }
    if (!archive_target_unchanged(archive_name, &snapshot) ||
        rename(tmp_name, archive_name) != 0) {
        fprintf(stderr, "boltpack: cannot replace archive\n");
        remove(tmp_name);
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    return 0;
}

/* ---- cmd_add ----------------------------------------------------------- */

static int cmd_add(int argc, char **argv, bp_uint32 block_size, bp_uint8 method,
                   int level, int interchange_profile)
{
    const char *archive_name;
    FILE *src_fp, *fp;
    bp_io_t io;
    bp_archive_t ar;
    char tmp_name[4096];
    struct stat snapshot, opened;
    mode_t stage_mode;
    int stage_fd;
    int rc, i, ar_open;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    if (archive_mode_for_stage(archive_name, &stage_mode, &snapshot, 1) != 0) {
        fprintf(stderr, "boltpack: archive must be a regular file: %s\n",
                archive_name);
        return 1;
    }

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }
    src_fp = fopen(archive_name, "rb");
    if (!src_fp || fstat(fileno(src_fp), &opened) != 0 ||
        opened.st_dev != snapshot.st_dev || opened.st_ino != snapshot.st_ino) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        if (src_fp) fclose(src_fp);
        return 1;
    }
    stage_fd = make_archive_stage(archive_name, tmp_name, sizeof(tmp_name));
    if (stage_fd < 0) {
        fprintf(stderr, "boltpack: cannot create temp file beside archive\n");
        fclose(src_fp);
        return 1;
    }
    fp = fdopen(stage_fd, "w+b");
    if (!fp) {
        close(stage_fd);
        unlink(tmp_name);
        fclose(src_fp);
        fprintf(stderr, "boltpack: cannot open temp file stream\n");
        return 1;
    }
    rc = copy_archive_stream(src_fp, fp);
    if (fclose(src_fp) != 0) rc = BP_ERR_IO;
    if (fflush(fp) != 0 || fseeko(fp, 0, SEEK_SET) != 0) rc = BP_ERR_IO;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot copy archive to temporary file\n");
        fclose(fp);
        unlink(tmp_name);
        return 1;
    }
    bp_io_from_file(&io, fp);
    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        unlink(tmp_name);
        return 1;
    }
    ar_open = 1;
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            goto add_failed;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);
    for (i = 1; i < argc; i++) {
        const char *path    = argv[i];
        const char *arcname = clean_path(path);
        rc = add_path(&ar, path, arcname);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            goto add_failed;
        }
    }
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        ar_open = 0;
        goto add_failed;
    }
    ar_open = 0;
    rc = BP_OK;
    if (fchmod(fileno(fp), stage_mode) != 0) rc = BP_ERR_IO;
    if (fflush(fp) != 0) rc = BP_ERR_IO;
    if (fclose(fp) != 0) rc = BP_ERR_IO;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot finish temporary archive\n");
        unlink(tmp_name);
        return 1;
    }
    if (!archive_target_unchanged(archive_name, &snapshot) ||
        rename(tmp_name, archive_name) != 0) {
        fprintf(stderr, "boltpack: cannot replace archive\n");
        unlink(tmp_name);
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    return 0;

add_failed:
    if (ar_open) bp_archive_close(&ar);
    fclose(fp);
    unlink(tmp_name);
    return 1;
}

/* ---- cmd_create -------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int level, int interchange_profile)
{
    const char *archive_name;
    FILE *fp;
    char tmp_name[4096];
    struct stat snapshot;
    mode_t stage_mode;
    int stage_fd;
    bp_io_t aio;
    bp_archive_t ar;
    int rc, i;
    clock_t start;
    double elapsed;

#ifdef BOLTPACK_NET
    bp_net_cli_strip_create_args(&argc, argv);
#endif

    if (argc < 2) {
        fprintf(stderr, "boltpack: create requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    if (archive_mode_for_stage(archive_name, &stage_mode, &snapshot, 0) != 0) {
        fprintf(stderr, "boltpack: archive target is not a regular file: %s\n",
                archive_name);
        return 1;
    }

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }
    stage_fd = make_archive_stage(archive_name, tmp_name, sizeof(tmp_name));
    if (stage_fd < 0) {
        fprintf(stderr, "boltpack: cannot create temporary archive beside: %s\n",
                archive_name);
        return 1;
    }
    fp = fdopen(stage_fd, "w+b");
    if (!fp) {
        close(stage_fd);
        unlink(tmp_name);
        fprintf(stderr, "boltpack: cannot open temporary archive stream\n");
        return 1;
    }
    bp_io_from_file(&aio, fp);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp); unlink(tmp_name); return 1;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);

    start = clock();
    for (i = 1; i < argc; i++) {
        const char *path    = argv[i];
        const char *arcname = clean_path(path);
        rc = add_path(&ar, path, arcname);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            bp_archive_close(&ar); fclose(fp); unlink(tmp_name); return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar); fclose(fp); unlink(tmp_name); return 1;
    }

    elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;

    if (g_verbosity < VERB_SILENT) {
        bp_uint64 total_orig = {0, 0};
        bp_uint64 total_comp = {0, 0};
        char numbuf[32], speedbuf[64];
        bp_uint32 file_count = 0, dir_count = 0, j;

        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) dir_count++;
            else                                            file_count++;
            total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
            total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
        }
        format_size64(total_orig, numbuf, sizeof(numbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)dir_count,  dir_count  == 1 ? "y" : "ies",
               numbuf);
        if (!bp_u64_is_zero(total_orig) && !bp_u64_is_zero(total_comp)) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            char compbuf[32];
            format_size64(total_comp, compbuf, sizeof(compbuf));
            printf(", %.0f%% (%s)", (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0, compbuf);
        }
        if (elapsed > 0.0) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            printf(", %.2fs", elapsed);
            format_speed(orig_d / elapsed, speedbuf, sizeof(speedbuf));
            printf(", %s", speedbuf);
        }
        printf("\n");
    }
    bp_archive_close(&ar);
    rc = BP_OK;
    if (fchmod(fileno(fp), stage_mode) != 0) rc = BP_ERR_IO;
    if (fflush(fp) != 0) rc = BP_ERR_IO;
    if (fclose(fp) != 0) rc = BP_ERR_IO;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot finish temporary archive\n");
        unlink(tmp_name);
        return 1;
    }
    if (snapshot.st_ino != 0 &&
        !archive_target_unchanged(archive_name, &snapshot)) {
        fprintf(stderr, "boltpack: archive target changed during creation\n");
        unlink(tmp_name);
        return 1;
    }
    if (rename(tmp_name, archive_name) != 0) {
        fprintf(stderr, "boltpack: cannot publish archive: %s\n",
                strerror(errno));
        unlink(tmp_name);
        return 1;
    }
#ifdef BOLTPACK_NET
    if (bp_net_cli_maybe_send(archive_name) != 0) {
        fprintf(stderr,
                "boltpack: archive saved locally but network send failed\n");
        return 1;
    }
#endif
    return 0;
}

/* ---- cmd_list ---------------------------------------------------------- */

static int cmd_list(const char *archive_name)
{
    FILE *fp;
    bp_io_t aio;
    int rc;
    list_ctx_t ctx;

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
               (unsigned long)ctx.file_count, ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,  ctx.dir_count  == 1 ? "y" : "ies",
               origbuf);
        if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
            double orig_d = (double)ctx.total_orig.lo + (double)ctx.total_orig.hi * 4294967296.0;
            double comp_d = (double)ctx.total_comp.lo + (double)ctx.total_comp.hi * 4294967296.0;
            printf(", %.0f%% (%s)", (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0, compbuf);
        }
        printf("\n");
    }
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------- */

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE *fp;
    bp_io_t aio;
    extract_ctx_t ctx;
    int rc;
    clock_t start;
    double elapsed;

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
    if (extract_ctx_init(&ctx) != BP_OK) {
        fprintf(stderr, "boltpack: out of memory preparing extraction\n");
        fclose(fp);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    ctx.out_dir  = out_dir;
    ctx.force    = force;
    ctx.dry_run  = dry_run;
    start = clock();

    rc = extract_run(&aio, &ctx,
                     g_verbosity == VERB_DEFAULT ? progress_cb : NULL);
    if (fclose(fp) != 0 && (rc == BP_OK || rc == BP_ERR_PARTIAL))
        rc = BP_ERR_IO;
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && ctx.open_error != BP_OK)
        rc = ctx.open_error;
    if (rc == BP_OK || rc == BP_ERR_PARTIAL) {
        extract_finish_file_mode(&ctx);
        extract_finish_directories(&ctx);
    }
    if (rc == BP_OK && ctx.partial_count != 0) rc = BP_ERR_PARTIAL;
    if (rc != BP_OK) {
        if (rc == BP_ERR_PARTIAL)
            fprintf(stderr, "boltpack: extraction completed with partial preservation (%lu item%s affected)\n",
                    (unsigned long)ctx.partial_count,
                    ctx.partial_count == 1 ? "" : "s");
        else
            fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        extract_ctx_destroy(&ctx);
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
    if (ctx.unsafe_count != 0) {
        fprintf(stderr,
                "boltpack: %lu entr%s skipped (unsafe name or link target)\n",
                (unsigned long)ctx.unsafe_count,
                ctx.unsafe_count == 1 ? "y" : "ies");
    }
    extract_ctx_destroy(&ctx);
    return 0;
}

/* ---- cmd_test ---------------------------------------------------------- */

static int cmd_test(const char *archive_name)
{
    FILE *fp;
    bp_io_t aio;
    test_ctx_t ctx;
    int rc;

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
               (unsigned long)ctx.ok_count, ctx.ok_count == 1 ? "" : "s");
        if (ctx.fail_count > 0)
            printf(", %lu file%s FAILED",
                   (unsigned long)ctx.fail_count,
                   ctx.fail_count == 1 ? "" : "s");
        printf("\n");
    }
    return ctx.fail_count > 0 ? 1 : 0;
}

/* ---- usage ------------------------------------------------------------- */

static void usage_basic(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.bolt [files...]\n\n"
        "Commands:\n"
        "  create (c)   Create archive from files\n"
        "  add (a)      Add files to existing archive\n"
        "  extract (x)  Extract archive\n"
        "  list (l)     List archive contents\n"
        "  test (t)     Test archive integrity\n"
        "  info (i)     Show archive information\n"
        "  delete (d)   Delete files from archive\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr,
        "\nCommon options:\n"
        "  -b SIZE    Block size: 8k, 16k, 32k, 64k, 256k\n"
        "  --interchange-profile-1  Mini-compatible limits for create/add\n"
        "  -0         Store only (no compression)\n"
        "  -1         BoltLite compression\n"
        "  -2         boltlz compression (default)\n"
        "  -F         Use Fluff compression\n");
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff/zstd: 1-9)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff/zstd 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8; zstd 3)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff/zstd 9)\n"
        "  -o PATH    Output directory for extraction\n"
        "  -f         Force overwrite existing files\n"
        "  -n         Dry run\n"
        "  -q / -qq   Quiet / silent\n");
    fprintf(stderr,
        "\nExamples:\n"
        "  boltpack create backup.bolt folder\n"
        "  boltpack extract -o out backup.bolt\n"
        "  boltpack create --fluff backup.bolt folder\n");
#ifdef BOLTPACK_NET
    bp_net_cli_usage(stderr);
#endif
    fprintf(stderr,
        "\nFor advanced compression options, run: boltpack help advanced\n");
}

static void usage_advanced(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- advanced macOS CLI help\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.bolt [files...]\n\n"
        "Commands:\n"
        "  create (c)   Create archive from files\n"
        "  add (a)      Add files to existing archive\n"
        "  extract (x)  Extract archive\n"
        "  list (l)     List archive contents\n"
        "  test (t)     Test archive integrity\n"
        "  info (i)     Show archive information\n"
        "  delete (d)   Delete files from archive\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr,
        "\nOptions:\n"
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k (default: 32k, 256k with -F)\n"
        "  --interchange-profile-1  Mini-compatible limits for create/add\n"
        "  -0, --store      Store only (no compression)\n"
        "  -1, --boltlite       Use BoltLite compression\n"
        "  -2, --boltlz    Use boltlz compression (default)\n"
        "  -F, --fluff      Use Fluff compression\n");
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff/zstd: 1-9)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff/zstd 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8; zstd 3)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff/zstd 9)\n"
        "  --block-size N   Long-form alias for -b\n"
        "  --output PATH    Long-form alias for -o\n");
    fprintf(stderr,
        "  --force          Long-form alias for -f\n"
        "  --dry-run        Long-form alias for -n\n"
        "  --quiet          Long-form alias for -q\n"
        "  --silent         Long-form alias for -qq\n");
}

static void usage(void)
{
    usage_basic();
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char command;
    bp_uint32 block_size = 0;
    bp_uint8  method     = BP_METHOD_BOLTLZ;
    int opt_level = 0;
    const char *out_dir  = NULL;
    int force   = 0;
    int dry_run = 0;
    int interchange_profile = 0;
    int codec_given = 0;
    int i;

    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "help") == 0 ||
        strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        if (argc >= 3 &&
            (strcmp(argv[2], "advanced") == 0 ||
             strcmp(argv[2], "--advanced") == 0 ||
             strcmp(argv[2], "--help-advanced") == 0)) {
            usage_advanced();
        } else {
            usage_basic();
        }
        return 0;
    }

    if (strcmp(argv[1], "--help-advanced") == 0) {
        usage_advanced();
        return 0;
    }

    command = parse_command_name(argv[1]);
    if (command == '\0') {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]);
        usage(); return 1;
    }
    {
        int needs_arg = 1;
#ifdef BOLTPACK_NET
        if (command == 'L' || command == 'S') needs_arg = 0;
#endif
        if (needs_arg && argc < 3) { usage(); return 1; }
    }

    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage_basic();
            return 0;
        } else if (strcmp(argv[i], "--help-advanced") == 0) {
            usage_advanced();
            return 0;
        } else if (strcmp(argv[i], "--interchange-profile-1") == 0) {
            interchange_profile = 1;
            i++;
        } else if (strcmp(argv[i], "-b") == 0 ||
                   strcmp(argv[i], "--block-size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            block_size = parse_block_size(argv[i + 1]);
            if (block_size == 0) {
                fprintf(stderr, "boltpack: invalid block size: %s\n",
                        argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-0") == 0 ||
                   strcmp(argv[i], "--store") == 0) {
            method = BP_METHOD_STORED; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-1") == 0 ||
                   strcmp(argv[i], "--boltlite") == 0) {
            method = BP_METHOD_BOLTLITE; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-2") == 0 ||
                   strcmp(argv[i], "--boltlz") == 0) {
            method = BP_METHOD_BOLTLZ; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-F") == 0 ||
                   strcmp(argv[i], "--fluff") == 0) {
            method = BP_METHOD_FLUFF; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-Z") == 0 ||
                   strcmp(argv[i], "--zstd") == 0) {
            method = BP_METHOD_ZSTD; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-l") == 0 ||
                   strcmp(argv[i], "--level") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            opt_level = atoi(argv[i + 1]);
            if (opt_level < 1 || opt_level > 9) {
                fprintf(stderr, "boltpack: -l level must be 1-9\n");
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--light") == 0) {
            opt_level = -1;
            i++;
        } else if (strcmp(argv[i], "--standard") == 0) {
            opt_level = -2;
            i++;
        } else if (strcmp(argv[i], "--best") == 0) {
            opt_level = -3;
            i++;
        } else if (strcmp(argv[i], "-qq") == 0 ||
                   strcmp(argv[i], "--silent") == 0) {
            g_verbosity = VERB_SILENT; i++;
        } else if (strcmp(argv[i], "-q") == 0 ||
                   strcmp(argv[i], "--quiet") == 0) {
            g_verbosity = VERB_QUIET; i++;
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            out_dir = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "-f") == 0 ||
                   strcmp(argv[i], "--force") == 0) {
            force = 1; i++;
        } else if (strcmp(argv[i], "-n") == 0 ||
                   strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1; i++;
#ifdef BOLTPACK_NET
        } else if (bp_net_cli_take_option(&i, argc, argv)) {
            /* consumed a network option */
#endif
        } else {
            fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]);
            usage(); return 1;
        }
    }

    if (opt_level < 0) {
        if (method == BP_METHOD_BOLTLZ)
            opt_level = (opt_level == -1) ? 1 : 2;
        else if (method == BP_METHOD_FLUFF)
            opt_level = (opt_level == -1) ? 1 : (opt_level == -2 ? 8 : 9);
        else if (method == BP_METHOD_ZSTD)
            opt_level = (opt_level == -1) ? 1 : (opt_level == -2 ? 3 : 9);
    }
    if (opt_level < 0) {
        fprintf(stderr,
                "boltpack: quality presets are only supported for BoltLZ, Fluff, and zstd\n");
        return 1;
    }
    if (opt_level > 0) {
        if (method == BP_METHOD_BOLTLZ && opt_level > 2) {
            fprintf(stderr, "boltpack: BoltLZ level must be 1-2\n");
            return 1;
        }
        if (method != BP_METHOD_BOLTLZ && method != BP_METHOD_FLUFF &&
            method != BP_METHOD_ZSTD) {
            fprintf(stderr,
                    "boltpack: -l is only supported for BoltLZ, Fluff, and zstd\n");
            return 1;
        }
    }
    if (interchange_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: Interchange Profile 1 requires create/add, BoltLite or Stored, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (command == 'x' && i + 1 < argc) {
        fprintf(stderr,
                "boltpack: extract options must precede the archive name\n");
        return 1;
    }

    if ((method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD)
        && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method,
                                opt_level, interchange_profile);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method,
                             opt_level, interchange_profile);
    case 'l':
        if (i >= argc) {
            fprintf(stderr, "boltpack: list requires archive name\n");
            return 1;
        }
        return cmd_list(argv[i]);
    case 'x':
        if (i >= argc) {
            fprintf(stderr, "boltpack: extract requires archive name\n");
            return 1;
        }
        return cmd_extract(argv[i], out_dir, force, dry_run);
    case 't':
        if (i >= argc) {
            fprintf(stderr, "boltpack: test requires archive name\n");
            return 1;
        }
        return cmd_test(argv[i]);
    case 'i':
        if (i >= argc) {
            fprintf(stderr, "boltpack: info requires archive name\n");
            return 1;
        }
        return cmd_info(argv[i]);
    case 'd': return cmd_delete(argc - i, argv + i);
#ifdef BOLTPACK_NET
    case 'L': return cmd_listen(out_dir, force);
    case 'S': return cmd_scan();
#endif
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage(); return 1;
    }
}
