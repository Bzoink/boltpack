/*
 * main.c -- DOS CLI frontend for BoltPack
 *
 * ANSI C (C89/C90) with DJGPP POSIX extensions.
 * Primary target: DJGPP cross-compiler (i586-pc-msdosdjgpp-gcc).
 * For other DOS compilers (Turbo C, Watcom), minor adjustments
 * may be needed: mkdir() signature, header differences, etc.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "../../core/archiver.h"
#include "../../core/bp_time.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"
#include "../../core/bp_metadata.h"
#include "dos_rename.h"

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

#ifdef __DJGPP__
#include <io.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* ---- DOS path limit ---------------------------------------------------- */

#define BP_MAX_PATH 260

/* ---- local codepage ---------------------------------------------------- */

/* DJGPP / DOS uses CP850 for Western Europe, CP437 for US.
 * Change to BP_CP_437 if needed for a US-only build. */
#define DOS_CODEPAGE  BP_CP_850

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
    io->read  = file_read;
    io->write = file_write;
    io->seek  = file_seek;
    io->tell  = file_tell;
    io->handle = fp;
}

typedef struct {
    const bp_uint8 *data;
    bp_uint32 size;
    bp_uint32 pos;
} dos_meta_reader_t;

static long dos_meta_read(void *handle, void *buf, bp_uint32 count)
{
    dos_meta_reader_t *reader = (dos_meta_reader_t *)handle;
    bp_uint32 available = reader->size - reader->pos;
    bp_uint32 amount = count < available ? count : available;
    if (amount == 0) return 0;
    memcpy(buf, reader->data + reader->pos, amount);
    reader->pos += amount;
    return (long)amount;
}

#if defined(__DJGPP__) || defined(BP_DOS_TEST_ATTRIBUTES)
#define BP_DOS_FAT_ATTRIBUTES 1

static int dos_get_fat_attributes(const char *path, bp_uint8 *attributes)
{
#ifdef BP_DOS_TEST_ATTRIBUTES
    struct stat st;
    const char *base, *dot;
    bp_uint8 result = 0;
    if (stat(path, &st) != 0) return BP_ERR_IO;
    if (!(st.st_mode & S_IWUSR)) result |= 0x01U;
    base = strrchr(path, '/');
    {
        const char *bslash = strrchr(path, '\\');
        if (bslash != NULL && (base == NULL || bslash > base)) base = bslash;
    }
    base = base != NULL ? base + 1 : path;
    if (base[0] == '.' && base[1] != '\0') result |= 0x02U;
    dot = strrchr(base, '.');
    if (dot != NULL &&
        ((dot[1] == 'S' || dot[1] == 's') &&
         (dot[2] == 'Y' || dot[2] == 'y') &&
         (dot[3] == 'S' || dot[3] == 's') && dot[4] == '\0'))
        result |= 0x04U;
    if (S_ISREG(st.st_mode)) result |= 0x20U;
    *attributes = result;
    return BP_OK;
#else
    int result = _chmod(path, 0, 0);
    if (result < 0) return BP_ERR_IO;
    *attributes = (bp_uint8)((unsigned)result & BP_META_FAT_MASK);
    return BP_OK;
#endif
}

static int dos_apply_fat_attributes(const char *path, bp_uint8 valid,
                                    bp_uint8 value)
{
    bp_uint8 current;
    bp_uint8 desired;
    int rc = dos_get_fat_attributes(path, &current);
    if (rc != BP_OK) return BP_ERR_PARTIAL;
    desired = (bp_uint8)((current & (bp_uint8)(BP_META_FAT_MASK & ~valid)) |
                         (value & valid));
#ifdef BP_DOS_TEST_ATTRIBUTES
    {
        struct stat st;
        mode_t mode;
        if (((desired ^ current) & (bp_uint8)~0x01U) != 0)
            return BP_ERR_PARTIAL;
        if ((valid & 0x01U) && ((desired ^ current) & 0x01U)) {
            if (stat(path, &st) != 0) return BP_ERR_PARTIAL;
            mode = st.st_mode;
            if (desired & 0x01U) mode &= (mode_t)~0222;
            else mode |= (mode_t)0200;
            if (chmod(path, mode) != 0) return BP_ERR_PARTIAL;
        }
    }
#elif defined(__DJGPP__)
    {
        int set_value = (int)(desired & BP_META_FAT_MASK);
        int result = _chmod(path, 1, (mode_t)set_value);
        if (result < 0) return BP_ERR_PARTIAL;
    }
#endif
    rc = dos_get_fat_attributes(path, &current);
    return rc == BP_OK && (current & valid) == (value & valid) ?
           BP_OK : BP_ERR_PARTIAL;
}

static int dos_add_portable_fat_metadata(bp_archive_t *ar,
                                         const char *archive_name,
                                         bp_uint8 subject_type,
                                         const char *source_path)
{
    bp_uint8 payload[BP_META_FAT_SIZE];
    bp_uint8 attr;
    dos_meta_reader_t reader;
    bp_io_t io;
    int rc;
    rc = dos_get_fat_attributes(source_path, &attr);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot read DOS file attributes: %s\n",
                source_path);
        return rc;
    }
    rc = bp_metadata_fat_encode(payload, sizeof(payload), subject_type,
                                BP_META_FAT_MASK, attr);
    if (rc != BP_OK) return rc;
    reader.data = payload;
    reader.size = (bp_uint32)sizeof(payload);
    reader.pos = 0;
    io.read = dos_meta_read;
    io.write = NULL;
    io.seek = NULL;
    io.tell = NULL;
    io.handle = &reader;
    return bp_archive_add_entry_ex(ar, archive_name, BP_TYPE_META,
                                   BP_META_PORTABLE, &io,
                                   bp_u64_from_u32((bp_uint32)sizeof(payload)),
                                   bp_u64_from_u32(0), 0);
}
#else
static int dos_get_fat_attributes(const char *path, bp_uint8 *attributes)
{
    (void)path;
    (void)attributes;
    return BP_ERR_PARTIAL;
}

static int dos_apply_fat_attributes(const char *path, bp_uint8 valid,
                                    bp_uint8 value)
{
    (void)path;
    (void)valid;
    (void)value;
    return BP_ERR_PARTIAL;
}
#endif

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
    int len, i, j, commas;

    sprintf(tmp, "%lu", (unsigned long)n);
    len = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > bufsize) {
        strcpy(buf, tmp);
        return;
    }
    j = len + commas;
    buf[j] = '\0';
    for (i = len - 1; i >= 0; i--) {
        buf[--j] = tmp[i];
        if (i > 0 && (len - i) % 3 == 0) {
            buf[--j] = ',';
        }
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

/* return 1 if path ends with / or \ */
static int ends_with_sep(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && (s[len - 1] == '/' || s[len - 1] == '\\');
}

/* concatenate dir + "/" + name into buf */
static void path_join(char *buf, size_t bufsize, const char *dir,
                      const char *name)
{
    if (ends_with_sep(dir)) {
        snprintf(buf, bufsize, "%s%s", dir, name);
    } else {
        snprintf(buf, bufsize, "%s/%s", dir, name);
    }
}

/* strip leading "./" or ".\" */
static const char *clean_path(const char *path)
{
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) {
        path += 2;
    }
    return path;
}

