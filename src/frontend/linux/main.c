/*
 * main.c -- Linux CLI frontend for BoltPack
 *
 * ANSI C (C89/C90) only (with POSIX extensions for directory recursion).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifdef __linux__
#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define BP_USE_DIRFD_EXTRACTION 1
#define BP_STAT_HAS_NSEC 1
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define BP_USE_DIRFD_EXTRACTION 1
#define BP_STAT_HAS_NSEC 1
#elif defined(BP_POSIX_USE_DIRFD_EXTRACTION)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define BP_USE_DIRFD_EXTRACTION 1
#define BP_STAT_HAS_NSEC 1
#else
#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L
#endif

#ifndef BP_CREATOR_OS
#  define BP_CREATOR_OS BP_OS_LINUX
#endif
#ifndef BP_CREATOR_CPU
#  define BP_CREATOR_CPU BP_CPU_UNKNOWN
#endif

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/bp_codepage.h"
#include "../../core/fluff.h"
#include "../posix/archive_preflight.h"
#ifdef BOLTPACK_ZSTD
#include "../../core/zstd_wrap.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <utime.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#ifdef BOLTPACK_FAST_FLUFF
#include "../../core/fluff_fast.h"
#include <omp.h>
#endif

/* ---- verbosity --------------------------------------------------------- */

#define VERB_DEFAULT 0
#define VERB_QUIET   1
#define VERB_SILENT  2

typedef enum {
    FAST_THREAD_AUTO = 0,
    FAST_THREAD_SINGLE = 1,
    FAST_THREAD_EXPLICIT = 2
} fast_thread_policy_t;

#define FAST_FLUFF_AUTO_MIN_BYTES (4UL * 1024UL * 1024UL)
#define FAST_BOLTLZ_AUTO_MIN_BYTES (1UL * 1024UL * 1024UL)
#define FAST_COMPRESSION_MAX_THREADS 64
/* Mixed-corpus tuning favored a 4 MiB large-file cutoff at -j 16, -g 4. */
#define FAST_FLUFF_LARGE_FILE_THRESHOLD_SCALE 0.25

static int g_verbosity    = VERB_DEFAULT;
static fast_thread_policy_t g_fast_thread_policy = FAST_THREAD_AUTO;

/*
 * Network send/receive (listen/scan/--send).  The shared CLI glue lives in
 * common/bp_net_cli.h; this frontend only provides bp_frontend_extract_io()
 * (below) plus the integration hooks in parse_command_name/cmd_create/main.
 */
#ifdef BOLTPACK_NET
#include "../common/bp_net_cli.h"
#endif

/*
 * Path of the in-progress temp archive written by cmd_create.
 * Cleared on success (after rename).  The signal handler unlinks it so
 * Ctrl+C does not leave a partial .blt file on disk.
 */
static char g_tmp_archive[4096 + 64] = "";

static void cleanup_tmp_archive(void)
{
    if (g_tmp_archive[0] != '\0') {
        unlink(g_tmp_archive);
        g_tmp_archive[0] = '\0';
    }
}

static void sig_cleanup_handler(int sig)
{
    cleanup_tmp_archive();
    signal(sig, SIG_DFL);
    raise(sig);
}

static int g_fast_threads = 0; /* explicit thread count for -j/--threads */
static int g_fast_group_blocks = 0; /* 0 = codec default; >0 = grouped reset span */
static char g_tmp_dir[4096] = ""; /* directory for fast-fluff temp files */

/* ---- bp_io_t adapters for FILE * --------------------------------------- */

static long file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

typedef struct {
    FILE *fp;
    bp_uint32 checksum;
} source_checksum_reader_t;

static long source_checksum_read(void *handle, void *buf, bp_uint32 count)
{
    source_checksum_reader_t *reader = (source_checksum_reader_t *)handle;
    size_t n = fread(buf, 1, (size_t)count, reader->fp);
    if (n != 0)
        reader->checksum = bp_crc32(reader->checksum, (const bp_uint8 *)buf,
                                    (bp_uint32)n);
    return (long)n;
}

static int source_checksum_seek(void *handle, bp_off_t offset, int origin)
{
    source_checksum_reader_t *reader = (source_checksum_reader_t *)handle;
    return fseeko(reader->fp, (off_t)offset, origin);
}

static bp_off_t source_checksum_tell(void *handle)
{
    source_checksum_reader_t *reader = (source_checksum_reader_t *)handle;
    return (bp_off_t)ftello(reader->fp);
}

static int source_file_checksum(FILE *fp, bp_uint32 *checksum)
{
    bp_uint8 buffer[16384];
    bp_uint32 value = 0;
    size_t n;

    if (fp == NULL || checksum == NULL || fseeko(fp, (off_t)0, SEEK_SET) != 0)
        return -1;
    clearerr(fp);
    while ((n = fread(buffer, 1, sizeof(buffer), fp)) != 0)
        value = bp_crc32(value, buffer, (bp_uint32)n);
    if (ferror(fp)) return -1;
    *checksum = value;
    return 0;
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
    io->read  = file_read;
    io->write = file_write;
    io->seek  = file_seek;
    io->tell  = file_tell;
    io->handle = fp;
}

/*
 * Set g_tmp_dir to the directory containing archive_name.
 * Fast-fluff temp files will be placed there instead of /tmp,
 * avoiding ENOSPC when /tmp is a small tmpfs.
 */
static void set_tmp_dir_from_archive(const char *archive_name)
{
    const char *slash;
    size_t dlen;

    g_tmp_dir[0] = '\0';
    if (archive_name == NULL) return;

    slash = strrchr(archive_name, '/');
    if (slash != NULL) {
        dlen = slash == archive_name ? 1U : (size_t)(slash - archive_name);
        if (dlen < sizeof(g_tmp_dir)) {
            memcpy(g_tmp_dir, archive_name, dlen);
            g_tmp_dir[dlen] = '\0';
        }
    } else {
        g_tmp_dir[0] = '.';
        g_tmp_dir[1] = '\0';
    }
}

#ifdef BOLTPACK_FAST_FLUFF
/* Forward declaration — defined near add_job_list_parallel below. */
static int tmp_fits_in_ram(double hint_bytes);
#else
static int tmp_fits_in_ram(double hint_bytes)
{
    (void)hint_bytes;
    return 1; /* no fast-fluff staging; always use tmpfile() */
}
#endif

/*
 * Open a temporary file for fast-fluff staging.
 *
 * hint_bytes: upper bound on the expected temp file size (pass 0 if unknown).
 *
 * Strategy:
 *   1. Prefer /tmp (typically a RAM-backed tmpfs on Linux) — fast, avoids
 *      extra HDD I/O on the output volume.
 *   2. If hint_bytes would not comfortably fit in /tmp (leaving <256 MB
 *      headroom), route to the archive directory instead so we never exhaust
 *      a small tmpfs with large model files.
 *   3. If the archive directory also fails, fall back to tmpfile().
 */
/*
 * Open a temp file at a pre-decided location.
 * use_sys_tmp != 0 → /tmp (tmpfile); 0 → archive dir (mkstemp + unlink).
 * Does NOT call statvfs; the caller must have already decided.
 * This is safe to call from inside an OMP parallel region.
 */
static FILE *open_tmp_file_at(int use_sys_tmp)
{
    if (use_sys_tmp) {
        FILE *fp = tmpfile();
        if (fp != NULL) return fp;
        /* tmpfile() failed; fall through to archive directory. */
    }

    /* Use archive directory (same filesystem as output, avoids tmpfs ENOSPC). */
    if (g_tmp_dir[0] != '\0') {
        char template[4096 + 32];
        int n;
        int fd;
        FILE *fp;

        n = snprintf(template, sizeof(template), "%s/bptmp_XXXXXX", g_tmp_dir);
        if (n > 0 && n < (int)sizeof(template)) {
            fd = mkstemp(template);
            if (fd >= 0) {
                unlink(template);
                fp = fdopen(fd, "w+b");
                if (fp != NULL)
                    return fp;
                close(fd);
            }
        }
    }

    return tmpfile();
}

/*
 * Single-file wrapper: decide via statvfs then open.
 * Not safe to call from an OMP parallel region — use open_tmp_file_at()
 * with a pre-computed flag instead.
 */
