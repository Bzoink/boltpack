/*
 * main.c -- Haiku / BeOS CLI frontend for BoltPack
 *
 * Target:   Haiku R1 (x86_64 primary; x86 32-bit secondary)
 *           BeOS R5 (x86, PowerPC) -- same API, same binary format
 * Compiler: gcc (ships with Haiku) or x86_64-unknown-haiku-gcc cross-compiler
 *
 * POSIX I/O + directory recursion are identical to linux/main.c.
 * This frontend additionally:
 *   - On create: reads the MIME type from the BFS "BEOS:TYPE" attribute and
 *     writes a BP_TYPE_META entry (platform=BP_OS_BEOS) immediately after
 *     each BP_TYPE_DATA entry.
 *   - On extract: reads BP_TYPE_META entries with platform=BP_OS_BEOS and
 *     restores the MIME type via fs_write_attr(), then applies Unix mode via
 *     chmod().
 *
 * Metadata payload format (from format.md, platform = 0x14):
 *   Offset  Size  Field
 *   0       2     mode      Unix permissions (low 9 bits of mode_t)
 *   2       1     mime_len  Length of MIME type string
 *   3       N     mime_type MIME type bytes (no null terminator)
 *
 * HAIKU_META_MAX is 3 + 256 = 259 bytes; 256 is more than enough for any
 * valid MIME type string.
 *
 * Cross-compilation from Linux:
 *   x86_64-unknown-haiku-gcc -ansi -pedantic -Wall -Wextra -O2 \
 *       -I src/core                         \
 *       src/core/compress.c src/core/archiver.c                \
 *       src/core/bp_codepage.c                                 \
 *       src/frontend/haiku/main.c                              \
 *       src/core/boltlz.c                             \
 *       -o boltpack
 *
 * ANSI C (C89/C90) throughout (with POSIX extensions via _POSIX_C_SOURCE).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _POSIX_C_SOURCE 200112L

#define BP_CREATOR_OS  BP_OS_HAIKU
#define BP_CREATOR_CPU BP_CPU_X86_64

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"

#ifdef BOLTPACK_NET
#include "../common/bp_net_cli.h"
#endif

#include <fs_attr.h>

/* Haiku type constant for MIME string attributes; may not be in all headers */
#ifndef B_MIME_STRING_TYPE
#define B_MIME_STRING_TYPE 'MIMS'
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <utime.h>
#include <fcntl.h>

/* ---- Haiku metadata limit --------------------------------------------- */

/* MIME type strings are at most ~64 bytes in practice; 256 is generous */
#define HAIKU_MIME_MAX   256
#define HAIKU_META_MAX   (3 + HAIKU_MIME_MAX)

/* BFS attribute name for the MIME type */
#define BEOS_TYPE_ATTR   "BEOS:TYPE"

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

/* ---- read-only memory buffer I/O (for metadata payload) --------------- */

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

/* ---- write-only memory buffer I/O (for receiving metadata payload) ---- */

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
 * meta_extract_t: carries the in-progress metadata payload during extract.
 * io MUST be first so that (meta_extract_t *)out_io is valid.
 */
typedef struct {
    bp_io_t       io;                    /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[HAIKU_META_MAX + 4];
} meta_extract_t;

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
    default:                       return "unknown error";
    }
}

/* ---- speed / size / number formatting (shared with linux/main.c) ------- */

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
        if (d < 1048576.0)       sprintf(buf, "%.1f KB", d / 1024.0);
        else if (d < 1073741824.0) sprintf(buf, "%.1f MB", d / 1048576.0);
        else                     sprintf(buf, "%.2f GB", d / 1073741824.0);
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

typedef struct {
    bp_uint32 file_count_final;
    bp_uint32 dir_count;
    int       is_create;
} progress_ctx_t;

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

