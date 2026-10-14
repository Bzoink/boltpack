/*
 * main.c -- Linux CLI frontend for BoltPack
 *
 * ANSI C (C89/C90) only (with POSIX extensions for directory recursion).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L

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
#include <utime.h>

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

/*
 * Globals for atomic cmd_add: if a signal arrives while the archive is
 * being appended in-place, restore the critical tail (file table + footer)
 * to leave the archive in its original state.
 * All functions used in the signal handler (lseek, write, ftruncate) are
 * async-signal-safe per POSIX.
 */
static int            g_add_fd       = -1;   /* raw fd of the archive being appended */
static off_t          g_add_tail_off =  0;   /* byte offset of the saved tail */
static unsigned char *g_add_tail     = NULL; /* malloc'd tail bytes */
static size_t         g_add_tail_sz  =  0;   /* size of tail backup */

static void sig_add_restore_handler(int sig)
{
    if (g_add_fd >= 0 && g_add_tail != NULL && g_add_tail_sz > 0) {
        (void)lseek(g_add_fd, g_add_tail_off, SEEK_SET);
        (void)write(g_add_fd, g_add_tail, g_add_tail_sz);
        (void)ftruncate(g_add_fd, g_add_tail_off + (off_t)g_add_tail_sz);
    }
    g_add_fd     = -1;
    g_add_tail   = NULL;
    g_add_tail_sz = 0;
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
    if (slash != NULL && slash > archive_name) {
        dlen = (size_t)(slash - archive_name);
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
    bp_uint8   entry_type;
    bp_uint64  file_size;
    bp_uint64  mtime;
    bp_uint16  flags; /* BP_ENTRY_FLAG_MODE_VALID | (mode & BP_ENTRY_FLAG_MODE_MASK) */
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
static int add_job_list_parallel(bp_archive_t *ar, const add_job_list_t *list);
#endif

static void free_add_job(add_job_t *job)
{
    if (job == NULL) return;
    free(job->path);
    free(job->arcname);
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
                      bp_uint64 file_size, bp_uint64 mtime, bp_uint16 flags)
{
    add_job_t *new_items;
    char *path_copy;
    char *arc_copy;
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
    if (path_copy == NULL || arc_copy == NULL) {
        free(path_copy);
        free(arc_copy);
        return BP_ERR_NOMEM;
    }

    list->items[list->count].path = path_copy;
    list->items[list->count].arcname = arc_copy;
    list->items[list->count].entry_type = entry_type;
    list->items[list->count].file_size = file_size;
    list->items[list->count].mtime = mtime;
    list->items[list->count].flags = flags;
    list->count++;
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

static int add_job(bp_archive_t *ar, const add_job_t *job)
{
    int rc;

    if (job == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    if (job->entry_type == BP_TYPE_DIR) {
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

        /* Re-stat through the open fd to close the TOCTOU window between
         * the earlier stat (used for job->file_size) and the actual read.
         * If the file changed size, warn and use the real size so the
         * archive header is consistent with what was actually read. */
        actual_size = job->file_size;
        if (fstat(fileno(fp), &st) == 0) {
            bp_uint64 real_size = bp_off_to_u64((bp_off_t)st.st_size);
            if (real_size.lo != job->file_size.lo ||
                real_size.hi != job->file_size.hi) {
                char was_buf[32];
                char now_buf[32];
                format_size64(job->file_size, was_buf, sizeof(was_buf));
                format_size64(real_size, now_buf, sizeof(now_buf));
                fprintf(stderr,
                        "boltpack: warning: %s changed size during archive "
                        "(was %s, now %s) -- archiving current size\n",
                        job->path, was_buf, now_buf);
                actual_size = real_size;
            }
        }

        bp_io_from_file(&fio, fp);

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

        print_added_data_result(ar, job->arcname, actual_size);
        return BP_OK;
    }

    if (job->entry_type == BP_TYPE_SYMLINK) {
        char target[4096];
        ssize_t tlen;
        bp_io_t dummy_io;
        FILE *tmp;

        tlen = readlink(job->path, target, sizeof(target) - 1);
        if (tlen < 0) {
            fprintf(stderr, "boltpack: cannot read symlink: %s\n", job->path);
            return BP_ERR_IO;
        }
        target[tlen] = '\0';

        tmp = tmpfile();
        if (tmp == NULL) return BP_ERR_IO;
        fwrite(target, 1, (size_t)tlen, tmp);
        rewind(tmp);
        bp_io_from_file(&dummy_io, tmp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("    linking: %s -> %s\n", job->arcname, target);
        }

        rc = bp_archive_add_entry_ex(ar, job->arcname, BP_TYPE_SYMLINK,
                                   BP_CREATOR_OS, &dummy_io,
                                   bp_u64_from_u32((bp_uint32)tlen),
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
    job.mtime = bp_u64_from_u32((bp_uint32)st.st_mtime);
    job.flags = BP_ENTRY_FLAG_MODE_VALID | (bp_uint16)(st.st_mode & BP_ENTRY_FLAG_MODE_MASK);

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

static int collect_directory_jobs(add_job_list_t *list, const char *dir_path,
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

static int collect_path_jobs(add_job_list_t *list, const char *path,
                             const char *arcname)
{
    struct stat st;
    char dir_arc[4096];
    int rc;

    if (lstat(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        bp_uint16 mode_flags = BP_ENTRY_FLAG_MODE_VALID
                             | (bp_uint16)(st.st_mode & BP_ENTRY_FLAG_MODE_MASK);
        if (!ends_with_slash(arcname)) {
            snprintf(dir_arc, sizeof(dir_arc), "%s/", arcname);
        } else {
            snprintf(dir_arc, sizeof(dir_arc), "%s", arcname);
        }
        rc = append_job(list, path, dir_arc, BP_TYPE_DIR,
                        bp_u64_from_u32(0),
                        bp_u64_from_u32((bp_uint32)st.st_mtime), mode_flags);
        if (rc != BP_OK) return rc;
        return collect_directory_jobs(list, path, dir_arc);
    }

    if (S_ISREG(st.st_mode)) {
        bp_uint16 mode_flags = BP_ENTRY_FLAG_MODE_VALID
                             | (bp_uint16)(st.st_mode & BP_ENTRY_FLAG_MODE_MASK);
        return append_job(list, path, arcname, BP_TYPE_DATA,
                          bp_off_to_u64((bp_off_t)st.st_size),
                          bp_u64_from_u32((bp_uint32)st.st_mtime), mode_flags);
    }

    if (S_ISLNK(st.st_mode)) {
        bp_uint16 mode_flags = BP_ENTRY_FLAG_MODE_VALID
                             | (bp_uint16)(st.st_mode & BP_ENTRY_FLAG_MODE_MASK);
        return append_job(list, path, arcname, BP_TYPE_SYMLINK,
                          bp_u64_from_u32(0),
                          bp_u64_from_u32((bp_uint32)st.st_mtime), mode_flags);
    }

    return BP_OK;
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

static int add_job_list_serial(bp_archive_t *ar, const add_job_list_t *list)
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

static int choose_fast_fluff_threads(const add_job_list_t *list,
                                     bp_uint8 method)
{
#ifdef BOLTPACK_FAST_FLUFF
    bp_uint64 total_size;
    bp_uint64 min_size;
    int data_count;
    int threads;

    if (method != BP_METHOD_FLUFF
#ifdef BOLTPACK_ZSTD
        && method != BP_METHOD_ZSTD
#endif
        )
        return 0;
    if (g_fast_thread_policy == FAST_THREAD_SINGLE)
        return 0;

    threads = fluff_fast_get_threads();
    if (threads <= 1)
        return 0;

    summarize_data_jobs(list, &total_size, &data_count);
    if (data_count <= 0)
        return 0;

    if (g_fast_thread_policy == FAST_THREAD_EXPLICIT) {
        if (g_fast_threads > 0 && g_fast_threads < threads)
            threads = g_fast_threads;
    } else {
        min_size = bp_u64_from_ulong(FAST_FLUFF_AUTO_MIN_BYTES);
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
                                    const add_job_list_t *list,
                                    bp_uint8 method)
{
    int threads;

    threads = choose_fast_fluff_threads(list, method);
    if (threads > 1) {
        if (method == BP_METHOD_FLUFF)
            bp_set_fast_fluff(ar, threads);
#ifdef BOLTPACK_FAST_FLUFF
        {
            int saved_threads;
            int rc;

            saved_threads = g_fast_threads;
            g_fast_threads = threads;
            rc = add_job_list_parallel(ar, list);
            g_fast_threads = saved_threads;
            return rc;
        }
#endif
    }

    return add_job_list_serial(ar, list);
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

static int add_data_job_batch(bp_archive_t *ar, const add_job_t *jobs,
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

        /* Recheck size through the open fd to close the TOCTOU window. */
        if (local_rc == BP_OK) {
            if (fstat(fileno(src_fp), &st_chk) == 0) {
                bp_uint64 real_sz = bp_off_to_u64((bp_off_t)st_chk.st_size);
                if (real_sz.lo != jobs[i].file_size.lo ||
                    real_sz.hi != jobs[i].file_size.hi) {
                    char was_buf[32];
                    char now_buf[32];
                    format_size64(jobs[i].file_size, was_buf, sizeof(was_buf));
                    format_size64(real_sz, now_buf, sizeof(now_buf));
                    /* fprintf to stderr is thread-safe on Linux (glibc) */
                    fprintf(stderr,
                            "boltpack: warning: %s changed size during archive "
                            "(was %s, now %s) -- archiving current size\n",
                            jobs[i].path, was_buf, now_buf);
                    actual_sz = real_sz;
                }
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

static int add_job_list_parallel(bp_archive_t *ar, const add_job_list_t *list)
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

/* ---- extract callbacks ------------------------------------------------- */

typedef struct {
    const char *out_dir;    /* -o PATH or NULL */
    int         force;      /* -f flag */
    int         dry_run;    /* -n flag */
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
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[4096];
    FILE *fp;
    bp_io_t *io;
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

    if (ctx->out_dir) {
        path_join(path, sizeof(path), ctx->out_dir, name);
    } else {
        snprintf(path, sizeof(path), "%s", name);
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", entry->name);
        }
        if (!ctx->dry_run) {
            mkdirs(path);
        }
        return NULL; /* no data to write for directories */
    }

    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_META) {
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

    if (ctx->dry_run) return NULL;

    /* symlinks: we need to capture the target data, use tmpfile */
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        fp = tmpfile();
        if (fp == NULL) return NULL;
        io = (bp_io_t *)malloc(sizeof(bp_io_t));
        if (io == NULL) { fclose(fp); return NULL; }
        bp_io_from_file(io, fp);
        return io;
    }

    /* regular file */
    if (!ctx->force) {
        struct stat st_buf;
        if (stat(path, &st_buf) == 0) {
            fprintf(stderr, "boltpack: file exists (use -f to overwrite): %s\n",
                    path);
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
    FILE *fp = (FILE *)out->handle;
    const char *name = extract_entry_name(entry, ctx);

    if (entry->entry_type == BP_TYPE_SYMLINK) {
        /* read back the target path from the tmpfile */
        char target[4096];
        char path[4096];
        long len;

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
                    if (ctx->out_dir) {
                        path_join(path, sizeof(path), ctx->out_dir, name);
                    } else {
                        snprintf(path, sizeof(path), "%s", name);
                    }
                    mkdirs_for_file(path);
                    symlink(target, path);
                }
            }
        }
        fclose(fp);
        free(out);
        return;
    }

    /* close regular file, set permissions (if stored), then set mtime */
    fclose(fp);
    {
        char path[4096];
        struct utimbuf ut;

        if (ctx->out_dir) {
            path_join(path, sizeof(path), ctx->out_dir, name);
        } else {
            snprintf(path, sizeof(path), "%s", name);
        }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) {
            chmod(path, (mode_t)(entry->flags & BP_ENTRY_FLAG_MODE_MASK));
        }
        ut.actime  = (time_t)entry->mtime.lo;
        ut.modtime = (time_t)entry->mtime.lo;
        utime(path, &ut);
    }
    free(out);
}

#ifdef BOLTPACK_NET
/*
 * Extract an already-open archive using the frontend's existing callbacks.
 * Called by bp_net.c after a network transfer is received and verified.
 * Path safety of entry names is validated by the caller before this runs.
 */
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
        "  -1               LZSS compression\n"
        "  -2               boltlz compression (default)\n"
        "  -F               Use fluff compression\n"
        "  -Z               Use zstd compression (modern builds)\n");
    fprintf(stderr,
        "  -l N             Compression level (boltlz: 1-2; fluff/zstd: 1-9)\n"
        "  -o PATH          Output directory for extraction\n"
        "  -f               Force overwrite existing files\n"
        "  -n               Dry run\n"
        "  -q / -qq         Quiet / silent\n"
        "  --single-thread  Disable automatic parallel fluff\n");
    fprintf(stderr,
        "\nExamples:\n"
        "  boltpack create backup.bolt folder\n"
        "  boltpack extract backup.bolt -o out\n"
        "  boltpack create -F --single-thread backup.bolt folder\n");
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
    fprintf(stderr,
        "\nOptions:\n"
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k (default: 32k, 256k with -F/-Z)\n"
        "  -0               Store only (no compression)\n"
        "  -1               LZSS compression\n"
        "  -2               boltlz compression\n"
        "  -F               Use fluff compression (defaults to 256k blocks)\n"
        "  -Z               Use zstd compression (defaults to 256k blocks, method 4)\n");
    fprintf(stderr,
        "  -l N             Compression level (boltlz: 1-2; fluff: 1-9, 8=default; zstd: 1-9, 3=default)\n"
        "  -j N             Parallel threads for fluff and zstd compression (modern builds)\n");
    fprintf(stderr,
        "  -g N             Fluff blocks per history group (fast-fluff Linux builds, including arm64)\n"
        "  --single-thread  Disable automatic parallel fluff\n"
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
                      bp_uint8 method, int level)
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
    rc = bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        cleanup_tmp_archive();
        signal(SIGINT,  old_sigint);
        signal(SIGTERM, old_sigterm);
        return 1;
    }
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
    int rc;
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

    if (out_dir && !dry_run) {
        mkdirs(out_dir);
    }

    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
    ctx.dry_run = dry_run;

    start = wall_time_seconds();

    rc = bp_archive_extract_all(&aio, extract_open, extract_done,
                                g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                &ctx);
    fclose(fp);

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
    static const char * const names[5] = {"Stored", "LZSS", "boltlz", "Fluff", "zstd"};
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
    del_names = (const char **)&argv[1];
    del_count = argc - 1;

    src_fp = fopen(archive_name, "rb");
    if (src_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    snprintf(tmp_name, sizeof(tmp_name), "%s.bp_tmp", archive_name);
    dst_fp = fopen(tmp_name, "w+b");
    if (dst_fp == NULL) {
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

    if (g_verbosity < VERB_SILENT) {
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    }
    return 0;
}

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int level)
{
    const char *archive_name;
    add_job_list_t list;
    FILE *fp;
    bp_io_t io;
    bp_archive_t ar;
    int rc;
    int fin_rc;
    bp_uint64 tail_off;
    bp_uint64 tail_sz;
    unsigned char *tail_buf;
    size_t tail_bytes;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
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
     * Save the critical tail (file table + footer) so we can restore the
     * archive to its original state if the append fails or is interrupted.
     * We cap the backup at 256 MB; if the table is larger than that we
     * proceed without a backup (same behaviour as before this fix, but
     * that is only possible for archives with millions of entries).
     */
    tail_buf   = NULL;
    tail_bytes = 0;
    {
        FILE *qfp;
        bp_io_t qio;

        qfp = fopen(archive_name, "rb");
        if (qfp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
            return 1;
        }
        bp_io_from_file(&qio, qfp);
        rc = bp_archive_query_tail(&qio, &tail_off, &tail_sz);
        if (rc == BP_OK && tail_sz.hi == 0 &&
            tail_sz.lo <= 256U * 1024U * 1024U) {
            tail_bytes = (size_t)tail_sz.lo;
            tail_buf   = (unsigned char *)malloc(tail_bytes);
            if (tail_buf != NULL) {
                if (fseeko(qfp, (off_t)bp_u64_to_off(tail_off), SEEK_SET) != 0 ||
                    fread(tail_buf, 1, tail_bytes, qfp) != tail_bytes) {
                    free(tail_buf);
                    tail_buf   = NULL;
                    tail_bytes = 0;
                }
            }
        }
        fclose(qfp);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: cannot read archive: %s\n",
                    bp_strerror(rc));
            return 1;
        }
    }

    /* Open archive for in-place append. */
    fp = fopen(archive_name, "r+b");
    if (fp == NULL) {
        free(tail_buf);
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    /* Arm signal handler so Ctrl+C restores the tail. */
    if (tail_buf != NULL) {
        g_add_fd      = fileno(fp);
        g_add_tail_off = (off_t)bp_u64_to_off(tail_off);
        g_add_tail    = tail_buf;
        g_add_tail_sz = tail_bytes;
        signal(SIGINT,  sig_add_restore_handler);
        signal(SIGTERM, sig_add_restore_handler);
    }

    bp_io_from_file(&io, fp);
    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto add_fail_restore;
    }
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);

    rc = collect_arg_jobs(&list, argc - 1, argv + 1);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error preparing input: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        goto add_fail_restore;
    }

    rc = add_job_list_with_policy(&ar, &list, method);
    free_job_list(&list);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error adding input: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        goto add_fail_restore;
    }

    fin_rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);

    /* Disarm signal handler; restore path not needed on success. */
    if (tail_buf != NULL) {
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        g_add_fd      = -1;
        g_add_tail    = NULL;
        g_add_tail_sz = 0;
    }
    free(tail_buf);
    fclose(fp);

    if (fin_rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n",
                bp_strerror(fin_rc));
        return 1;
    }

    if (g_verbosity < VERB_SILENT) {
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    }
    return 0;

add_fail_restore:
    /* Restore critical tail to leave the archive uncorrupted. */
    if (tail_buf != NULL) {
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        g_add_fd      = -1;
        g_add_tail    = NULL;
        g_add_tail_sz = 0;
        if (fseeko(fp, (off_t)bp_u64_to_off(tail_off), SEEK_SET) == 0) {
            (void)fwrite(tail_buf, 1, tail_bytes, fp);
            fflush(fp);
            (void)ftruncate(fileno(fp),
                            (off_t)bp_u64_to_off(tail_off) + (off_t)tail_bytes);
        } else {
            fprintf(stderr,
                    "boltpack: warning: could not restore archive after error; "
                    "archive may be corrupt\n");
        }
        free(tail_buf);
    }
    fclose(fp);
    return 1;
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
            i++;
        } else if (strcmp(argv[i], "-1") == 0 ||
                   strcmp(argv[i], "--lzss") == 0) {
            method = BP_METHOD_LZSS;
            i++;
        } else if (strcmp(argv[i], "-2") == 0 ||
                   strcmp(argv[i], "--boltlz") == 0) {
            method = BP_METHOD_BOLTLZ;
            i++;
        } else if (strcmp(argv[i], "-F") == 0 ||
                   strcmp(argv[i], "--fluff") == 0) {
            method = BP_METHOD_FLUFF;
            i++;
        } else if (strcmp(argv[i], "-Z") == 0 ||
                   strcmp(argv[i], "--zstd") == 0) {
            method = BP_METHOD_ZSTD;
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

#ifndef BOLTPACK_FAST_FLUFF
    if (g_fast_thread_policy == FAST_THREAD_EXPLICIT && g_fast_threads > 0) {
        fprintf(stderr, "boltpack: -j requires a fast-fluff-enabled Linux build\n");
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
        return cmd_create(argc - i, argv + i, block_size, method, opt_level);
    case 'a':
        if ((method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD)
            && block_size == 0) {
            block_size = BP_BLOCK_256K;
        }
        return cmd_add(argc - i, argv + i, block_size, method, opt_level);
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