static FILE *open_tmp_file(double hint_bytes)
{
    return open_tmp_file_at(tmp_fits_in_ram(hint_bytes));
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
    case BP_ERR_PARTIAL:           return "some entries or attributes were not restored";
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

static double wall_time_seconds(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        return 0.0;
    }
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

    static double u64_to_double(bp_uint64 v)
    {
        return (double)v.lo + (double)v.hi * 4294967296.0;
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

static void format_number(unsigned long n, char *buf, size_t bufsize)
{
    char tmp[32];
    int len, i, j, commas;

    sprintf(tmp, "%lu", n);
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

static void format_u64_count(bp_uint64 v, char *buf, size_t bufsize)
{
    double value;
    (void)bufsize;
    value = (double)v.lo + (double)v.hi * 4294967296.0;
    if (value < 1000.0) {
        sprintf(buf, "%.0f", value);
    } else if (value < 1000000.0) {
        sprintf(buf, "%.1fK", value / 1000.0);
    } else if (value < 1000000000.0) {
        sprintf(buf, "%.1fM", value / 1000000.0);
    } else {
        sprintf(buf, "%.2fG", value / 1000000000.0);
    }
}

/* ---- progress callback for create/extract ------------------------------ */

typedef struct bp_restore_dir_s {
    struct bp_restore_dir_s *next;
    char *path;
    bp_uint64 mtime;
    bp_uint16 flags;
} bp_restore_dir_t;

typedef struct {
    bp_io_t io; /* first member: callbacks return &io */
    char *temporary;
    char *destination;
#ifdef BP_USE_DIRFD_EXTRACTION
    int parent_fd;
#endif
} bp_extract_output_t;

typedef struct {
    bp_uint32 file_count_final;
    bp_uint32 dir_count;
    int       is_create;
} progress_ctx_t;

static int progress_cb(const bp_progress_t *p, void *user_data)
{
    (void)p;
    (void)user_data;
    /* quiet mode: could show percentage here */
    return 0;
}

/* ---- path utilities ---------------------------------------------------- */

/* return 1 if path ends with '/' */
static int ends_with_slash(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && s[len - 1] == '/';
}

/* concatenate dir + "/" + name into buf (must be large enough) */
static void path_join(char *buf, size_t bufsize, const char *dir,
                      const char *name)
{
    if (ends_with_slash(dir)) {
        snprintf(buf, bufsize, "%s%s", dir, name);
    } else {
        snprintf(buf, bufsize, "%s/%s", dir, name);
    }
}

/* strip leading "./" */
static const char *clean_path(const char *path)
{
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    return path;
}

static char *dup_string(const char *s)
{
    char *copy;
    size_t len;

    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

/* ---- mkdir -p ---------------------------------------------------------- */

static int mkdirs(const char *path)
{
    char *tmp;
    char *p;

    tmp = (char *)malloc(strlen(path) + 1);
    if (tmp == NULL) return -1;
    strcpy(tmp, path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }
    /* create the final component */
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

/* ensure parent directory of path exists */
static int mkdirs_for_file(const char *path)
{
    char *tmp;
    char *slash;

    tmp = (char *)malloc(strlen(path) + 1);
    if (tmp == NULL) return -1;
    strcpy(tmp, path);

    slash = strrchr(tmp, '/');
    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        if (mkdirs(tmp) != 0) {
            free(tmp);
            return -1;
        }
    }
    free(tmp);
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

static char parse_command_name(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return '\0';
    if (s[1] == '\0')
        return s[0];
    if (strcmp(s, "create") == 0)
        return 'c';
    if (strcmp(s, "add") == 0)
        return 'a';
    if (strcmp(s, "extract") == 0)
        return 'x';
    if (strcmp(s, "list") == 0)
        return 'l';
    if (strcmp(s, "test") == 0)
        return 't';
    if (strcmp(s, "info") == 0)
        return 'i';
    if (strcmp(s, "delete") == 0)
        return 'd';
    if (strcmp(s, "debug-fluff") == 0)
        return 'T';
#ifdef BOLTPACK_NET
    {
        char nc = bp_net_cli_command(s);
        if (nc) return nc;
    }
#endif
    return '\0';
}

static int parse_thread_count_arg(const char *s)
{
    int n;

    n = atoi(s);
    if (n < 0) {
        fprintf(stderr, "boltpack: thread count must be >= 0\n");
        return 0;
    }
    if (n <= 1) {
        g_fast_thread_policy = FAST_THREAD_SINGLE;
        g_fast_threads = 0;
    } else {
        g_fast_thread_policy = FAST_THREAD_EXPLICIT;
        g_fast_threads = n;
    }
    return 1;
}

/* ---- recursive add ----------------------------------------------------- */

typedef struct {
    char      *path;
    char      *arcname;
    char      *symlink_target;
    bp_uint8   entry_type;
    bp_uint64  file_size;
    bp_uint64  mtime;
    bp_uint16  flags; /* BP_ENTRY_FLAG_MODE_VALID | (mode & BP_ENTRY_FLAG_MODE_MASK) */
    dev_t      source_dev;
    ino_t      source_ino;
    off_t      source_size;
    mode_t     source_mode;
    time_t     source_mtime;
    time_t     source_ctime;
    long       source_mtime_nsec;
    long       source_ctime_nsec;
    bp_uint32  packed_checksum;
    int        packed_checksum_valid;
} add_job_t;

typedef struct {
    add_job_t *items;
    int        count;
    int        cap;
} add_job_list_t;

#ifdef BOLTPACK_FAST_FLUFF
typedef struct {
    FILE              *tmp_fp;
    bp_io_t            tmp_io;
    bp_packed_entry_t  packed;
    int                rc;
} packed_job_t;
#endif

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);
static int collect_path_jobs(add_job_list_t *list, const char *path,
                             const char *arcname);
#ifdef BOLTPACK_FAST_FLUFF
static int add_job_list_parallel(bp_archive_t *ar, add_job_list_t *list);
#endif

static void free_add_job(add_job_t *job)
{
    if (job == NULL) return;
    free(job->path);
    free(job->arcname);
    free(job->symlink_target);
    memset(job, 0, sizeof(*job));
}

static void free_job_list(add_job_list_t *list)
{
    int i;

    if (list == NULL) return;
    for (i = 0; i < list->count; i++) {
        free_add_job(&list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int append_job(add_job_list_t *list, const char *path,
                      const char *arcname, bp_uint8 entry_type,
                      bp_uint64 file_size, bp_uint64 mtime, bp_uint16 flags,
                      const struct stat *source_stat,
                      const char *symlink_target)
{
    add_job_t *new_items;
    char *path_copy;
    char *arc_copy;
    char *target_copy = NULL;
    int new_cap;

    if (list->count == list->cap) {
        new_cap = list->cap ? list->cap * 2 : 64;
        new_items = (add_job_t *)realloc(list->items,
                                         (size_t)new_cap * sizeof(add_job_t));
        if (new_items == NULL) {
            return BP_ERR_NOMEM;
        }
        list->items = new_items;
        list->cap = new_cap;
    }

    path_copy = dup_string(path);
    arc_copy = dup_string(arcname);
    if (symlink_target != NULL) target_copy = dup_string(symlink_target);
    if (path_copy == NULL || arc_copy == NULL ||
        (symlink_target != NULL && target_copy == NULL)) {
        free(path_copy);
        free(arc_copy);
        free(target_copy);
        return BP_ERR_NOMEM;
    }

    list->items[list->count].path = path_copy;
    list->items[list->count].arcname = arc_copy;
    list->items[list->count].symlink_target = target_copy;
    list->items[list->count].entry_type = entry_type;
    list->items[list->count].file_size = file_size;
    list->items[list->count].mtime = mtime;
    list->items[list->count].flags = flags;
    list->items[list->count].source_dev = source_stat->st_dev;
    list->items[list->count].source_ino = source_stat->st_ino;
    list->items[list->count].source_size = source_stat->st_size;
    list->items[list->count].source_mode = source_stat->st_mode;
    list->items[list->count].source_mtime = source_stat->st_mtime;
    list->items[list->count].source_ctime = source_stat->st_ctime;
    list->items[list->count].packed_checksum = 0;
    list->items[list->count].packed_checksum_valid = 0;
#ifdef BP_STAT_HAS_NSEC
    list->items[list->count].source_mtime_nsec = source_stat->st_mtim.tv_nsec;
    list->items[list->count].source_ctime_nsec = source_stat->st_ctim.tv_nsec;
#else
    list->items[list->count].source_mtime_nsec = 0;
    list->items[list->count].source_ctime_nsec = 0;
#endif
    list->count++;
    return BP_OK;
}

static int source_stat_matches(const add_job_t *job, const struct stat *st)
{
    if (job == NULL || st == NULL || st->st_dev != job->source_dev ||
        st->st_ino != job->source_ino || st->st_size != job->source_size ||
        st->st_mode != job->source_mode ||
        st->st_mtime != job->source_mtime ||
        st->st_ctime != job->source_ctime)
        return 0;
#ifdef BP_STAT_HAS_NSEC
    if (st->st_mtim.tv_nsec != job->source_mtime_nsec ||
        st->st_ctim.tv_nsec != job->source_ctime_nsec)
        return 0;
#endif
    return 1;
}

static int source_stat_snapshot_matches(const struct stat *a,
                                        const struct stat *b)
{
    if (a == NULL || b == NULL || a->st_dev != b->st_dev ||
        a->st_ino != b->st_ino || a->st_size != b->st_size ||
        a->st_mode != b->st_mode ||
        a->st_mtime != b->st_mtime ||
        a->st_ctime != b->st_ctime)
        return 0;
#ifdef BP_STAT_HAS_NSEC
    if (a->st_mtim.tv_nsec != b->st_mtim.tv_nsec ||
        a->st_ctim.tv_nsec != b->st_ctim.tv_nsec)
        return 0;
#endif
    return 1;
}

static int source_readlink_snapshot(int parent_fd, const char *leaf,
                                    const char *path,
                                    const struct stat *expected,
                                    char *target, size_t target_size,
                                    ssize_t *target_len)
{
    struct stat current;
    ssize_t n;
#ifdef __linux__
    int link_fd;
    int link_flags;
    if (parent_fd >= 0) {
        link_flags = O_PATH | O_NOFOLLOW | O_CLOEXEC;
        link_fd = openat(parent_fd, leaf, link_flags);
        if (link_fd < 0) return BP_ERR_IO;
        if (fstat(link_fd, &current) != 0 || !S_ISLNK(current.st_mode) ||
            !source_stat_snapshot_matches(expected, &current)) {
            close(link_fd);
            return BP_ERR_IO;
        }
        n = readlinkat(link_fd, "", target, target_size);
        if (n < 0 || (size_t)n >= target_size ||
            fstat(link_fd, &current) != 0 ||
            !source_stat_snapshot_matches(expected, &current)) {
            close(link_fd);
            return BP_ERR_IO;
        }
        close(link_fd);
        *target_len = n;
        return BP_OK;
    }
#endif
    if (parent_fd >= 0) {
        n = readlinkat(parent_fd, leaf, target, target_size);
        if (n < 0 || (size_t)n >= target_size ||
            fstatat(parent_fd, leaf, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISLNK(current.st_mode) ||
            !source_stat_snapshot_matches(expected, &current))
            return BP_ERR_IO;
    } else {
        n = readlink(path, target, target_size);
        if (n < 0 || (size_t)n >= target_size || lstat(path, &current) != 0 ||
            !S_ISLNK(current.st_mode) ||
            !source_stat_snapshot_matches(expected, &current))
            return BP_ERR_IO;
    }
    *target_len = n;
    return BP_OK;
}

/* Files at least this large get inline progress during the serial path. */
#define PROGRESS_THRESHOLD_BYTES (64UL * 1024UL * 1024UL)

static int serial_progress_cb(const bp_progress_t *p, void *user_data)
{
    double done;
    double total;
    double pct;
    char done_buf[32];

    (void)user_data;

    if (g_verbosity != VERB_DEFAULT) return 0;

    total = (double)p->file_orig_size.lo +
            (double)p->file_orig_size.hi * 4294967296.0;
    done  = (double)p->file_bytes_done.lo +
            (double)p->file_bytes_done.hi * 4294967296.0;

    if (total <= 0.0) return 0;

    pct = done / total * 100.0;
    format_size64(p->file_bytes_done, done_buf, sizeof(done_buf));

    /* \r overwrites the line; the final result line will use \n. */
    fprintf(stdout, "\rcompressing: %s  %s / %.0f%%   ",
            p->file_name, done_buf, pct);
    fflush(stdout);

    return 0;
}

static void print_added_data_result(bp_archive_t *ar, const char *arcname,
                                    bp_uint64 file_size)
{
    bp_entry_t *e;
    bp_uint64 comp;

    if (g_verbosity != VERB_DEFAULT || ar == NULL || ar->entries_written == 0) {
        return;
    }

    printf("compressing: %s", arcname);
    e = &ar->entry_list[ar->entries_written - 1];
    comp = e->compressed_size;
    if (bp_u64_is_zero(comp) || e->method == BP_METHOD_STORED
        || bp_u64_gte(comp, file_size)) {
        char sizebuf[32];
        format_size64(file_size, sizebuf, sizeof(sizebuf));
        printf("  %s (stored)\n", sizebuf);
    } else {
        char sizebuf[32];
        double orig;
        double cmp;
        double pct;

        orig = (double)file_size.lo + (double)file_size.hi * 4294967296.0;
        cmp = (double)comp.lo + (double)comp.hi * 4294967296.0;
        pct = (orig > 0.0) ? (cmp / orig * 100.0) : 0.0;
        format_size64(comp, sizebuf, sizeof(sizebuf));
        printf("  %s (%.0f%%)\n", sizebuf, pct);
    }
}

static int add_job(bp_archive_t *ar, add_job_t *job)
{
    int rc;
    struct stat st;

    if (job == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    if (job->entry_type == BP_TYPE_DIR) {
        if (lstat(job->path, &st) != 0 || !S_ISDIR(st.st_mode) ||
            !source_stat_matches(job, &st)) {
            fprintf(stderr,
                    "boltpack: source changed during archive creation: %s\n",
                    job->path);
            return BP_ERR_IO;
        }
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", job->arcname);
        }
        return bp_archive_add_entry_ex(ar, job->arcname, BP_TYPE_DIR,
                                      BP_CREATOR_OS, NULL,
                                      bp_u64_from_u32(0), job->mtime,
                                      job->flags);
    }

    if (job->entry_type == BP_TYPE_DATA) {
        FILE *fp;
        bp_io_t fio;
        source_checksum_reader_t source_reader;
        double file_d;
        int use_progress;
        bp_progress_fn saved_fn;
        void *saved_data;
        struct stat st;
        bp_uint64 actual_size;

        fp = fopen(job->path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", job->path);
            return BP_ERR_IO;
        }

        /* Compare the opened source with its enumeration snapshot. */
        actual_size = job->file_size;
        if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode) ||
            !source_stat_matches(job, &st)) {
            fprintf(stderr,
                    "boltpack: source changed during archive creation: %s\n",
                    job->path);
            fclose(fp);
            return BP_ERR_IO;
        }

        source_reader.fp = fp;
        source_reader.checksum = 0;
        fio.read = source_checksum_read;
        fio.write = file_write;
        fio.seek = source_checksum_seek;
        fio.tell = source_checksum_tell;
        fio.handle = &source_reader;

        /* Install per-block progress for large files so the terminal is not
         * silent for minutes at a time.  Save and restore so nested callers
         * (e.g. add_job_list_serial) are not affected. */
        file_d = (double)actual_size.lo +
                 (double)actual_size.hi * 4294967296.0;
        use_progress = (g_verbosity == VERB_DEFAULT &&
                        file_d >= (double)PROGRESS_THRESHOLD_BYTES);
        saved_fn   = ar->progress_fn;
        saved_data = ar->progress_data;
        if (use_progress) {
            ar->progress_fn   = serial_progress_cb;
            ar->progress_data = NULL;
        }

        rc = bp_archive_add_entry_ex(ar, job->arcname, BP_TYPE_DATA,
                                   BP_CREATOR_OS, &fio,
                                   actual_size, job->mtime, job->flags);
        if (rc == BP_OK &&
            (fstat(fileno(fp), &st) != 0 || !source_stat_matches(job, &st))) {
            fprintf(stderr,
                    "boltpack: source changed while archiving: %s\n",
                    job->path);
            rc = BP_ERR_IO;
        }
        fclose(fp);

        ar->progress_fn   = saved_fn;
        ar->progress_data = saved_data;

        if (use_progress) {
            /* Clear the progress line so the result line starts clean. */
            fprintf(stdout, "\r%*s\r", 78, "");
        }

        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n",
                    job->path, bp_strerror(rc));
            return rc;
        }

        job->packed_checksum = source_reader.checksum;
        job->packed_checksum_valid = 1;
        print_added_data_result(ar, job->arcname, actual_size);
        return BP_OK;
    }

    if (job->entry_type == BP_TYPE_SYMLINK) {
        const char *target = job->symlink_target;
        size_t target_len;
        bp_io_t dummy_io;
        FILE *tmp;

        if (target == NULL) return BP_ERR_INVALID_ARG;
        target_len = strlen(target);

        tmp = tmpfile();
        if (tmp == NULL) return BP_ERR_IO;
        if (fwrite(target, 1, target_len, tmp) != target_len) {
            fclose(tmp);
            return BP_ERR_IO;
        }
        rewind(tmp);
        bp_io_from_file(&dummy_io, tmp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("    linking: %s -> %s\n", job->arcname, target);
        }

        rc = bp_archive_add_entry_ex(ar, job->arcname, BP_TYPE_SYMLINK,
                                   BP_CREATOR_OS, &dummy_io,
                                   bp_u64_from_u32((bp_uint32)target_len),
                                   job->mtime, job->flags);
        fclose(tmp);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n",
                    job->path, bp_strerror(rc));
        }
        return rc;
    }

    return BP_OK;
}

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

static bp_uint64 archive_mtime(time_t value, bp_uint16 *flags)
{
    if (value < (time_t)0) {
        fprintf(stderr, "boltpack: warning: pre-1970 modification time cannot be stored\n");
        return bp_u64_from_u32(0);
    }
    *flags |= BP_ENTRY_FLAG_MTIME_VALID;
    return bp_off_to_u64((bp_off_t)value);
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct stat st;
    add_job_t job;
    char dir_arc[4096];
    int rc;

    if (lstat(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    memset(&job, 0, sizeof(job));
    job.path = (char *)path;
    job.flags = BP_ENTRY_FLAG_MODE_VALID | (bp_uint16)(st.st_mode & BP_ENTRY_FLAG_MODE_MASK);
    job.mtime = archive_mtime(st.st_mtime, &job.flags);
    job.source_dev = st.st_dev;
    job.source_ino = st.st_ino;
    job.source_size = st.st_size;
    job.source_mode = st.st_mode;
    job.source_mtime = st.st_mtime;
    job.source_ctime = st.st_ctime;
#ifdef BP_STAT_HAS_NSEC
    job.source_mtime_nsec = st.st_mtim.tv_nsec;
    job.source_ctime_nsec = st.st_ctim.tv_nsec;
#else
    job.source_mtime_nsec = 0;
    job.source_ctime_nsec = 0;
#endif

    if (S_ISDIR(st.st_mode)) {
        if (!ends_with_slash(arcname)) {
            snprintf(dir_arc, sizeof(dir_arc), "%s/", arcname);
        } else {
            snprintf(dir_arc, sizeof(dir_arc), "%s", arcname);
        }

        job.arcname = dir_arc;
        job.entry_type = BP_TYPE_DIR;
        rc = add_job(ar, &job);
        if (rc != BP_OK) {
            return rc;
        }
        return add_directory(ar, path, dir_arc);
    }

    job.arcname = (char *)arcname;
    if (S_ISREG(st.st_mode)) {
        job.entry_type = BP_TYPE_DATA;
        job.file_size = bp_off_to_u64((bp_off_t)st.st_size);
        return add_job(ar, &job);
    }

    if (S_ISLNK(st.st_mode)) {
        job.entry_type = BP_TYPE_SYMLINK;
        return add_job(ar, &job);
    }

    return BP_OK;
}

static int collect_path_jobs_from_stat(add_job_list_t *list, const char *path,
                                       const char *arcname,
                                       const struct stat *st,
                                       int parent_fd, const char *leaf);

static int collect_directory_jobs_path(add_job_list_t *list,
                                       const char *dir_path,
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
        path_join(child_arc, sizeof(child_arc), arc_prefix, ent->d_name);

        rc = collect_path_jobs(list, child_path, child_arc);
        if (rc != BP_OK) {
            closedir(d);
            return rc;
        }
    }

    closedir(d);
    return BP_OK;
}

static int collect_directory_jobs_fd(add_job_list_t *list, int dir_fd,
                                     const char *dir_path,
                                     const char *arc_prefix)
{
#ifdef BP_USE_DIRFD_EXTRACTION
    DIR *d;
    struct dirent *ent;
    int scan_fd, rc;
    char child_path[4096];
    char child_arc[4096];

    scan_fd = dup(dir_fd);
    if (scan_fd < 0) return BP_ERR_IO;
    d = fdopendir(scan_fd);
    if (d == NULL) { close(scan_fd); return BP_ERR_IO; }
    rc = BP_OK;
    while ((ent = readdir(d)) != NULL) {
        struct stat st;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (fstatat(dir_fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            rc = BP_ERR_IO;
            break;
        }
        if (strlen(dir_path) + strlen(ent->d_name) +
                (ends_with_slash(dir_path) ? 1U : 2U) > sizeof(child_path) ||
            strlen(arc_prefix) + strlen(ent->d_name) +
                (ends_with_slash(arc_prefix) ? 1U : 2U) > sizeof(child_arc)) {
            rc = BP_ERR_NAME_TOO_LONG;
            break;
        }
        path_join(child_path, sizeof(child_path), dir_path, ent->d_name);
        path_join(child_arc, sizeof(child_arc), arc_prefix, ent->d_name);
        rc = collect_path_jobs_from_stat(list, child_path, child_arc, &st,
                                         dir_fd, ent->d_name);
        if (rc != BP_OK) break;
    }
    if (closedir(d) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc;
#else
    (void)dir_fd;
    return collect_directory_jobs_path(list, dir_path, arc_prefix);
#endif
}

static int collect_path_jobs_from_stat(add_job_list_t *list, const char *path,
                                       const char *arcname,
                                       const struct stat *st,
                                       int parent_fd, const char *leaf)
{
    char dir_arc[4096];
    bp_uint16 mode_flags;
    bp_uint64 mtime;
    int rc;

    mode_flags = BP_ENTRY_FLAG_MODE_VALID
               | (bp_uint16)(st->st_mode & BP_ENTRY_FLAG_MODE_MASK);
    mtime = archive_mtime(st->st_mtime, &mode_flags);
    if (S_ISDIR(st->st_mode)) {
        int child_fd;
        struct stat opened;
        int open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW
        open_flags |= O_NOFOLLOW;
#endif
        if (!ends_with_slash(arcname)) {
            snprintf(dir_arc, sizeof(dir_arc), "%s/", arcname);
        } else {
            snprintf(dir_arc, sizeof(dir_arc), "%s", arcname);
        }
        rc = append_job(list, path, dir_arc, BP_TYPE_DIR,
                        bp_u64_from_u32(0),
                        mtime, mode_flags, st, NULL);
        if (rc != BP_OK) return rc;
        if (parent_fd >= 0)
            child_fd = openat(parent_fd, leaf, open_flags);
        else
            child_fd = open(path, open_flags);
        if (child_fd < 0 || fstat(child_fd, &opened) != 0 ||
            !S_ISDIR(opened.st_mode) || opened.st_dev != st->st_dev ||
            opened.st_ino != st->st_ino) {
            if (child_fd >= 0) close(child_fd);
            fprintf(stderr,
                    "boltpack: source directory changed during enumeration: %s\n",
                    path);
            return BP_ERR_IO;
        }
        rc = collect_directory_jobs_fd(list, child_fd, path, dir_arc);
        close(child_fd);
        return rc;
    }

    if (S_ISREG(st->st_mode)) {
        return append_job(list, path, arcname, BP_TYPE_DATA,
                          bp_off_to_u64((bp_off_t)st->st_size),
                          mtime, mode_flags, st, NULL);
    }

    if (S_ISLNK(st->st_mode)) {
        char target[4096];
        ssize_t target_len;
        int target_rc;
        target_rc = source_readlink_snapshot(parent_fd, leaf, path, st,
                                             target, sizeof(target),
                                             &target_len);
        if (target_rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: source symlink changed during enumeration: %s\n",
                    path);
            return target_rc;
        }
        target[target_len] = '\0';
        return append_job(list, path, arcname, BP_TYPE_SYMLINK,
                          bp_u64_from_u32(0), mtime, mode_flags, st, target);
    }

    return BP_OK;
}

static int collect_path_jobs(add_job_list_t *list, const char *path,
                             const char *arcname)
{
    struct stat st;
#ifdef BP_USE_DIRFD_EXTRACTION
    char parent[4096];
    char leaf[4096];
    size_t path_len;
    size_t leaf_start;
    size_t slash_pos;
    int parent_fd;
    int rc;
    int open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;

    path_len = strlen(path);
    while (path_len > 1 && path[path_len - 1] == '/') path_len--;
    slash_pos = path_len;
    while (slash_pos > 0 && path[slash_pos - 1] != '/') slash_pos--;
    leaf_start = slash_pos;
    if ((path_len == 1 && path[0] == '/') ||
        path_len - leaf_start == 0) {
        /* The root path itself is represented by "." relative to "/". */
        strcpy(parent, "/");
        strcpy(leaf, ".");
    } else {
        if (path_len - leaf_start >= sizeof(leaf)) return BP_ERR_NAME_TOO_LONG;
        memcpy(leaf, path + leaf_start, path_len - leaf_start);
        leaf[path_len - leaf_start] = '\0';
        if (slash_pos == 0) {
            strcpy(parent, ".");
        } else if (slash_pos == 1) {
            strcpy(parent, "/");
        } else {
            if (slash_pos - 1 >= sizeof(parent)) return BP_ERR_NAME_TOO_LONG;
            memcpy(parent, path, slash_pos - 1);
            parent[slash_pos - 1] = '\0';
        }
    }
    parent_fd = open(parent, open_flags);
    if (parent_fd < 0 ||
        fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (parent_fd >= 0) close(parent_fd);
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }
    rc = collect_path_jobs_from_stat(list, path, arcname, &st,
                                     parent_fd, leaf);
    close(parent_fd);
    return rc;
#else
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }
    return collect_path_jobs_from_stat(list, path, arcname, &st, -1, NULL);
#endif
}

static int collect_arg_jobs(add_job_list_t *list, int argc, char **argv)
{
    int i;
    int rc;

    for (i = 0; i < argc; i++) {
        const char *path;
        const char *arcname;

        path = argv[i];
        arcname = clean_path(path);
        rc = collect_path_jobs(list, path, arcname);
        if (rc != BP_OK) {
            return rc;
        }
    }

    return BP_OK;
}

static int add_job_list_serial(bp_archive_t *ar, add_job_list_t *list)
{
    int i;
    int rc;

    for (i = 0; i < list->count; i++) {
        rc = add_job(ar, &list->items[i]);
        if (rc != BP_OK) {
            return rc;
        }
    }

    return BP_OK;
}

static int validate_source_jobs(const add_job_list_t *list)
{
    int i;
    for (i = 0; i < list->count; i++) {
        const add_job_t *job = &list->items[i];
        struct stat st;
        int type_matches;
        if (lstat(job->path, &st) != 0 || !source_stat_matches(job, &st))
            type_matches = 0;
        else if (job->entry_type == BP_TYPE_DIR)
            type_matches = S_ISDIR(st.st_mode);
        else if (job->entry_type == BP_TYPE_DATA)
            type_matches = S_ISREG(st.st_mode);
        else if (job->entry_type == BP_TYPE_SYMLINK)
            type_matches = S_ISLNK(st.st_mode);
        else
            type_matches = 0;
        if (!type_matches) {
            fprintf(stderr,
                    "boltpack: source changed during archive creation: %s\n",
                    job->path);
            return BP_ERR_IO;
        }
        if (job->entry_type == BP_TYPE_DATA && job->packed_checksum_valid) {
            FILE *fp = fopen(job->path, "rb");
            bp_uint32 checksum;
            int valid = fp != NULL;
            if (valid &&
                (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode) ||
                 !source_stat_matches(job, &st) ||
                 source_file_checksum(fp, &checksum) != 0 ||
                 checksum != job->packed_checksum ||
                 fstat(fileno(fp), &st) != 0 || !source_stat_matches(job, &st)))
                valid = 0;
            if (fp != NULL && fclose(fp) != 0) valid = 0;
            if (!valid || lstat(job->path, &st) != 0 ||
                !source_stat_matches(job, &st)) {
                fprintf(stderr,
                        "boltpack: source changed during archive creation: %s\n",
                        job->path);
                return BP_ERR_IO;
            }
        }
    }
    return BP_OK;
}

static void summarize_data_jobs(const add_job_list_t *list,
                                bp_uint64 *total_size,
                                int *data_count)
{
    int i;

    *total_size = bp_u64_from_u32(0);
    *data_count = 0;
    for (i = 0; i < list->count; i++) {
        if (list->items[i].entry_type != BP_TYPE_DATA)
            continue;
        *total_size = bp_u64_add(*total_size, list->items[i].file_size);
        (*data_count)++;
    }
}

static int choose_parallel_compression_threads(const add_job_list_t *list,
                                              bp_uint8 method)
{
#ifdef BOLTPACK_FAST_FLUFF
    bp_uint64 total_size;
    bp_uint64 min_size;
    int data_count;
    int threads;

    if (method != BP_METHOD_BOLTLZ && method != BP_METHOD_FLUFF
#ifdef BOLTPACK_ZSTD
        && method != BP_METHOD_ZSTD
#endif
        )
        return 0;
    if (g_fast_thread_policy == FAST_THREAD_SINGLE)
        return 0;

    threads = (g_fast_thread_policy == FAST_THREAD_EXPLICIT)
        ? g_fast_threads : fluff_fast_get_threads();
    if (threads <= 1)
        return 0;
    if (threads > FAST_COMPRESSION_MAX_THREADS)
        threads = FAST_COMPRESSION_MAX_THREADS;

    summarize_data_jobs(list, &total_size, &data_count);
    if (data_count <= 0)
        return 0;

    if (g_fast_thread_policy == FAST_THREAD_EXPLICIT) {
        if (g_fast_threads > 0 && g_fast_threads < threads)
            threads = g_fast_threads;
    } else {
        min_size = bp_u64_from_ulong(method == BP_METHOD_BOLTLZ
            ? FAST_BOLTLZ_AUTO_MIN_BYTES : FAST_FLUFF_AUTO_MIN_BYTES);
        if (!bp_u64_gte(total_size, min_size))
            return 0;
    }

    return threads > 1 ? threads : 0;
#else
    (void)list;
    (void)method;
    return 0;
#endif
}

static int add_job_list_with_policy(bp_archive_t *ar,
                                    add_job_list_t *list,
                                    bp_uint8 method)
{
    int threads;
    int rc;

    threads = choose_parallel_compression_threads(list, method);
    if (threads > 1) {
        if (method == BP_METHOD_FLUFF)
            bp_set_fast_fluff(ar, threads);
        else if (method == BP_METHOD_BOLTLZ)
            bp_set_fast_boltlz(ar, threads);
#ifdef BOLTPACK_FAST_FLUFF
        {
            int saved_threads;
            int rc;

            saved_threads = g_fast_threads;
            g_fast_threads = threads;
            rc = add_job_list_parallel(ar, list);
            g_fast_threads = saved_threads;
            if (rc != BP_OK) return rc;
            return validate_source_jobs(list);
        }
#endif
    }

    rc = add_job_list_serial(ar, list);
    if (rc != BP_OK) return rc;
    return validate_source_jobs(list);
}

#ifdef BOLTPACK_FAST_FLUFF
static void cleanup_packed_job(packed_job_t *job)
{
    if (job == NULL) return;
    if (job->tmp_fp != NULL) {
        fclose(job->tmp_fp);
        job->tmp_fp = NULL;
    }
    bp_packed_entry_dispose(&job->packed);
    memset(&job->tmp_io, 0, sizeof(job->tmp_io));
    job->rc = BP_OK;
}

static int job_prefers_inner_threads(const bp_archive_t *ar,
                                     const add_job_t *job,
                                     int total_threads)
{
    int group_blocks;
    double threshold;

    if (ar == NULL || job == NULL || total_threads <= 1)
        return 0;
    if (job->entry_type != BP_TYPE_DATA)
        return 0;

    if (ar->method == BP_METHOD_BOLTLZ) {
        threshold = 4.0 * (double)ar->block_size;
        return u64_to_double(job->file_size) >= threshold;
    }

    group_blocks = fluff_fast_get_group_blocks();
    if (group_blocks < 1)
        group_blocks = 1;

    threshold = FAST_FLUFF_LARGE_FILE_THRESHOLD_SCALE *
                (double)total_threads * (double)group_blocks *
                (double)ar->block_size;
    return u64_to_double(job->file_size) >= threshold;
}

static int choose_data_batch_size(const bp_archive_t *ar,
                                  const add_job_t *jobs,
                                  int remaining,
                                  int total_threads)
{
    int batch_n;

    if (remaining <= 0)
        return 0;
    if (total_threads <= 1)
        return 1;
    if (job_prefers_inner_threads(ar, &jobs[0], total_threads))
        return 1;

    batch_n = 0;
    while (batch_n < remaining &&
           batch_n < total_threads &&
           jobs[batch_n].entry_type == BP_TYPE_DATA) {
        if (batch_n > 0 &&
            job_prefers_inner_threads(ar, &jobs[batch_n], total_threads)) {
            break;
        }
        batch_n++;
    }

    return batch_n > 0 ? batch_n : 1;
}

static int add_data_job_batch(bp_archive_t *ar, add_job_t *jobs,
                              int batch_n)
{
    packed_job_t *packed_jobs;
    int i;
    int rc;
    int batch_use_sys_tmp;
    double total_batch;

    packed_jobs = (packed_job_t *)calloc((size_t)batch_n, sizeof(*packed_jobs));
    if (packed_jobs == NULL) {
        return BP_ERR_NOMEM;
    }

    /*
     * Decide once, before any thread starts, whether the whole batch fits
     * in /tmp.  All threads share the same read-only flag, preventing the
     * race where N threads each see "enough space" before any has written.
     */
    total_batch = 0.0;
    for (i = 0; i < batch_n; i++) {
        total_batch += (double)jobs[i].file_size.lo +
                       (double)jobs[i].file_size.hi * 4294967296.0;
    }
    batch_use_sys_tmp = tmp_fits_in_ram(total_batch);

#pragma omp parallel for schedule(dynamic) num_threads(batch_n)
    for (i = 0; i < batch_n; i++) {
        bp_archive_t worker_ar;
        FILE *src_fp;
        FILE *tmp_fp;
        bp_io_t src_io;
        int local_rc;
        struct stat st_chk;
        bp_uint64 actual_sz;

        memset(&worker_ar, 0, sizeof(worker_ar));
        worker_ar.block_size = ar->block_size;
        worker_ar.method = ar->method;
        worker_ar.method_level = ar->method_level;
        worker_ar.log_fn = ar->log_fn;
        worker_ar.log_data = ar->log_data;
        worker_ar.creator_os = ar->creator_os;
        worker_ar.creator_cpu = ar->creator_cpu;
        worker_ar.fast_fluff_threads = (batch_n == 1) ? ar->fast_fluff_threads : 0;
        local_rc = BP_OK;
        src_fp = NULL;
        tmp_fp = NULL;
        packed_jobs[i].tmp_fp = NULL;
        memset(&packed_jobs[i].packed, 0, sizeof(packed_jobs[i].packed));
        actual_sz = jobs[i].file_size;

        src_fp = fopen(jobs[i].path, "rb");
        if (src_fp == NULL) {
            local_rc = BP_ERR_IO;
        }

        /* Compare the opened source with its enumeration snapshot. */
        if (local_rc == BP_OK) {
            if (fstat(fileno(src_fp), &st_chk) != 0 ||
                !S_ISREG(st_chk.st_mode) ||
                !source_stat_matches(&jobs[i], &st_chk)) {
                fprintf(stderr,
                        "boltpack: source changed during archive creation: %s\n",
                        jobs[i].path);
                local_rc = BP_ERR_IO;
            }
            tmp_fp = open_tmp_file_at(batch_use_sys_tmp);
            if (tmp_fp == NULL) {
                local_rc = BP_ERR_IO;
            }
        }
        if (local_rc == BP_OK) {
            bp_io_from_file(&src_io, src_fp);
            bp_io_from_file(&packed_jobs[i].tmp_io, tmp_fp);
            local_rc = bp_archive_pack_entry_ex(&worker_ar,
                                               jobs[i].arcname,
                                               BP_TYPE_DATA,
                                               BP_CREATOR_OS,
                                               &src_io,
                                               actual_sz,
                                               jobs[i].mtime,
                                               &packed_jobs[i].tmp_io,
                                               &packed_jobs[i].packed,
                                               jobs[i].flags);
        }

        if (local_rc == BP_OK &&
            (fstat(fileno(src_fp), &st_chk) != 0 ||
             !source_stat_matches(&jobs[i], &st_chk))) {
            fprintf(stderr,
                    "boltpack: source changed while archiving: %s\n",
                    jobs[i].path);
            local_rc = BP_ERR_IO;
        }

        if (src_fp != NULL) {
            fclose(src_fp);
        }
        bp_archive_close(&worker_ar);

        if (local_rc != BP_OK) {
            if (tmp_fp != NULL) {
                fclose(tmp_fp);
            }
            packed_jobs[i].tmp_fp = NULL;
        } else {
            rewind(tmp_fp);
            packed_jobs[i].tmp_fp = tmp_fp;
        }
        packed_jobs[i].rc = local_rc;
    }

    for (i = 0; i < batch_n; i++) {
        if (packed_jobs[i].rc != BP_OK) {
            rc = packed_jobs[i].rc;
            for (i = 0; i < batch_n; i++) {
                cleanup_packed_job(&packed_jobs[i]);
            }
            free(packed_jobs);
            return rc;
        }
    }

    for (i = 0; i < batch_n; i++) {
        rc = bp_archive_commit_packed_entry(ar, &packed_jobs[i].tmp_io,
                                            &packed_jobs[i].packed);
        if (rc != BP_OK) {
            int j;
            for (j = i; j < batch_n; j++) {
                cleanup_packed_job(&packed_jobs[j]);
            }
            free(packed_jobs);
            return rc;
        }
        jobs[i].packed_checksum = packed_jobs[i].packed.checksum;
        jobs[i].packed_checksum_valid = 1;
        /* Use packed.original_size (reflects actual size used during pack). */
        print_added_data_result(ar, jobs[i].arcname,
                                packed_jobs[i].packed.original_size);
        cleanup_packed_job(&packed_jobs[i]);
    }

    free(packed_jobs);
    return BP_OK;
}

/*
 * Returns non-zero if a file of hint_bytes can comfortably land in /tmp
 * (RAM-backed tmpfs), leaving at least 256 MB headroom.
 * Used to decide whether the parallel temp-file path is worthwhile.
 */
static int tmp_fits_in_ram(double hint_bytes)
{
    struct statvfs st;
    if (hint_bytes <= 0.0) return 1;
    if (statvfs("/tmp", &st) != 0) return 1; /* can't tell -- assume yes */
    return hint_bytes + 256.0 * 1024.0 * 1024.0 <=
           (double)st.f_bavail * (double)st.f_frsize;
}

static int add_job_list_parallel(bp_archive_t *ar, add_job_list_t *list)
{
    int i;
    int batch_n;
    int rc;

    i = 0;
    while (i < list->count) {
        if (list->items[i].entry_type == BP_TYPE_DATA) {
            batch_n = choose_data_batch_size(ar, &list->items[i],
                                             list->count - i,
                                             g_fast_threads);
            /*
             * For a single large file (batch_n == 1, inner threading),
             * the parallel path creates a temp file the same size as the
             * source, reads it back, then writes it to the archive — 3x
             * I/O on the output volume.  When that temp file cannot land
             * in RAM (because /tmp has insufficient space), it would go
             * to the HDD archive directory, making this pure overhead for
             * incompressible data.  Fall back to the serial path instead:
             * bp_archive_add_entry writes directly to the archive (1x I/O)
             * and still uses inner fast-fluff threading via ar->fast_fluff_threads.
             */
            if (batch_n == 1 &&
                job_prefers_inner_threads(ar, &list->items[i], g_fast_threads)) {
                double hint = (double)list->items[i].file_size.lo +
                              (double)list->items[i].file_size.hi * 4294967296.0;
                if (!tmp_fits_in_ram(hint)) {
                    rc = add_job(ar, &list->items[i]);
                    if (rc != BP_OK) return rc;
                    i++;
                    continue;
                }
            }
            if (batch_n == 1 && ar->method == BP_METHOD_BOLTLZ &&
                !job_prefers_inner_threads(ar, &list->items[i],
                                           g_fast_threads)) {
                rc = add_job(ar, &list->items[i]);
                if (rc != BP_OK) return rc;
                i++;
                continue;
            }
            rc = add_data_job_batch(ar, &list->items[i], batch_n);
            if (rc != BP_OK) {
                return rc;
            }
            i += batch_n;
        } else {
            rc = add_job(ar, &list->items[i]);
            if (rc != BP_OK) {
                return rc;
            }
            i++;
        }
    }

    return BP_OK;
}

static int add_paths_queued(bp_archive_t *ar, int argc, char **argv)
{
    add_job_list_t list;
    int rc;

    memset(&list, 0, sizeof(list));
    rc = collect_arg_jobs(&list, argc, argv);
    if (rc != BP_OK) {
        free_job_list(&list);
        return rc;
    }

    rc = add_job_list_parallel(ar, &list);
    if (rc == BP_OK) rc = validate_source_jobs(&list);
    free_job_list(&list);
    return rc;
}
#endif

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

    format_number(bp_u64_to_ulong(entry->original_size), orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_ulong(entry->compressed_size), comp_buf, sizeof(comp_buf));

    printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, entry->name);
    return 0;
}