/* convert backslashes to forward slashes in-place (for archive names) */
static void normalize_separators(char *path)
{
    char *p;
    for (p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* ---- mkdir -p ---------------------------------------------------------- */

static int dos_mkdir(const char *path)
{
    return mkdir(path, 0755);
}

static int mkdirs(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *p;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (dos_mkdir(tmp) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (dos_mkdir(tmp) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* ensure parent directory of path exists */
static int mkdirs_for_file(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *slash;
    char *bslash;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    /* find last separator (either / or \) */
    slash = strrchr(tmp, '/');
    bslash = strrchr(tmp, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }

    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        if (mkdirs(tmp) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- parse block size -------------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (strcmp(s, "8k") == 0 || strcmp(s, "8K") == 0)
        return BP_BLOCK_8K;
    if (strcmp(s, "16k") == 0 || strcmp(s, "16K") == 0)
        return BP_BLOCK_16K;
    if (strcmp(s, "32k") == 0 || strcmp(s, "32K") == 0)
        return BP_BLOCK_32K;
    if (strcmp(s, "64k") == 0 || strcmp(s, "64K") == 0)
        return BP_BLOCK_64K;
    if (strcmp(s, "256k") == 0 || strcmp(s, "256K") == 0)
        return BP_BLOCK_256K;
    return 0;
}

/* ---- recursive add ----------------------------------------------------- */

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int add_directory(bp_archive_t *ar, const char *dir_path,
                         const char *arc_prefix)
{
    DIR *d;
    struct dirent *ent;
    int rc;
    char child_path[BP_MAX_PATH];
    char child_arc[BP_MAX_PATH];

    d = opendir(dir_path);
    if (d == NULL) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", dir_path);
        return BP_ERR_IO;
    }

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        path_join(child_path, sizeof(child_path), dir_path, ent->d_name);
        path_join(child_arc, sizeof(child_arc), arc_prefix, ent->d_name);

        rc = add_path(ar, child_path, child_arc);
        if (rc != BP_OK) {
            closedir(d);
            return rc;
        }
    }

    closedir(d);
    return BP_OK;
}

static bp_uint16 dos_archive_mtime(time_t value, bp_uint64 *mtime)
{
    bp_off_t seconds;

    *mtime = bp_u64_from_u32(0);
    seconds = (bp_off_t)value;
    if (seconds < 0 || (time_t)seconds != value)
        return 0;
    *mtime = bp_off_to_u64(seconds);
    return BP_ENTRY_FLAG_MTIME_VALID;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct stat st;
    int rc;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 2];
        bp_uint64 mtime;
        bp_uint16 mtime_flags;

        /* ensure arc name ends with '/' */
        if (!ends_with_sep(arcname)) {
            sprintf(dir_arc, "%s/", arcname);
        } else {
            strcpy(dir_arc, arcname);
        }

        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", dir_arc);
        }

        /* Store as UTF-8 in the archive */
        if (bp_local_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8),
                             DOS_CODEPAGE) != 0) return BP_ERR_INVALID_ARG;

        mtime_flags = dos_archive_mtime(st.st_mtime, &mtime);
        rc = bp_archive_add_entry_ex(ar, dir_arc_utf8, BP_TYPE_DIR, BP_OS_DOS,
                                     NULL, bp_u64_from_u32(0), mtime,
                                     mtime_flags);
        if (rc != BP_OK) return rc;

#ifdef BP_DOS_FAT_ATTRIBUTES
        rc = dos_add_portable_fat_metadata(ar, dir_arc_utf8, BP_TYPE_DIR, path);
        if (rc != BP_OK) return rc;
#endif

        return add_directory(ar, path, dir_arc);

    } else if (S_ISREG(st.st_mode)) {
        FILE *fp;
        bp_io_t fio;
        bp_uint64 file_size = bp_u64_from_ulong((unsigned long)st.st_size);
        bp_uint64 mtime;
        bp_uint16 mtime_flags;
        char arcname_utf8[BP_MAX_PATH * 2];

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

        /* Store as UTF-8 in the archive */
        if (bp_local_to_utf8(arcname, arcname_utf8, sizeof(arcname_utf8),
                             DOS_CODEPAGE) != 0) {
            fclose(fp);
            return BP_ERR_INVALID_ARG;
        }

        mtime_flags = dos_archive_mtime(st.st_mtime, &mtime);
        rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DATA, BP_OS_DOS,
                                     &fio, file_size, mtime, mtime_flags);
        fclose(fp);

        if (rc != BP_OK) return rc;

#ifdef BP_DOS_FAT_ATTRIBUTES
        rc = dos_add_portable_fat_metadata(ar, arcname_utf8, BP_TYPE_DATA, path);
        if (rc != BP_OK) return rc;
#endif

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e = &ar->entry_list[ar->entries_written - 1];
            bp_uint32 comp = bp_u64_to_u32(e->compressed_size);
            bp_uint32 orig = bp_u64_to_u32(file_size);
            if (comp == 0 || e->method == BP_METHOD_STORED
                || comp >= orig) {
                char sizebuf[32];
                format_size(orig, sizebuf, sizeof(sizebuf));
                printf("  %s (stored)\n", sizebuf);
            } else {
                char sizebuf[32];
                double pct = (double)comp / (double)orig * 100.0;
                format_size(comp, sizebuf, sizeof(sizebuf));
                printf("  %s (%.0f%%)\n", sizebuf, pct);
            }
        }

        return BP_OK;
    }

    /* skip other file types (no symlinks, devices, etc. on DOS) */
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

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
    } else {
        ctx->file_count++;
    }
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);

    format_number(bp_u64_to_u32(entry->original_size), orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_u32(entry->compressed_size), comp_buf, sizeof(comp_buf));

    {
        char disp_name[BP_MAX_PATH];
        if (bp_utf8_to_local(entry->name, disp_name, sizeof(disp_name),
                             DOS_CODEPAGE) != 0) {
            strncpy(disp_name, entry->name, BP_MAX_PATH - 1);
            disp_name[BP_MAX_PATH - 1] = '\0';
        }
        printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, disp_name);
    }
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

#define DOS_DIR_QUEUE_MAX 256U
#define DOS_DIR_QUEUE_BYTES_MAX (32UL * 1024UL)

typedef struct dos_dir_restore_s {
    struct dos_dir_restore_s *next;
    char *path;
    bp_uint64 mtime;
    bp_uint16 flags;
    unsigned char fat_valid;
    unsigned char fat_value;
    int fat_seen;
} dos_dir_restore_t;

typedef struct {
    const char *out_dir;    /* -o PATH or NULL */
    int         force;      /* -f flag */
    int         dry_run;    /* -n flag */
    bp_uint32   file_count;
    bp_uint32   dir_count;
    int         mtime_errors;
    int         skip_errors;
    int         metadata_errors;
    char        target_path[BP_MAX_PATH];
    char        stage_path[BP_MAX_PATH];
    char        subject_path[BP_MAX_PATH];
    char       *subject_name;
    int         subject_type;
    int         subject_ok;
    int         subject_native_seen;
    struct stat stage_open_stat;
    int         stage_open_stat_valid;
    dos_dir_restore_t *directories;
    dos_dir_restore_t *subject_directory;
    unsigned int directory_count;
    unsigned long directory_path_bytes;
} extract_ctx_t;

typedef struct {
    bp_io_t io;
    int fd;
} dos_stage_output_t;

static void dos_subject_clear(extract_ctx_t *ctx)
{
    free(ctx->subject_name);
    ctx->subject_name = NULL;
    ctx->subject_path[0] = '\0';
    ctx->subject_type = 0;
    ctx->subject_ok = 0;
    ctx->subject_native_seen = 0;
    ctx->subject_directory = NULL;
}