static void path_join(char *buf, size_t bufsize, const char *dir,
                      const char *name)
{
    if (ends_with_slash(dir))
        snprintf(buf, bufsize, "%s%s", dir, name);
    else
        snprintf(buf, bufsize, "%s/%s", dir, name);
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

/* ---- Haiku metadata encode / decode / apply ---------------------------- */

/*
 * haiku_encode_meta: build the BP_TYPE_META payload for a file.
 *   buf[0..1] = mode (little-endian uint16, low 9 bits of mode_t)
 *   buf[2]    = mime_len
 *   buf[3..N] = MIME type bytes (no null terminator)
 * Returns the total payload length.
 */
static int haiku_encode_meta(unsigned char *buf, bp_uint16 mode,
                              const char *mime, int mime_len)
{
    buf[0] = (unsigned char)(mode & 0xFF);
    buf[1] = (unsigned char)((mode >> 8) & 0xFF);
    buf[2] = (unsigned char)(mime_len & 0xFF);
    if (mime_len > 0)
        memcpy(buf + 3, mime, (size_t)mime_len);
    return 3 + mime_len;
}

/*
 * haiku_decode_meta: parse a BP_TYPE_META payload.
 * Returns 0 on success, -1 if data is too short or malformed.
 */
static int haiku_decode_meta(const unsigned char *buf, int len,
                             bp_uint16 *mode_out,
                             char *mime_out, int *mime_len_out)
{
    int mime_len;
    if (len < 3) return -1;
    *mode_out = (bp_uint16)((unsigned int)buf[0] | ((unsigned int)buf[1] << 8));
    mime_len = (int)buf[2];
    if (len < 3 + mime_len) return -1;
    *mime_len_out = mime_len;
    if (mime_len > 0)
        memcpy(mime_out, buf + 3, (size_t)mime_len);
    mime_out[mime_len] = '\0';
    return 0;
}

/*
 * haiku_read_meta: read mode and MIME type from a file on disk.
 * mime_buf must be at least HAIKU_MIME_MAX+1 bytes.
 * Returns the actual MIME string length (0 if no BEOS:TYPE attribute).
 */
static int haiku_read_meta(const char *path, bp_uint16 *mode_out,
                           char *mime_buf)
{
    struct stat st;
    int fd;
    ssize_t n;

    mime_buf[0] = '\0';
    *mode_out = 0;

    if (lstat(path, &st) != 0) return 0;
    *mode_out = (bp_uint16)(st.st_mode & 0x1FF);

    /* Read BEOS:TYPE attribute using fs_read_attr */
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    n = fs_read_attr(fd, BEOS_TYPE_ATTR, B_MIME_STRING_TYPE,
                     0, mime_buf, HAIKU_MIME_MAX);
    close(fd);

    if (n <= 0) {
        mime_buf[0] = '\0';
        return 0;
    }
    if (n > HAIKU_MIME_MAX) n = HAIKU_MIME_MAX;
    mime_buf[n] = '\0';
    /* strip trailing null if present */
    while (n > 0 && mime_buf[n - 1] == '\0') n--;
    mime_buf[n] = '\0';
    return (int)n;
}

/*
 * haiku_apply_meta: restore mode and MIME type to a file on disk.
 */
static void haiku_apply_meta(const char *path, bp_uint16 mode,
                             const char *mime, int mime_len)
{
    int fd;

    chmod(path, (mode_t)(mode & 0x1FF));

    if (mime_len <= 0) return;

    fd = open(path, O_RDWR);
    if (fd < 0) return;

    /* Write with a null terminator (Haiku convention for MIME strings) */
    fs_write_attr(fd, BEOS_TYPE_ATTR, B_MIME_STRING_TYPE,
                  0, mime, (size_t)(mime_len + 1));
    close(fd);
}

/* ---- add a meta entry after the data entry ----------------------------- */

static int add_meta_entry(bp_archive_t *ar, const char *arcname,
                          const char *fs_path)
{
    unsigned char  meta_buf[HAIKU_META_MAX + 4];
    bp_uint16      mode;
    char           mime[HAIKU_MIME_MAX + 1];
    int            mime_len;
    int            meta_len;
    bp_io_t        meta_io;
    rbuf_ctx_t     rctx;

    mime_len = haiku_read_meta(fs_path, &mode, mime);

    /* If no useful metadata exists, skip (mode=0, no MIME) */
    if (mode == 0 && mime_len == 0) return BP_OK;

    meta_len = haiku_encode_meta(meta_buf, mode, mime, mime_len);

    rctx.buf  = meta_buf;
    rctx.size = (bp_uint32)meta_len;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry(ar, arcname, BP_TYPE_META, BP_OS_BEOS,
                                &meta_io, bp_u64_from_u32((bp_uint32)meta_len),
                                bp_u64_from_u32(0));
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
        path_join(child_path, sizeof(child_path), dir_path, ent->d_name);
        path_join(child_arc,  sizeof(child_arc),  arc_prefix, ent->d_name);
        rc = add_path(ar, child_path, child_arc);
        if (rc != BP_OK) { closedir(d); return rc; }
    }
    closedir(d);
    return BP_OK;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct stat st;
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

        rc = bp_archive_add_entry(ar, dir_arc, BP_TYPE_DIR, BP_CREATOR_OS,
                                  NULL, bp_u64_from_u32(0),
                                  bp_u64_from_u32((bp_uint32)st.st_mtime));
        if (rc != BP_OK) return rc;
        rc = add_meta_entry(ar, dir_arc, path);
        if (rc != BP_OK) return rc;
        return add_directory(ar, path, dir_arc);

    } else if (S_ISREG(st.st_mode)) {
        FILE *fp;
        bp_io_t fio;
        bp_uint64 file_size = bp_off_to_u64((bp_off_t)st.st_size);

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

        rc = bp_archive_add_entry(ar, arcname, BP_TYPE_DATA, BP_CREATOR_OS,
                                  &fio, file_size,
                                  bp_u64_from_u32((bp_uint32)st.st_mtime));
        fclose(fp);
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
                char sizebuf[32];
                double orig = (double)file_size.lo + (double)file_size.hi * 4294967296.0;
                double cmp  = (double)comp.lo      + (double)comp.hi      * 4294967296.0;
                double pct  = (orig > 0.0) ? (cmp / orig * 100.0) : 0.0;
                format_size64(comp, sizebuf, sizeof(sizebuf));
                printf("  %s (%.0f%%)\n", sizebuf, pct);
            }
        }

        /* META entry follows immediately after DATA */
        rc = add_meta_entry(ar, arcname, path);
        return rc;

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

        rc = bp_archive_add_entry(ar, arcname, BP_TYPE_SYMLINK, BP_CREATOR_OS,
                                  &dummy_io,
                                  bp_u64_from_u32((bp_uint32)tlen),
                                  bp_u64_from_u32((bp_uint32)st.st_mtime));
        fclose(tmp);
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
    const char *out_dir;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
    bp_uint32   unsafe_count;  /* entries skipped for unsafe names/targets */
    int         slash_warned;  /* printed the leading-'/' warning once */
} extract_ctx_t;

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
    extract_ctx_t  *ctx = (extract_ctx_t *)user_data;
    char            path[4096];
    FILE           *fp;
    bp_io_t        *io;
    meta_extract_t *mex;
    bp_uint32       meta_sz;
    const char     *name;

    /* A hostile archive must not choose where we write: refuse ".."
     * components, backslashes, and drive letters outright. */
    name = extract_entry_name(entry, ctx);
    if (!bp_name_is_safe(name)) {
        fprintf(stderr, "boltpack: skipping entry with unsafe name: %s\n",
                entry->name);
        ctx->unsafe_count++;
        return NULL;
    }

    if (ctx->out_dir)
        path_join(path, sizeof(path), ctx->out_dir, name);
    else
        snprintf(path, sizeof(path), "%s", name);

    /* --- Directory ---------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", entry->name);
        if (!ctx->dry_run) mkdirs(path);
        return NULL;
    }

    /* --- Haiku/BeOS metadata ------------------------------------------ */
    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_OS_BEOS) return NULL;

        meta_sz = bp_u64_to_u32(entry->original_size);
        if (meta_sz == 0 || meta_sz > HAIKU_META_MAX + 4) return NULL;

        mex = (meta_extract_t *)malloc(sizeof(meta_extract_t));
        if (!mex) return NULL;

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

    /* --- Resource fork: skip ------------------------------------------ */
    if (entry->entry_type == BP_TYPE_RSRC) {
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [resource fork - not a Mac archive]\n",
                   entry->name);
        return NULL;
    }

    /* --- Symlink ------------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->file_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf("    linking: %s\n", entry->name);
        if (ctx->dry_run) return NULL;
        fp = tmpfile();
        if (fp == NULL) return NULL;
        io = (bp_io_t *)malloc(sizeof(bp_io_t));
        if (io == NULL) { fclose(fp); return NULL; }
        bp_io_from_file(io, fp);
        return io;
    }

    /* --- Regular file ------------------------------------------------- */
    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT)
        printf(" extracting: %s\n", entry->name);
    if (ctx->dry_run) return NULL;

    if (!ctx->force) {
        struct stat st_buf;
        if (stat(path, &st_buf) == 0) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n", path);
            return NULL;
        }
    }
    mkdirs_for_file(path);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot create: %s\n", path);
        return NULL;
    }
    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (io == NULL) { fclose(fp); return NULL; }
    bp_io_from_file(io, fp);
    return io;
}