typedef struct {
    bp_posix_preflight_t paths;
    int error;
} extract_preflight_ctx_t;

static int extract_preflight_entry(const bp_entry_t *entry, void *user_data)
{
    extract_preflight_ctx_t *ctx = (extract_preflight_ctx_t *)user_data;
    int rc;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    if (!bp_name_is_safe(entry->name)) rc = BP_ERR_PARTIAL;
    else rc = bp_posix_preflight_add(&ctx->paths, entry->name,
                                     entry->name_len,
                                     entry->entry_type == BP_TYPE_DIR);
    if (rc != BP_OK) { ctx->error = rc; return 1; }
    return 0;
}

static int extract_preflight_archive(bp_io_t *io)
{
    extract_preflight_ctx_t ctx;
    int rc;
    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_list(io, extract_preflight_entry, &ctx);
    if (ctx.error != BP_OK) rc = ctx.error;
    if (rc == BP_OK) rc = bp_posix_preflight_finish(&ctx.paths);
    bp_posix_preflight_free(&ctx.paths);
    return rc;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct {
    const char *out_dir;    /* -o PATH or NULL */
    int         force;      /* -f flag */
    int         dry_run;    /* -n flag */
    bp_uint32   file_count;
    bp_uint32   dir_count;
    bp_uint32   unsafe_count;  /* entries skipped for unsafe names/targets */
    bp_uint32   partial_count;
    bp_uint32   queued_dirs;
    int         open_error;
    bp_restore_dir_t *dirs;
    int         slash_warned;  /* printed the leading-'/' warning once */
#ifdef BP_USE_DIRFD_EXTRACTION
    int         root_fd;
#endif
} extract_ctx_t;

#ifdef BP_USE_DIRFD_EXTRACTION
#include "../posix/extract_paths.h"

static int restore_file_metadata_fd(int fd, const char *name, bp_uint16 flags,
                                    bp_uint64 mtime, extract_ctx_t *ctx)
{
    if (flags & BP_ENTRY_FLAG_MODE_VALID) {
        mode_t mode = (mode_t)(flags & BP_ENTRY_FLAG_MODE_MASK);
        if (mode & 06000) {
            fprintf(stderr, "boltpack: not restoring set-ID bits on %s\n", name);
            ctx->partial_count++;
            mode &= (mode_t)~06000;
        }
        if (fchmod(fd, mode) != 0) return BP_ERR_IO;
    }
    if (flags & BP_ENTRY_FLAG_MTIME_VALID) {
        bp_off_t seconds = bp_u64_to_off(mtime);
        time_t t = (time_t)seconds;
        struct timespec times[2];
        if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
            (bp_off_t)t != seconds) return BP_ERR_OVERFLOW;
        times[0].tv_sec = 0; times[0].tv_nsec = UTIME_OMIT;
        times[1].tv_sec = t; times[1].tv_nsec = 0;
        if (futimens(fd, times) != 0) return BP_ERR_IO;
    }
    return BP_OK;
}
#endif

