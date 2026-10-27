/*
 * main.c -- Windows CLI frontend for BoltPack
 *
 * ANSI C (C89/C90) with MinGW POSIX extensions.
 * Primary target: MinGW cross-compiler (i686-w64-mingw32-gcc, x86_64-w64-mingw32-gcc).
 * For other DOS compilers (Turbo C, Watcom), minor adjustments
 * may be needed: mkdir() signature, header differences, etc.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_metadata.h"
#ifdef BOLTPACK_ZSTD
#include "../../core/zstd_wrap.h"
#endif

#ifdef BOLTPACK_FAST_FLUFF
#include "../../core/fluff_fast.h"
#include <omp.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <wchar.h>
#include <utime.h>

#ifdef BOLTPACK_NET
#include "../common/bp_net_cli.h"
#endif

/* ---- Windows path limit ---------------------------------------------------- */

#define BP_MAX_PATH 260

/* ---- codepage conversion helpers --------------------------------------- */

/* Convert a string from the active ANSI codepage (ACP) to UTF-8.
 * Returns 0 on success, -1 on failure. */
static int acp_to_utf8(const char *acp, char *utf8, int utf8_size)
{
    wchar_t wbuf[BP_MAX_PATH];
    int wlen, ulen;

    wlen = MultiByteToWideChar(CP_ACP, 0, acp, -1, wbuf, BP_MAX_PATH);
    if (wlen == 0) return -1;

    ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, utf8, utf8_size,
                               NULL, NULL);
    if (ulen == 0) return -1;

    return 0;
}

/* Convert a string from UTF-8 to the active ANSI codepage (ACP).
 * Returns 0 on success, -1 on failure. */
static int utf8_to_acp(const char *utf8, char *acp, int acp_size)
{
    wchar_t wbuf[BP_MAX_PATH];
    int wlen, alen;
    BOOL used_default = FALSE;
    UINT cp = GetACP();

    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wbuf, BP_MAX_PATH);
    if (wlen == 0) return -1;

    alen = WideCharToMultiByte(CP_ACP, cp == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS,
                               wbuf, wlen, acp, acp_size, NULL,
                               cp == CP_UTF8 ? NULL : &used_default);
    if (alen == 0 || used_default) return -1;

    return 0;
}

typedef struct {
    bp_uint8 data[BP_META_FAT_SIZE];
    unsigned pos;
} win_fat_source_t;

static long win_fat_read(void *handle, void *buffer, bp_uint32 count)
{
    win_fat_source_t *source = (win_fat_source_t *)handle;
    unsigned remaining = BP_META_FAT_SIZE - source->pos;
    if (count > remaining) count = remaining;
    memcpy(buffer, source->data + source->pos, count);
    source->pos += count;
    return (long)count;
}

static int add_win_fat_metadata(bp_archive_t *ar, const char *name,
                                const char *path, bp_uint8 subject_type)
{
    win_fat_source_t source;
    bp_io_t io;
    DWORD attributes = GetFileAttributesA(path);
    bp_uint8 value;
    int rc;
    if (attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_IO;
    value = (bp_uint8)(attributes & BP_META_FAT_MASK);
    rc = bp_metadata_fat_encode(source.data, sizeof(source.data),
                                subject_type, BP_META_FAT_MASK, value);
    if (rc != BP_OK) return rc;
    source.pos = 0;
    io.read = win_fat_read; io.write = NULL; io.seek = NULL; io.tell = NULL;
    io.handle = &source;
    return bp_archive_add_entry_ex(ar, name, BP_TYPE_META, BP_META_PORTABLE,
                                   &io, bp_u64_from_u32(BP_META_FAT_SIZE),
                                   bp_u64_from_u32(0), 0);
}

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

static int g_verbosity = VERB_DEFAULT;
static fast_thread_policy_t g_fast_thread_policy = FAST_THREAD_AUTO;
static int g_fast_threads = 0; /* explicit thread count for -j/--threads */
static int g_fast_group_blocks = 0; /* 0 = codec default; >0 = grouped reset span */

/*
 * Path of the in-progress temp archive written by cmd_create.
 * Cleared on success (after MoveFileEx).  The console ctrl handler
 * deletes it so Ctrl+C does not leave a partial .blt file on disk.
 */
static char g_tmp_archive[BP_MAX_PATH * 2] = "";

static void cleanup_tmp_archive_win(void)
{
    if (g_tmp_archive[0] != '\0') {
        DeleteFileA(g_tmp_archive);
        g_tmp_archive[0] = '\0';
    }
}

static BOOL WINAPI ctrl_cleanup_handler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT) {
        cleanup_tmp_archive_win();
    }
    return FALSE; /* let default handler terminate the process */
}

/*
 * Globals for atomic cmd_add: if Ctrl+C arrives while the archive is
 * being appended in-place, restore the critical tail via the Win32 handle
 * so the archive is left in its original state.
 * Windows console ctrl handlers run on a separate thread, so Win32 I/O
 * calls (WriteFile, SetFilePointerEx, SetEndOfFile) are safe here.
 */
static HANDLE         g_add_handle   = INVALID_HANDLE_VALUE;
static __int64        g_add_tail_off = 0;
static unsigned char *g_add_tail     = NULL;
static size_t         g_add_tail_sz  = 0;
static unsigned char  g_add_header[BP_HEADER_SIZE];

static BOOL WINAPI ctrl_add_restore_handler(DWORD ctrl_type)
{
    LARGE_INTEGER li;
    DWORD written;

    if ((ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT ||
         ctrl_type == CTRL_BREAK_EVENT) &&
        g_add_handle != INVALID_HANDLE_VALUE &&
        g_add_tail != NULL && g_add_tail_sz > 0) {

        li.QuadPart = g_add_tail_off;
        SetFilePointerEx(g_add_handle, li, NULL, FILE_BEGIN);
        WriteFile(g_add_handle, g_add_tail, (DWORD)g_add_tail_sz,
                  &written, NULL);
        li.QuadPart = g_add_tail_off + (__int64)g_add_tail_sz;
        SetFilePointerEx(g_add_handle, li, NULL, FILE_BEGIN);
        SetEndOfFile(g_add_handle);
        li.QuadPart = 0;
        SetFilePointerEx(g_add_handle, li, NULL, FILE_BEGIN);
        WriteFile(g_add_handle, g_add_header, BP_HEADER_SIZE, &written, NULL);
    }
    g_add_handle  = INVALID_HANDLE_VALUE;
    g_add_tail    = NULL;
    g_add_tail_sz = 0;
    return FALSE; /* let default handler terminate the process */
}

/*
 * Directory of the output archive, used as the fallback location for
 * temp files when %TEMP% does not have enough space.  Set by
 * set_tmp_dir_from_archive() early in cmd_create / cmd_add.
 */
static char g_tmp_dir[BP_MAX_PATH] = "";

/*
 * Derive the archive directory from archive_name and store it in
 * g_tmp_dir.  On Windows paths may use / or \ as separators.
 */