static void extract_done(const bp_entry_t *entry, bp_io_t *out,
                         void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    const char *name = extract_entry_name(entry, ctx);

    /* --- Haiku metadata: apply mode and MIME type --------------------- */
    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;
        if (entry->platform == BP_OS_BEOS && !ctx->dry_run) {
            char      path[4096];
            bp_uint16 mode;
            char      mime[HAIKU_MIME_MAX + 1];
            int       mime_len;

            if (ctx->out_dir)
                path_join(path, sizeof(path), ctx->out_dir, name);
            else
                snprintf(path, sizeof(path), "%s", name);

            if (haiku_decode_meta(mex->wctx.buf, (int)mex->wctx.len,
                                  &mode, mime, &mime_len) == 0) {
                haiku_apply_meta(path, mode, mime, mime_len);
            }
        }
        free(mex);
        return;
    }

    /* --- Symlink ------------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        FILE *fp = (FILE *)out->handle;
        char  target[4096];
        char  path[4096];
        long  len;

        fseek(fp, 0, SEEK_END);
        len = ftell(fp);
        if (len > 0 && len < (long)sizeof(target)) {
            rewind(fp);
            if (fread(target, 1, (size_t)len, fp) == (size_t)len) {
                target[len] = '\0';
                /* Refuse targets that point outside the extraction root:
                 * a hostile archive could otherwise plant a link and write
                 * through it with a later entry. */
                if (!bp_symlink_target_is_safe(name, target)) {
                    fprintf(stderr,
                            "boltpack: skipping symlink with unsafe target: "
                            "%s -> %s\n", entry->name, target);
                    ctx->unsafe_count++;
                } else {
                    if (ctx->out_dir)
                        path_join(path, sizeof(path), ctx->out_dir, name);
                    else
                        snprintf(path, sizeof(path), "%s", name);
                    mkdirs_for_file(path);
                    symlink(target, path);
                }
            }
        }
        fclose(fp);
        free(out);
        return;
    }

    /* --- Regular file: close, set mtime ------------------------------- */
    {
        FILE *fp = (FILE *)out->handle;
        char path[4096];
        struct utimbuf ut;

        fclose(fp);
        if (ctx->out_dir)
            path_join(path, sizeof(path), ctx->out_dir, name);
        else
            snprintf(path, sizeof(path), "%s", name);
        ut.actime  = (time_t)entry->mtime.lo;
        ut.modtime = (time_t)entry->mtime.lo;
        utime(path, &ut);
        free(out);
    }
}