static int dos_subject_set(extract_ctx_t *ctx, const bp_entry_t *entry,
                           const char *path, dos_dir_restore_t *directory)
{
    size_t length = strlen(entry->name);
    char *name = (char *)malloc(length + 1U);
    if (name == NULL || (path != NULL && strlen(path) >= sizeof(ctx->subject_path))) {
        free(name);
        ctx->metadata_errors++;
        return BP_ERR_PARTIAL;
    }
    memcpy(name, entry->name, length + 1U);
    free(ctx->subject_name);
    ctx->subject_name = name;
    if (path != NULL) strcpy(ctx->subject_path, path);
    else ctx->subject_path[0] = '\0';
    ctx->subject_type = entry->entry_type;
    ctx->subject_native_seen = 0;
    ctx->subject_directory = directory;
    ctx->subject_ok = 1;
    return BP_OK;
}

static dos_dir_restore_t *dos_queue_directory(extract_ctx_t *ctx,
                                              const char *path,
                                              const bp_entry_t *entry)
{
    dos_dir_restore_t *node;
    size_t path_size = strlen(path) + 1U;
    if (ctx->directory_count >= DOS_DIR_QUEUE_MAX ||
        path_size > DOS_DIR_QUEUE_BYTES_MAX - ctx->directory_path_bytes) {
        ctx->metadata_errors++;
        return NULL;
    }
    node = (dos_dir_restore_t *)calloc(1, sizeof(*node));
    if (node == NULL) { ctx->metadata_errors++; return NULL; }
    node->path = (char *)malloc(path_size);
    if (node->path == NULL) {
        free(node);
        ctx->metadata_errors++;
        return NULL;
    }
    memcpy(node->path, path, path_size);
    node->mtime = entry->mtime;
    node->flags = entry->flags;
    node->next = ctx->directories;
    ctx->directories = node;
    ctx->directory_count++;
    ctx->directory_path_bytes += (unsigned long)path_size;
    return node;
}

static int dos_path_depth(const char *path)
{
    int depth = 0;
    for (; *path; path++)
        if (*path == '/' || *path == '\\') depth++;
    return depth;
}

static void dos_finish_directories(extract_ctx_t *ctx, int restore)
{
    while (ctx->directories != NULL) {
        dos_dir_restore_t **best_link = &ctx->directories;
        dos_dir_restore_t **link;
        dos_dir_restore_t *node;
        int best_depth = -1;
        for (link = &ctx->directories; *link != NULL; link = &(*link)->next) {
            int depth = dos_path_depth((*link)->path);
            if (depth > best_depth) {
                best_depth = depth;
                best_link = link;
            }
        }
        node = *best_link;
        *best_link = node->next;
        if (restore && (node->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
            bp_restore_mtime(node->path, node->mtime) != BP_OK) {
            fprintf(stderr, "boltpack: cannot restore directory timestamp: %s\n",
                    node->path);
            ctx->mtime_errors++;
        }
        if (restore && node->fat_seen && node->fat_valid != 0 &&
            dos_apply_fat_attributes(node->path, node->fat_valid,
                                     node->fat_value) != BP_OK) {
            fprintf(stderr, "boltpack: cannot restore directory attributes: %s\n",
                    node->path);
            ctx->metadata_errors++;
        }
        if (ctx->directory_count != 0) ctx->directory_count--;
        if (ctx->directory_path_bytes >= (unsigned long)strlen(node->path) + 1UL)
            ctx->directory_path_bytes -= (unsigned long)strlen(node->path) + 1UL;
        free(node->path);
        free(node);
    }
    ctx->subject_directory = NULL;
}

#define DOS_NAME_BUCKETS 1021U
#define DOS_NAME_NODE_LIMIT 65535U
#define DOS_NAME_KEY_LIMIT (16UL * 1024UL * 1024UL)

typedef struct dos_name_node_s {
    struct dos_name_node_s *next;
    char *keys;
    unsigned char kind;
    unsigned char explicit_dir;
} dos_name_node_t;

typedef struct {
    dos_name_node_t **buckets;
    unsigned long nodes;
    unsigned long key_bytes;
} dos_name_index_t;

typedef struct {
    dos_name_index_t *index;
    const char *out_dir;
    int long_names;
    int error;
} dos_preflight_t;

static void dos_name_index_free(dos_name_index_t *index)
{
    unsigned i;
    if (index->buckets == NULL) return;
    for (i = 0; i < DOS_NAME_BUCKETS; i++) {
        dos_name_node_t *node = index->buckets[i];
        while (node != NULL) {
            dos_name_node_t *next = node->next;
            free(node->keys);
            free(node);
            node = next;
        }
    }
    free(index->buckets);
    index->buckets = NULL;
    index->nodes = 0;
    index->key_bytes = 0;
}

static unsigned long dos_name_hash(const char *s)
{
    unsigned long hash = 2166136261UL;
    while (*s) hash = (hash ^ (unsigned char)*s++) * 16777619UL;
    return hash;
}

static int dos_component_supported(const char *component, size_t length)
{
    size_t stem = 0, i;
    char upper[5];
    if (length == 0 || component[length - 1] == '.' ||
        component[length - 1] == ' ') return 0;
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)component[i];
        if (ch < 0x20U || ch == '\\' || strchr("\"*:<>?|", ch) != NULL)
            return 0;
    }
    for (i = 0; i < length; i++)
        if (component[i] == ' ' && (i + 1U == length || component[i + 1U] == '.'))
            return 0;
    while (stem < length && component[stem] != '.') stem++;
    if (stem > 4U) return 1;
    for (i = 0; i < stem; i++) {
        unsigned char ch = (unsigned char)component[i];
        upper[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    }
    upper[stem] = '\0';
    if (strcmp(upper, "CON") == 0 || strcmp(upper, "PRN") == 0 ||
        strcmp(upper, "AUX") == 0 || strcmp(upper, "NUL") == 0) return 0;
    if (stem == 4 && upper[3] >= '1' && upper[3] <= '9' &&
        (memcmp(upper, "COM", 3) == 0 || memcmp(upper, "LPT", 3) == 0))
        return 0;
    return 1;
}

/* Return true only for a component that can be represented without a
 * generated short-name alias.  DJGPP has the platform's own 8.3 predicate;
 * the portable fallback accepts a conservative ASCII subset. */
static int dos_component_is_83(const char *component, size_t length)
{
#ifdef __DJGPP__
    char name[BP_MAX_PATH];
    if (length == 0 || length >= sizeof(name)) return 0;
    memcpy(name, component, length);
    name[length] = '\0';
    return _is_DOS83(name) != 0;
#else
    size_t stem = 0, ext = 0, i;
    if (length == 0) return 0;
    while (stem < length && component[stem] != '.') stem++;
    if (stem == 0 || stem > 8U) return 0;
    if (stem < length) {
        if (stem + 1U == length) return 0;
        ext = length - stem - 1U;
        if (ext > 3U || memchr(component + stem + 1U, '.', ext) != NULL)
            return 0;
    }
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)component[i];
        if (ch == '.') continue;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || strchr("$%'-_@~`!(){}^#&", ch)))
            return 0;
    }
    return 1;
#endif
}