static int restore_file_metadata(const char *path, bp_uint16 flags,
                                 bp_uint64 mtime, extract_ctx_t *ctx)
{
    if (flags & BP_ENTRY_FLAG_MODE_VALID) {
        mode_t mode = (mode_t)(flags & BP_ENTRY_FLAG_MODE_MASK);
        if (mode & 06000) {
            fprintf(stderr, "boltpack: not restoring set-ID bits on %s\n", path);
            ctx->partial_count++;
            mode &= (mode_t)~06000;
        }
        if (chmod(path, mode) != 0) return BP_ERR_IO;
    }
    if (flags & BP_ENTRY_FLAG_MTIME_VALID) {
        bp_off_t seconds = bp_u64_to_off(mtime);
        time_t t = (time_t)seconds;
        struct stat st;
        struct utimbuf ut;
        if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
            (bp_off_t)t != seconds) return BP_ERR_OVERFLOW;
        if (stat(path, &st) != 0) return BP_ERR_IO;
        ut.actime = st.st_atime;
        ut.modtime = t;
        if (utime(path, &ut) != 0) return BP_ERR_IO;
    }
    return BP_OK;
}

/* Restore the timestamp on a staged symlink itself, never its target. */
#if defined(AT_SYMLINK_NOFOLLOW) && defined(UTIME_OMIT)
#define BP_SYMLINK_PATH_FD AT_FDCWD
#else
#define BP_SYMLINK_PATH_FD (-1)
#endif
static int restore_symlink_mtime_at(int parent_fd, const char *path,
                                    bp_uint16 flags, bp_uint64 mtime,
                                    extract_ctx_t *ctx)
{
    (void)ctx;
    if (!(flags & BP_ENTRY_FLAG_MTIME_VALID)) return BP_OK;
#if defined(AT_SYMLINK_NOFOLLOW) && defined(UTIME_OMIT)
    {
        bp_off_t seconds = bp_u64_to_off(mtime);
        time_t t = (time_t)seconds;
        struct timespec times[2];
        if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
            (bp_off_t)t != seconds) return BP_ERR_OVERFLOW;
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
        times[1].tv_sec = t;
        times[1].tv_nsec = 0;
        if (utimensat(parent_fd, path, times, AT_SYMLINK_NOFOLLOW) != 0)
            return BP_ERR_IO;
    }
#else
    (void)parent_fd;
    (void)path;
    (void)mtime;
    fprintf(stderr, "boltpack: symlink time not restored\n");
    ctx->partial_count++;
#endif
    return BP_OK;
}