static void set_tmp_dir_from_archive(const char *archive_name)
{
    const char *slash;
    const char *bslash;
    const char *last;
    size_t dlen;

    g_tmp_dir[0] = '\0';
    if (archive_name == NULL) return;

    slash  = strrchr(archive_name, '/');
    bslash = strrchr(archive_name, '\\');
    last   = (bslash > slash) ? bslash : slash;

    if (last != NULL && last > archive_name) {
        dlen = (size_t)(last - archive_name);
        if (dlen < sizeof(g_tmp_dir)) {
            memcpy(g_tmp_dir, archive_name, dlen);
            g_tmp_dir[dlen] = '\0';
        }
    } else {
        g_tmp_dir[0] = '.';
        g_tmp_dir[1] = '\0';
    }
}

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
    return _fseeki64((FILE *)handle, (__int64)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)_ftelli64((FILE *)handle);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read  = file_read;
    io->write = file_write;
    io->seek  = file_seek;
    io->tell  = file_tell;
    io->handle = fp;
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
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (QueryPerformanceFrequency(&freq) &&
        QueryPerformanceCounter(&counter) &&
        freq.QuadPart != 0) {
        return (double)counter.QuadPart / (double)freq.QuadPart;
    }
    return (double)GetTickCount() / 1000.0;
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
    (void)bufsize;
    if (ends_with_sep(dir)) {
        sprintf(buf, "%s%s", dir, name);
    } else {
        sprintf(buf, "%s/%s", dir, name);
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

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#define bp_mkdir(path) _mkdir(path)
#else
#define bp_mkdir(path) mkdir(path, 0755)
#endif

static int win_mkdir(const char *path)
{
    return bp_mkdir(path);
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
            if (win_mkdir(tmp) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (win_mkdir(tmp) != 0 && errno != EEXIST) {
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

/* ---- queued add helpers ------------------------------------------------ */

typedef struct {
    char     *path;
    char     *arcname;
    bp_uint8  entry_type;
    bp_uint64 file_size;
    bp_uint64 mtime;
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

static int collect_path_jobs(add_job_list_t *list, const char *path,
                             const char *arcname);

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
                      bp_uint64 file_size, bp_uint64 mtime)
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
    list->count++;
    return BP_OK;
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
    char arcname_utf8[BP_MAX_PATH * 2];

    if (job == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    if (job->entry_type == BP_TYPE_DIR) {
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", job->arcname);
        }

        if (acp_to_utf8(job->arcname, arcname_utf8, sizeof(arcname_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", job->arcname);
            return BP_ERR_INVALID_ARG;
        }

        return bp_archive_add_entry(ar, arcname_utf8, BP_TYPE_DIR,
                                    BP_OS_WINDOWS, NULL,
                                    bp_u64_from_u32(0), job->mtime);
    }

    if (job->entry_type == BP_TYPE_DATA) {
        FILE *fp;
        bp_io_t fio;
        int rc;
        bp_uint64 actual_size;
        struct _stat64 st;

        fp = fopen(job->path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", job->path);
            return BP_ERR_IO;
        }

        /* Re-stat through the open fd to close the TOCTOU window. */
        actual_size = job->file_size;
        if (_fstat64(_fileno(fp), &st) == 0) {
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

        if (acp_to_utf8(job->arcname, arcname_utf8, sizeof(arcname_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", job->arcname);
            fclose(fp);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry(ar, arcname_utf8, BP_TYPE_DATA,
                                  BP_OS_WINDOWS, &fio,
                                  actual_size, job->mtime);
        fclose(fp);

        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n",
                    job->path, bp_strerror(rc));
            return rc;
        }

        print_added_data_result(ar, job->arcname, actual_size);
        return BP_OK;
    }

    return BP_OK;
}

static int sum_path_data_size(const char *path, bp_uint64 *total_size,
                              int *data_count)
{
    struct _stat64 st;

    if (_stat64(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *ent;
        char child_path[BP_MAX_PATH];
        int rc;

        d = opendir(path);
        if (d == NULL) {
            fprintf(stderr, "boltpack: cannot open directory: %s\n", path);
            return BP_ERR_IO;
        }

        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;

            path_join(child_path, sizeof(child_path), path, ent->d_name);
            rc = sum_path_data_size(child_path, total_size, data_count);
            if (rc != BP_OK) {
                closedir(d);
                return rc;
            }
        }

        closedir(d);
        return BP_OK;
    }

    if (S_ISREG(st.st_mode)) {
        *total_size = bp_u64_add(*total_size, bp_off_to_u64((bp_off_t)st.st_size));
        (*data_count)++;
    }

    return BP_OK;
}

static int summarize_input_args(int argc, char **argv, bp_uint64 *total_size,
                                int *data_count)
{
    int i;
    int rc;

    *total_size = bp_u64_from_u32(0);
    *data_count = 0;
    for (i = 0; i < argc; i++) {
        rc = sum_path_data_size(argv[i], total_size, data_count);
        if (rc != BP_OK)
            return rc;
    }
    return BP_OK;
}

static int choose_fast_fluff_threads(bp_uint64 total_size, int data_count,
                                     bp_uint8 method)
{
#ifdef BOLTPACK_FAST_FLUFF
    bp_uint64 min_size;
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
    if (threads <= 1 || data_count <= 0)
        return 0;
    if (threads > FAST_COMPRESSION_MAX_THREADS)
        threads = FAST_COMPRESSION_MAX_THREADS;

    if (g_fast_thread_policy == FAST_THREAD_EXPLICIT) {
        if (g_fast_threads > 0 && g_fast_threads < threads)
            threads = g_fast_threads;
    } else {
        min_size = bp_u64_from_ulong(method == BP_METHOD_BOLTLZ
            ? FAST_BOLTLZ_AUTO_MIN_BYTES : FAST_FLUFF_AUTO_MIN_BYTES);
        if (!bp_u64_gte(total_size, min_size))
            return 0;
    }

    if (data_count > 1 && threads > data_count)
        threads = data_count;

    return threads > 1 ? threads : 0;
#else
    (void)total_size;
    (void)data_count;
    (void)method;
    return 0;
#endif
}

static int collect_directory_jobs(add_job_list_t *list, const char *dir_path,
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
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
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
    struct _stat64 st;
    char dir_arc[BP_MAX_PATH];
    int rc;

    if (_stat64(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!ends_with_sep(arcname)) {
            sprintf(dir_arc, "%s/", arcname);
        } else {
            strcpy(dir_arc, arcname);
        }

        rc = append_job(list, path, dir_arc, BP_TYPE_DIR,
                        bp_u64_from_u32(0),
                        bp_off_to_u64((bp_off_t)st.st_mtime));
        if (rc != BP_OK) return rc;
        return collect_directory_jobs(list, path, dir_arc);
    }

    if (S_ISREG(st.st_mode)) {
        return append_job(list, path, arcname, BP_TYPE_DATA,
                          bp_off_to_u64((bp_off_t)st.st_size),
                          bp_off_to_u64((bp_off_t)st.st_mtime));
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
        char arcbuf[BP_MAX_PATH];

        path = argv[i];
        arcname = clean_path(path);
        strcpy(arcbuf, arcname);
        normalize_separators(arcbuf);

        rc = collect_path_jobs(list, path, arcbuf);
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

#ifdef BOLTPACK_FAST_FLUFF
/*
 * Returns non-zero if total_bytes can comfortably fit in %TEMP%,
 * leaving at least 256 MB headroom.  Checks cumulative size, not per-file,
 * so callers in batch mode must pass the sum of all files in the batch.
 */
static int tmp_fits_on_tmp_drive(double total_bytes)
{
    char tmp_dir[BP_MAX_PATH];
    ULARGE_INTEGER avail;

    if (total_bytes <= 0.0) return 1;
    if (!GetTempPathA(BP_MAX_PATH, tmp_dir)) return 1; /* can't tell -- assume yes */
    if (!GetDiskFreeSpaceExA(tmp_dir, &avail, NULL, NULL)) return 1;
    return total_bytes + 256.0 * 1024.0 * 1024.0 <= (double)avail.QuadPart;
}

/*
 * Returns non-zero if total_bytes can comfortably fit in the archive
 * destination directory (g_tmp_dir), leaving at least 256 MB headroom.
 */
static int tmp_fits_on_archive_drive(double total_bytes)
{
    ULARGE_INTEGER avail;

    if (total_bytes <= 0.0) return 1;
    if (g_tmp_dir[0] == '\0') return 0;
    if (!GetDiskFreeSpaceExA(g_tmp_dir, &avail, NULL, NULL)) return 1;
    return total_bytes + 256.0 * 1024.0 * 1024.0 <= (double)avail.QuadPart;
}

/*
 * Open a temp file at a pre-decided location.
 * use_sys_tmp != 0 -> %TEMP% (tmpfile); 0 -> archive dir (GetTempFileName +
 * FILE_FLAG_DELETE_ON_CLOSE).  Does NOT call GetDiskFreeSpaceEx;
 * the caller must have already decided.  Safe inside an OMP parallel region.
 */
static FILE *open_tmp_file_win_at(int use_sys_tmp)
{
    FILE *fp;
    char name[BP_MAX_PATH];
    HANDLE h;
    int fd;

    if (use_sys_tmp) {
        fp = tmpfile();
        if (fp != NULL) return fp;
        /* tmpfile() failed; fall through to archive directory. */
    }

    if (g_tmp_dir[0] != '\0') {
        if (GetTempFileNameA(g_tmp_dir, "bpt", 0, name)) {
            h = CreateFileA(name,
                            GENERIC_READ | GENERIC_WRITE,
                            0, NULL,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                            NULL);
            if (h != INVALID_HANDLE_VALUE) {
                fd = _open_osfhandle((intptr_t)h, 0);
                if (fd >= 0) {
                    fp = _fdopen(fd, "w+b");
                    if (fp != NULL) return fp;
                    _close(fd); /* also closes h */
                } else {
                    CloseHandle(h);
                }
            }
            /* Clean up orphaned named file before falling through. */
            DeleteFileA(name);
        }
    }

    return tmpfile(); /* last resort */
}

/*
 * Single-file wrapper kept for callers outside batch context.
 * Not safe inside an OMP parallel region -- use open_tmp_file_win_at()
 * with a pre-computed flag instead.
 */
static FILE *open_tmp_file_win(double hint_bytes)
{
    return open_tmp_file_win_at(tmp_fits_on_tmp_drive(hint_bytes));
}

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

static int add_data_job_batch(bp_archive_t *ar, const add_job_t *jobs,
                              int batch_n, int total_threads)
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
     * in %TEMP%.  Summing all files prevents the race where N threads each
     * see "enough space" before any has written.  If %TEMP% can't hold the
     * batch but the archive drive can, route all threads to the archive dir.
     */
    total_batch = 0.0;
    for (i = 0; i < batch_n; i++) {
        total_batch += (double)jobs[i].file_size.lo +
                       (double)jobs[i].file_size.hi * 4294967296.0;
    }
    if (tmp_fits_on_tmp_drive(total_batch)) {
        batch_use_sys_tmp = 1;
    } else if (tmp_fits_on_archive_drive(total_batch)) {
        batch_use_sys_tmp = 0; /* route to archive dir */
    } else {
        /* Neither drive has room; let open_tmp_file_win_at try anyway and
         * surface the error naturally when the write fails. */
        batch_use_sys_tmp = 0;
    }

#pragma omp parallel for schedule(dynamic) num_threads(batch_n)
    for (i = 0; i < batch_n; i++) {
        bp_archive_t worker_ar;
        FILE *src_fp;
        FILE *tmp_fp;
        bp_io_t src_io;
        int local_rc;
        char arcname_utf8[BP_MAX_PATH * 2];
        struct _stat64 st_chk;
        bp_uint64 actual_sz;

        memset(&worker_ar, 0, sizeof(worker_ar));
        worker_ar.block_size = ar->block_size;
        worker_ar.method = ar->method;
        worker_ar.method_level = ar->method_level;
        worker_ar.log_fn = ar->log_fn;
        worker_ar.log_data = ar->log_data;
        worker_ar.creator_os = ar->creator_os;
        worker_ar.creator_cpu = ar->creator_cpu;
        worker_ar.fast_fluff_threads = (batch_n == 1) ? total_threads : 0;
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
            if (_fstat64(_fileno(src_fp), &st_chk) == 0) {
                bp_uint64 real_sz = bp_off_to_u64((bp_off_t)st_chk.st_size);
                if (real_sz.lo != jobs[i].file_size.lo ||
                    real_sz.hi != jobs[i].file_size.hi) {
                    char was_buf[32];
                    char now_buf[32];
                    format_size64(jobs[i].file_size, was_buf, sizeof(was_buf));
                    format_size64(real_sz, now_buf, sizeof(now_buf));
                    fprintf(stderr,
                            "boltpack: warning: %s changed size during archive "
                            "(was %s, now %s) -- archiving current size\n",
                            jobs[i].path, was_buf, now_buf);
                    actual_sz = real_sz;
                }
            }
            tmp_fp = open_tmp_file_win_at(batch_use_sys_tmp);
            if (tmp_fp == NULL) {
                local_rc = BP_ERR_IO;
            }
        }
        if (local_rc == BP_OK) {
            bp_io_from_file(&src_io, src_fp);
            bp_io_from_file(&packed_jobs[i].tmp_io, tmp_fp);
            if (acp_to_utf8(jobs[i].arcname, arcname_utf8,
                            sizeof(arcname_utf8)) != 0) {
                local_rc = BP_ERR_INVALID_ARG;
            } else {
                local_rc = bp_archive_pack_entry(&worker_ar,
                                                 arcname_utf8,
                                                 BP_TYPE_DATA,
                                                 BP_OS_WINDOWS,
                                                 &src_io,
                                                 actual_sz,
                                                 jobs[i].mtime,
                                                 &packed_jobs[i].tmp_io,
                                                 &packed_jobs[i].packed);
            }
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

static int add_job_list_parallel(bp_archive_t *ar, const add_job_list_t *list,
                                 int total_threads)
{
    int i;
    int batch_n;
    int rc;

    i = 0;
    while (i < list->count) {
        if (list->items[i].entry_type == BP_TYPE_DATA) {
            batch_n = 0;
            while (i + batch_n < list->count &&
                   list->items[i + batch_n].entry_type == BP_TYPE_DATA &&
                   batch_n < total_threads) {
                batch_n++;
            }
            /*
             * When batch_n == 1 the single file gets all inner threads.
             * If the temp file for it won't fit on %TEMP%, avoid the
             * parallel path entirely: use the serial add_job path which
             * writes directly to the archive (no temp file, no extra I/O).
             * Inner fast-fluff threading is still applied via
             * ar->fast_fluff_threads which bp_archive_add_entry honours.
             */
            if (batch_n == 1) {
                double hint = (double)list->items[i].file_size.lo +
                              (double)list->items[i].file_size.hi * 4294967296.0;
                if (ar->method == BP_METHOD_BOLTLZ &&
                    hint < 4.0 * (double)ar->block_size) {
                    rc = add_job(ar, &list->items[i]);
                    if (rc != BP_OK) return rc;
                    i++;
                    continue;
                }
                if (!tmp_fits_on_tmp_drive(hint) && !tmp_fits_on_archive_drive(hint)) {
                    rc = add_job(ar, &list->items[i]);
                    if (rc != BP_OK) return rc;
                    i++;
                    continue;
                }
            }
            rc = add_data_job_batch(ar, &list->items[i], batch_n, total_threads);
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
#endif

static int add_job_list_with_policy(bp_archive_t *ar,
                                    const add_job_list_t *list,
                                    bp_uint8 method)
{
    bp_uint64 total_size;
    int data_count;
    int threads;

    summarize_data_jobs(list, &total_size, &data_count);
    threads = choose_fast_fluff_threads(total_size, data_count, method);
    if (threads > 1) {
        if (method == BP_METHOD_BOLTLZ)
            bp_set_fast_boltlz(ar, threads);
        else if (method == BP_METHOD_FLUFF)
            bp_set_fast_fluff(ar, threads);
#ifdef BOLTPACK_FAST_FLUFF
        return add_job_list_parallel(ar, list, threads);
#endif
    }

    return add_job_list_serial(ar, list);
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

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct _stat64 st;
    int rc;

    if (_stat64(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 2];

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
        if (acp_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", dir_arc);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry(ar, dir_arc_utf8, BP_TYPE_DIR, BP_OS_WINDOWS,
                                  NULL, bp_u64_from_u32(0),
                                  bp_off_to_u64((bp_off_t)st.st_mtime));
        if (rc != BP_OK) return rc;

        rc = add_win_fat_metadata(ar, dir_arc_utf8, path, BP_TYPE_DIR);
        if (rc != BP_OK) return rc;

        return add_directory(ar, path, dir_arc);

    } else if (S_ISREG(st.st_mode)) {
        FILE *fp;
        bp_io_t fio;
        bp_uint64 file_size = bp_off_to_u64((bp_off_t)st.st_size);
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
        if (acp_to_utf8(arcname, arcname_utf8, sizeof(arcname_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", arcname);
            fclose(fp);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry(ar, arcname_utf8, BP_TYPE_DATA, BP_OS_WINDOWS,
                                  &fio, file_size,
                                  bp_off_to_u64((bp_off_t)st.st_mtime));
        fclose(fp);

        if (rc != BP_OK) return rc;

        rc = add_win_fat_metadata(ar, arcname_utf8, path, BP_TYPE_DATA);
        if (rc != BP_OK) return rc;

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
                double pct = ((double)comp.lo + (double)comp.hi * 4294967296.0) /
                             ((double)file_size.lo + (double)file_size.hi * 4294967296.0) * 100.0;
                format_size64(comp, sizebuf, sizeof(sizebuf));
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
        if (utf8_to_acp(entry->name, disp_name, sizeof(disp_name)) != 0) {
            strncpy(disp_name, entry->name, BP_MAX_PATH - 1);
            disp_name[BP_MAX_PATH - 1] = '\0';
        }
        printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, disp_name);
    }
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct win_path_node_s {
    struct win_path_node_s *left;
    struct win_path_node_s *right;
    int height;
    wchar_t name[BP_MAX_PATH];
} win_path_node_t;

static int win_path_height(const win_path_node_t *node)
{
    return node == NULL ? 0 : node->height;
}

static void win_path_update_height(win_path_node_t *node)
{
    int left = win_path_height(node->left);
    int right = win_path_height(node->right);
    node->height = (left > right ? left : right) + 1;
}

static win_path_node_t *win_path_rotate_right(win_path_node_t *root)
{
    win_path_node_t *top = root->left;
    root->left = top->right;
    top->right = root;
    win_path_update_height(root);
    win_path_update_height(top);
    return top;
}

static win_path_node_t *win_path_rotate_left(win_path_node_t *root)
{
    win_path_node_t *top = root->right;
    root->right = top->left;
    top->left = root;
    win_path_update_height(root);
    win_path_update_height(top);
    return top;
}

static int win_path_compare(const wchar_t *left, const wchar_t *right)
{
    int rc = CompareStringOrdinal(left, -1, right, -1, TRUE);
    if (rc == CSTR_LESS_THAN) return -1;
    if (rc == CSTR_GREATER_THAN) return 1;
    return rc == CSTR_EQUAL ? 0 : 2;
}

static int win_path_compare_n(const wchar_t *left, size_t left_length,
                              const char *right, size_t right_length)
{
    wchar_t wide[16];
    size_t i;
    int rc;
    if (left_length != right_length || right_length >= 16U) return 1;
    for (i = 0; i < right_length; i++) wide[i] = (unsigned char)right[i];
    rc = CompareStringOrdinal(left, (int)left_length, wide,
                              (int)right_length, TRUE);
    if (rc == CSTR_EQUAL) return 0;
    return rc == CSTR_LESS_THAN ? -1 : 1;
}

static win_path_node_t *win_path_insert(win_path_node_t *root,
                                        const wchar_t *name,
                                        int *result)
{
    int cmp, balance;
    win_path_node_t *child;
    if (root == NULL) {
        root = (win_path_node_t *)calloc(1, sizeof(*root));
        if (root == NULL) { *result = BP_ERR_NOMEM; return NULL; }
        wcscpy(root->name, name);
        root->height = 1;
        *result = BP_OK;
        return root;
    }
    cmp = win_path_compare(name, root->name);
    if (cmp == 2) { *result = BP_ERR_INVALID_ARG; return root; }
    if (cmp == 0) { *result = BP_ERR_PARTIAL; return root; }
    if (cmp < 0) {
        child = win_path_insert(root->left, name, result);
        if (*result != BP_OK) return root;
        root->left = child;
    } else {
        child = win_path_insert(root->right, name, result);
        if (*result != BP_OK) return root;
        root->right = child;
    }
    win_path_update_height(root);
    balance = win_path_height(root->left) - win_path_height(root->right);
    if (balance > 1) {
        if (win_path_compare(name, root->left->name) > 0)
            root->left = win_path_rotate_left(root->left);
        return win_path_rotate_right(root);
    }
    if (balance < -1) {
        if (win_path_compare(name, root->right->name) < 0)
            root->right = win_path_rotate_right(root->right);
        return win_path_rotate_left(root);
    }
    return root;
}

static void win_path_free(win_path_node_t *root)
{
    if (root == NULL) return;
    win_path_free(root->left);
    win_path_free(root->right);
    free(root);
}

static int win_component_is_device(const wchar_t *component, size_t length)
{
    size_t base = 0;
    size_t i;
    static const char *const devices[] = {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"
    };
    while (base < length && component[base] != L'.') base++;
    while (base > 0 && (component[base - 1] == L' ' ||
                        component[base - 1] == L'.')) base--;
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        size_t n = strlen(devices[i]);
        if (base == n && win_path_compare_n(component, base, devices[i], n) == 0)
            return 1;
    }
    if (base == 4 &&
        (win_path_compare_n(component, 3, "COM", 3) == 0 ||
         win_path_compare_n(component, 3, "LPT", 3) == 0) &&
        component[3] >= L'1' && component[3] <= L'9') return 1;
    return 0;
}

static int win_project_windows_path(const char *local_name, wchar_t *key,
                                    int key_capacity)
{
    int length, i, start;
    length = MultiByteToWideChar(CP_ACP, 0, local_name, -1,
                                 key, key_capacity);
    if (length <= 0) return BP_ERR_INVALID_ARG;
    length--;
    for (i = 0; i < length; i++) {
        if (key[i] == L'/') key[i] = L'\\';
        if (key[i] == L':' || key[i] == L'<' || key[i] == L'>' ||
            key[i] == L'"' || key[i] == L'|' || key[i] == L'?' ||
            key[i] == L'*' || key[i] < 32) return BP_ERR_INVALID_ARG;
    }
    while (length > 0 && key[length - 1] == L'\\') length--;
    if (length == 0) return BP_ERR_INVALID_ARG;
    key[length] = L'\0';
    start = 0;
    for (i = 0; i <= length; i++) {
        if (key[i] == L'\\' || key[i] == L'\0') {
            int component_length = i - start;
            if (component_length <= 0 || key[i - 1] == L'.' ||
                key[i - 1] == L' ' ||
                (component_length == 1 && key[start] == L'.') ||
                win_component_is_device(key + start,
                                        (size_t)component_length))
                return BP_ERR_INVALID_ARG;
            start = i + 1;
        }
    }
    return BP_OK;
}

typedef struct {
    const char *out_dir;    /* -o PATH or NULL */
    int         force;      /* -f flag */
    int         dry_run;    /* -n flag */
    bp_uint32   file_count;
    bp_uint32   dir_count;
    bp_uint32   unsafe_count;  /* entries skipped for unsafe names */
    int         slash_warned;  /* printed the leading-'/' warning once */
    struct win_dir_attr_s *directories;
    win_path_node_t *projected_names;
    char        last_subject_path[BP_MAX_PATH];
    int         last_subject_valid;
    int         last_subject_ok;
} extract_ctx_t;

static int win_register_output_path(extract_ctx_t *ctx, const char *local_name)
{
    wchar_t key[BP_MAX_PATH];
    int rc = win_project_windows_path(local_name, key, BP_MAX_PATH);
    if (rc != BP_OK) return rc;
    ctx->projected_names = win_path_insert(ctx->projected_names, key, &rc);
    return rc;
}

typedef struct {
    bp_io_t io;
    bp_uint8 data[BP_META_MAX_SIZE];
    unsigned length;
} win_metadata_sink_t;

typedef struct win_dir_attr_s {
    struct win_dir_attr_s *next;
    bp_uint8 valid;
    bp_uint8 value;
    bp_uint64 mtime;
    int mtime_valid;
    char path[BP_MAX_PATH];
} win_dir_attr_t;

typedef struct {
    bp_io_t io;
    char target[BP_MAX_PATH];
    char temporary[BP_MAX_PATH];
    int force;
} win_staged_file_t;

static long win_metadata_write(void *handle, const void *data, bp_uint32 size)
{
    win_metadata_sink_t *sink = (win_metadata_sink_t *)handle;
    if (size > BP_META_MAX_SIZE - sink->length) return -1;
    memcpy(sink->data + sink->length, data, size);
    sink->length += (unsigned)size;
    return (long)size;
}

static int win_apply_fat_attributes(const char *path, bp_uint8 valid,
                                    bp_uint8 value);

static LONG g_extract_temp_counter = 0;

static int win_open_staged_file(const char *target, char *temporary,
                                size_t temporary_size, FILE **file)
{
    char directory[BP_MAX_PATH];
    char *slash1, *slash2, *slash;
    size_t length;
    unsigned attempt;
    HANDLE handle;
    int fd;
    FILE *fp;
    if (strlen(target) >= sizeof(directory) || temporary_size < BP_MAX_PATH)
        return BP_ERR_NAME_TOO_LONG;
    strcpy(directory, target);
    slash1 = strrchr(directory, '\\');
    slash2 = strrchr(directory, '/');
    slash = slash1;
    if (slash2 != NULL && (slash == NULL || slash2 > slash)) slash = slash2;
    if (slash == NULL) strcpy(directory, ".");
    else {
        length = (size_t)(slash - directory);
        if (length == 0) length = 1;
        else if (length == 2 && directory[1] == ':') length = 3;
        directory[length] = '\0';
    }
    for (attempt = 0; attempt < 256U; attempt++) {
        unsigned long serial = (unsigned long)GetTickCount() ^
            ((unsigned long)GetCurrentProcessId() << 16) ^
            (unsigned long)InterlockedIncrement(&g_extract_temp_counter) ^ attempt;
        int written = _snprintf(temporary, temporary_size,
                                "%s\\bp%08lx.tmp", directory, serial);
        if (written < 0 || (size_t)written >= temporary_size) return BP_ERR_NAME_TOO_LONG;
        handle = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return BP_ERR_IO;
        }
        fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_WRONLY);
        if (fd < 0) { CloseHandle(handle); DeleteFileA(temporary); return BP_ERR_IO; }
        fp = _fdopen(fd, "wb");
        if (fp == NULL) { _close(fd); DeleteFileA(temporary); return BP_ERR_IO; }
        *file = fp;
        return BP_OK;
    }
    return BP_ERR_IO;
}

static int win_queue_directory_attributes(extract_ctx_t *ctx,
                                          const char *path,
                                          bp_uint8 valid, bp_uint8 value)
{
    win_dir_attr_t *node, **at;
    size_t length = strlen(path);
    if (length >= BP_MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    for (node = ctx->directories; node != NULL; node = node->next) {
        if (strcmp(node->path, path) == 0) {
            node->valid = valid;
            node->value = value;
            return BP_OK;
        }
    }
    node = (win_dir_attr_t *)calloc(1, sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->valid = valid; node->value = value;
    memcpy(node->path, path, length + 1);
    at = &ctx->directories;
    while (*at != NULL && strlen((*at)->path) >= length) at = &(*at)->next;
    node->next = *at; *at = node;
    return BP_OK;
}

static int win_queue_directory_time(extract_ctx_t *ctx, const char *path,
                                    bp_uint64 mtime, int valid)
{
    win_dir_attr_t *node, **at;
    size_t length = strlen(path);
    if (length >= BP_MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    for (node = ctx->directories; node != NULL; node = node->next) {
        if (strcmp(node->path, path) == 0) {
            node->mtime = mtime;
            node->mtime_valid = valid;
            return BP_OK;
        }
    }
    node = (win_dir_attr_t *)calloc(1, sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    memcpy(node->path, path, length + 1);
    node->mtime = mtime;
    node->mtime_valid = valid;
    at = &ctx->directories;
    while (*at != NULL && strlen((*at)->path) >= length) at = &(*at)->next;
    node->next = *at; *at = node;
    return BP_OK;
}

static int win_finish_directory_attributes(extract_ctx_t *ctx, int apply)
{
    win_dir_attr_t *node = ctx->directories;
    int result = BP_OK;
    ctx->directories = NULL;
    while (node != NULL) {
        win_dir_attr_t *next = node->next;
        if (apply) {
            if (node->mtime_valid) {
                bp_off_t seconds = bp_u64_to_off(node->mtime);
                time_t t = (time_t)seconds;
                struct _stat64 st;
                struct utimbuf ut;
                if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), node->mtime) != 0 ||
                    (bp_off_t)t != seconds || _stat64(node->path, &st) != 0) {
                    result = BP_ERR_PARTIAL;
                } else {
                    ut.actime = (time_t)st.st_atime;
                    ut.modtime = t;
                    if (utime(node->path, &ut) != 0) result = BP_ERR_PARTIAL;
                }
                if (result != BP_OK)
                    fprintf(stderr, "boltpack: directory timestamp not restored: %s\n",
                            node->path);
            }
            if (node->valid != 0 &&
                win_apply_fat_attributes(node->path, node->valid,
                                         node->value) != BP_OK) {
                fprintf(stderr, "boltpack: directory FAT attributes not restored: %s\n",
                        node->path);
                result = BP_ERR_PARTIAL;
            }
        }
        free(node);
        node = next;
    }
    return result;
}

static int win_apply_fat_attributes(const char *path, bp_uint8 valid,
                                    bp_uint8 value)
{
    DWORD old_attributes = GetFileAttributesA(path);
    DWORD desired, actual;
    if (old_attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_PARTIAL;
    desired = (old_attributes & ~(DWORD)valid) | value;
    if (!SetFileAttributesA(path, desired)) return BP_ERR_PARTIAL;
    actual = GetFileAttributesA(path);
    if (actual == INVALID_FILE_ATTRIBUTES || (actual & valid) != value)
        return BP_ERR_PARTIAL;
    return BP_OK;
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
    char path[BP_MAX_PATH];
    char local_name[BP_MAX_PATH];
    FILE *fp;
    win_staged_file_t *staged;
    const char *name;
    int rc;

    /* Validate on the UTF-8 archive name before any codepage conversion:
     * a hostile archive must not choose where we write.  Rejects ".."
     * components, backslashes, and drive letters. */
    name = extract_entry_name(entry, ctx);
    if (!bp_name_is_safe(name)) {
        fprintf(stderr, "boltpack: skipping entry with unsafe name: %s\n",
                entry->name);
        ctx->unsafe_count++;
        return NULL;
    }

    /* Convert UTF-8 archive name to ACP for filesystem and display */
    if (utf8_to_acp(name, local_name, sizeof(local_name)) != 0) {
        fprintf(stderr, "boltpack: cannot represent archive name: %s\n", name);
        ctx->unsafe_count++;
        return NULL;
    }

    if (ctx->out_dir) {
        path_join(path, sizeof(path), ctx->out_dir, local_name);
    } else {
        strcpy(path, local_name);
    }

    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR) {
        ctx->last_subject_valid = 0;
        ctx->last_subject_ok = 0;
        rc = win_register_output_path(ctx, local_name);
        if (rc != BP_OK) {
            if (rc == BP_ERR_PARTIAL)
                fprintf(stderr, "boltpack: destination name collision: %s\n", name);
            else
                fprintf(stderr, "boltpack: unsupported Windows filename: %s\n", name);
            ctx->unsafe_count++;
            return NULL;
        }
        if (strlen(path) >= sizeof(ctx->last_subject_path)) {
            ctx->unsafe_count++;
            return NULL;
        }
        strcpy(ctx->last_subject_path, path);
        ctx->last_subject_valid = 1;
        ctx->last_subject_ok = 0;
    }

    if (entry->entry_type == BP_TYPE_META) {
        win_metadata_sink_t *sink;
        if (entry->platform != BP_META_PORTABLE) {
            ctx->unsafe_count++;
            return NULL;
        }
        if (!ctx->last_subject_valid || !ctx->last_subject_ok ||
            strcmp(ctx->last_subject_path, path) != 0) {
            ctx->unsafe_count++;
            return NULL;
        }
        sink = (win_metadata_sink_t *)calloc(1, sizeof(*sink));
        if (sink == NULL) { ctx->unsafe_count++; return NULL; }
        sink->io.read = NULL; sink->io.write = win_metadata_write;
        sink->io.seek = NULL; sink->io.tell = NULL; sink->io.handle = sink;
        return &sink->io;
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", local_name);
        }
        if (ctx->dry_run) ctx->last_subject_ok = 1;
        else {
            DWORD attrs;
            if (mkdirs(path) != 0) ctx->unsafe_count++;
            else {
                attrs = GetFileAttributesA(path);
                if (attrs == INVALID_FILE_ATTRIBUTES ||
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                    (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) ctx->unsafe_count++;
                else if (win_queue_directory_time(ctx, path, entry->mtime,
                                (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0) != BP_OK)
                    ctx->unsafe_count++;
                else ctx->last_subject_ok = 1;
            }
        }
        return NULL;
    }

    /* skip non-data entries not supported on DOS */
    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_META) {
        if (g_verbosity == VERB_DEFAULT) {
            printf("   skipping: %s [%s - not supported on this platform]\n",
                   local_name,
                   entry->entry_type == BP_TYPE_RSRC ? "resource fork"
                                                     : "metadata");
        }
        ctx->unsafe_count++;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity == VERB_DEFAULT) {
            printf("   skipping: %s [symlink - not supported on DOS]\n",
                   local_name);
        }
        ctx->unsafe_count++;
        return NULL;
    }

    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT) {
        printf(" extracting: %s\n", local_name);
    }

    if (ctx->dry_run) {
        ctx->last_subject_ok = 1;
        return NULL;
    }

    /* check for existing file */
    if (!ctx->force) {
        struct _stat64 st_buf;
        if (_stat64(path, &st_buf) == 0) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n", path);
            ctx->unsafe_count++;
            return NULL;
        }
    }

    mkdirs_for_file(path);
    staged = (win_staged_file_t *)calloc(1, sizeof(*staged));
    if (staged == NULL) { ctx->unsafe_count++; return NULL; }
    rc = win_open_staged_file(path, staged->temporary,
                              sizeof(staged->temporary), &fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot create: %s\n", path);
        free(staged);
        ctx->unsafe_count++;
        return NULL;
    }
    strcpy(staged->target, path);
    staged->force = ctx->force;
    bp_io_from_file(&staged->io, fp);
    return &staged->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH];
    char local_name[BP_MAX_PATH];
    const char *name = extract_entry_name(entry, ctx);
    int result = BP_OK;

    if (entry->entry_type == BP_TYPE_META) {
        win_metadata_sink_t *sink = (win_metadata_sink_t *)out;
        bp_portable_metadata_t parsed;
        int parse_rc;
        if (status != BP_OK) { free(sink); return status; }
        parse_rc = bp_metadata_parse(sink->data, sink->length, &parsed);
        if (parse_rc == BP_ERR_CORRUPT) { free(sink); return parse_rc; }
        if (utf8_to_acp(name, local_name, sizeof(local_name)) != 0) {
            free(sink); return BP_ERR_PARTIAL;
        }
        if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, local_name);
        else strcpy(path, local_name);
        if (parsed.has_fat && !ctx->dry_run) {
            if (parsed.subject_type == BP_TYPE_DIR)
                result = win_queue_directory_attributes(ctx, path,
                                                        parsed.fat_valid,
                                                        parsed.fat_value);
            else
                result = win_apply_fat_attributes(path, parsed.fat_valid,
                                                  parsed.fat_value);
            if (result != BP_OK)
                fprintf(stderr, "boltpack: FAT attributes not restored: %s\n", path);
        }
        free(sink);
        if ((parse_rc == BP_ERR_PARTIAL || parsed.has_unknown) && result == BP_OK)
            result = BP_ERR_PARTIAL;
        return result;
    }

    {
        win_staged_file_t *staged = (win_staged_file_t *)out;
        FILE *fp = (FILE *)out->handle;
        if (utf8_to_acp(name, local_name, sizeof(local_name)) != 0) {
            fclose(fp); DeleteFileA(staged->temporary); free(staged);
            return BP_ERR_PARTIAL;
        }
        if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, local_name);
        else strcpy(path, local_name);
        if (fclose(fp) != 0 && status == BP_OK) status = BP_ERR_IO;
        if (status != BP_OK) {
            DeleteFileA(staged->temporary);
            free(staged);
            return status;
        }

        if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
            bp_off_t seconds = bp_u64_to_off(entry->mtime);
            time_t t = (time_t)seconds;
            struct _stat64 st;
            struct utimbuf ut;
            if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), entry->mtime) != 0 ||
                (bp_off_t)t != seconds || _stat64(staged->temporary, &st) != 0) {
                fprintf(stderr, "boltpack: cannot restore timestamp: %s\n", path);
                result = BP_ERR_PARTIAL;
            } else {
                ut.actime = (time_t)st.st_atime;
                ut.modtime = t;
                if (utime(staged->temporary, &ut) != 0) {
                    fprintf(stderr, "boltpack: cannot restore timestamp: %s\n", path);
                    result = BP_ERR_PARTIAL;
                }
            }
        }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) result = BP_ERR_PARTIAL;
        if (!MoveFileExA(staged->temporary, path,
                         MOVEFILE_WRITE_THROUGH |
                         (staged->force ? MOVEFILE_REPLACE_EXISTING : 0))) {
            fprintf(stderr, "boltpack: cannot commit extracted file: %s\n", path);
            DeleteFileA(staged->temporary);
            free(staged);
            return BP_ERR_IO;
        }
        ctx->last_subject_ok = 1;
        free(staged);
    }
    return result;
}

#ifdef BOLTPACK_NET
/* Extract an already-open archive (received over the network) using this
 * frontend's own extract callbacks.  Declared in bp_net.h; called by the
 * shared network glue in bp_net_cli.h.  Path names have already been
 * safety-checked by the caller. */
int bp_frontend_extract_io(bp_io_t *aio, const char *out_dir, int force)
{
    extract_ctx_t ctx;

    if (out_dir && out_dir[0]) {
        mkdirs(out_dir);
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
    ctx.dry_run = 0;
    {
        int rc = bp_archive_extract_all_ex(aio, extract_open, extract_done,
                                           NULL, &ctx);
        int dir_rc = win_finish_directory_attributes(&ctx,
                              rc == BP_OK || rc == BP_ERR_PARTIAL);
        if (dir_rc != BP_OK && rc == BP_OK) rc = dir_rc;
        if (ctx.unsafe_count != 0 && rc == BP_OK) rc = BP_ERR_PARTIAL;
        win_path_free(ctx.projected_names);
        return rc;
    }
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
        char disp_name[BP_MAX_PATH];
        if (utf8_to_acp(entry->name, disp_name, sizeof(disp_name)) != 0) {
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

static void usage_basic(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver (Windows)\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.blt [files...]\n\n"
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
        "  --interchange-profile-1  Mini-compatible create/add limits\n"
        "                         (BoltLite or Stored, 16 KiB blocks)\n"
        "  -o PATH          Output directory for extraction\n"
        "  -f               Force overwrite existing files\n"
        "  -n               Dry run\n"
        "  -q / -qq         Quiet / silent\n");
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr, "  --single-thread  Disable automatic parallel compression\n");
#endif
    fprintf(stderr,
        "\nExamples:\n"
        "  boltpack create backup.blt folder\n"
        "  boltpack extract -o out backup.blt\n");
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr, "  boltpack create -F --single-thread backup.blt folder\n");
#endif
    fprintf(stderr,
        "\nFor advanced compression options, run: boltpack help advanced\n");
#ifdef BOLTPACK_NET
    bp_net_cli_usage(stderr);
#endif
}

static void usage_advanced(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- advanced Windows CLI help\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.blt [files...]\n\n"
        "Commands:\n"
        "  create (c)       Create archive from files\n"
        "  add (a)          Add files to existing archive\n"
        "  extract (x)      Extract archive\n"
        "  list (l)         List archive contents\n"
        "  test (t)         Test archive integrity\n"
        "  info (i)         Show archive information\n"
        "  delete (d)       Delete files from archive\n",
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
        "  -2               boltlz compression (default)\n"
        "  -F               Use fluff compression (defaults to 256k blocks)\n");
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
        "  -j N             Parallel BoltLZ and fluff"
#ifdef BOLTPACK_ZSTD
        " and zstd"
#endif
        "\n"
        "  -g N             Fluff blocks per history group\n"
        "  --single-thread  Disable automatic parallel compression\n");
#endif
    fprintf(stderr,
        "  --interchange-profile-1  Enforce mini-compatible limits (create/add)\n"
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
    char tmp_path[BP_MAX_PATH * 2];
    int fd;
    HANDLE h;

#ifdef BOLTPACK_NET
    /* --send/--name may appear among the file arguments; pull them out. */
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
     * MoveFileEx atomically on success.  This ensures a failed or interrupted
     * run never leaves a partial .blt file at the final path.
     */
    if (!GetTempFileNameA(g_tmp_dir[0] ? g_tmp_dir : ".", "bpa", 0, tmp_path)) {
        fprintf(stderr, "boltpack: cannot create temp archive\n");
        return 1;
    }
    h = CreateFileA(tmp_path,
                    GENERIC_READ | GENERIC_WRITE,
                    0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "boltpack: cannot open temp archive: %s\n", tmp_path);
        DeleteFileA(tmp_path);
        return 1;
    }
    fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
        fprintf(stderr, "boltpack: _open_osfhandle failed\n");
        CloseHandle(h);
        DeleteFileA(tmp_path);
        return 1;
    }
    fp = _fdopen(fd, "w+b");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: _fdopen failed\n");
        _close(fd);
        DeleteFileA(tmp_path);
        return 1;
    }

    /* Register global path so ctrl handler can delete it on Ctrl+C. */
    memcpy(g_tmp_archive, tmp_path, strlen(tmp_path) + 1);
    SetConsoleCtrlHandler(ctrl_cleanup_handler, TRUE);

    bp_io_from_file(&aio, fp);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        return 1;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);

    start = wall_time_seconds();

    rc = collect_arg_jobs(&list, argc - 1, argv + 1);
    if (rc != BP_OK) {
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
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
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        return 1;
    }

    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        return 1;
    }

    elapsed = wall_time_seconds() - start;

    /* Close the file so MoveFileEx can rename it, but keep ar alive: the
     * summary below reads ar.entry_list, which bp_archive_close frees. */
    fclose(fp);

    /* Atomically publish the completed archive. */
    if (!MoveFileExA(tmp_path, archive_name,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr, "boltpack: rename failed (error %lu)\n",
                (unsigned long)GetLastError());
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        bp_archive_close(&ar);
        return 1;
    }
    g_tmp_archive[0] = '\0'; /* rename succeeded -- nothing to clean up */
    SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);

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