static int dos_output_path_is_83(const char *path)
{
    const char *p = path;
    const char *start;
    size_t length;
    if (p == NULL || *p == '\0') return 1;
    if (((p[0] >= 'A' && p[0] <= 'Z') ||
         (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;
    start = p;
    for (;;) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            length = (size_t)(p - start);
            if (length != 0 &&
                !((length == 1U && start[0] == '.') ||
                  (length == 2U && start[0] == '.' && start[1] == '.')) &&
                (!dos_component_supported(start, length) ||
                 !dos_component_is_83(start, length))) return 0;
            if (*p == '\0') break;
            start = p + 1;
        }
        p++;
    }
    return 1;
}

static int dos_destination_has_lfn(const char *path)
{
#ifdef __DJGPP__
    return _use_lfn(path != NULL && *path != '\0' ? path : ".") != 0;
#elif defined(BP_DOS_TEST_ATTRIBUTES)
    const char *no_lfn = getenv("BP_DOS_TEST_NO_LFN");
    (void)path;
    return no_lfn == NULL || strcmp(no_lfn, "1") != 0;
#else
    (void)path;
    return 0;
#endif
}

static int dos_name_index_add(dos_name_index_t *index, const char *exact,
                              int final, unsigned char kind)
{
    char folded[BP_MAX_PATH];
    unsigned long hash, key_size;
    unsigned bucket;
    dos_name_node_t *node;
    size_t length = strlen(exact);

    if (bp_local_casefold(exact, folded, sizeof(folded), DOS_CODEPAGE) != 0)
        return BP_ERR_PARTIAL;
    hash = dos_name_hash(folded);
    bucket = (unsigned)(hash % DOS_NAME_BUCKETS);
    for (node = index->buckets[bucket]; node != NULL; node = node->next) {
        const char *old_exact = node->keys;
        const char *old_folded = old_exact + strlen(old_exact) + 1;
        if (dos_name_hash(old_folded) != hash || strcmp(old_folded, folded) != 0)
            continue;
        if (strcmp(old_exact, exact) != 0) return BP_ERR_CORRUPT;
        if (!final) return node->kind == BP_TYPE_DIR ? BP_OK : BP_ERR_CORRUPT;
        if (kind != BP_TYPE_DIR) return BP_ERR_CORRUPT;
        if (node->kind != BP_TYPE_DIR || node->explicit_dir) return BP_ERR_CORRUPT;
        node->explicit_dir = 1;
        return BP_OK;
    }

    if (index->nodes >= DOS_NAME_NODE_LIMIT) return BP_ERR_OVERFLOW;
    key_size = (unsigned long)(2U * (length + 1U));
    if (key_size > DOS_NAME_KEY_LIMIT - index->key_bytes) return BP_ERR_OVERFLOW;
    node = (dos_name_node_t *)malloc(sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->keys = (char *)malloc((size_t)key_size);
    if (node->keys == NULL) { free(node); return BP_ERR_NOMEM; }
    memcpy(node->keys, exact, length + 1U);
    memcpy(node->keys + length + 1U, folded, length + 1U);
    node->kind = final ? kind : BP_TYPE_DIR;
    node->explicit_dir = (unsigned char)(final && kind == BP_TYPE_DIR);
    node->next = index->buckets[bucket];
    index->buckets[bucket] = node;
    index->nodes++;
    index->key_bytes += key_size;
    return BP_OK;
}

static int dos_preflight_entry(const bp_entry_t *entry, void *user_data)
{
    dos_preflight_t *preflight = (dos_preflight_t *)user_data;
    char local[BP_MAX_PATH], component[BP_MAX_PATH], full[BP_MAX_PATH];
    size_t length, component_length;
    const char *p, *segment_start;
    int rc;

    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    if (!bp_name_is_safe(entry->name) ||
        bp_utf8_to_local(entry->name, local, sizeof(local), DOS_CODEPAGE) != 0) {
        preflight->error = BP_ERR_PARTIAL;
        return 1;
    }
    length = strlen(local);
    if (entry->entry_type == BP_TYPE_DIR && length > 0 && local[length - 1] == '/')
        local[--length] = '\0';
    if (length == 0 ||
        (preflight->out_dir != NULL &&
         strlen(preflight->out_dir) + length + 2U >= BP_MAX_PATH)) {
        preflight->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }
    if (preflight->out_dir != NULL)
        path_join(full, sizeof(full), preflight->out_dir, local);
    else if (length < sizeof(full)) strcpy(full, local);
    else { preflight->error = BP_ERR_NAME_TOO_LONG; return 1; }

    segment_start = local;
    for (p = local; ; p++) {
        if (*p == '/' || *p == '\0') {
            int final = (*p == '\0');
            if (!dos_component_supported(segment_start,
                                         (size_t)(p - segment_start))) {
                preflight->error = BP_ERR_PARTIAL;
                return 1;
            }
            if (!preflight->long_names &&
                !dos_component_is_83(segment_start,
                                     (size_t)(p - segment_start))) {
                preflight->error = BP_ERR_PARTIAL;
                return 1;
            }
            component_length = (size_t)(p - local);
            if (component_length == 0 || component_length >= sizeof(component)) {
                preflight->error = BP_ERR_CORRUPT;
                return 1;
            }
            memcpy(component, local, component_length);
            component[component_length] = '\0';
            rc = dos_name_index_add(preflight->index, component, final,
                                    final ? entry->entry_type : BP_TYPE_DIR);
            if (rc != BP_OK) { preflight->error = rc; return 1; }
            if (final) break;
            segment_start = p + 1;
        }
    }
    return 0;
}

static int dos_preflight_archive(bp_io_t *io, const char *out_dir)
{
    dos_name_index_t index;
    dos_preflight_t preflight;
    int rc;
    memset(&index, 0, sizeof(index));
    index.buckets = (dos_name_node_t **)calloc(DOS_NAME_BUCKETS,
                                                sizeof(*index.buckets));
    if (index.buckets == NULL) return BP_ERR_NOMEM;
    preflight.index = &index;
    preflight.out_dir = out_dir;
    preflight.long_names = dos_destination_has_lfn(out_dir);
    preflight.error = BP_OK;
    if (!preflight.long_names && !dos_output_path_is_83(out_dir)) {
        dos_name_index_free(&index);
        return BP_ERR_PARTIAL;
    }
    rc = bp_archive_list(io, dos_preflight_entry, &preflight);
    if (preflight.error != BP_OK) rc = preflight.error;
    dos_name_index_free(&index);
    return rc;
}

static bp_uint32 g_extract_temp_id = 1;

static int dos_open_unique_sibling(const char *target, char *stage,
                                   size_t stage_size, const char *prefix,
                                   const char *suffix, int access_mode)
{
    const char *slash = strrchr(target, '/');
    const char *bslash = strrchr(target, '\\');
    size_t parent_len, need;
    unsigned long tries;
    size_t prefix_len = strlen(prefix), suffix_len = strlen(suffix);

    if (bslash != NULL && (slash == NULL || bslash > slash)) slash = bslash;
    parent_len = slash != NULL ? (size_t)(slash - target + 1) : 0;
    need = parent_len + prefix_len + 6U + suffix_len + 1U;
    if (need > stage_size) return -1;

    for (tries = 0; tries < 0x1000000UL; tries++) {
        int fd;
        unsigned long id = (unsigned long)(g_extract_temp_id++ & 0xFFFFFFUL);
        if (g_extract_temp_id == 0) g_extract_temp_id = 1;
        if (parent_len != 0) memcpy(stage, target, parent_len);
        sprintf(stage + parent_len, "%s%06lX%s", prefix, id, suffix);
        fd = open(stage, access_mode | O_CREAT | O_EXCL | O_BINARY, 0666);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

/* DOS has no mkstemp on every supported runtime. Reserve a short 8.3-style
 * sibling with O_EXCL so the staged output cannot truncate an existing file. */
static int extract_open_stage(const char *target, char *stage,
                              size_t stage_size)
{
    return dos_open_unique_sibling(target, stage, stage_size, "BP", ".TMP",
                                   O_WRONLY);
}

static long extract_stage_write(void *handle, const void *data,
                                bp_uint32 count)
{
    dos_stage_output_t *output = (dos_stage_output_t *)handle;
    const char *bytes = (const char *)data;
    unsigned long written = 0;
    while (written < (unsigned long)count) {
        long n = (long)write(output->fd, bytes + written,
                             (unsigned long)count - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return written == 0 ? -1L : (long)written;
        written += (unsigned long)n;
    }
    return (long)written;
}

static int extract_backup_name(const char *target, char *backup,
                               size_t backup_size)
{
    const char *slash = strrchr(target, '/');
    const char *bslash = strrchr(target, '\\');
    size_t parent_len, need;
    unsigned long tries;

    if (bslash != NULL && (slash == NULL || bslash > slash)) slash = bslash;
    parent_len = slash != NULL ? (size_t)(slash - target + 1) : 0;
    need = parent_len + 2U + 6U + 4U + 1U;
    if (need > backup_size) return -1;

    for (tries = 0; tries < 0x1000000UL; tries++) {
        struct stat st;
        unsigned long id = (unsigned long)(g_extract_temp_id++ & 0xFFFFFFUL);
        if (g_extract_temp_id == 0) g_extract_temp_id = 1;
        if (parent_len != 0) memcpy(backup, target, parent_len);
        sprintf(backup + parent_len, "BK%06lX.BAK", id);
        if (stat(backup, &st) != 0 && errno == ENOENT) return 0;
    }
    return -1;
}

static int dos_archive_same_renamed_stat(const struct stat *a,
                                         const struct stat *b);
static int dos_stage_path_matches(const char *path,
                                  const struct stat *opened);

static int extract_publish(extract_ctx_t *ctx)
{
    struct stat st, backup_stat;
    char backup[BP_MAX_PATH];
    int have_target = stat(ctx->target_path, &st) == 0;
    if (have_target) {
        if (!ctx->force || !S_ISREG(st.st_mode)) return BP_ERR_IO;
        if (extract_backup_name(ctx->target_path, backup, sizeof(backup)) != 0)
            return BP_ERR_IO;
        if (dos_rename_no_replace(ctx->target_path, backup) != 0)
            return BP_ERR_IO;
        if (lstat(backup, &backup_stat) != 0 ||
            !dos_archive_same_renamed_stat(&backup_stat, &st)) {
            if (dos_rename_no_replace(backup, ctx->target_path) != 0)
                fprintf(stderr,
                        "boltpack: existing file remains at recovery path: %s\n",
                        backup);
            return BP_ERR_IO;
        }
        if (dos_rename_no_replace(ctx->stage_path, ctx->target_path) != 0) {
            int restore_rc = dos_rename_no_replace(backup, ctx->target_path);
            if (restore_rc != 0)
                fprintf(stderr,
                        "boltpack: existing file remains at recovery path: %s\n",
                        backup);
            return BP_ERR_IO;
        }
        if (remove(backup) != 0) return BP_ERR_PARTIAL;
        return BP_OK;
    }
    if (errno != ENOENT) return BP_ERR_IO;
    return dos_rename_no_replace(ctx->stage_path, ctx->target_path) == 0 ?
           BP_OK : BP_ERR_IO;
}

static int dos_archive_stat(const char *path, int *exists,
                           struct stat *snapshot)
{
    if (lstat(path, snapshot) == 0) {
        if (!S_ISREG(snapshot->st_mode) || snapshot->st_nlink > 1)
            return BP_ERR_IO;
        *exists = 1;
        return BP_OK;
    }
    if (errno == ENOENT) {
        memset(snapshot, 0, sizeof(*snapshot));
        *exists = 0;
        return BP_OK;
    }
    return BP_ERR_IO;
}

static int dos_archive_same_stat(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_nlink == b->st_nlink &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_ctime == b->st_ctime;
}

/* The staged extraction object must still be reachable at its original path
   after the output handle closes. Compare identity and length only: DOS FAT
   wrappers can report mode and timestamps differently across open/path stats. */
static int dos_stage_same_object(const struct stat *opened,
                                 const struct stat *named)
{
    return S_ISREG(opened->st_mode) && S_ISREG(named->st_mode) &&
           opened->st_dev == named->st_dev &&
           opened->st_ino == named->st_ino &&
           opened->st_size == named->st_size;
}

static int dos_stage_path_matches(const char *path,
                                  const struct stat *opened)
{
    struct stat named;
    return path != NULL && opened != NULL && lstat(path, &named) == 0 &&
           dos_stage_same_object(opened, &named);
}

static void dos_remove_stage_if_same(const char *path,
                                     const struct stat *opened)
{
    if (dos_stage_path_matches(path, opened)) remove(path);
}

#ifdef BP_TEST_DOS_PARENT_SUBSTITUTION
static int g_dos_test_parent_substituted;

static void dos_test_write_marker(const char *name, const char *value)
{
    FILE *fp = fopen(name, "wb");
    if (fp == NULL) return;
    fputs(value, fp);
    fclose(fp);
}

static void dos_test_substitute_stage_parent(extract_ctx_t *ctx)
{
    char parent[BP_MAX_PATH], sentinel[BP_MAX_PATH];
    char *slash, *backslash;
    FILE *fp;

    if (g_dos_test_parent_substituted) return;
    strcpy(parent, ctx->target_path);
    slash = strrchr(parent, '/');
    backslash = strrchr(parent, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash))
        slash = backslash;
    if (slash == NULL) return;
    *slash = '\0';
    if (rename(parent, "RACEPARK") != 0 || mkdir(parent, 0755) != 0) {
        fprintf(stderr, "DOS test hook could not substitute extraction parent\n");
        return;
    }
    strcpy(sentinel, parent);
    strcat(sentinel, "\\KEEP.TXT");
    fp = fopen(sentinel, "wb");
    if (fp == NULL) return;
    fputs("new-root-sentinel\r\n", fp);
    fclose(fp);
    /* Put a different file at the stale stage pathname. A path-only publisher
       would mistake it for the verified object still held by the closed stage. */
    fp = fopen(ctx->stage_path, "wb");
    if (fp == NULL) return;
    fputs("counterfeit-stage\r\n", fp);
    fclose(fp);
    g_dos_test_parent_substituted = 1;
    dos_test_write_marker("RACEHOOK.TXT", "parent substituted after stage close\r\n");
    fprintf(stderr, "DOS test hook substituted output parent after stage close\n");
}
#endif

/* FAT implementations may assign a new inode number when rename moves a
   directory entry. Verify the archived object fields after the recovery rename
   without treating that path-derived inode as a content identity. */
static int dos_archive_same_renamed_stat(const struct stat *a,
                                         const struct stat *b)
{
    return a->st_dev == b->st_dev &&
           S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_nlink == b->st_nlink && a->st_size == b->st_size &&
           a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
}

static int dos_archive_capture_attributes(const char *path, int exists,
                                          bp_uint8 *valid, bp_uint8 *value)
{
    *valid = 0;
    *value = 0;
    if (!exists) return BP_OK;
#ifdef BP_DOS_FAT_ATTRIBUTES
    {
        bp_uint8 attr;
        int rc = dos_get_fat_attributes(path, &attr);
        if (rc != BP_OK || (attr & 0x01U)) return BP_ERR_IO;
        *valid = (bp_uint8)(BP_META_FAT_MASK & (bp_uint8)~0x01U);
        *value = attr;
    }
#else
    (void)path;
#endif
    return BP_OK;
}

static FILE *dos_archive_open_stage(const char *target, char *stage,
                                    size_t stage_size)
{
    int fd = dos_open_unique_sibling(target, stage, stage_size,
                                     "BP", ".TMP", O_RDWR);
    FILE *fp;
    if (fd < 0) return NULL;
    fp = fdopen(fd, "w+b");
    if (fp == NULL) {
        close(fd);
        remove(stage);
    }
    return fp;
}

static int dos_archive_copy(FILE *source, FILE *destination)
{
    unsigned char buffer[8192];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), source)) != 0) {
        if (fwrite(buffer, 1, count, destination) != count) return BP_ERR_IO;
    }
    if (ferror(source) || fflush(destination) != 0) return BP_ERR_IO;
    return BP_OK;
}