/* Inserting longer paths first ensures children precede their parents. */
static int queue_directory(extract_ctx_t *ctx, const char *path,
                           const bp_entry_t *entry)
{
    bp_restore_dir_t *node, **at;
    size_t n = strlen(path);
    if (ctx->queued_dirs >= 65535U) return BP_ERR_OVERFLOW;
    node = (bp_restore_dir_t *)malloc(sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->path = (char *)malloc(n + 1);
    if (node->path == NULL) { free(node); return BP_ERR_NOMEM; }
    memcpy(node->path, path, n + 1);
    node->mtime = entry->mtime;
    node->flags = entry->flags;
    at = &ctx->dirs;
    while (*at != NULL && strlen((*at)->path) >= n) at = &(*at)->next;
    node->next = *at;
    *at = node;
    ctx->queued_dirs++;
    return BP_OK;
}

static int finish_directories(extract_ctx_t *ctx, int apply)
{
    int rc = BP_OK;
    while (ctx->dirs != NULL) {
        bp_restore_dir_t *node = ctx->dirs;
        ctx->dirs = node->next;
        if (apply) {
#ifdef BP_USE_DIRFD_EXTRACTION
            int fd = extract_open_directory(ctx->root_fd, node->path, 0);
            int r = fd < 0 ? BP_ERR_IO :
                    restore_file_metadata_fd(fd, node->path, node->flags, node->mtime, ctx);
            if (fd >= 0) close(fd);
#else
            int r = restore_file_metadata(node->path, node->flags, node->mtime, ctx);
#endif
            if (r != BP_OK) {
                fprintf(stderr, "boltpack: cannot restore attributes: %s\n", node->path);
                if (rc == BP_OK) rc = r;
            }
        }
        free(node->path);
        free(node);
    }
    return rc;
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

#ifndef BP_USE_DIRFD_EXTRACTION
/* Reject existing symlinks in archive-controlled directory components.
 * This prevents traversal through a pre-existing link, but is not a defense
 * against another process replacing a directory between this check and use. */
static int extract_parent_is_safe(const extract_ctx_t *ctx, const char *name,
                                  int include_leaf)
{
    char path[4096];
    size_t base, i, n = strlen(name);
    struct stat st;
    if (ctx->out_dir != NULL) {
        base = strlen(ctx->out_dir);
        if (base + n + 2 > sizeof(path)) return 0;
        memcpy(path, ctx->out_dir, base);
        if (base != 0 && path[base - 1] != '/') path[base++] = '/';
    } else base = 0;
    memcpy(path + base, name, n + 1);
    for (i = 0; i <= n; i++) {
        char saved;
        if (name[i] != '/' && !(include_leaf && name[i] == '\0')) continue;
        if (i == 0) continue;
        saved = path[base + i];
        path[base + i] = '\0';
        if (lstat(path, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return 0;
        } else if (errno != ENOENT) return 0;
        path[base + i] = saved;
    }
    return 1;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[4096];
    FILE *fp;
    bp_extract_output_t *output;
    const char *name;

    /* A hostile archive must not choose where we write: refuse ".."
     * components, backslashes, and drive letters outright. */
    name = extract_entry_name(entry, ctx);
    if (!bp_name_is_safe(name)) {
        fprintf(stderr, "boltpack: skipping entry with unsafe name: %s\n",
                entry->name);
        ctx->unsafe_count++;
        return NULL;
    }

    if (!ctx->dry_run && !extract_parent_is_safe(ctx, name,
                                                entry->entry_type == BP_TYPE_DIR)) {
        fprintf(stderr, "boltpack: unsafe directory in path: %s\n", entry->name);
        ctx->unsafe_count++;
        return NULL;
    }

    if (ctx->out_dir) {
        if (strlen(ctx->out_dir) + strlen(name) + 2 > sizeof(path)) {
            ctx->open_error = BP_ERR_NAME_TOO_LONG;
            return NULL;
        }
        path_join(path, sizeof(path), ctx->out_dir, name);
    } else {
        if (strlen(name) >= sizeof(path)) {
            ctx->open_error = BP_ERR_NAME_TOO_LONG;
            return NULL;
        }
        snprintf(path, sizeof(path), "%s", name);
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", entry->name);
        }
        if (!ctx->dry_run) {
            int r;
            if (mkdirs(path) != 0) {
                ctx->open_error = BP_ERR_IO;
                return NULL;
            }
            r = queue_directory(ctx, path, entry);
            if (r != BP_OK) ctx->open_error = r;
        }
        return NULL; /* no data to write for directories */
    }

    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_META) {
        ctx->partial_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf("   skipping: %s [%s - not supported on this platform]\n",
                   entry->name,
                   entry->entry_type == BP_TYPE_RSRC ? "resource fork"
                                                     : "metadata");
        }
        return NULL; /* skip non-data entries on Linux */
    }

    if (entry->entry_type == BP_TYPE_SYMLINK) {
        /* symlink handled in done_fn after data is available */
        ctx->file_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf("    linking: %s\n", entry->name);
        }
    } else {
        ctx->file_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf(" extracting: %s\n", entry->name);
        }
    }

    if (ctx->dry_run) return bp_archive_dry_run_stream();

    /* symlinks: we need to capture the target data, use tmpfile */
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        fp = tmpfile();
        if (fp == NULL) { ctx->open_error = BP_ERR_IO; return NULL; }
        output = (bp_extract_output_t *)calloc(1, sizeof(*output));
        if (output == NULL) { fclose(fp); ctx->open_error = BP_ERR_NOMEM; return NULL; }
        bp_io_from_file(&output->io, fp);
        return &output->io;
    }

    /* regular file */
    if (!ctx->force) {
        struct stat st_buf;
        if (lstat(path, &st_buf) == 0) {
            fprintf(stderr, "boltpack: file exists (use -f to overwrite): %s\n",
                    path);
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
    }

    if (mkdirs_for_file(path) != 0) { ctx->open_error = BP_ERR_IO; return NULL; }
    output = (bp_extract_output_t *)calloc(1, sizeof(*output));
    if (output == NULL) { ctx->open_error = BP_ERR_NOMEM; return NULL; }
    output->destination = (char *)malloc(strlen(path) + 1);
    output->temporary = (char *)malloc(strlen(path) + 32);
    if (output->destination == NULL || output->temporary == NULL) {
        free(output->destination); free(output->temporary); free(output);
        ctx->open_error = BP_ERR_NOMEM; return NULL;
    }
    strcpy(output->destination, path);
    {
        const char *slash = strrchr(path, '/');
        int fd;
        if (slash != NULL)
            sprintf(output->temporary, "%.*s/.boltpack-XXXXXX",
                    (int)(slash - path), path);
        else
            strcpy(output->temporary, ".boltpack-XXXXXX");
        fd = mkstemp(output->temporary);
        if (fd < 0) {
            fprintf(stderr, "boltpack: cannot create temporary file for %s\n", path);
            free(output->destination); free(output->temporary); free(output);
            ctx->open_error = BP_ERR_IO; return NULL;
        }
        fp = fdopen(fd, "wb");
        if (fp == NULL) {
            close(fd); unlink(output->temporary);
            free(output->destination); free(output->temporary); free(output);
            ctx->open_error = BP_ERR_IO; return NULL;
        }
    }
    bp_io_from_file(&output->io, fp);
    return &output->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    bp_extract_output_t *output = (bp_extract_output_t *)out;
    FILE *fp = (FILE *)out->handle;
    const char *name = extract_entry_name(entry, ctx);
    int rc = BP_OK;

    if (status != BP_OK) {
        fclose(fp);
        if (output->temporary != NULL) unlink(output->temporary);
        free(output->temporary);
        free(output->destination);
        free(output);
        return BP_OK;
    }

    if (entry->entry_type == BP_TYPE_SYMLINK) {
        char target[4096], path[4096], stage[4096], stage_leaf[64];
        long len;
        int target_safe = 0, staged = 0, tries;
        static unsigned long stage_counter;

        len = fseek(fp, 0, SEEK_END) == 0 ? ftell(fp) : -1;
        if (len > 0 && len < (long)sizeof(target)) {
            rewind(fp);
            if (fread(target, 1, (size_t)len, fp) == (size_t)len) {
                if (memchr(target, '\0', (size_t)len) != NULL) rc = BP_ERR_CORRUPT;
                else {
                    target[len] = '\0';
                    if (!bp_symlink_target_is_safe(name, target)) {
                        fprintf(stderr,
                                "boltpack: skipping symlink with unsafe target: "
                                "%s -> %s\n", entry->name, target);
                        ctx->unsafe_count++;
                    } else target_safe = 1;
                }
            } else rc = BP_ERR_IO;
        } else rc = BP_ERR_CORRUPT;
        if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        if (rc == BP_OK && target_safe) {
            const char *slash;
            size_t prefix_len;
            if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, name);
            else if (strlen(name) >= sizeof(path)) rc = BP_ERR_OVERFLOW;
            else strcpy(path, name);
            if (rc == BP_OK && (!extract_parent_is_safe(ctx, name, 0) ||
                                mkdirs_for_file(path) != 0)) rc = BP_ERR_IO;
            slash = rc == BP_OK ? strrchr(path, '/') : NULL;
            prefix_len = slash != NULL ? (size_t)(slash - path) + 1U : 0U;
            if (rc == BP_OK && prefix_len + sizeof(stage_leaf) > sizeof(stage))
                rc = BP_ERR_OVERFLOW;
            for (tries = 0; rc == BP_OK && tries < 256; tries++) {
                sprintf(stage_leaf, ".boltpack-link-%08lx-%08lx",
                        (unsigned long)getpid(), ++stage_counter);
                if (prefix_len != 0) memcpy(stage, path, prefix_len);
                strcpy(stage + prefix_len, stage_leaf);
                if (symlink(target, stage) == 0) { staged = 1; break; }
                if (errno != EEXIST) { rc = BP_ERR_IO; break; }
            }
            if (rc == BP_OK && !staged) rc = BP_ERR_IO;
            if (rc == BP_OK)
                rc = restore_symlink_mtime_at(BP_SYMLINK_PATH_FD, stage, entry->flags,
                                              entry->mtime, ctx);
            if (rc == BP_OK) {
                if (ctx->force) {
                    if (rename(stage, path) != 0) rc = BP_ERR_IO;
                    else staged = 0;
                } else {
                    if (link(stage, path) != 0) rc = BP_ERR_IO;
                    else if (unlink(stage) != 0) rc = BP_ERR_IO;
                    else staged = 0;
                }
            }
            if (staged) unlink(stage);
        }
        free(output);
        return rc;
    }

    if (fclose(fp) != 0) rc = BP_ERR_IO;
    if (rc == BP_OK)
        rc = restore_file_metadata(output->temporary, entry->flags, entry->mtime, ctx);
    if (rc == BP_OK) {
        if (ctx->force) {
            if (rename(output->temporary, output->destination) != 0) rc = BP_ERR_IO;
        } else {
            if (link(output->temporary, output->destination) != 0) rc = BP_ERR_IO;
            else if (unlink(output->temporary) != 0) rc = BP_ERR_IO;
        }
    }
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot finish extracting %s\n", output->destination);
        unlink(output->temporary);
    }
    free(output->temporary);
    free(output->destination);
    free(output);
    return rc;
}

#else /* BP_USE_DIRFD_EXTRACTION: use directory-relative operations */

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    const char *name = extract_entry_name(entry, ctx);
    bp_extract_output_t *output;
    char leaf[4096], temporary[64];
    FILE *fp;
    int parent_fd, fd, tries;
    static unsigned long temporary_counter;

    if (!bp_name_is_safe(name)) {
        fprintf(stderr, "boltpack: skipping entry with unsafe name: %s\n", entry->name);
        ctx->unsafe_count++;
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", entry->name);
        if (!ctx->dry_run) {
            fd = extract_open_directory(ctx->root_fd, name, 1);
            if (fd < 0) {
                fprintf(stderr, "boltpack: unsafe or inaccessible directory: %s\n", name);
                ctx->open_error = BP_ERR_IO;
            } else {
                int r;
                close(fd);
                r = queue_directory(ctx, name, entry);
                if (r != BP_OK && ctx->open_error == BP_OK) ctx->open_error = r;
            }
        }
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_META) {
        ctx->partial_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [%s - not supported on this platform]\n",
                   entry->name, entry->entry_type == BP_TYPE_RSRC ? "resource fork" : "metadata");
        return NULL;
    }
    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT)
        printf(entry->entry_type == BP_TYPE_SYMLINK ? "    linking: %s\n" : " extracting: %s\n",
               entry->name);
    if (ctx->dry_run) return bp_archive_dry_run_stream();

    output = (bp_extract_output_t *)calloc(1, sizeof(*output));
    if (output == NULL) { ctx->open_error = BP_ERR_NOMEM; return NULL; }
    output->parent_fd = -1;
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        fp = tmpfile();
        if (fp == NULL) { free(output); ctx->open_error = BP_ERR_IO; return NULL; }
        bp_io_from_file(&output->io, fp);
        return &output->io;
    }

    parent_fd = extract_open_parent(ctx->root_fd, name, leaf, sizeof(leaf));
    if (parent_fd < 0) {
        fprintf(stderr, "boltpack: unsafe or inaccessible directory: %s\n", name);
        free(output); ctx->open_error = BP_ERR_IO; return NULL;
    }
    output->parent_fd = parent_fd;
    if (!ctx->force) {
        struct stat st;
        if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            fprintf(stderr, "boltpack: file exists or cannot be inspected: %s\n", name);
            close(parent_fd); free(output); ctx->open_error = BP_ERR_IO; return NULL;
        }
    }
    output->destination = (char *)malloc(strlen(leaf) + 1);
    output->temporary = (char *)malloc(sizeof(temporary));
    if (output->destination == NULL || output->temporary == NULL) {
        close(parent_fd); free(output->destination); free(output->temporary); free(output);
        ctx->open_error = BP_ERR_NOMEM; return NULL;
    }
    strcpy(output->destination, leaf);
    fd = -1;
    for (tries = 0; tries < 256; tries++) {
        sprintf(temporary, ".boltpack-%08lx-%08lx", (unsigned long)getpid(), ++temporary_counter);
        fd = openat(parent_fd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        fprintf(stderr, "boltpack: cannot create temporary file for %s\n", name);
        close(parent_fd); free(output->destination); free(output->temporary); free(output);
        ctx->open_error = BP_ERR_IO; return NULL;
    }
    strcpy(output->temporary, temporary);
    fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd); unlinkat(parent_fd, temporary, 0); close(parent_fd);
        free(output->destination); free(output->temporary); free(output);
        ctx->open_error = BP_ERR_IO; return NULL;
    }
    bp_io_from_file(&output->io, fp);
    return &output->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    bp_extract_output_t *output = (bp_extract_output_t *)out;
    FILE *fp = (FILE *)out->handle;
    const char *name = extract_entry_name(entry, ctx);
    int rc = BP_OK;

    if (status != BP_OK) {
        fclose(fp);
        if (output->temporary != NULL) unlinkat(output->parent_fd, output->temporary, 0);
        if (output->parent_fd >= 0) close(output->parent_fd);
        free(output->temporary); free(output->destination); free(output);
        return BP_OK;
    }
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        char target[4096], leaf[4096], temporary[64];
        long len = fseek(fp, 0, SEEK_END) == 0 ? ftell(fp) : -1;
        int parent_fd = -1;
        int target_safe = 0, staged = 0, tries;
        static unsigned long stage_counter;
        if (len <= 0 || len >= (long)sizeof(target)) rc = BP_ERR_CORRUPT;
        else {
            rewind(fp);
            if (fread(target, 1, (size_t)len, fp) != (size_t)len ||
                memchr(target, '\0', (size_t)len) != NULL) rc = BP_ERR_CORRUPT;
            else {
                target[len] = '\0';
                if (!bp_symlink_target_is_safe(name, target)) {
                    fprintf(stderr, "boltpack: skipping symlink with unsafe target: %s -> %s\n",
                            entry->name, target);
                    ctx->unsafe_count++;
                } else target_safe = 1;
            }
        }
        if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        if (rc == BP_OK && target_safe) {
            parent_fd = extract_open_parent(ctx->root_fd, name, leaf, sizeof(leaf));
            if (parent_fd < 0) rc = BP_ERR_IO;
            for (tries = 0; rc == BP_OK && tries < 256; tries++) {
                sprintf(temporary, ".boltpack-link-%08lx-%08lx",
                        (unsigned long)getpid(), ++stage_counter);
                if (symlinkat(target, parent_fd, temporary) == 0) {
                    staged = 1;
                    break;
                }
                if (errno != EEXIST) { rc = BP_ERR_IO; break; }
            }
            if (rc == BP_OK && !staged) rc = BP_ERR_IO;
            if (rc == BP_OK)
                rc = restore_symlink_mtime_at(parent_fd, temporary,
                                              entry->flags, entry->mtime, ctx);
            if (rc == BP_OK) {
                if (ctx->force) {
                    if (renameat(parent_fd, temporary, parent_fd, leaf) != 0)
                        rc = BP_ERR_IO;
                    else staged = 0;
                } else {
                    if (extract_commit_noreplace(parent_fd, temporary, leaf) != 0)
                        rc = BP_ERR_IO;
                    else staged = 0;
                }
            }
            if (staged) unlinkat(parent_fd, temporary, 0);
        }
        if (parent_fd >= 0) close(parent_fd);
        free(output);
        return rc;
    }

    if (fflush(fp) != 0) rc = BP_ERR_IO;
    if (rc == BP_OK)
        rc = restore_file_metadata_fd(fileno(fp), name, entry->flags, entry->mtime, ctx);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc == BP_OK) {
        if (ctx->force) {
            if (renameat(output->parent_fd, output->temporary,
                         output->parent_fd, output->destination) != 0) rc = BP_ERR_IO;
        } else {
            if (extract_commit_noreplace(output->parent_fd, output->temporary,
                                         output->destination) != 0) rc = BP_ERR_IO;
        }
    }
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot finish extracting %s\n", name);
        unlinkat(output->parent_fd, output->temporary, 0);
    }
    close(output->parent_fd);
    free(output->temporary); free(output->destination); free(output);
    return rc;
}
#endif /* BP_USE_DIRFD_EXTRACTION */