#ifdef BOLTPACK_NET
/* Extract a verified network-received archive using the Haiku callbacks. */
int bp_frontend_extract_io(bp_io_t *aio, const char *out_dir, int force)
{
    extract_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
    return bp_archive_extract_all(aio, extract_open, extract_done, NULL, &ctx);
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
    bp_uint32 deleted = 0;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }
    archive_name = argv[0];
    del_names    = (const char **)&argv[1];
    del_count    = argc - 1;

    src_fp = fopen(archive_name, "rb");
    if (!src_fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    snprintf(tmp_name, sizeof(tmp_name), "%s.bp_tmp", archive_name);
    dst_fp = fopen(tmp_name, "w+b");
    if (!dst_fp) {
        fprintf(stderr, "boltpack: cannot create temp file: %s\n", tmp_name);
        fclose(src_fp);
        return 1;
    }

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);
    rc = bp_archive_delete(&src_io, &dst_io, del_names, (bp_uint32)del_count,
                           &deleted);
    fclose(src_fp);
    fclose(dst_fp);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        remove(tmp_name);
        return 1;
    }
    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        remove(tmp_name);
        return 1;
    }
    if (remove(archive_name) != 0 || rename(tmp_name, archive_name) != 0) {
        fprintf(stderr, "boltpack: cannot replace archive\n");
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    return 0;
}