static int dos_archive_publish(const char *target, const char *stage,
                               int expected_exists,
                               const struct stat *expected_stat,
                               bp_uint8 attr_valid, bp_uint8 attr_value)
{
    struct stat current, backup_stat;
    char backup[BP_MAX_PATH];
    int exists, rc;

    if (attr_valid != 0 &&
        dos_apply_fat_attributes(stage, attr_valid, attr_value) != BP_OK)
        return BP_ERR_PARTIAL;
    rc = dos_archive_stat(target, &exists, &current);
    if (rc != BP_OK || exists != expected_exists ||
        (exists && !dos_archive_same_stat(&current, expected_stat)))
        return BP_ERR_IO;

    if (!exists)
        return dos_rename_no_replace(stage, target) == 0 ? BP_OK : BP_ERR_IO;

    if (extract_backup_name(target, backup, sizeof(backup)) != 0 ||
        dos_rename_no_replace(target, backup) != 0)
        return BP_ERR_IO;
    if (lstat(backup, &backup_stat) != 0 ||
        !dos_archive_same_renamed_stat(&backup_stat, expected_stat)) {
        if (dos_rename_no_replace(backup, target) != 0)
            fprintf(stderr,
                    "boltpack: original archive remains at recovery path: %s\n",
                    backup);
        return BP_ERR_IO;
    }
    if (dos_rename_no_replace(stage, target) != 0) {
        if (dos_rename_no_replace(backup, target) != 0)
            fprintf(stderr,
                    "boltpack: original archive remains at recovery path: %s\n",
                    backup);
        return BP_ERR_IO;
    }
    if (remove(backup) != 0) {
        fprintf(stderr, "boltpack: previous archive remains at recovery path: %s\n",
                backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH];
    char local_name[BP_MAX_PATH];
    dos_stage_output_t *output;
    int fd;

    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR ||
        entry->entry_type == BP_TYPE_SYMLINK)
        dos_subject_clear(ctx);

    if (entry->entry_type == BP_TYPE_META) {
        bp_metadata_sink_t *sink;
        if (entry->platform != BP_META_PORTABLE || !ctx->subject_ok ||
            ctx->subject_name == NULL || strcmp(entry->name, ctx->subject_name) != 0) {
            ctx->metadata_errors++;
            return NULL;
        }
        sink = (bp_metadata_sink_t *)malloc(sizeof(*sink));
        if (sink == NULL) {
            ctx->metadata_errors++;
            return NULL;
        }
        bp_metadata_sink_init(sink);
        return &sink->io;
    }

    if (entry->entry_type == BP_TYPE_RSRC) {
        ctx->skip_errors++;
        return NULL;
    }

    if (!bp_name_is_safe(entry->name)) {
        ctx->skip_errors++;
        return NULL;
    }

    /* Convert UTF-8 archive name to local codepage for filesystem and display */
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                         DOS_CODEPAGE) != 0) {
        ctx->skip_errors++;
        return NULL;
    }

    if (ctx->out_dir && strlen(ctx->out_dir) + strlen(local_name) + 2 >= sizeof(path)) {
        ctx->skip_errors++;
        return NULL;
    }
    if (ctx->out_dir) {
        path_join(path, sizeof(path), ctx->out_dir, local_name);
    } else {
        strcpy(path, local_name);
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", local_name);
        }
        if (!ctx->dry_run) {
            dos_dir_restore_t *directory;
            if (mkdirs(path) != 0) {
                ctx->skip_errors++;
                return NULL;
            }
            directory = dos_queue_directory(ctx, path, entry);
            (void)dos_subject_set(ctx, entry, path, directory);
        } else {
            (void)dos_subject_set(ctx, entry, path, NULL);
        }
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity == VERB_DEFAULT) {
            printf("   skipping: %s [symlink - not supported on DOS]\n",
                   local_name);
        }
        ctx->skip_errors++;
        return NULL;
    }

    if (entry->entry_type != BP_TYPE_DATA) {
        ctx->skip_errors++;
        return NULL;
    }

    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT) {
        printf(" extracting: %s\n", local_name);
    }

    if (ctx->dry_run) {
        (void)dos_subject_set(ctx, entry, path, NULL);
        return bp_archive_dry_run_stream();
    }

    /* check for existing file */
    if (!ctx->force) {
        struct stat st_buf;
        if (stat(path, &st_buf) == 0) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n", path);
            ctx->skip_errors++;
            return NULL;
        }
    }

    if (mkdirs_for_file(path) != 0) {
        fprintf(stderr, "boltpack: cannot create parent directory: %s\n", path);
        ctx->skip_errors++;
        return NULL;
    }
    fd = extract_open_stage(path, ctx->stage_path, sizeof(ctx->stage_path));
    if (fd < 0) {
        fprintf(stderr, "boltpack: cannot create staged output for: %s\n", path);
        ctx->skip_errors++;
        return NULL;
    }

    output = (dos_stage_output_t *)malloc(sizeof(*output));
    if (output == NULL) {
        struct stat opened_stage;
        int have_snapshot = fstat(fd, &opened_stage) == 0;
        close(fd);
        if (have_snapshot)
            dos_remove_stage_if_same(ctx->stage_path, &opened_stage);
        ctx->skip_errors++;
        return NULL;
    }
    output->fd = fd;
    output->io.read = NULL;
    output->io.write = extract_stage_write;
    output->io.seek = NULL;
    output->io.tell = NULL;
    output->io.handle = output;
    strcpy(ctx->target_path, path);
    return &output->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    dos_stage_output_t *output;
    int rc = BP_OK;

    if (entry->entry_type == BP_TYPE_META) {
        bp_metadata_sink_t *sink = (bp_metadata_sink_t *)out->handle;
        bp_portable_metadata_t parsed;
        int parse_rc, subject_match;
        if (status != BP_OK) {
            free(sink);
            return BP_OK;
        }
        parse_rc = bp_metadata_parse(sink->data, sink->length, &parsed);
        if (parse_rc == BP_ERR_CORRUPT) {
            free(sink);
            return parse_rc;
        }
        rc = parse_rc;
        subject_match = ctx->subject_ok && ctx->subject_name != NULL &&
                        strcmp(entry->name, ctx->subject_name) == 0 &&
                        bp_metadata_matches_subject(&parsed, ctx->subject_type);
        if (!subject_match) rc = BP_ERR_PARTIAL;
        if (subject_match && parsed.has_fat && !ctx->dry_run) {
            if (ctx->subject_type == BP_TYPE_DIR) {
                if (ctx->subject_directory == NULL) {
                    rc = BP_ERR_PARTIAL;
                } else {
                    ctx->subject_directory->fat_seen = 1;
                    ctx->subject_directory->fat_valid = parsed.fat_valid;
                    ctx->subject_directory->fat_value = parsed.fat_value;
                }
            } else if (ctx->subject_type == BP_TYPE_DATA &&
                       dos_apply_fat_attributes(ctx->subject_path,
                                                parsed.fat_valid,
                                                parsed.fat_value) != BP_OK) {
                fprintf(stderr, "boltpack: cannot restore DOS attributes: %s\n",
                        ctx->subject_path);
                rc = BP_ERR_PARTIAL;
            }
        }
        if (rc == BP_ERR_PARTIAL) ctx->metadata_errors++;
        free(sink);
        return rc;
    }

    output = (dos_stage_output_t *)out->handle;

    ctx->stage_open_stat_valid =
        fstat(output->fd, &ctx->stage_open_stat) == 0;
    if (close(output->fd) != 0) {
        if (ctx->stage_open_stat_valid)
            dos_remove_stage_if_same(ctx->stage_path, &ctx->stage_open_stat);
        free(out);
        ctx->skip_errors++;
        return BP_ERR_IO;
    }
    if (status != BP_OK) {
        if (ctx->stage_open_stat_valid)
            dos_remove_stage_if_same(ctx->stage_path, &ctx->stage_open_stat);
        free(out);
        return BP_OK;
    }