#ifdef BOLTPACK_NET
/*
 * Extract an already-open archive using the frontend's existing callbacks.
 * Called by bp_net.c after a network transfer is received and verified.
 * Path safety of entry names is validated by the caller before this runs.
 */
int bp_frontend_extract_io(bp_io_t *aio, const char *out_dir, int force)
{
    extract_ctx_t ctx;
    int rc, dir_rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
#ifdef BP_USE_DIRFD_EXTRACTION
    ctx.root_fd = -1;
    if (out_dir != NULL && mkdirs(out_dir) != 0) return BP_ERR_IO;
    ctx.root_fd = open(out_dir ? out_dir : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (ctx.root_fd < 0) return BP_ERR_IO;
#endif
    rc = bp_archive_extract_all_ex(aio, extract_open, extract_done, NULL, &ctx);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && ctx.open_error != BP_OK)
        rc = ctx.open_error;
    dir_rc = finish_directories(
        &ctx, (rc == BP_OK || rc == BP_ERR_PARTIAL) &&
              ctx.open_error == BP_OK);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && dir_rc != BP_OK) rc = dir_rc;
    if (rc == BP_OK && (ctx.partial_count || ctx.unsafe_count)) rc = BP_ERR_PARTIAL;
#ifdef BP_USE_DIRFD_EXTRACTION
    close(ctx.root_fd);
#endif
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
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k\n"
        "  -0               Store only (no compression)\n"
        "  -1               BoltLite compression\n"
        "  -2               boltlz compression (default)\n"
        "  -F               Use fluff compression\n");
    fprintf(stderr,
        "  --interchange-profile-1  Enforce mini-compatible limits (create/add)\n");
#ifdef BOLTPACK_ZSTD
    fprintf(stderr, "  -Z               Use zstd compression\n");
#endif
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff/zstd: 1-9)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff/zstd 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8; zstd 3)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff/zstd 9)\n");
#else
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff: 1-9)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff 9)\n");
#endif
    fprintf(stderr,
        "  -o PATH          Output directory for extraction\n"
        "  -f               Force overwrite existing files\n"
        "  -n               Dry run\n"
        "  -q / -qq         Quiet / silent\n");
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr, "  --single-thread  Disable automatic parallel compression\n");
#endif
    fprintf(stderr,
        "\nExamples:\n"
        "  boltpack create backup.bolt folder\n"
        "  boltpack extract -o out backup.bolt\n");
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr, "  boltpack create -F --single-thread backup.bolt folder\n");
#endif
#ifdef BOLTPACK_NET
    bp_net_cli_usage(stderr);
#endif
    fprintf(stderr,
        "\nFor advanced compression and debug options, run: boltpack help advanced\n");
}

static void usage_advanced(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- advanced Linux CLI help\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.bolt [files...]\n\n"
        "Commands:\n"
        "  create (c)       Create archive from files\n"
        "  add (a)          Add files to existing archive\n"
        "  extract (x)      Extract archive\n"
        "  list (l)         List archive contents\n"
        "  test (t)         Test archive integrity\n"
        "  info (i)         Show archive information\n"
        "  delete (d)       Delete files from archive\n"
        "  debug-fluff (T)  Compress file with fluff (test/debug mode, Linux only)\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr, "\nOptions:\n");
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k (default: 32k, 256k with -F/-Z)\n");
#else
    fprintf(stderr,
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k (default: 32k, 256k with -F)\n");
#endif
    fprintf(stderr,
        "  -0               Store only (no compression)\n"
        "  -1               BoltLite compression\n"
        "  -2               boltlz compression\n"
        "  -F               Use fluff compression (defaults to 256k blocks)\n"
        "  --interchange-profile-1  BoltLite default (or -0 Stored), 16 KiB, mini limits\n");
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -Z               Use zstd compression (defaults to 256k blocks, method 4)\n");
#endif
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff: 1-9, 8=default; zstd: 1-9, 3=default)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff/zstd 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8; zstd 3)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff/zstd 9)\n");
#else
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff: 1-9, 8=default)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff 9)\n");
#endif
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr,
        "  -j N             Parallel threads for BoltLZ and fluff"
#ifdef BOLTPACK_ZSTD
        " and zstd"
#endif
        "\n"
        "  -g N             Fluff blocks per history group\n"
        "  --single-thread  Disable automatic parallel compression\n");
#endif
    fprintf(stderr,
        "  -q               Quiet (summary only)\n"
        "  -qq              Silent (errors only)\n"
        "  -o PATH          Output directory for extraction\n"
        "  -f               Force overwrite existing files\n"
        "  -n               Dry run\n");
}

static void usage(void)
{
    usage_basic();
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int level, int interchange_profile)
{
    const char *archive_name;
    add_job_list_t list;
    FILE *fp;
    bp_io_t aio;
    bp_archive_t ar;
    int rc;
    double start;
    double elapsed;
    char tmp_path[4096 + 64];
    int tmp_fd;
    int n;
    void (*old_sigint)(int);
    void (*old_sigterm)(int);

#ifdef BOLTPACK_NET
    /* Allow --send/--name to appear anywhere among the create arguments. */
    bp_net_cli_strip_create_args(&argc, argv);
#endif

    if (argc < 2) {
        fprintf(stderr, "boltpack: create requires archive name and files\n");
        return 1;
    }

    archive_name = argv[0];
    set_tmp_dir_from_archive(archive_name);
    memset(&list, 0, sizeof(list));

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    /*
     * Write to a temp file in the same directory as the archive, then
     * rename() atomically on success.  This ensures a failed or interrupted
     * run never leaves a partial .blt file at the final path.
     */
    n = snprintf(tmp_path, sizeof(tmp_path), "%s/bparch_XXXXXX",
                 g_tmp_dir[0] ? g_tmp_dir : ".");
    if (n <= 0 || n >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "boltpack: archive path too long\n");
        return 1;
    }
    tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        fprintf(stderr, "boltpack: cannot create temp archive: %s\n",
                strerror(errno));
        return 1;
    }
    fp = fdopen(tmp_fd, "w+b");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: fdopen failed\n");
        close(tmp_fd);
        unlink(tmp_path);
        return 1;
    }

    /* Register global path so signal handler can unlink it. */
    memcpy(g_tmp_archive, tmp_path, strlen(tmp_path) + 1);
    old_sigint  = signal(SIGINT,  sig_cleanup_handler);
    old_sigterm = signal(SIGTERM, sig_cleanup_handler);

    bp_io_from_file(&aio, fp);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        cleanup_tmp_archive();
        signal(SIGINT,  old_sigint);
        signal(SIGTERM, old_sigterm);
        return 1;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);
    start = wall_time_seconds();

    rc = collect_arg_jobs(&list, argc - 1, argv + 1);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error preparing input: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive();
        signal(SIGINT,  old_sigint);
        signal(SIGTERM, old_sigterm);
        free_job_list(&list);
        return 1;
    }

    rc = add_job_list_with_policy(&ar, &list, method);
    free_job_list(&list);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error adding input: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive();
        signal(SIGINT,  old_sigint);
        signal(SIGTERM, old_sigterm);
        return 1;
    }

    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive();
        signal(SIGINT,  old_sigint);
        signal(SIGTERM, old_sigterm);
        return 1;
    }

    elapsed = wall_time_seconds() - start;

    /* Save counts and sizes BEFORE bp_archive_close releases entry_list. */
    {
        bp_uint64 total_orig = {0, 0};
        bp_uint64 total_comp = {0, 0};
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

        bp_archive_close(&ar);
        fclose(fp);

        /* Atomically publish the completed archive. */
        if (rename(tmp_path, archive_name) != 0) {
            fprintf(stderr, "boltpack: rename failed: %s\n", strerror(errno));
            cleanup_tmp_archive();
            signal(SIGINT,  old_sigint);
            signal(SIGTERM, old_sigterm);
            return 1;
        }
        g_tmp_archive[0] = '\0'; /* rename succeeded — nothing to clean up */
        signal(SIGINT,  old_sigint);
        signal(SIGTERM, old_sigterm);

        /* summary line */
        if (g_verbosity < VERB_SILENT) {
            char numbuf[32], speedbuf[64];
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
    int rc, dir_rc;
    double start;
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
    rc = extract_preflight_archive(&aio);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: archive has names that cannot be mapped safely: %s\n",
                bp_strerror(rc));
        fclose(fp);
        return 1;
    }

    if (out_dir && !dry_run) {
        if (mkdirs(out_dir) != 0) {
            fprintf(stderr, "boltpack: cannot create output directory: %s\n", out_dir);
            fclose(fp);
            return 1;
        }
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
    ctx.dry_run = dry_run;
#ifdef BP_USE_DIRFD_EXTRACTION
    ctx.root_fd = -1;
    if (!dry_run) {
        ctx.root_fd = open(out_dir ? out_dir : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (ctx.root_fd < 0) {
            fprintf(stderr, "boltpack: cannot open output directory: %s\n", out_dir ? out_dir : ".");
            fclose(fp);
            return 1;
        }
    }
#endif

    start = wall_time_seconds();

    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done,
                                   g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                   &ctx);
    fclose(fp);

    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && ctx.open_error != BP_OK)
        rc = ctx.open_error;
    dir_rc = finish_directories(
        &ctx, (rc == BP_OK || rc == BP_ERR_PARTIAL) &&
              ctx.open_error == BP_OK);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && dir_rc != BP_OK) rc = dir_rc;
    if (rc == BP_OK && (ctx.partial_count || ctx.unsafe_count)) rc = BP_ERR_PARTIAL;
#ifdef BP_USE_DIRFD_EXTRACTION
    if (ctx.root_fd >= 0) close(ctx.root_fd);
#endif

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }

    elapsed = wall_time_seconds() - start;

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

    if (ctx.unsafe_count != 0) {
        fprintf(stderr,
                "boltpack: %lu entr%s skipped (unsafe name or link target)\n",
                (unsigned long)ctx.unsafe_count,
                ctx.unsafe_count == 1 ? "y" : "ies");
        return 1;
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
        if (total_orig.hi != 0 || total_orig.lo != 0) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
            printf(" (%.0f%%)", pct);
        }
        printf("\n");
    }

    return 0;
}

static int preserve_archive_owner_mode(int fd,
                                       const struct stat *original_stat)
{
    struct stat staged_stat;

    if (fstat(fd, &staged_stat) != 0) return 0;
    if ((staged_stat.st_uid != original_stat->st_uid ||
         staged_stat.st_gid != original_stat->st_gid) &&
        fchown(fd, original_stat->st_uid, original_stat->st_gid) != 0)
        return 0;
    return fchmod(fd, original_stat->st_mode & 07777) == 0;
}

static int cmd_delete(int argc, char **argv)
{
    const char *archive_name;
    const char **del_names;
    int del_count;
    FILE *src_fp, *dst_fp;
    bp_io_t src_io, dst_io;
    char tmp_name[4096 + 64];
    bp_uint32 deleted = 0;
    struct stat original_stat, opened_stat, current_stat;
    void (*old_sigint)(int);
    void (*old_sigterm)(int);
    int handlers_set = 0;
    int tmp_fd = -1;
    int result = 1;
    int n;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }

    archive_name = argv[0];
    del_names = (const char **)&argv[1];
    del_count = argc - 1;
    tmp_name[0] = '\0';
    set_tmp_dir_from_archive(archive_name);

    if (lstat(archive_name, &original_stat) != 0 ||
        !S_ISREG(original_stat.st_mode)) {
        fprintf(stderr, "boltpack: archive must be a regular, non-symlink file: %s\n",
                archive_name);
        return 1;
    }
    if (original_stat.st_nlink > 1) {
        fprintf(stderr,
                "boltpack: refusing to replace an archive with multiple hard links: %s\n",
                archive_name);
        return 1;
    }

    src_fp = fopen(archive_name, "rb");
    if (src_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    if (fstat(fileno(src_fp), &opened_stat) != 0 ||
        !S_ISREG(opened_stat.st_mode) ||
        opened_stat.st_dev != original_stat.st_dev ||
        opened_stat.st_ino != original_stat.st_ino ||
        opened_stat.st_size != original_stat.st_size ||
        opened_stat.st_mtime != original_stat.st_mtime) {
        fprintf(stderr, "boltpack: archive changed while opening: %s\n",
                archive_name);
        fclose(src_fp);
        return 1;
    }

    n = snprintf(tmp_name, sizeof(tmp_name), "%s/bpdel_XXXXXX",
                 g_tmp_dir[0] ? g_tmp_dir : ".");
    if (n <= 0 || (size_t)n >= sizeof(tmp_name)) {
        fprintf(stderr, "boltpack: archive path too long\n");
        fclose(src_fp);
        return 1;
    }
    tmp_fd = mkstemp(tmp_name);
    if (tmp_fd < 0) {
        fprintf(stderr, "boltpack: cannot create temporary archive: %s\n",
                strerror(errno));
        fclose(src_fp);
        return 1;
    }
    dst_fp = fdopen(tmp_fd, "w+b");
    if (dst_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open temporary archive stream\n");
        close(tmp_fd);
        unlink(tmp_name);
        fclose(src_fp);
        return 1;
    }
    tmp_fd = -1;

    memcpy(g_tmp_archive, tmp_name, strlen(tmp_name) + 1);
    old_sigint = signal(SIGINT, sig_cleanup_handler);
    old_sigterm = signal(SIGTERM, sig_cleanup_handler);
    handlers_set = 1;

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);

    rc = bp_archive_delete(&src_io, &dst_io, del_names, (bp_uint32)del_count,
                           &deleted);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        goto delete_cleanup;
    }

    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        goto delete_cleanup;
    }

    if (fflush(dst_fp) != 0 ||
        !preserve_archive_owner_mode(fileno(dst_fp), &original_stat) ||
        fsync(fileno(dst_fp)) != 0) {
        fprintf(stderr, "boltpack: cannot finalize temporary archive: %s\n",
                strerror(errno));
        goto delete_cleanup;
    }
    if (fclose(dst_fp) != 0) {
        dst_fp = NULL;
        fprintf(stderr, "boltpack: cannot close temporary archive: %s\n",
                strerror(errno));
        goto delete_cleanup;
    }
    dst_fp = NULL;
    if (fclose(src_fp) != 0) {
        src_fp = NULL;
        fprintf(stderr, "boltpack: cannot close source archive: %s\n",
                strerror(errno));
        goto delete_cleanup;
    }
    src_fp = NULL;

    if (lstat(archive_name, &current_stat) != 0 ||
        !S_ISREG(current_stat.st_mode) ||
        current_stat.st_dev != original_stat.st_dev ||
        current_stat.st_ino != original_stat.st_ino ||
        current_stat.st_size != original_stat.st_size ||
        current_stat.st_mtime != original_stat.st_mtime) {
        fprintf(stderr, "boltpack: archive changed during delete: %s\n",
                archive_name);
        goto delete_cleanup;
    }

    if (rename(tmp_name, archive_name) != 0) {
        fprintf(stderr, "boltpack: cannot publish deleted archive: %s\n",
                strerror(errno));
        goto delete_cleanup;
    }
    g_tmp_archive[0] = '\0';
    result = 0;