/* ---- cmd_add ----------------------------------------------------------- */

static int cmd_add(int argc, char **argv, bp_uint32 block_size, bp_uint8 method,
                   int level)
{
    const char *archive_name;
    FILE *fp;
    bp_io_t io;
    bp_archive_t ar;
    int rc, i;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }
    fp = fopen(archive_name, "r+b");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    bp_io_from_file(&io, fp);
    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        return 1;
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
            bp_archive_close(&ar); fclose(fp);
            return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    fclose(fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    return 0;
}

/* ---- cmd_create -------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int level)
{
    const char *archive_name;
    FILE *fp;
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

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }
    fp = fopen(archive_name, "w+b");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot create: %s\n", archive_name);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp); return 1;
    }
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
            bp_archive_close(&ar); fclose(fp); return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar); fclose(fp); return 1;
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
    fclose(fp);
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
    if (out_dir && !dry_run) mkdirs(out_dir);

    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir  = out_dir;
    ctx.force    = force;
    ctx.dry_run  = dry_run;
    start = clock();

    rc = bp_archive_extract_all(&aio, extract_open, extract_done,
                                g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                &ctx);
    fclose(fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
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
        return 1;
    }
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
        "  -0         Store only (no compression)\n"
        "  -1         BoltLite compression\n"
        "  -2         boltlz compression (default)\n"
        "  -F         Use Fluff compression\n"
        "  -l N       Compression level (boltlz: 1-2; Fluff/zstd: 1-9)\n"
        "  -o PATH    Output directory for extraction\n"
        "  -f         Force overwrite existing files\n"
        "  -n         Dry run\n"
        "  -q / -qq   Quiet / silent\n");
    fprintf(stderr,
        "\nExamples:\n"
        "  boltpack create backup.bolt folder\n"
        "  boltpack extract backup.bolt -o out\n"
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
        "BoltPack v%d.%d -- advanced Haiku CLI help\n"
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
        "  -0, --store      Store only (no compression)\n"
        "  -1, --boltlite       Use BoltLite compression\n"
        "  -2, --boltlz    Use boltlz compression (default)\n"
        "  -F, --fluff      Use Fluff compression\n"
        "  -l N, --level N  boltlz level 1-2; Fluff/zstd level 1-9\n"
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
            method = BP_METHOD_STORED; i++;
        } else if (strcmp(argv[i], "-1") == 0 ||
                   strcmp(argv[i], "--boltlite") == 0) {
            method = BP_METHOD_BOLTLITE; i++;
        } else if (strcmp(argv[i], "-2") == 0 ||
                   strcmp(argv[i], "--boltlz") == 0) {
            method = BP_METHOD_BOLTLZ; i++;
        } else if (strcmp(argv[i], "-F") == 0 ||
                   strcmp(argv[i], "--fluff") == 0) {
            method = BP_METHOD_FLUFF; i++;
        } else if (strcmp(argv[i], "-Z") == 0 ||
                   strcmp(argv[i], "--zstd") == 0) {
            method = BP_METHOD_ZSTD; i++;
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

    if ((method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD)
        && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method, opt_level);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method, opt_level);
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