#ifdef BP_TEST_DOS_PARENT_SUBSTITUTION
    dos_test_substitute_stage_parent(ctx);
#endif
    {
        struct stat named_stage;
        if (!ctx->stage_open_stat_valid ||
            lstat(ctx->stage_path, &named_stage) != 0 ||
            !dos_stage_same_object(&ctx->stage_open_stat, &named_stage)) {
#ifdef BP_TEST_DOS_PARENT_SUBSTITUTION
            dos_test_write_marker("RACEREF.TXT",
                                  "staged path identity changed; publication refused\r\n");
#endif
            fprintf(stderr,
                    "boltpack: staged output path changed; refusing publication: %s\n",
                    ctx->target_path);
            /* Do not remove a different object now occupying the old path.
               The verified stage can remain in the moved original directory. */
            ctx->skip_errors++;
            free(out);
            return BP_ERR_IO;
        }
    }

    if ((entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
        bp_restore_mtime(ctx->stage_path, entry->mtime) != BP_OK) {
        fprintf(stderr, "boltpack: cannot restore timestamp: %s\n",
                ctx->target_path);
        ctx->mtime_errors++;
        rc = BP_ERR_PARTIAL;
    }

    {
        int publish_rc = extract_publish(ctx);
        if (publish_rc != BP_OK) {
            if (ctx->stage_open_stat_valid)
                dos_remove_stage_if_same(ctx->stage_path,
                                         &ctx->stage_open_stat);
            if (publish_rc == BP_ERR_PARTIAL) {
                ctx->skip_errors++;
                rc = BP_ERR_PARTIAL;
            }
            else {
                ctx->skip_errors++;
                free(out);
                return BP_ERR_IO;
            }
        }
    }
    if (dos_subject_set(ctx, entry, ctx->target_path, NULL) != BP_OK)
        rc = BP_ERR_PARTIAL;
    ctx->stage_path[0] = '\0';
    ctx->target_path[0] = '\0';
    free(out);
    return rc;
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
                             DOS_CODEPAGE) != 0) {
            strncpy(disp_name, entry->name, BP_MAX_PATH - 1);
            disp_name[BP_MAX_PATH - 1] = '\0';
        }
        printf("  testing: %s", disp_name);
        if (result == BP_OK) {
            printf(" OK\n");
        } else {
            printf(" FAILED -- %s\n", bp_strerror(result));
        }
    }

    if (result == BP_OK) {
        ctx->ok_count++;
    } else {
        ctx->fail_count++;
    }
}