delete_cleanup:
    if (dst_fp != NULL) fclose(dst_fp);
    if (src_fp != NULL) fclose(src_fp);
    if (tmp_fd >= 0) close(tmp_fd);
    if (result != 0 && tmp_name[0] != '\0') unlink(tmp_name);
    g_tmp_archive[0] = '\0';
    if (handlers_set) {
        signal(SIGINT, old_sigint);
        signal(SIGTERM, old_sigterm);
    }

    if (result == 0 && g_verbosity < VERB_SILENT) {
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    }
    return result;
}

static int same_archive_file(const struct stat *left,
                             const struct stat *right)
{
    if (!S_ISREG(left->st_mode) || !S_ISREG(right->st_mode) ||
        left->st_dev != right->st_dev || left->st_ino != right->st_ino ||
        left->st_size != right->st_size || left->st_mtime != right->st_mtime)
        return 0;
#if defined(__APPLE__)
    if (left->st_mtimespec.tv_nsec != right->st_mtimespec.tv_nsec ||
        left->st_ctimespec.tv_sec != right->st_ctimespec.tv_sec ||
        left->st_ctimespec.tv_nsec != right->st_ctimespec.tv_nsec)
        return 0;
#else
    if (left->st_mtim.tv_nsec != right->st_mtim.tv_nsec ||
        left->st_ctim.tv_sec != right->st_ctim.tv_sec ||
        left->st_ctim.tv_nsec != right->st_ctim.tv_nsec)
        return 0;
#endif
    return 1;
}

static int copy_archive_stream(FILE *source, FILE *destination)
{
    unsigned char buffer[65536];
    size_t count;

    for (;;) {
        count = fread(buffer, 1, sizeof(buffer), source);
        if (count != 0 && fwrite(buffer, 1, count, destination) != count)
            return 0;
        if (count < sizeof(buffer)) {
            if (ferror(source)) return 0;
            break;
        }
    }
    return fflush(destination) == 0;
}

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int level, int interchange_profile)
{
    const char *archive_name;
    add_job_list_t list;
    FILE *source_fp;
    FILE *fp;
    bp_io_t io;
    bp_archive_t ar;
    struct stat original_stat;
    struct stat opened_stat;
    struct stat current_stat;
    char tmp_name[4096 + 64];
    int tmp_fd;
    int dir_fd;
    int n;
    int rc;
    int fin_rc;
    int archive_open;
    int handlers_set;
    int result;
    void (*old_sigint)(int);
    void (*old_sigterm)(int);

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }

    archive_name = argv[0];
    set_tmp_dir_from_archive(archive_name);
    memset(&list, 0, sizeof(list));
    tmp_name[0] = '\0';
    tmp_fd = -1;
    dir_fd = -1;
    source_fp = NULL;
    fp = NULL;
    archive_open = 0;
    handlers_set = 0;
    result = 1;

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    if (lstat(archive_name, &original_stat) != 0 ||
        !S_ISREG(original_stat.st_mode)) {
        fprintf(stderr, "boltpack: archive must be a regular, non-symlink file: %s\n",
                archive_name);
        goto add_cleanup;
    }
    if (original_stat.st_nlink > 1) {
        fprintf(stderr,
                "boltpack: refusing to replace an archive with multiple hard links: %s\n",
                archive_name);
        goto add_cleanup;
    }
    source_fp = fopen(archive_name, "rb");
    if (source_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        goto add_cleanup;
    }
    if (fstat(fileno(source_fp), &opened_stat) != 0 ||
        !same_archive_file(&original_stat, &opened_stat)) {
        fprintf(stderr, "boltpack: archive changed while opening: %s\n",
                archive_name);
        goto add_cleanup;
    }

    n = snprintf(tmp_name, sizeof(tmp_name), "%s/bpadd_XXXXXX",
                 g_tmp_dir[0] ? g_tmp_dir : ".");
    if (n <= 0 || (size_t)n >= sizeof(tmp_name)) {
        fprintf(stderr, "boltpack: archive path too long\n");
        goto add_cleanup;
    }
    tmp_fd = mkstemp(tmp_name);
    if (tmp_fd < 0) {
        fprintf(stderr, "boltpack: cannot create temporary archive: %s\n",
                strerror(errno));
        goto add_cleanup;
    }
    memcpy(g_tmp_archive, tmp_name, strlen(tmp_name) + 1);
    fp = fdopen(tmp_fd, "w+b");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open temporary archive stream\n");
        goto add_cleanup;
    }
    tmp_fd = -1;
    old_sigint = signal(SIGINT, sig_cleanup_handler);
    old_sigterm = signal(SIGTERM, sig_cleanup_handler);
    handlers_set = 1;

    if (!copy_archive_stream(source_fp, fp)) {
        fprintf(stderr, "boltpack: cannot copy source archive to staging file\n");
        goto add_cleanup;
    }
    if (fstat(fileno(source_fp), &opened_stat) != 0 ||
        !same_archive_file(&original_stat, &opened_stat)) {
        fprintf(stderr, "boltpack: archive changed while staging append: %s\n",
                archive_name);
        goto add_cleanup;
    }

    bp_io_from_file(&io, fp);
    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto add_cleanup;
    }
    archive_open = 1;
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            goto add_cleanup;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);

    rc = collect_arg_jobs(&list, argc - 1, argv + 1);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error preparing input: %s\n",
                bp_strerror(rc));
        goto add_cleanup;
    }

    rc = add_job_list_with_policy(&ar, &list, method);
    free_job_list(&list);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error adding input: %s\n",
                bp_strerror(rc));
        goto add_cleanup;
    }

    fin_rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    archive_open = 0;
    if (fin_rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(fin_rc));
        goto add_cleanup;
    }

    if (fflush(fp) != 0 ||
        !preserve_archive_owner_mode(fileno(fp), &original_stat) ||
        fsync(fileno(fp)) != 0) {
        fprintf(stderr, "boltpack: cannot finalize staged archive: %s\n",
                strerror(errno));
        goto add_cleanup;
    }
    if (fclose(fp) != 0) {
        fp = NULL;
        fprintf(stderr, "boltpack: cannot close staged archive: %s\n",
                strerror(errno));
        goto add_cleanup;
    }
    fp = NULL;

    if (fstat(fileno(source_fp), &opened_stat) != 0 ||
        lstat(archive_name, &current_stat) != 0 ||
        !same_archive_file(&original_stat, &opened_stat) ||
        !same_archive_file(&original_stat, &current_stat)) {
        fprintf(stderr, "boltpack: archive changed during append: %s\n",
                archive_name);
        goto add_cleanup;
    }
    if (fclose(source_fp) != 0) {
        source_fp = NULL;
        fprintf(stderr, "boltpack: cannot close source archive: %s\n",
                strerror(errno));
        goto add_cleanup;
    }
    source_fp = NULL;

    if (rename(tmp_name, archive_name) != 0) {
        fprintf(stderr, "boltpack: cannot publish appended archive: %s\n",
                strerror(errno));
        goto add_cleanup;
    }
    g_tmp_archive[0] = '\0';
    result = 0;

#ifdef O_DIRECTORY
    dir_fd = open(g_tmp_dir[0] ? g_tmp_dir : ".", O_RDONLY | O_DIRECTORY);
#else
    dir_fd = open(g_tmp_dir[0] ? g_tmp_dir : ".", O_RDONLY);
#endif
    if (dir_fd < 0 || fsync(dir_fd) != 0) {
        fprintf(stderr,
                "boltpack: warning: appended archive published, but directory sync failed: %s\n",
                strerror(errno));
    }
    if (dir_fd >= 0) {
        close(dir_fd);
        dir_fd = -1;
    }

add_cleanup:
    if (archive_open) bp_archive_close(&ar);
    if (fp != NULL) fclose(fp);
    if (source_fp != NULL) fclose(source_fp);
    if (tmp_fd >= 0) close(tmp_fd);
    if (result != 0 && g_tmp_archive[0] != '\0') cleanup_tmp_archive();
    if (handlers_set) {
        signal(SIGINT, old_sigint);
        signal(SIGTERM, old_sigterm);
    }
    if (dir_fd >= 0) close(dir_fd);
    free_job_list(&list);

    if (result == 0 && g_verbosity < VERB_SILENT) {
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    }
    return result;
}

/* ---- test fluff compression with statistics ----------------------------- */

