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
 * Metadata (BP_TYPE_META) payload for BP_OS_ATARI:
 *   Byte 0 : TOS attribute byte (FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_ARCH)
 *            (only bits 0x27 are stored; FA_LABEL(0x08) and FA_DIR(0x10)
 *             are never written)
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
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"

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
#include <utime.h>

/* ---- constants --------------------------------------------------------- */

/* TOS supports paths up to 128 chars officially; mintlib typically allows
 * longer under MiNT.  512 is a safe upper bound. */
#define BP_MAX_PATH  512

/* Atari ST factory ROM encoding (Western European). */
#define ATARI_CP     BP_CP_ATARIST

/* Metadata payload size: 1 byte (TOS attribute flags) */
#define ATARI_META_PAYLOAD  1

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
 * meta_extract_t: used when extracting a BP_TYPE_META entry.
 * io MUST be the first member so that (meta_extract_t *)out_io is valid
 * (address of first member equals address of struct in C89).
 */
typedef struct {
    bp_io_t       io;       /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[4];   /* 1-byte Atari attribute + padding */
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

static void path_join(char *buf, size_t bufsize, const char *dir,
                      const char *name)
{
    (void)bufsize;
    if (ends_with_sep(dir)) {
        sprintf(buf, "%s%s", dir, name);
    } else {
        sprintf(buf, "%s/%s", dir, name);
    }
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

static int mkdirs(const char *path)
{
    char  tmp[BP_MAX_PATH];
    char *p;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (atari_mkdir(tmp) != 0 && errno != EEXIST) {
                *p = saved;
                return -1;
            }
            *p = saved;
        }
    }
    if (atari_mkdir(tmp) != 0 && errno != EEXIST) return -1;
    return 0;
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

/*
 * Read TOS file attributes.  Returns the masked attribute byte on success,
 * or 0 on failure.  Fattrib(path, 0, 0) returns the current attributes.
 */
static unsigned char atari_get_attrib(const char *path)
{
    long result = Fattrib(path, 0, 0);
    if (result < 0) return 0;
    return (unsigned char)((int)result & ATARI_ATTR_MASK);
}

/*
 * Write TOS file attributes.  Silently ignores errors (e.g. on FAT volumes
 * where certain bits are unsupported).
 */
static void atari_apply_attrib(const char *path, unsigned char attr)
{
    (void)Fattrib(path, 1, (int)(attr & ATARI_ATTR_MASK));
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
                          const char *fs_path)
{
    unsigned char meta_buf[ATARI_META_PAYLOAD];
    bp_io_t       meta_io;
    rbuf_ctx_t    rctx;

    meta_buf[0] = atari_get_attrib(fs_path);

    rctx.buf  = meta_buf;
    rctx.size = ATARI_META_PAYLOAD;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry(ar, arcname, BP_TYPE_META, BP_OS_ATARI,
                                &meta_io, bp_u64_from_u32((bp_uint32)ATARI_META_PAYLOAD),
                                bp_u64_from_u32(0));
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

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
    int         rc;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 2];

        if (!ends_with_sep(arcname))
            sprintf(dir_arc, "%s/", arcname);
        else
            strcpy(dir_arc, arcname);

        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", dir_arc);

        if (bp_local_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8),
                             ATARI_CP) != 0)
            strcpy(dir_arc_utf8, dir_arc);

        rc = bp_archive_add_entry(ar, dir_arc_utf8, BP_TYPE_DIR,
                                  BP_OS_ATARI, NULL, bp_u64_from_u32(0),
                                  bp_u64_from_u32((bp_uint32)st.st_mtime));
        if (rc != BP_OK) return rc;

        rc = add_meta_entry(ar, dir_arc_utf8, path);
        if (rc != BP_OK) return rc;

        return add_directory(ar, path, dir_arc);

    } else if (S_ISREG(st.st_mode)) {
        FILE      *fp;
        bp_io_t    fio;
        bp_uint64  file_size = bp_u64_from_ulong((unsigned long)st.st_size);
        char       arcname_utf8[BP_MAX_PATH * 2];

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

        if (bp_local_to_utf8(arcname, arcname_utf8, sizeof(arcname_utf8),
                             ATARI_CP) != 0)
            strcpy(arcname_utf8, arcname);

        rc = bp_archive_add_entry(ar, arcname_utf8, BP_TYPE_DATA,
                                  BP_OS_ATARI, &fio, file_size,
                                  bp_u64_from_u32((bp_uint32)st.st_mtime));
        fclose(fp);
        if (rc != BP_OK) return rc;

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
        return add_meta_entry(ar, arcname_utf8, path);
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

    if (bp_utf8_to_local(entry->name, disp_name, sizeof(disp_name),
                         ATARI_CP) != 0) {
        strncpy(disp_name, entry->name, sizeof(disp_name) - 1);
        disp_name[sizeof(disp_name) - 1] = '\0';
    }

    format_number(bp_u64_to_u32(entry->original_size),  orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_u32(entry->compressed_size), comp_buf, sizeof(comp_buf));

    printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, disp_name);
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct {
    const char *out_dir;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
} extract_ctx_t;

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t  *ctx = (extract_ctx_t *)user_data;
    char            path[BP_MAX_PATH];
    char            local_name[BP_MAX_PATH];
    FILE           *fp;
    bp_io_t        *io;
    meta_extract_t *mex;
    bp_uint32       meta_sz;

    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                         ATARI_CP) != 0) {
        strncpy(local_name, entry->name, sizeof(local_name) - 1);
        local_name[sizeof(local_name) - 1] = '\0';
    }

    if (ctx->out_dir)
        path_join(path, sizeof(path), ctx->out_dir, local_name);
    else
        strcpy(path, local_name);

    /* --- Directory ---------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", local_name);
        if (!ctx->dry_run) mkdirs(path);
        return NULL;
    }

    /* --- Atari metadata: capture into buffer for apply in done() ------ */
    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_OS_ATARI) return NULL;

        meta_sz = bp_u64_to_u32(entry->original_size);
        if (meta_sz == 0 || meta_sz > ATARI_META_PAYLOAD + 4)
            return NULL;

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

        return &mex->io; /* io is first field; cast back is valid in C89 */
    }

    /* --- Resource fork (skip) ----------------------------------------- */
    if (entry->entry_type == BP_TYPE_RSRC) {
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [resource fork - not supported on TOS]\n",
                   local_name);
        return NULL;
    }

    /* --- Regular file ------------------------------------------------- */
    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT)
        printf(" extracting: %s\n", local_name);

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
    if (!io) { fclose(fp); return NULL; }
    bp_io_from_file(io, fp);
    return io;
}