/* ---- usage ------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver (DOS)\n"
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
        "  -0        Store only (no compression)\n"
        "  -1        Use BoltLite compression\n"
        "  -F        Use fluff LZ77+Huffman compression (better ratio than boltlz)\n"
        "  --interchange-profile-1  Mini-compatible limits for create/add\n"
        "  -q        Quiet (summary only)\n"
        "  -qq       Silent (errors only)\n"
        "  -o PATH   Output directory for extraction\n"
        "  -f        Force overwrite existing files\n"
        "  -n        Dry run\n");
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int interchange_profile)
{
    const char *archive_name;
    FILE *fp;
    bp_io_t aio;
    bp_archive_t ar;
    struct stat original_stat;
    char stage[BP_MAX_PATH];
    int original_exists;
    bp_uint8 attr_valid, attr_value;
    int rc, i;
    clock_t start;
    double elapsed;

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

    rc = dos_archive_stat(archive_name, &original_exists, &original_stat);
    if (rc != BP_OK ||
        dos_archive_capture_attributes(archive_name, original_exists,
                                       &attr_valid, &attr_value) != BP_OK) {
        fprintf(stderr, "boltpack: cannot safely replace archive: %s\n",
                archive_name);
        return 1;
    }

    fp = dos_archive_open_stage(archive_name, stage, sizeof(stage));
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot create staged archive beside: %s\n",
                archive_name);
        return 1;
    }

    bp_io_from_file(&aio, fp);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        remove(stage);
        return 1;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;

    start = clock();

    for (i = 1; i < argc; i++) {
        const char *path = argv[i];
        const char *stripped = clean_path(path);
        char arcbuf[BP_MAX_PATH];

        /* convert to forward slashes for archive storage */
        strcpy(arcbuf, stripped);
        normalize_separators(arcbuf);

        rc = add_path(&ar, path, arcbuf);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            bp_archive_close(&ar);
            fclose(fp);
            remove(stage);
            return 1;
        }
    }

    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar);
        fclose(fp);
        remove(stage);
        return 1;
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "boltpack: cannot close staged archive: %s\n", stage);
        bp_archive_close(&ar);
        remove(stage);
        return 1;
    }
    rc = dos_archive_publish(archive_name, stage, original_exists,
                             &original_stat, attr_valid, attr_value);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish archive: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        remove(stage);
        return 1;
    }

    elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;

    /* summary line */
    if (g_verbosity < VERB_SILENT) {
        bp_uint64 total_orig = {0, 0};
        bp_uint64 total_comp = {0, 0};
        char numbuf[32], speedbuf[64];
        bp_uint32 file_count = 0, dir_count = 0;
        bp_uint32 j;

        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) {
                dir_count++;
            } else {
                file_count++;
            }
            total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
            total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
        }

        format_size64(total_orig, numbuf, sizeof(numbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)dir_count, dir_count == 1 ? "y" : "ies",
               numbuf);

        if (!bp_u64_is_zero(total_orig) && !bp_u64_is_zero(total_comp)) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
            char compbuf[32];
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
    return 0;
}

static int cmd_list(const char *archive_name)
{
    FILE *fp;
    bp_io_t aio;
    int rc;

    fp = fopen(archive_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    bp_io_from_file(&aio, fp);

    printf("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");

    {
        list_ctx_t ctx;
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
                   ctx.dir_count == 1 ? "y" : "ies",
                   origbuf);
            if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
                double orig_d = (double)ctx.total_orig.lo + (double)ctx.total_orig.hi * 4294967296.0;
                double comp_d = (double)ctx.total_comp.lo + (double)ctx.total_comp.hi * 4294967296.0;
                double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
                printf(", %.0f%% (%s)", pct, compbuf);
            }
            printf("\n");
        }
    }
    return 0;
}

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
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp);
        return 1;
    }

    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
    ctx.dry_run = dry_run;

    rc = dos_preflight_archive(&aio, out_dir);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: destination-name preflight failed: %s\n",
                bp_strerror(rc));
        fclose(fp);
        return 1;
    }

    if (out_dir && !dry_run && mkdirs(out_dir) != 0) {
        fprintf(stderr, "boltpack: cannot create output directory: %s\n", out_dir);
        fclose(fp);
        return 1;
    }

    start = clock();

    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done,
                                   g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                   &ctx);
    dos_finish_directories(&ctx, rc == BP_OK || rc == BP_ERR_PARTIAL);
    dos_subject_clear(&ctx);
    fclose(fp);

    if (rc == BP_OK && (ctx.mtime_errors || ctx.skip_errors ||
                        ctx.metadata_errors))
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
               ctx.dir_count == 1 ? "y" : "ies");
        if (elapsed > 0.0) {
            printf(", %.2fs", elapsed);
        }
        printf("\n");
    }

    return 0;
}