#ifdef BOLTPACK_NET
    {
        int src = bp_net_cli_maybe_send(archive_name);
        if (src != 0) return src;
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

    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done,
                                   g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                   &ctx);
    fclose(fp);

    {
        int dir_rc = win_finish_directory_attributes(&ctx,
                              rc == BP_OK || rc == BP_ERR_PARTIAL);
        if (dir_rc != BP_OK && rc == BP_OK) rc = dir_rc;
    }
    win_path_free(ctx.projected_names);

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
                "boltpack: %lu entr%s skipped or attributes not restored\n",
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
        if (!bp_u64_is_zero(total_orig)) {
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
    char tmp_name[BP_MAX_PATH];
    bp_uint32 deleted = 0;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }

    archive_name = argv[0];
    del_names = (const char **)&argv[1];
    del_count = argc - 1;

    /* Archive names use forward slashes; normalize user input. */
    {
        int j;
        for (j = 0; j < del_count; j++)
            normalize_separators((char *)del_names[j]);
    }

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
                   bp_uint8 method, int level, int interchange_profile)
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
    HANDLE win_handle;
    unsigned char original_header[BP_HEADER_SIZE];

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
     * Refuse the append when the bounded backup cannot be made.
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
        if (fread(original_header, 1, BP_HEADER_SIZE, qfp) != BP_HEADER_SIZE) {
            fprintf(stderr, "boltpack: cannot back up archive header\n");
            fclose(qfp);
            return 1;
        }
        bp_io_from_file(&qio, qfp);
        rc = bp_archive_query_tail(&qio, &tail_off, &tail_sz);
        if (rc == BP_OK && tail_sz.hi == 0 &&
            tail_sz.lo <= 256U * 1024U * 1024U) {
            tail_bytes = (size_t)tail_sz.lo;
            tail_buf   = (unsigned char *)malloc(tail_bytes);
            if (tail_buf != NULL) {
                bp_off_t off = bp_u64_to_off(tail_off);
                if (_fseeki64(qfp, (__int64)off, SEEK_SET) != 0 ||
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
        if (tail_buf == NULL) {
            fprintf(stderr, "boltpack: cannot safely back up archive table for append\n");
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

    /* Arm ctrl handler so Ctrl+C restores the tail. */
    win_handle = INVALID_HANDLE_VALUE;
    if (tail_buf != NULL) {
        win_handle = (HANDLE)_get_osfhandle(_fileno(fp));
        if (win_handle == INVALID_HANDLE_VALUE) {
            free(tail_buf);
            fclose(fp);
            fprintf(stderr, "boltpack: cannot protect archive during append\n");
            return 1;
        } else {
            g_add_handle  = win_handle;
            g_add_tail_off = (__int64)bp_u64_to_off(tail_off);
            g_add_tail    = tail_buf;
            g_add_tail_sz = tail_bytes;
            memcpy(g_add_header, original_header, BP_HEADER_SIZE);
            SetConsoleCtrlHandler(ctrl_add_restore_handler, TRUE);
        }
    }

    bp_io_from_file(&io, fp);
    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto add_fail_restore_win;
    }
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            bp_archive_close(&ar);
            goto add_fail_restore_win;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);

    rc = collect_arg_jobs(&list, argc - 1, argv + 1);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error preparing input: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        goto add_fail_restore_win;
    }

    rc = add_job_list_with_policy(&ar, &list, method);
    free_job_list(&list);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error adding input: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        goto add_fail_restore_win;
    }

    fin_rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);

    if (fin_rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(fin_rc));
        goto add_fail_restore_win;
    }

    /* Disarm ctrl handler. */
    if (win_handle != INVALID_HANDLE_VALUE) {
        SetConsoleCtrlHandler(ctrl_add_restore_handler, FALSE);
        g_add_handle  = INVALID_HANDLE_VALUE;
        g_add_tail    = NULL;
        g_add_tail_sz = 0;
    }
    free(tail_buf);
    fclose(fp);

    if (g_verbosity < VERB_SILENT) {
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    }
    return 0;