static void extract_done(const bp_entry_t *entry, bp_io_t *out,
                         void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;

    /* --- Metadata: apply TOS attributes to fs object ------------------ */
    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;
        if (entry->platform == BP_OS_ATARI && !ctx->dry_run &&
            mex->wctx.len >= ATARI_META_PAYLOAD) {
            char path[BP_MAX_PATH];
            char local_name[BP_MAX_PATH];

            if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                                 ATARI_CP) != 0) {
                strncpy(local_name, entry->name, sizeof(local_name) - 1);
                local_name[sizeof(local_name) - 1] = '\0';
            }
            if (ctx->out_dir)
                path_join(path, sizeof(path), ctx->out_dir, local_name);
            else
                strcpy(path, local_name);

            atari_apply_attrib(path, mex->buf[0]);
        }
        free(mex);
        return;
    }

    /* --- Regular file: close and set modification time ---------------- */
    {
        FILE *fp = (FILE *)out->handle;
        fclose(fp);

        if (!ctx->dry_run) {
            char           path[BP_MAX_PATH];
            char           local_name[BP_MAX_PATH];
            struct utimbuf ut;

            if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                                 ATARI_CP) != 0) {
                strncpy(local_name, entry->name, sizeof(local_name) - 1);
                local_name[sizeof(local_name) - 1] = '\0';
            }
            if (ctx->out_dir)
                path_join(path, sizeof(path), ctx->out_dir, local_name);
            else
                strcpy(path, local_name);

            ut.actime  = (time_t)entry->mtime.lo;
            ut.modtime = (time_t)entry->mtime.lo;
            utime(path, &ut);
        }
        free(out);
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
        if (bp_utf8_to_local(entry->name, disp_name, sizeof(disp_name),
                             ATARI_CP) != 0) {
            strncpy(disp_name, entry->name, sizeof(disp_name) - 1);
            disp_name[sizeof(disp_name) - 1] = '\0';
        }
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
        "  -0        Store only (no compression)\n"
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
                      bp_uint8 method)
{
    const char  *archive_name;
    FILE        *fp;
    bp_io_t      aio;
    bp_archive_t ar;
    int          rc, i;
    clock_t      start;
    double       elapsed;

    if (argc < 2) {
        fprintf(stderr, "boltpack: create requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    if (block_size == 0) block_size = atari_auto_block_size();

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
        bp_uint32 file_count = 0, dir_count = 0, j;
        char      numbuf[32], speedbuf[64];

        for (j = 0; j < ar.entries_written; j++) {
            if      (ar.entry_list[j].entry_type == BP_TYPE_DIR)  dir_count++;
            else if (ar.entry_list[j].entry_type == BP_TYPE_DATA) file_count++;
            total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
            total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
        }
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

    bp_archive_close(&ar);
    fclose(fp);
    return 0;
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

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE          *fp;
    bp_io_t        aio;
    extract_ctx_t  ctx;
    int            rc;
    clock_t        start;
    double         elapsed;

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
    ctx.out_dir = out_dir;
    ctx.force   = force;
    ctx.dry_run = dry_run;

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
    FILE        *src_fp, *dst_fp;
    bp_io_t      src_io, dst_io;
    char         tmp_name[BP_MAX_PATH];
    bp_uint32    deleted = 0;
    int          rc;

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
    sprintf(tmp_name, "%s.bp_tmp", archive_name);
    dst_fp = fopen(tmp_name, "w+b");
    if (!dst_fp) {
        fprintf(stderr, "boltpack: cannot create temp file: %s\n", tmp_name);
        fclose(src_fp); return 1;
    }

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);

    rc = bp_archive_delete(&src_io, &dst_io, del_names,
                           (bp_uint32)del_count, &deleted);
    fclose(src_fp);
    fclose(dst_fp);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        remove(tmp_name); return 1;
    }
    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        remove(tmp_name); return 1;
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

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method)
{
    const char  *archive_name;
    FILE        *fp;
    bp_io_t      io;
    bp_archive_t ar;
    int          rc, i;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    if (block_size == 0) block_size = atari_auto_block_size();

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
        fclose(fp); return 1;
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
            bp_archive_close(&ar); fclose(fp); return 1;
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

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char       command;
    bp_uint32  block_size = 0; /* 0 = auto-detect via Malloc(-1L) */
    bp_uint8   method    = BP_METHOD_BOLTLZ;
    const char *out_dir  = NULL;
    int        force     = 0;
    int        dry_run   = 0;
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
        } else if (strcmp(argv[i], "-0") == 0) {
            method = BP_METHOD_STORED; i++;
        } else if (strcmp(argv[i], "-F") == 0) {
            method = BP_METHOD_FLUFF; i++;
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

    if (method == BP_METHOD_FLUFF && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method);
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