static int cmd_test(const char *archive_name)
{
    FILE *fp;
    bp_io_t aio;
    test_ctx_t ctx;
    int rc;

    fp = fopen(archive_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp);
        return 1;
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
        if (ctx.fail_count > 0) {
            printf(", %lu file%s FAILED",
                   (unsigned long)ctx.fail_count,
                   ctx.fail_count == 1 ? "" : "s");
        }
        printf("\n");
    }

    return ctx.fail_count > 0 ? 1 : 0;
}

/* ---- info -------------------------------------------------------------- */

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
        bp_uint64 total_orig = info.total_original;
        bp_uint64 total_comp = info.total_compressed;
        bp_uint64 arc_size = info.archive_size;
        char origbuf[32], compbuf[32], arcbuf[32];

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
    const char *archive_name;
    const char **del_names;
    int del_count;
    FILE *src_fp, *dst_fp;
    bp_io_t src_io, dst_io;
    char stage[BP_MAX_PATH];
    struct stat original_stat, opened_stat;
    int original_exists;
    bp_uint8 attr_valid, attr_value;
    bp_uint32 deleted = 0;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }

    archive_name = argv[0];
    del_names = (const char **)&argv[1];
    del_count = argc - 1;

    rc = dos_archive_stat(archive_name, &original_exists, &original_stat);
    if (rc != BP_OK || !original_exists ||
        dos_archive_capture_attributes(archive_name, 1,
                                       &attr_valid, &attr_value) != BP_OK) {
        fprintf(stderr, "boltpack: cannot safely edit archive: %s\n",
                archive_name);
        return 1;
    }

    src_fp = fopen(archive_name, "rb");
    if (src_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    if (fstat(fileno(src_fp), &opened_stat) != 0 ||
        !dos_archive_same_stat(&opened_stat, &original_stat)) {
        fprintf(stderr, "boltpack: archive changed while opening: %s\n",
                archive_name);
        fclose(src_fp);
        return 1;
    }

    dst_fp = dos_archive_open_stage(archive_name, stage, sizeof(stage));
    if (dst_fp == NULL) {
        fprintf(stderr, "boltpack: cannot create staged archive beside: %s\n",
                archive_name);
        fclose(src_fp);
        return 1;
    }

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);

    rc = bp_archive_delete(&src_io, &dst_io, del_names, (bp_uint32)del_count,
                           &deleted);

    if (fclose(src_fp) != 0) rc = BP_ERR_IO;
    if (fclose(dst_fp) != 0) rc = BP_ERR_IO;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        remove(stage);
        return 1;
    }

    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        remove(stage);
        return 1;
    }

    rc = dos_archive_publish(archive_name, stage, 1, &original_stat,
                             attr_valid, attr_value);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish deleted archive: %s\n",
                bp_strerror(rc));
        remove(stage);
        return 1;
    }

    if (g_verbosity < VERB_SILENT) {
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    }
    return 0;
}

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int interchange_profile)
{
    const char *archive_name;
    FILE *source_fp, *fp;
    bp_io_t io;
    bp_archive_t ar;
    struct stat original_stat, opened_stat;
    char stage[BP_MAX_PATH];
    int original_exists;
    bp_uint8 attr_valid, attr_value;
    int rc, i;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }

    archive_name = argv[0];

    rc = dos_archive_stat(archive_name, &original_exists, &original_stat);
    if (rc != BP_OK || !original_exists ||
        dos_archive_capture_attributes(archive_name, 1,
                                       &attr_valid, &attr_value) != BP_OK) {
        fprintf(stderr, "boltpack: cannot safely edit archive: %s\n",
                archive_name);
        return 1;
    }

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    source_fp = fopen(archive_name, "rb");
    if (source_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    if (fstat(fileno(source_fp), &opened_stat) != 0 ||
        !dos_archive_same_stat(&opened_stat, &original_stat)) {
        fprintf(stderr, "boltpack: archive changed while opening: %s\n",
                archive_name);
        fclose(source_fp);
        return 1;
    }

    fp = dos_archive_open_stage(archive_name, stage, sizeof(stage));
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot create staged archive beside: %s\n",
                archive_name);
        fclose(source_fp);
        return 1;
    }
    rc = dos_archive_copy(source_fp, fp);
    if (fclose(source_fp) != 0) rc = BP_ERR_IO;
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot copy original archive to stage\n");
        fclose(fp);
        remove(stage);
        return 1;
    }

    bp_io_from_file(&io, fp);

    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        remove(stage);
        return 1;
    }
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            bp_archive_close(&ar);
            fclose(fp);
            remove(stage);
            return 1;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }

    /* add new files */
    for (i = 1; i < argc; i++) {
        const char *path = argv[i];
        const char *arcname = clean_path(path);

        rc = add_path(&ar, path, arcname);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            bp_archive_close(&ar); fclose(fp);
            remove(stage);
            return 1;
        }
    }

    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    if (fclose(fp) != 0) rc = BP_ERR_IO;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        remove(stage);
        return 1;
    }

    rc = dos_archive_publish(archive_name, stage, 1, &original_stat,
                             attr_valid, attr_value);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish appended archive: %s\n",
                bp_strerror(rc));
        remove(stage);
        return 1;
    }

    if (g_verbosity < VERB_SILENT) {
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    }
    return 0;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char command;
    bp_uint32 block_size = 0;
    bp_uint8 method = BP_METHOD_BOLTLZ;
    const char *out_dir = NULL;
    int force = 0;
    int dry_run = 0;
    int interchange_profile = 0;
    int codec_given = 0;
    int i;

    if (argc < 3) {
        usage();
        return 1;
    }

    /* first arg is the command */
    if (strlen(argv[1]) != 1) {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]);
        usage();
        return 1;
    }
    command = argv[1][0];

    /* parse options starting from argv[2] */
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
            method = BP_METHOD_STORED;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-1") == 0) {
            method = BP_METHOD_BOLTLITE;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-F") == 0) {
            method = BP_METHOD_FLUFF;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "--interchange-profile-1") == 0) {
            interchange_profile = 1;
            i++;
        } else if (strcmp(argv[i], "-qq") == 0) {
            g_verbosity = VERB_SILENT;
            i++;
        } else if (strcmp(argv[i], "-q") == 0) {
            g_verbosity = VERB_QUIET;
            i++;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: -o requires an argument\n");
                return 1;
            }
            out_dir = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0) {
            force = 1;
            i++;
        } else if (strcmp(argv[i], "-n") == 0) {
            dry_run = 1;
            i++;
        } else {
            fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]);
            usage();
            return 1;
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
        if (!codec_given) method = BP_METHOD_BOLTLITE;
    }

    if (method == BP_METHOD_FLUFF && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c':
        return cmd_create(argc - i, argv + i, block_size, method,
                          interchange_profile);
    case 'a':
        return cmd_add(argc - i, argv + i, block_size, method,
                       interchange_profile);
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
    case 'd':
        return cmd_delete(argc - i, argv + i);
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage();
        return 1;
    }
}