add_fail_restore_win:
    /* Restore critical tail to leave the archive uncorrupted. */
    if (win_handle != INVALID_HANDLE_VALUE) {
        SetConsoleCtrlHandler(ctrl_add_restore_handler, FALSE);
        g_add_handle  = INVALID_HANDLE_VALUE;
        g_add_tail    = NULL;
        g_add_tail_sz = 0;
    }
    if (tail_buf != NULL) {
        if (fflush(fp) != 0 ||
            _fseeki64(fp, (__int64)bp_u64_to_off(tail_off), SEEK_SET) != 0 ||
            fwrite(tail_buf, 1, tail_bytes, fp) != tail_bytes ||
            _fseeki64(fp, 0, SEEK_SET) != 0 ||
            fwrite(original_header, 1, BP_HEADER_SIZE, fp) != BP_HEADER_SIZE ||
            fflush(fp) != 0 ||
            _chsize_s(_fileno(fp), (__int64)bp_u64_to_off(tail_off) + (__int64)tail_bytes) != 0) {
            fprintf(stderr,
                    "boltpack: warning: could not restore archive after error; "
                    "archive may be corrupt\n");
        }
        free(tail_buf);
    }
    fclose(fp);
    return 1;
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

    /* Match console output codepage to the ANSI codepage used by the
     * C runtime for file names, so that non-ASCII characters (e.g.
     * Swedish å ä ö) display correctly instead of being reinterpreted
     * through the OEM codepage (CP850/CP437). */
    SetConsoleOutputCP(GetACP());

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
#ifdef BOLTPACK_NET
    if (command == '\0') {
        command = bp_net_cli_command(argv[1]);   /* 'L' listen, 'S' scan */
    }
#endif
    if (command == '\0') {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]);
        usage();
        return 1;
    }

    /* listen/scan take no archive argument; everything else needs one. */
    if (argc < 3
#ifdef BOLTPACK_NET
        && command != 'L' && command != 'S'
#endif
       ) {
        usage();
        return 1;
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
            /* consumed a network option (--send/--name/--auto/...) */
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
        fprintf(stderr, "boltpack: -j requires an OpenMP-enabled Windows build\n");
        return 1;
    }
    if (g_fast_group_blocks > 0) {
        fprintf(stderr, "boltpack: -g requires a fast-fluff-enabled Windows build\n");
        return 1;
    }
#endif

#ifdef BOLTPACK_FAST_FLUFF
    if (g_fast_group_blocks > 0)
        fluff_fast_set_group_blocks(g_fast_group_blocks);
#endif

    switch (command) {
    case 'c':
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
#endif
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage();
        return 1;
    }
}