static int cmd_test_fluff(const char *input_file)
{
    FILE *in_fp;
    FILE *out_stats;
    bp_uint8 *input_buf;
    bp_uint8 *output_buf;
    bp_uint8 *verify_buf;
    bp_uint32 input_size;
    bp_uint32 max_output;
    bp_uint32 blocks_processed;
    bp_uint32 total_input_bytes;
    bp_uint32 total_output_bytes;
    size_t bytes_read;
    int rc;
    double ratio;
    fluff_ctx_t *ctx;
    const bp_uint32 BLOCK_SIZE = 256 * 1024;  /* 256 KB blocks */
    const bp_uint32 MAX_INPUT_SIZE = 10 * 1024 * 1024; /* 10 MB limit */
    bp_uint32 block_start;

    /* Open input file */
    in_fp = fopen(input_file, "rb");
    if (in_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open input: %s\n", input_file);
        return 1;
    }

    /* Get input file size */
    fseek(in_fp, 0, SEEK_END);
    input_size = (bp_uint32)ftell(in_fp);
    fseek(in_fp, 0, SEEK_SET);

    if (input_size > MAX_INPUT_SIZE) {
        fprintf(stderr, "boltpack: input file too large (%lu bytes, max %lu)\n",
                (unsigned long)input_size,
                (unsigned long)MAX_INPUT_SIZE);
        fclose(in_fp);
        return 1;
    }

    /* Allocate input buffer */
    input_buf = (bp_uint8 *)malloc(input_size);
    if (input_buf == NULL) {
        fprintf(stderr, "boltpack: out of memory for input buffer\n");
        fclose(in_fp);
        return 1;
    }

    /* Read input */
    bytes_read = fread(input_buf, 1, input_size, in_fp);
    fclose(in_fp);

    if (bytes_read != input_size) {
        fprintf(stderr, "boltpack: read error on input file\n");
        free(input_buf);
        return 1;
    }

    if (g_verbosity < VERB_SILENT) {
        printf("Input size: %lu bytes\n", (unsigned long)input_size);
    }

    /* Initialize compression */
    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        free(input_buf);
        return 1;
    }

    /* Create persistent compression context */
    ctx = fluff_ctx_alloc();
    if (ctx == NULL) {
        fprintf(stderr, "boltpack: failed to allocate compression context\n");
        free(input_buf);
        return 1;
    }
    fluff_ctx_reset(ctx);

    /* Allocate output buffer (for maximum block size) */
    max_output = fluff_compress_bound(BLOCK_SIZE);
    output_buf = (bp_uint8 *)malloc(max_output * 100);  /* room for all blocks */
    if (output_buf == NULL) {
        fprintf(stderr, "boltpack: out of memory for output buffer\n");
        free(input_buf);
        fluff_ctx_free(ctx);
        return 1;
    }

    /* Allocate verification buffer (input size) */
    verify_buf = (bp_uint8 *)malloc(input_size);
    if (verify_buf == NULL) {
        fprintf(stderr, "boltpack: out of memory for verification buffer\n");
        free(input_buf);
        free(output_buf);
        fluff_ctx_free(ctx);
        return 1;
    }

    /* Compress all blocks */
    total_input_bytes = 0;
    total_output_bytes = 0;
    blocks_processed = 0;
    block_start = 0;

    while (block_start < input_size) {
        bp_uint32 block_end = block_start + BLOCK_SIZE;
        bp_uint32 block_len = (block_end > input_size) ? (input_size - block_start) : BLOCK_SIZE;
        bp_uint32 block_output_len = max_output;

        rc = fluff_compress_block_ctx(input_buf + block_start, block_len,
                                       output_buf + total_output_bytes, &block_output_len,
                                       ctx);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: compression failed on block %lu: %s\n",
                    (unsigned long)blocks_processed, bp_strerror(rc));
            free(input_buf);
            free(output_buf);
            free(verify_buf);
            fluff_ctx_free(ctx);
            return 1;
        }

        total_input_bytes += block_len;
        total_output_bytes += block_output_len;
        blocks_processed++;
        block_start = block_end;
    }

    if (g_verbosity < VERB_SILENT) {
        printf("Blocks processed: %lu\n", (unsigned long)blocks_processed);
        printf("Compressed size: %lu bytes\n", (unsigned long)total_output_bytes);
        ratio = (total_input_bytes > 0) ? (100.0 * total_output_bytes / total_input_bytes) : 0.0;
        printf("Compression ratio: %.2f%%\n", ratio);
    }

    /* Verify by decompressing all blocks */
    {
        bp_uint32 verify_pos = 0;
        bp_uint32 output_pos = 0;
        bp_uint32 verify_size;
        bp_uint32 block_idx;

        block_start = 0;
        for (block_idx = 0; block_idx < blocks_processed; block_idx++) {
            (void)block_start;  /* unused in simplified version */

            /* Simple approach: decompress remaining and check */
            verify_size = input_size - verify_pos;
            rc = fluff_decompress_block_ctx(output_buf + output_pos, 
                                             total_output_bytes - output_pos,
                                             verify_buf + verify_pos, &verify_size,
                                             ctx);
            if (rc != BP_OK) {
                fprintf(stderr, "boltpack: decompression failed: %s\n",
                        bp_strerror(rc));
                free(input_buf);
                free(output_buf);
                free(verify_buf);
                fluff_ctx_free(ctx);
                return 1;
            }

            verify_pos += verify_size;
            output_pos += (total_output_bytes - output_pos);
            /* Note: This simple test just decompresses all at once.
             * A production version would track block boundaries properly. */
            break;
        }

        if (verify_pos != input_size || memcmp(input_buf, verify_buf, input_size) != 0) {
            fprintf(stderr, "boltpack: round-trip verification FAILED\n");
            free(input_buf);
            free(output_buf);
            free(verify_buf);
            fluff_ctx_free(ctx);
            return 1;
        }

        if (g_verbosity < VERB_SILENT) {
            printf("Round-trip verification: PASSED\n");
        }
    }

    /* Write statistics to file */
    out_stats = fopen("fluff-stats.txt", "w");
    if (out_stats != NULL) {
        fluff_matchfinder_outcome_stats_t outcome_stats;
        fluff_rep_stats_t rep_stats;
        bp_uint32 lit_count;
        bp_uint32 match_count;
        bp_uint32 match_bytes;
        bp_uint32 chain_probes;
        bp_uint32 max_depth;
        bp_uint32 match_searches;
        bp_uint32 no_match_saturations;
        bp_uint32 subhash_wins;
        bp_uint32 chain_cmp_bytes;
        bp_uint32 rep_total_probes;
        bp_uint32 rep_total_matches;
        bp_uint32 rep_total_wins;
        double avg_match_len;
        double pct_literals;
        double pct_matches;
        double avg_cmp_per_probe;
        double saturation_pct;
        double subhash_win_pct;
        double rep_avg_cmp_per_probe;

        fluff_ctx_get_literal_count(ctx, &lit_count);
        fluff_ctx_get_match_count(ctx, &match_count);
        fluff_ctx_get_total_match_bytes(ctx, &match_bytes);
        fluff_ctx_get_chain_stats(ctx, &chain_probes, &max_depth);
        fluff_ctx_get_matchfinder_stats(ctx, &match_searches,
                                        &no_match_saturations,
                                        &subhash_wins,
                                        &chain_cmp_bytes);
        fluff_ctx_get_matchfinder_outcome_stats(ctx, &outcome_stats);
        fluff_ctx_get_rep_stats(ctx, &rep_stats);

        pct_literals = (lit_count + match_count > 0) ?
                       (100.0 * lit_count / (lit_count + match_count)) : 0.0;
        pct_matches = (lit_count + match_count > 0) ?
                      (100.0 * match_count / (lit_count + match_count)) : 0.0;
        avg_match_len = (match_count > 0) ? ((double)match_bytes / match_count) : 0.0;
        avg_cmp_per_probe = (chain_probes > 0) ?
                            ((double)chain_cmp_bytes / chain_probes) : 0.0;
        saturation_pct = (match_searches > 0) ?
                         (100.0 * no_match_saturations / match_searches) : 0.0;
        subhash_win_pct = (match_searches > 0) ?
                          (100.0 * subhash_wins / match_searches) : 0.0;
        rep_total_probes = rep_stats.rep0.probes + rep_stats.rep1.probes +
                           rep_stats.rep2.probes;
        rep_total_matches = rep_stats.rep0.matches + rep_stats.rep1.matches +
                            rep_stats.rep2.matches;
        rep_total_wins = rep_stats.rep0.wins + rep_stats.rep1.wins +
                         rep_stats.rep2.wins;
        rep_avg_cmp_per_probe = (rep_total_probes > 0) ?
            ((double)(rep_stats.rep0.cmp_bytes + rep_stats.rep1.cmp_bytes +
                      rep_stats.rep2.cmp_bytes) / rep_total_probes) : 0.0;

        fprintf(out_stats, "=== Fluff Compression Statistics ===\n");
        fprintf(out_stats, "Input size:        %lu bytes\n", (unsigned long)total_input_bytes);
        fprintf(out_stats, "Output size:       %lu bytes\n", (unsigned long)total_output_bytes);
        fprintf(out_stats, "Blocks processed:  %lu\n", (unsigned long)blocks_processed);
        ratio = (total_input_bytes > 0) ? (100.0 * total_output_bytes / total_input_bytes) : 0.0;
        fprintf(out_stats, "Compression ratio: %.2f%%\n", ratio);
        
        fprintf(out_stats, "\n=== LZ77 Parse Statistics ===\n");
        fprintf(out_stats, "Literals:          %lu (%.1f%%)\n", (unsigned long)lit_count, pct_literals);
        fprintf(out_stats, "Matches:           %lu (%.1f%%)\n", (unsigned long)match_count, pct_matches);
        fprintf(out_stats, "Match bytes:       %lu\n", (unsigned long)match_bytes);
        fprintf(out_stats, "Avg match length:  %.2f bytes\n", avg_match_len);
        
        fprintf(out_stats, "\n=== Hash Chain Statistics ===\n");
        fprintf(out_stats, "Chain probes:      %lu\n", (unsigned long)chain_probes);
        fprintf(out_stats, "Max chain depth:   %lu\n", (unsigned long)max_depth);
        fprintf(out_stats, "Match searches:    %lu\n", (unsigned long)match_searches);
        fprintf(out_stats, "No-match saturation: %lu (%.2f%%)\n",
            (unsigned long)no_match_saturations, saturation_pct);
        fprintf(out_stats, "Sub-hash wins:     %lu (%.2f%%)\n",
            (unsigned long)subhash_wins, subhash_win_pct);
        fprintf(out_stats, "Chain cmp bytes:   %lu\n",
            (unsigned long)chain_cmp_bytes);
        fprintf(out_stats, "Avg bytes/probe:   %.2f\n", avg_cmp_per_probe);

        fprintf(out_stats, "\n=== Match-Finder Outcome Breakdown ===\n");
        fprintf(out_stats,
            "No match:          %lu searches, %lu probes, %.2f bytes/probe\n",
            (unsigned long)outcome_stats.no_match.searches,
            (unsigned long)outcome_stats.no_match.probes,
            (outcome_stats.no_match.probes > 0) ?
                ((double)outcome_stats.no_match.cmp_bytes /
                 outcome_stats.no_match.probes) : 0.0);
        fprintf(out_stats,
            "Short filtered:    %lu searches, %lu probes, %.2f bytes/probe\n",
            (unsigned long)outcome_stats.short_filtered.searches,
            (unsigned long)outcome_stats.short_filtered.probes,
            (outcome_stats.short_filtered.probes > 0) ?
                ((double)outcome_stats.short_filtered.cmp_bytes /
                 outcome_stats.short_filtered.probes) : 0.0);
        fprintf(out_stats,
            "Accepted match:    %lu searches, %lu probes, %.2f bytes/probe\n",
            (unsigned long)outcome_stats.accepted_match.searches,
            (unsigned long)outcome_stats.accepted_match.probes,
            (outcome_stats.accepted_match.probes > 0) ?
                ((double)outcome_stats.accepted_match.cmp_bytes /
                 outcome_stats.accepted_match.probes) : 0.0);
        fprintf(out_stats,
            "Rep selected:      %lu searches, %lu probes, %.2f bytes/probe\n",
            (unsigned long)outcome_stats.rep_selected.searches,
            (unsigned long)outcome_stats.rep_selected.probes,
            (outcome_stats.rep_selected.probes > 0) ?
                ((double)outcome_stats.rep_selected.cmp_bytes /
                 outcome_stats.rep_selected.probes) : 0.0);
        fprintf(out_stats,
            "Lookahead drop:    %lu searches, %lu probes, %.2f bytes/probe\n",
            (unsigned long)outcome_stats.lookahead_discarded.searches,
            (unsigned long)outcome_stats.lookahead_discarded.probes,
            (outcome_stats.lookahead_discarded.probes > 0) ?
                ((double)outcome_stats.lookahead_discarded.cmp_bytes /
                 outcome_stats.lookahead_discarded.probes) : 0.0);

        fprintf(out_stats, "\n=== Rep-Distance Probe Stats ===\n");
        fprintf(out_stats, "Rep searches:      %lu\n",
            (unsigned long)rep_stats.searches);
        fprintf(out_stats, "Rep probes:        %lu\n",
            (unsigned long)rep_total_probes);
        fprintf(out_stats, "Rep matches:       %lu\n",
            (unsigned long)rep_total_matches);
        fprintf(out_stats, "Rep wins:          %lu\n",
            (unsigned long)rep_total_wins);
        fprintf(out_stats, "Rep avg bytes/probe: %.2f\n",
            rep_avg_cmp_per_probe);
        fprintf(out_stats,
            "Rep0:              %lu probes, %lu matches, %lu wins, %.2f bytes/probe\n",
            (unsigned long)rep_stats.rep0.probes,
            (unsigned long)rep_stats.rep0.matches,
            (unsigned long)rep_stats.rep0.wins,
            (rep_stats.rep0.probes > 0) ?
                ((double)rep_stats.rep0.cmp_bytes / rep_stats.rep0.probes) : 0.0);
        fprintf(out_stats,
            "Rep1:              %lu probes, %lu matches, %lu wins, %.2f bytes/probe\n",
            (unsigned long)rep_stats.rep1.probes,
            (unsigned long)rep_stats.rep1.matches,
            (unsigned long)rep_stats.rep1.wins,
            (rep_stats.rep1.probes > 0) ?
                ((double)rep_stats.rep1.cmp_bytes / rep_stats.rep1.probes) : 0.0);
        fprintf(out_stats,
            "Rep2:              %lu probes, %lu matches, %lu wins, %.2f bytes/probe\n",
            (unsigned long)rep_stats.rep2.probes,
            (unsigned long)rep_stats.rep2.matches,
            (unsigned long)rep_stats.rep2.wins,
            (rep_stats.rep2.probes > 0) ?
                ((double)rep_stats.rep2.cmp_bytes / rep_stats.rep2.probes) : 0.0);

        if (total_output_bytes < total_input_bytes) {
            fprintf(out_stats, "\n=== Space Saved ===\n");
            fprintf(out_stats, "Saved:             %lu bytes\n",
                    (unsigned long)(total_input_bytes - total_output_bytes));
        } else {
            fprintf(out_stats, "\n=== Compression Overhead ===\n");
            fprintf(out_stats, "Overhead:          %lu bytes\n",
                    (unsigned long)(total_output_bytes - total_input_bytes));
        }
        fclose(out_stats);
        if (g_verbosity < VERB_SILENT) {
            printf("Statistics saved to: fluff-stats.txt\n");
        }
    }

    if (g_verbosity < VERB_SILENT) {
        printf("fluff test completed successfully\n");
    }

    free(input_buf);
    free(output_buf);
    free(verify_buf);
    fluff_ctx_free(ctx);
    return 0;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char command;
    bp_uint32 block_size = 0;
    bp_uint8 method = BP_METHOD_BOLTLZ;
    int opt_level = 0; /* 0 = use default (no -l flag given) */
    const char *out_dir = NULL;
    int force = 0;
    int dry_run = 0;
    int interchange_profile = 0;
    int codec_given = 0;
    int i;

    if (argc < 2) {
        usage();
        return 1;
    }

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

    /* first arg is the command */
    command = parse_command_name(argv[1]);
    if (command == '\0') {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]);
        usage();
        return 1;
    }

    /* listen/scan may run with no further arguments; all else needs args */
    {
        int needs_arg = 1;
#ifdef BOLTPACK_NET
        if (command == 'L' || command == 'S') needs_arg = 0;
#endif
        if (needs_arg && argc < 3) {
            usage();
            return 1;
        }
    }

    /* parse options starting from argv[2] */
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
            method = BP_METHOD_STORED;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-1") == 0 ||
                   strcmp(argv[i], "--boltlite") == 0) {
            method = BP_METHOD_BOLTLITE;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-2") == 0 ||
                   strcmp(argv[i], "--boltlz") == 0) {
            method = BP_METHOD_BOLTLZ;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-F") == 0 ||
                   strcmp(argv[i], "--fluff") == 0) {
            method = BP_METHOD_FLUFF;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-Z") == 0 ||
                   strcmp(argv[i], "--zstd") == 0) {
            method = BP_METHOD_ZSTD;
            codec_given = 1;
            i++;
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
        } else if (strcmp(argv[i], "-j") == 0 ||
                   strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            if (!parse_thread_count_arg(argv[i + 1])) {
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--single-thread") == 0) {
            g_fast_thread_policy = FAST_THREAD_SINGLE;
            g_fast_threads = 0;
            i++;
        } else if (strcmp(argv[i], "-g") == 0 ||
                   strcmp(argv[i], "--group-size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            g_fast_group_blocks = atoi(argv[i + 1]);
            if (g_fast_group_blocks < 0) {
                fprintf(stderr, "boltpack: -g group size must be >= 0\n");
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-qq") == 0 ||
                   strcmp(argv[i], "--silent") == 0) {
            g_verbosity = VERB_SILENT;
            i++;
        } else if (strcmp(argv[i], "-q") == 0 ||
                   strcmp(argv[i], "--quiet") == 0) {
            g_verbosity = VERB_QUIET;
            i++;
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            out_dir = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0 ||
                   strcmp(argv[i], "--force") == 0) {
            force = 1;
            i++;
        } else if (strcmp(argv[i], "-n") == 0 ||
                   strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
            i++;
#ifdef BOLTPACK_NET
        } else if (bp_net_cli_take_option(&i, argc, argv)) {
            /* consumed a network option */
#endif
        } else {
            fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]);
            usage();
            return 1;
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

#ifndef BOLTPACK_FAST_FLUFF
    if (g_fast_thread_policy == FAST_THREAD_EXPLICIT && g_fast_threads > 0) {
        fprintf(stderr, "boltpack: -j requires an OpenMP-enabled modern Linux build\n");
        return 1;
    }
    if (g_fast_group_blocks > 0) {
        fprintf(stderr, "boltpack: -g requires a fast-fluff-enabled Linux build\n");
        return 1;
    }
#endif

#ifdef BOLTPACK_FAST_FLUFF
    if (g_fast_group_blocks > 0)
        fluff_fast_set_group_blocks(g_fast_group_blocks);
#endif

    switch (command) {
    case 'c':
        /* fluff/zstd work best with blocks matching the codec window size */
        if ((method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD)
            && block_size == 0) {
            block_size = BP_BLOCK_256K;
        }
        return cmd_create(argc - i, argv + i, block_size, method, opt_level,
                          interchange_profile);
    case 'a':
        if ((method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD)
            && block_size == 0) {
            block_size = BP_BLOCK_256K;
        }
        return cmd_add(argc - i, argv + i, block_size, method, opt_level,
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
    case 'T':
        if (i >= argc) {
            fprintf(stderr, "boltpack: T (test-fluff) requires input file\n");
            return 1;
        }
        return cmd_test_fluff(argv[i]);
    case 'i':
        if (i >= argc) {
            fprintf(stderr, "boltpack: info requires archive name\n");
            return 1;
        }
        return cmd_info(argv[i]);
    case 'd':
        return cmd_delete(argc - i, argv + i);
#ifdef BOLTPACK_NET
    case 'L':
        return cmd_listen(out_dir, force);
    case 'S':
        return cmd_scan();
#else
    case 'L':
    case 'S':
        fprintf(stderr, "boltpack: network support not compiled in "
                        "(rebuild with ENABLE_NET=1)\n");
        return 1;
#endif
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage();
        return 1;
    }
}
