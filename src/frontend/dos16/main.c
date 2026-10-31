/*
 * main.c -- DOS 16-bit CLI frontend for BoltPack mini
 *
 * Target: Open Watcom C 16-bit, compact memory model (-mc).
 * Compiler:  wcc -bt=dos -mc -0 -za -DSIZEOF_INT=2 -DSIZEOF_LONG=4
 *             -DBP_MINI
 *            -DBP_CREATOR_OS=BP_OS_DOS -DBP_CREATOR_CPU=BP_CPU_8086
 * Linker:    wlink system dos
 * Binary:    BPMINI.EXE
 *
 * Commands:
 *   c <archive> <files...>   create archive
 *   x <archive> [dest]       extract archive
 *   l <archive>              list archive contents
 *   t <archive>              test archive integrity
 *   i <archive>              show archive information
 *   r <archive> <backup>     restore a verified BK####.BAK if target is missing
 *
 * ANSI C (C89/C90) with Watcom-specific directory scanning.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/mini/archiver.h"
#include "../../core/mini/stack_probe.h"
#include "../mini_shared/mini_common.h"
#include "../mini_shared/dos_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BP_TEST_STACK_HIGHWATER
#include <malloc.h>
#include <dos.h>
#include <i86.h>
static unsigned int bp_test_stack_low = 65535U;
static const char *bp_test_stack_low_file;
static unsigned int bp_test_stack_low_line;
static unsigned long bp_test_dos_block_low = 0xffffffffUL;
#ifdef BP_TEST_ALLOC_ACCOUNTING
void bp_test_alloc_report(void);
#endif

static unsigned int bp_test_largest_dos_block_paras(void)
{
    union REGS inregs;
    union REGS outregs;

    /* DOS returns its largest free block in BX when this request is too big. */
    memset(&inregs, 0, sizeof(inregs));
    inregs.x.ax = 0x4800U;
    inregs.x.bx = 0xffffU;
    intdos(&inregs, &outregs);
    if ((outregs.x.cflag & 1U) == 0 || outregs.x.ax != 8U)
        return 0;
    return outregs.x.bx;
}

void bp_test_stack_probe(const char *source_file, unsigned int source_line)
{
    unsigned int available = stackavail();
    unsigned int block_paras = bp_test_largest_dos_block_paras();
    unsigned long block_bytes = (unsigned long)block_paras * 16UL;
    if (available < bp_test_stack_low) {
        bp_test_stack_low = available;
        bp_test_stack_low_file = source_file;
        bp_test_stack_low_line = source_line;
    }
    if (block_paras != 0 && block_bytes < bp_test_dos_block_low)
        bp_test_dos_block_low = block_bytes;
}

static void bp_test_stack_report(void)
{
    printf("BP_STACK_LOW_WATER=%u FILE=%s LINE=%u\n", bp_test_stack_low,
           bp_test_stack_low_file, bp_test_stack_low_line);
    if (bp_test_dos_block_low != 0xffffffffUL)
        printf("BP_DOS_LARGEST_BLOCK_LOW_BYTES=%lu\n", bp_test_dos_block_low);
    else
        printf("BP_DOS_LARGEST_BLOCK_LOW_BYTES=unknown\n");
#ifdef BP_TEST_ALLOC_ACCOUNTING
    bp_test_alloc_report();
#endif
}
#endif
#include <errno.h>

#ifndef __WATCOMC__
#include <time.h>
#include <sys/stat.h>
#include <utime.h>
#endif
#ifdef BP_TEST_ALLOC_CLI
#include <stdio.h>
void bp_test_alloc_fail_size_after(size_t size, long matching_allocations);
long bp_test_alloc_live_count(void);
static int g_test_alloc_enabled;
static long g_test_alloc_index;
static int g_test_last_error;
#endif

#ifdef __WATCOMC__
#  include <direct.h>  /* mkdir */
#endif

/* ---- constants --------------------------------------------------------- */

#define BP_MAX_PATH   260
#define PROG_NAME     "bpmini"
#define BP_XSTR(x)    BP_STR(x)
#define BP_STR(x)     #x
#define BP_VER        BP_XSTR(BP_VERSION_MAJOR) "." BP_XSTR(BP_VERSION_MINOR)

/* ---- globals ----------------------------------------------------------- */

static int g_verbose = 1;
#ifdef __WATCOMC__
static int g_time_partial = 0;
#endif

/* ---- platform helpers -------------------------------------------------- */

static void make_parents(const char *path)
{
    char buf[BP_MAX_PATH];
    char *p;
    int  n;

    n = (int)strlen(path);
    if (n <= 0 || n >= BP_MAX_PATH) return;
    memcpy(buf, path, (size_t)n + 1);

    for (p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
#ifdef __WATCOMC__
            mkdir(buf);
#else
            (void)buf;
#endif
            *p = saved;
        }
    }
}

#ifndef __WATCOMC__
static int set_mtime(const char *path, bp_uint64 mtime)
{
    struct stat st;
    struct utimbuf ut;
    if (mtime.hi != 0) return -1;
    if (stat(path, &st) != 0) return -1;
    ut.actime  = st.st_atime;
    ut.modtime = (time_t)mtime.lo;
    if (ut.modtime < (time_t)0 ||
        (unsigned long)ut.modtime != (unsigned long)mtime.lo)
        return -1;
    return utime(path, &ut);
}
#endif

/* ---- path join (Watcom only) ------------------------------------------ */

#ifdef __WATCOMC__
static void join_sep(char *dst, int sz, const char *base, char sep,
                     const char *name)
{
    int bn = (int)strlen(base);
    int nn = (int)strlen(name);
    if (base[0] && bn + 1 + nn < sz) {
        memcpy(dst, base, (size_t)bn);
        dst[bn] = sep;
        memcpy(dst + bn + 1, name, (size_t)(nn + 1));
    } else if (nn < sz) {
        memcpy(dst, name, (size_t)(nn + 1));
    } else {
        dst[0] = '\0';
    }
}

static void bp_wpad(int fd, const char *s, int width)
{
    static const char sp[13] = "            ";
    int n = (int)strlen(s);
    write(fd, s, (unsigned)n);
    if (n < width) write(fd, sp, (unsigned)(width - n));
}
#endif /* __WATCOMC__ */

/* ---- list callback ----------------------------------------------------- */

static int on_list(const bp_mini_entry_t *e, void *user_data)
{
    char size_buf[32];
    (void)user_data;
    format_size64_mini(e->original_size, size_buf);

#ifdef __WATCOMC__
    switch (e->entry_type) {
    case BP_TYPE_DIR:    write(1, "  D  ", 5); break;
    case BP_TYPE_SYMLINK: write(1, "  L  ", 5); break;
    default:             write(1, "     ", 5); break;
    }
    bp_wpad(1, size_buf, 12);
    write(1, "  ", 2);
    print_utf8_safe(e->name, (unsigned int)e->name_len);
    write(1, "\n", 1);
#else
    switch (e->entry_type) {
    case BP_TYPE_DIR:    printf("  D  "); break;
    case BP_TYPE_SYMLINK: printf("  L  "); break;
    default:             printf("     "); break;
    }
    printf("%-12s  ", size_buf);
    print_utf8_safe(e->name, (unsigned int)e->name_len);
    putchar('\n');
#endif
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct {
    char dest[BP_MAX_PATH];
    char current_path[BP_MAX_PATH];
    int name_error;
    bp_mini_directory_queue_t directories;
} extract_ctx_t;

static int on_extract_directory(const bp_mini_entry_t *e, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH], local_name[BP_MAX_PATH], *p;
    BP_STACK_PROBE();
    /* Validate archive UTF-8 before conversion. Local CP437 bytes may not be
     * valid UTF-8 and must not be passed back to the archive-name validator. */
    if (!bp_mini_name_is_safe(e->name) ||
        bp_mini_utf8_to_local(e->name, local_name, sizeof(local_name)) != 0 ||
        (ctx->dest[0] && strlen(ctx->dest) + strlen(local_name) + 2 >= sizeof(path))) {
        fprintf(stderr, PROG_NAME ": cannot represent archive directory: %s\n", e->name);
        ctx->name_error = 1;
        return BP_ERR_PARTIAL;
    }
#ifdef __WATCOMC__
    if (g_verbose) {
        write(1, "  d  ", 5);
        print_utf8_safe(e->name, (unsigned int)e->name_len);
        write(1, "\n", 1);
    }
#else
    if (g_verbose) {
        printf("  d  ");
        print_utf8_safe(e->name, (unsigned int)e->name_len);
        putchar('\n');
    }
#endif
#ifdef __WATCOMC__
    join_sep(path, BP_MAX_PATH, ctx->dest, '\\', local_name);
#else
    if (ctx->dest[0]) snprintf(path, BP_MAX_PATH, "%s\\%s", ctx->dest, local_name);
    else snprintf(path, BP_MAX_PATH, "%s", local_name);
#endif
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    /* DOS path APIs may reject the trailing slash used to mark archive dirs. */
    if (p > path && p[-1] == '\\') p[-1] = '\0';
    make_parents(path);
#ifdef __WATCOMC__
    {
        unsigned attr;
        (void)mkdir(path);
        if (_dos_getfileattr(path, &attr) != 0 || !(attr & _A_SUBDIR)) {
            fprintf(stderr, PROG_NAME ": cannot create directory: %s\n", path);
            return BP_ERR_IO;
        }
    }
#else
    return BP_ERR_PARTIAL;
#endif
    strcpy(ctx->current_path, path);
    return bp_mini_directory_add(&ctx->directories, path, e);
}

static bp_io_t *on_extract_open(const bp_mini_entry_t *e, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH];
    char local_name[BP_MAX_PATH];
    static bp_io_t io;
    BP_STACK_PROBE();

    /* Validate archive UTF-8 before conversion; local CP437 bytes are opaque. */
    if (!bp_mini_name_is_safe(e->name) ||
        bp_mini_utf8_to_local(e->name, local_name, sizeof(local_name)) != 0) {
        fprintf(stderr, PROG_NAME ": cannot represent archive name: %s\n", e->name);
        ctx->name_error = 1;
        return NULL;
    }
    if (ctx->dest[0] && strlen(ctx->dest) + strlen(local_name) + 2 >= sizeof(path)) {
        ctx->name_error = 1;
        return NULL;
    }

#ifdef __WATCOMC__
    join_sep(path, BP_MAX_PATH, ctx->dest, '\\', local_name);
#else
    if (ctx->dest[0]) {
        snprintf(path, BP_MAX_PATH, "%s\\%s", ctx->dest, local_name);
    } else {
        snprintf(path, BP_MAX_PATH, "%s", local_name);
    }
#endif
    {
        char *p;
        for (p = path; *p; p++) if (*p == '/') *p = '\\';
    }
    make_parents(path);

    if (g_verbose) {
#ifdef __WATCOMC__
        write(1, "  x  ", 5);
        print_utf8_safe(e->name, (unsigned int)e->name_len);
        write(1, "\n", 1);
#else
        printf("  x  ");
        print_utf8_safe(e->name, (unsigned int)e->name_len);
        putchar('\n');
#endif
    }

#ifdef __WATCOMC__
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0);
        if (fd < 0) {
            bp_wstr(2, PROG_NAME ": cannot create ");
            bp_wstr(2, path); write(2, "\n", 1);
            return NULL;
        }
        bp_io_from_handle(&io, fd);
    }
#else
    {
        struct stat st;
        FILE *fp;
        if (stat(path, &st) == 0) {
            fprintf(stderr, PROG_NAME ": file already exists: %s\n", path);
            return NULL;
        }
        fp = fopen(path, "wb");
        if (fp == NULL) {
            fprintf(stderr, PROG_NAME ": cannot create %s\n", path);
            return NULL;
        }
        bp_io_from_file(&io, fp);
    }
#endif
    strcpy(ctx->current_path, path);
    return &io;
}

static int on_extract_done(const bp_mini_entry_t *e, bp_io_t *out,
                           int status, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    int rc = BP_OK;
#ifdef __WATCOMC__
    int fd;
    unsigned dos_date;
    unsigned dos_time;
    unsigned actual_date;
    unsigned actual_time;
    long tz_offset;
    int time_result;
    BP_STACK_PROBE();
    if (out == NULL) return BP_ERR_INVALID_ARG;
    fd = (int)(long)out->handle;
    if (close(fd) != 0) {
        (void)remove(ctx->current_path);
        return BP_ERR_IO;
    }
    if (status != BP_OK) {
        (void)remove(ctx->current_path);
        return BP_OK;
    }
    if (e->flags & BP_ENTRY_FLAG_MTIME_VALID) {
        time_result = bp_dos_timezone(&tz_offset) == 0 ?
                      bp_dos_utc_to_local(e->mtime, tz_offset, &dos_date, &dos_time) : -1;
        if (time_result < 0) {
            bp_wstr(2, PROG_NAME ": cannot restore time (set BP_TZ_OFFSET): ");
            bp_wstr(2, ctx->current_path); write(2, "\n", 1);
            rc = BP_ERR_PARTIAL;
        } else {
            int time_fd = open(ctx->current_path, O_RDWR | O_BINARY);
            if (time_fd < 0 || _dos_setftime(time_fd, dos_date, dos_time) != 0) {
                bp_wstr(2, PROG_NAME ": cannot restore time: ");
                bp_wstr(2, ctx->current_path); write(2, "\n", 1);
                rc = BP_ERR_PARTIAL;
            } else if (_dos_getftime(time_fd, &actual_date, &actual_time) != 0 ||
                       actual_date != dos_date || actual_time != dos_time) {
                bp_wstr(2, PROG_NAME ": filesystem did not retain time: ");
                bp_wstr(2, ctx->current_path); write(2, "\n", 1);
                rc = BP_ERR_PARTIAL;
            }
            if (time_fd >= 0 && close(time_fd) != 0) rc = BP_ERR_PARTIAL;
        }
        if (rc == BP_OK && time_result > 0) {
            bp_wstr(2, PROG_NAME ": time rounded to two seconds: ");
            bp_wstr(2, ctx->current_path); write(2, "\n", 1);
            rc = BP_ERR_PARTIAL;
        }
    }
#else
    BP_STACK_PROBE();
    if (out == NULL) return BP_ERR_INVALID_ARG;
    if (fclose((FILE *)out->handle) != 0) {
        (void)remove(ctx->current_path);
        return BP_ERR_IO;
    }
    if (status != BP_OK) {
        (void)remove(ctx->current_path);
        return BP_OK;
    }
    if ((e->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
        set_mtime(ctx->current_path, e->mtime) != 0)
        rc = BP_ERR_PARTIAL;
#endif
    if (e->flags & BP_ENTRY_FLAG_MODE_VALID) {
        fprintf(stderr, PROG_NAME ": POSIX file mode not restored: %s\n", ctx->current_path);
        if (rc == BP_OK) rc = BP_ERR_PARTIAL;
    }
    return rc;
}

/* ---- test callback ----------------------------------------------------- */

static int on_extract_metadata(const bp_mini_entry_t *entry,
                               const bp_portable_metadata_t *metadata, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    BP_STACK_PROBE();
    (void)entry;
    if (!metadata->has_fat) return metadata->has_unknown ? BP_ERR_PARTIAL : BP_OK;
    if (metadata->subject_type == BP_TYPE_DIR) {
        int rc = bp_mini_directory_set_fat(&ctx->directories, ctx->current_path,
                                           metadata->fat_valid, metadata->fat_value);
        if (rc != BP_OK) return rc;
        return metadata->has_unknown ? BP_ERR_PARTIAL : BP_OK;
    }
#ifdef __WATCOMC__
    {
        unsigned old_attr, desired, actual;
        if (_dos_getfileattr(ctx->current_path, &old_attr) != 0) return BP_ERR_PARTIAL;
        desired = (old_attr & ~(unsigned)metadata->fat_valid) | metadata->fat_value;
        if (_dos_setfileattr(ctx->current_path, desired) != 0 ||
            _dos_getfileattr(ctx->current_path, &actual) != 0 ||
            (actual & metadata->fat_valid) != metadata->fat_value) {
            bp_wstr(2, PROG_NAME ": cannot restore FAT attributes: ");
            bp_wstr(2, ctx->current_path); write(2, "\n", 1);
            return BP_ERR_PARTIAL;
        }
    }
    return metadata->has_unknown ? BP_ERR_PARTIAL : BP_OK;
#else
    (void)ctx;
    return BP_ERR_PARTIAL;
#endif
}

static int finish_directory(const bp_mini_directory_t *directory, void *user_data)
{
    int result = BP_OK;
    (void)user_data;
    BP_STACK_PROBE();
    if (directory->flags & BP_ENTRY_FLAG_MODE_VALID) {
        fprintf(stderr, PROG_NAME ": POSIX directory mode not restored: %s\n", directory->path);
        result = BP_ERR_PARTIAL;
    }
#ifdef __WATCOMC__
    /* DOS handle-based file time calls cannot open directories. */
    if (directory->flags & BP_ENTRY_FLAG_MTIME_VALID) {
        fprintf(stderr, PROG_NAME ": directory time not restored: %s\n", directory->path);
        result = BP_ERR_PARTIAL;
    }
    if (directory->fat_valid) {
        unsigned old_attr, desired, actual;
        int attr_result = BP_OK;
        if (_dos_getfileattr(directory->path, &old_attr) != 0) attr_result = BP_ERR_PARTIAL;
        else {
            desired = (old_attr & ~(unsigned)directory->fat_valid) | directory->fat_value;
            if (_dos_setfileattr(directory->path, desired & BP_META_FAT_MASK) != 0 ||
                _dos_getfileattr(directory->path, &actual) != 0 ||
                (actual & directory->fat_valid) != directory->fat_value)
                attr_result = BP_ERR_PARTIAL;
        }
        if (attr_result != BP_OK) {
            fprintf(stderr, PROG_NAME ": directory attributes not fully restored: %s\n", directory->path);
            result = BP_ERR_PARTIAL;
        }
    }
#else
    if ((directory->flags & BP_ENTRY_FLAG_MTIME_VALID) || directory->fat_valid)
        result = BP_ERR_PARTIAL;
#endif
    return result;
}

static void on_test(const bp_mini_entry_t *e, int result, void *user_data)
{
    (void)user_data;
#ifdef __WATCOMC__
    if (result == BP_OK) {
        write(1, "  OK   ", 7);
    } else {
        write(1, "  FAIL ", 7);
    }
    print_utf8_safe(e->name, (unsigned int)e->name_len);
    if (result != BP_OK) {
        write(1, " (", 2);
        bp_wstr(1, bp_mini_strerror(result));
        write(1, ")\n", 2);
    } else {
        write(1, "\n", 1);
    }
#else
    if (result == BP_OK) {
        printf("  OK   "); print_utf8_safe(e->name, e->name_len); putchar('\n');
    } else {
        printf("  FAIL "); print_utf8_safe(e->name, e->name_len);
        printf(" (%s)\n", bp_mini_strerror(result));
    }
#endif
}

/* ---- cmd_create -------------------------------------------------------- */

#ifdef __WATCOMC__

static int add_path(bp_mini_archive_t *ar, const char *arc_prefix,
                    const char *fs_path, bp_uint8 platform,
                    unsigned int depth);

/* The 8 KiB DOS16 stack is qualified through 34 nested directory frames.
 * Reject a deeper tree before entering another recursive frame. */
#define BP_DOS16_SOURCE_MAX_DEPTH 34U

typedef struct {
    int         fd;
    bp_uint32   total;
    bp_uint32   done;
    const char *arc_name;
} prog_ctx_t;

static long prog_read(void *handle, void *buf, bp_uint32 count)
{
    prog_ctx_t *ctx;
    long n;
    unsigned int pct;

    ctx = (prog_ctx_t *)handle;
    n = (long)read(ctx->fd, buf, (unsigned)count);
    ctx->done += (bp_uint32)n;

    if (g_verbose && ctx->total > 0) {
        if (ctx->total >= 100UL) {
            pct = (unsigned int)(ctx->done / (ctx->total / 100UL));
        } else {
            pct = (unsigned int)((ctx->done * 100UL) / ctx->total);
        }
        if (pct > 99) pct = 99;
        write(1, "  a  ", 5); bp_wstr(1, ctx->arc_name);
        write(1, "  ", 2); bp_wuint3(1, pct); write(1, "%\r", 2);
    }
    return n;
}

static int add_one_file(bp_mini_archive_t *ar, const char *arc_name,
                        const char *fs_path, bp_uint8 platform)
{
    int fd;
    bp_uint64 fsize;
    bp_uint64 mtime;
    bp_io_t file_io;
    prog_ctx_t prog;
    unsigned dos_date;
    unsigned dos_time;
    long tz_offset;
    bp_uint16 time_flags;
    int rc;
    BP_STACK_PROBE();

    fd = open(fs_path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot open ");
        bp_wstr(2, fs_path); write(2, "\n", 1);
        return BP_ERR_IO;
    }

    fsize.lo = (bp_uint32)(unsigned long)_filelength(fd);
    fsize.hi = 0;

    time_flags = 0;
    mtime.lo = 0; mtime.hi = 0;
    if (bp_dos_timezone(&tz_offset) == 0 &&
        _dos_getftime(fd, &dos_date, &dos_time) == 0 &&
        bp_dos_local_to_utc(dos_date, dos_time, tz_offset, &mtime) == 0)
        time_flags = BP_ENTRY_FLAG_MTIME_VALID;
    else {
        mtime.lo = 0; mtime.hi = 0;
        g_time_partial = 1;
        bp_wstr(2, PROG_NAME ": unknown file time (set BP_TZ_OFFSET): ");
        bp_wstr(2, fs_path); write(2, "\n", 1);
    }

    if (g_verbose && fsize.lo > 0) {
        write(1, "  a  ", 5); bp_wstr(1, arc_name);
        write(1, "  ", 2); bp_wuint3(1, 0); write(1, "%\r", 2);
    }

    prog.fd       = fd;
    prog.total    = fsize.lo;
    prog.done     = 0;
    prog.arc_name = arc_name;
    file_io.read   = prog_read;
    file_io.write  = NULL;
    file_io.seek   = NULL;
    file_io.tell   = NULL;
    file_io.handle = &prog;

    rc = bp_mini_add_local_entry(ar, arc_name, BP_TYPE_DATA, platform,
                                 &file_io, fsize, mtime, time_flags);
    close(fd);

    if (g_verbose) {
        unsigned int pct;
        pct = 0;
        if (rc == BP_OK && ar->entries_written > 0) {
            bp_mini_entry_t *ent = &ar->entry_list[ar->entries_written - 1];
            if (ent->original_size.lo > 0 && ent->original_size.hi == 0) {
                unsigned long orig = (unsigned long)ent->original_size.lo;
                unsigned long comp = (unsigned long)ent->compressed_size.lo;
                if (orig >= 100UL) {
                    pct = (unsigned int)(comp / (orig / 100UL));
                } else {
                    pct = (unsigned int)((comp * 100UL) / orig);
                }
                if (pct > 100) pct = 100;
            }
        }
        write(1, "  a  ", 5); bp_wstr(1, arc_name);
        write(1, "  ", 2); bp_wuint3(1, pct); write(1, "%\n", 2);
    }

    if (rc == BP_OK) {
        unsigned attr;
        if (_dos_getfileattr(fs_path, &attr) != 0) {
            bp_wstr(2, PROG_NAME ": cannot read FAT attributes: ");
            bp_wstr(2, fs_path); write(2, "\n", 1);
            g_time_partial = 1;
        } else {
            rc = bp_mini_add_fat_metadata(ar, arc_name, BP_TYPE_DATA,
                                          BP_META_FAT_MASK, (bp_uint8)(attr & BP_META_FAT_MASK));
        }
    }
    return rc;
}

static int add_dir_entry(bp_mini_archive_t *ar, const char *arc_name,
                         const char *fs_path, bp_uint8 platform)
{
    bp_uint64 z, mtime;
    bp_uint16 flags = 0;
    struct find_t found;
    unsigned attr;
    long offset;
    int rc;
    BP_STACK_PROBE();
    z.lo = 0; z.hi = 0; mtime = z;
    if (bp_dos_timezone(&offset) == 0 &&
        _dos_findfirst(fs_path, _A_NORMAL | _A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_SUBDIR, &found) == 0 &&
        (found.attrib & _A_SUBDIR) &&
        bp_dos_local_to_utc(found.wr_date, found.wr_time, offset, &mtime) == 0)
        flags = BP_ENTRY_FLAG_MTIME_VALID;
    else {
        mtime = z;
        g_time_partial = 1;
        fprintf(stderr, PROG_NAME ": unknown directory time (set BP_TZ_OFFSET): %s\n", fs_path);
    }
    if (g_verbose) {
        write(1, "  a  ", 5); bp_wstr(1, arc_name); write(1, "/\n", 2);
    }
    rc = bp_mini_add_local_entry(ar, arc_name, BP_TYPE_DIR, platform, NULL, z, mtime, flags);
    if (rc != BP_OK) return rc;
    if (_dos_getfileattr(fs_path, &attr) != 0) {
        g_time_partial = 1;
        fprintf(stderr, PROG_NAME ": cannot read directory attributes: %s\n", fs_path);
        return BP_OK;
    }
    return bp_mini_add_fat_metadata(ar, arc_name, BP_TYPE_DIR,
                                    BP_META_FAT_MASK, (bp_uint8)(attr & BP_META_FAT_MASK));
}

static int add_path(bp_mini_archive_t *ar, const char *arc_prefix,
                    const char *fs_path, bp_uint8 platform,
                    unsigned int depth)
{
    struct find_t ff;
    bp_mini_path_buffers_t path_buffers;
    char *pattern;
    char *child_fs;
    char *child_arc;
    unsigned int ret;
    unsigned int pattern_capacity;
    unsigned int child_fs_capacity;
    unsigned int child_arc_capacity;
    unsigned attr;
    int rc;
    int plen;
    int alen;
    BP_STACK_PROBE();

    rc      = BP_OK;
    if (depth > BP_DOS16_SOURCE_MAX_DEPTH) return BP_ERR_NAME_TOO_LONG;
    if (_dos_getfileattr(fs_path, &attr) != 0) return BP_ERR_IO;
    if (!(attr & _A_SUBDIR)) return add_one_file(ar, arc_prefix, fs_path, platform);
    if (arc_prefix[0]) {
        rc = add_dir_entry(ar, arc_prefix, fs_path, platform);
        if (rc != BP_OK) return rc;
    }

    plen = (int)strlen(fs_path);
    alen = (int)strlen(arc_prefix);
    pattern_capacity = (unsigned int)plen + 5U;
    child_fs_capacity = (unsigned int)plen + 1U +
                        (unsigned int)sizeof(ff.name);
    child_arc_capacity = (unsigned int)alen + (alen != 0 ? 1U : 0U) +
                         (unsigned int)sizeof(ff.name);
    if (pattern_capacity > BP_MAX_PATH || child_fs_capacity > BP_MAX_PATH ||
        child_arc_capacity > BP_MAX_PATH)
        return BP_ERR_NAME_TOO_LONG;
#ifdef BP_TEST_ALLOC_CLI
    if (g_test_alloc_enabled) {
        if (g_test_alloc_index == 0)
            bp_test_alloc_fail_size_after(pattern_capacity, 0);
        else if (g_test_alloc_index == 1)
            bp_test_alloc_fail_size_after(child_fs_capacity, 0);
        else if (g_test_alloc_index == 2)
            bp_test_alloc_fail_size_after(child_arc_capacity,
                child_arc_capacity == child_fs_capacity ? 1 : 0);
    }
#endif
    rc = bp_mini_path_buffers_alloc_sized(&path_buffers, pattern_capacity,
                                          child_fs_capacity,
                                          child_arc_capacity);
    if (rc != BP_OK) return rc;
    pattern = path_buffers.pattern;
    child_fs = path_buffers.child_fs;
    child_arc = path_buffers.child_arc;

    /* Build search pattern: fs_path\*.* */
    memcpy(pattern, fs_path, (size_t)plen);
    memcpy(pattern + plen, "\\*.*", 5);

    ret = _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_HIDDEN |
                                  _A_SYSTEM | _A_SUBDIR, &ff);
    if (ret != 0) {
        rc = (ret == 2 || ret == 18) ? BP_OK : BP_ERR_IO;
        goto done;
    }

    do {
        int nn;
        if (strcmp(ff.name, ".") == 0 || strcmp(ff.name, "..") == 0) {
            continue;
        }

        join_sep(child_fs, (int)child_fs_capacity, fs_path, '\\', ff.name);

        nn = (int)strlen(ff.name);
        if (arc_prefix[0]) {
            join_sep(child_arc, (int)child_arc_capacity,
                     arc_prefix, '/', ff.name);
        } else if ((unsigned int)(nn + 1) <= child_arc_capacity) {
            memcpy(child_arc, ff.name, (size_t)(nn + 1));
        } else {
            child_arc[0] = '\0';
        }

        if (ff.attrib & _A_SUBDIR) {
            if (depth >= BP_DOS16_SOURCE_MAX_DEPTH)
                rc = BP_ERR_NAME_TOO_LONG;
            else
                rc = add_path(ar, child_arc, child_fs, platform, depth + 1U);
        } else {
            rc = add_one_file(ar, child_arc, child_fs, platform);
        }
        if (rc != BP_OK) break;

    } while (_dos_findnext(&ff) == 0);

done:
    bp_mini_path_buffers_free(&path_buffers);
    return rc;
}

#endif /* __WATCOMC__ */

#ifdef __WATCOMC__
static unsigned int g_archive_stage_id = 1U;

static int dos16_sibling_name(const char *target, char *out, size_t out_size,
                              const char *prefix, const char *extension,
                              unsigned int id)
{
    const char *slash = strrchr(target, '/');
    const char *bslash = strrchr(target, '\\');
    size_t parent_length, prefix_length, extension_length, i;
    unsigned int value = id % 10000U;
    if (bslash != NULL && (slash == NULL || bslash > slash)) slash = bslash;
    parent_length = slash != NULL ? (size_t)(slash - target + 1) : 0;
    prefix_length = strlen(prefix);
    extension_length = strlen(extension);
    if (parent_length + prefix_length + 4U + extension_length + 1U > out_size)
        return -1;
    if (parent_length != 0) memcpy(out, target, parent_length);
    memcpy(out + parent_length, prefix, prefix_length);
    for (i = 0; i < 4; i++) {
        out[parent_length + prefix_length + 3U - i] =
            (char)('0' + value % 10U);
        value /= 10U;
    }
    memcpy(out + parent_length + prefix_length + 4U, extension,
           extension_length + 1U);
    return 0;
}

static int dos16_open_archive_stage(const char *target, char *stage,
                                    size_t stage_size)
{
    unsigned int tries;
    for (tries = 0; tries < 10000U; tries++) {
        int fd;
        char backup[BP_MAX_PATH];
        unsigned int id = g_archive_stage_id++ % 10000U;
        if (dos16_sibling_name(target, stage, stage_size, "BPK", ".TMP",
                               id) != 0) return -1;
        if (dos16_sibling_name(target, backup, sizeof(backup), "BK", ".BAK",
                               id) != 0) return -1;
        if ((access(backup, 0) == 0) || (errno != ENOENT)) continue;
        fd = open(stage, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

static int dos16_stage_id(const char *path, unsigned int *id)
{
    const char *base = path;
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    unsigned int value = 0;
    unsigned int i;
    if (slash != NULL && (bslash == NULL || slash > bslash)) base = slash + 1;
    else if (bslash != NULL) base = bslash + 1;
    if (strlen(base) != 11U || base[0] != 'B' || base[1] != 'P' ||
        base[2] != 'K' || base[7] != '.' || base[8] != 'T' ||
        base[9] != 'M' || base[10] != 'P')
        return -1;
    for (i = 3; i < 7; i++) {
        if (base[i] < '0' || base[i] > '9') return -1;
        value = value * 10U + (unsigned int)(base[i] - '0');
    }
    *id = value;
    return 0;
}

static int dos16_backup_id(const char *path, unsigned int *id)
{
    const char *base = path;
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    unsigned int value = 0;
    unsigned int i;
    if (slash != NULL && (bslash == NULL || slash > bslash)) base = slash + 1;
    else if (bslash != NULL) base = bslash + 1;
    if (strlen(base) != 10U || base[0] != 'B' || base[1] != 'K' ||
        base[6] != '.' || base[7] != 'B' || base[8] != 'A' || base[9] != 'K')
        return -1;
    for (i = 2; i < 6; i++) {
        if (base[i] < '0' || base[i] > '9') return -1;
        value = value * 10U + (unsigned int)(base[i] - '0');
    }
    *id = value;
    return 0;
}

static int dos16_path_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static int dos16_publish_archive(const char *target, const char *stage,
                                 int original_exists,
                                 unsigned int original_attributes)
{
    char backup[BP_MAX_PATH];
    if (original_exists) {
        unsigned int stage_id;
        if (dos16_stage_id(stage, &stage_id) != 0 ||
            dos16_sibling_name(target, backup, sizeof(backup), "BK", ".BAK",
                               stage_id) != 0 || access(backup, 0) == 0 ||
            errno != ENOENT ||
            rename(target, backup) != 0) return BP_ERR_IO;
#ifdef BP_TEST_EXIT_AFTER_BACKUP
        exit(91);
#endif
        if (rename(stage, target) != 0) {
            if (rename(backup, target) != 0) {
                bp_wstr(2, PROG_NAME ": original archive remains at recovery path: ");
                bp_wstr(2, backup); write(2, "\n", 1);
            }
            return BP_ERR_IO;
        }
        original_attributes &= (_A_HIDDEN | _A_SYSTEM | _A_ARCH);
        if (_dos_setfileattr(target, original_attributes) != 0) {
            bp_wstr(2, PROG_NAME ": cannot preserve archive attributes: ");
            bp_wstr(2, target); write(2, "\n", 1);
            if (remove(target) != 0 || rename(backup, target) != 0) {
                bp_wstr(2, PROG_NAME ": previous archive remains at recovery path: ");
                bp_wstr(2, backup); write(2, "\n", 1);
            }
            return BP_ERR_PARTIAL;
        }
        if (remove(backup) != 0) {
            bp_wstr(2, PROG_NAME ": previous archive remains at recovery path: ");
            bp_wstr(2, backup); write(2, "\n", 1);
            return BP_ERR_PARTIAL;
        }
        return BP_OK;
    }
    return rename(stage, target) == 0 ? BP_OK : BP_ERR_IO;
}
#endif /* __WATCOMC__ */

static int cmd_create(const char *arc_path, int argc, char **argv,
                      bp_uint32 block_size, bp_uint8 method)
{
#ifdef __WATCOMC__
    int fd;
    int original_exists = 0;
    unsigned int original_attributes = 0;
    unsigned int target_attributes;
    char stage_path[BP_MAX_PATH];
#else
    FILE *fp;
#endif
    bp_io_t arc_io;
    bp_mini_archive_t ar;
    int rc;
    int i;
    BP_STACK_PROBE();

#ifdef __WATCOMC__
    if (access(arc_path, 0) == 0) {
        if (_dos_getfileattr(arc_path, &target_attributes) != 0 ||
            (target_attributes & (_A_SUBDIR | _A_RDONLY)) != 0) {
            bp_wstr(2, PROG_NAME ": cannot safely replace archive ");
            bp_wstr(2, arc_path); write(2, "\n", 1);
            return 1;
        }
        original_exists = 1;
        original_attributes = target_attributes &
                             (_A_HIDDEN | _A_SYSTEM | _A_ARCH);
    } else if (errno != ENOENT) {
        bp_wstr(2, PROG_NAME ": cannot inspect archive ");
        bp_wstr(2, arc_path); write(2, "\n", 1);
        return 1;
    }
    fd = dos16_open_archive_stage(arc_path, stage_path, sizeof(stage_path));
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot create staged archive beside ");
        bp_wstr(2, arc_path); write(2, "\n", 1);
        return 1;
    }
    bp_io_from_handle(&arc_io, fd);
#else
    fp = fopen(arc_path, "wb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot create %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);
#endif

    rc = bp_mini_compress_init();
    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": compress init failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
        close(fd);
        remove(stage_path);
#else
        fprintf(stderr, PROG_NAME ": compress init failed: %s\n",
                bp_mini_strerror(rc));
        fclose(fp);
#endif
        return 1;
    }

    rc = bp_mini_archive_create(&ar, &arc_io, block_size, method);
    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": create failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
        close(fd);
        remove(stage_path);
#else
        fprintf(stderr, PROG_NAME ": create failed: %s\n",
                bp_mini_strerror(rc));
        fclose(fp);
#endif
        return 1;
    }

    for (i = 0; i < argc; i++) {
        char arc_name[BP_MAX_PATH];
        char *p;

        strncpy(arc_name, argv[i], BP_MAX_PATH - 1);
        arc_name[BP_MAX_PATH - 1] = '\0';

        for (p = arc_name; *p; p++) if (*p == '\\') *p = '/';

        if (arc_name[0] == '.' && arc_name[1] == '/') {
            memmove(arc_name, arc_name + 2, strlen(arc_name) - 1);
        }

#ifdef __WATCOMC__
        rc = add_path(&ar, arc_name, argv[i], BP_OS_DOS, 1U);
#else
        fprintf(stderr, PROG_NAME ": directory scanning requires Watcom C\n");
        rc = BP_ERR_IO;
#endif
        if (rc != BP_OK) {
#ifdef BP_TEST_STACK_HIGHWATER
            printf("BP_TEST_ADD_ERROR=%d\n", rc);
#endif
#ifdef __WATCOMC__
            bp_wstr(2, PROG_NAME ": error adding "); bp_wstr(2, argv[i]);
            write(2, ": ", 2);
            bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
            fprintf(stderr, PROG_NAME ": error adding %s: %s\n",
                    argv[i], bp_mini_strerror(rc));
#endif
            break;
        }
    }

    if (rc == BP_OK) {
        rc = bp_mini_archive_finalize(&ar);
        if (rc != BP_OK) {
#ifdef __WATCOMC__
            bp_wstr(2, PROG_NAME ": finalize failed: ");
            bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
            fprintf(stderr, PROG_NAME ": finalize failed: %s\n",
                    bp_mini_strerror(rc));
#endif
        }
    }

    bp_mini_archive_close(&ar);
#ifdef __WATCOMC__
    if (close(fd) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc == BP_OK)
        rc = dos16_publish_archive(arc_path, stage_path, original_exists,
                                   original_attributes);
    if (rc != BP_OK) remove(stage_path);
#else
    fclose(fp);
#endif

    if (rc == BP_OK && g_verbose) {
#ifdef __WATCOMC__
        write(1, "Archive: ", 9); bp_wstr(1, arc_path);
        write(1, " (", 2);
        bp_wulong(1, (unsigned long)ar.entries_written);
        write(1, " entr", 5);
        bp_wstr(1, ar.entries_written == 1 ? "y" : "ies");
        write(1, ")\n", 2);
#else
        printf("Archive: %s (%u entr%s)\n", arc_path,
               (unsigned)ar.entries_written,
               ar.entries_written == 1 ? "y" : "ies");
#endif
    }
#ifdef BP_TEST_ALLOC_CLI
    g_test_last_error = rc;
#endif
#ifdef __WATCOMC__
    return (rc == BP_OK && !g_time_partial) ? 0 : 1;
#else
    return (rc == BP_OK) ? 0 : 1;
#endif
}

/* ---- cmd_extract ------------------------------------------------------- */

static int dos16_component_is_83(const char *component, size_t length)
{
    size_t stem = 0, ext = 0, i;
    char upper[5];
    if (length == 0 || component[length - 1U] == '.' ||
        component[length - 1U] == ' ') return 0;
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
        if (ch >= 0x80U) continue;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || strchr("$%'-_@~`!(){}^#&", ch)))
            return 0;
    }
    if (stem <= 4U) {
        for (i = 0; i < stem; i++) {
            unsigned char ch = (unsigned char)component[i];
            upper[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
        }
        upper[stem] = '\0';
        if (strcmp(upper, "CON") == 0 || strcmp(upper, "PRN") == 0 ||
            strcmp(upper, "AUX") == 0 || strcmp(upper, "NUL") == 0 ||
            (stem == 4U && upper[3] >= '1' && upper[3] <= '9' &&
             (memcmp(upper, "COM", 3) == 0 || memcmp(upper, "LPT", 3) == 0)))
            return 0;
    }
    return 1;
}

static int dos16_path_is_83(const char *path, bp_uint8 entry_type)
{
    const char *p = path;
    const char *start;
    const char *end;
    size_t length;
    if (path == NULL || *path == '\0') return 1;
    length = strlen(path);
    if (entry_type == BP_TYPE_DIR && length != 0 &&
        (path[length - 1U] == '/' || path[length - 1U] == '\\'))
        length--;
    end = path + length;
    if (((p[0] >= 'A' && p[0] <= 'Z') ||
         (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
    while (p < end && (*p == '/' || *p == '\\')) p++;
    if (p >= end) return 1;
    start = p;
    for (p = start; ; p++) {
        size_t component_length;
        if (p == end || *p == '/' || *p == '\\') {
            component_length = (size_t)(p - start);
            if (component_length != 0 &&
                !((component_length == 1U && start[0] == '.') ||
                  (component_length == 2U && start[0] == '.' && start[1] == '.')) &&
                !dos16_component_is_83(start, component_length)) return 0;
            if (p == end) break;
            start = p + 1;
        }
    }
    return 1;
}

static int dos16_projected_name_preflight(const bp_mini_entry_t *entry,
                                          void *user_data)
{
    bp_mini_projected_preflight_t *preflight =
        (bp_mini_projected_preflight_t *)user_data;
    char local_name[BP_MAX_PATH];
    if (entry != NULL && entry->entry_type != BP_TYPE_META &&
        entry->entry_type != BP_TYPE_RSRC &&
        bp_mini_name_is_safe(entry->name) &&
        bp_mini_utf8_to_local(entry->name, local_name, sizeof(local_name)) == 0 &&
        !dos16_path_is_83(local_name, entry->entry_type)) {
        preflight->result = BP_ERR_PARTIAL;
        return 1;
    }
    return bp_mini_projected_name_preflight(entry, user_data);
}

static int cmd_extract(const char *arc_path, const char *dest)
{
#ifdef __WATCOMC__
    int fd;
#else
    FILE *fp;
#endif
    bp_io_t arc_io;
    extract_ctx_t ctx;
    bp_mini_projected_index_t name_index;
    bp_mini_projected_preflight_t preflight;
    int rc;
    BP_STACK_PROBE();

#ifdef __WATCOMC__
    fd = open(arc_path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot open ");
        bp_wstr(2, arc_path); write(2, "\n", 1);
        return 1;
    }
    bp_io_from_handle(&arc_io, fd);
#else
    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);
#endif

    memset(&ctx, 0, sizeof(ctx));
    if (dest) {
        if (strlen(dest) >= sizeof(ctx.dest) ||
            !dos16_path_is_83(dest, BP_TYPE_DIR)) {
#ifdef __WATCOMC__
            close(fd);
            bp_wstr(2, PROG_NAME ": destination path is not representable\n");
#else
            fclose(fp);
            fprintf(stderr, PROG_NAME ": destination path is not representable\n");
#endif
            return 1;
        }
        strcpy(ctx.dest, dest);
    }

    preflight.index = &name_index;
    preflight.result = BP_OK;
    rc = bp_mini_projected_index_init(&name_index);
    if (rc == BP_OK) {
        rc = bp_mini_archive_list(&arc_io, dos16_projected_name_preflight,
                                  &preflight);
        if (rc == BP_OK && preflight.result != BP_OK) rc = preflight.result;
    }
    bp_mini_projected_index_free(&name_index);
    if (rc != BP_OK) {
#ifdef __WATCOMC__
        close(fd);
        bp_wstr(2, PROG_NAME ": destination-name preflight failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fclose(fp);
        fprintf(stderr, PROG_NAME ": destination-name preflight failed: %s\n",
                bp_mini_strerror(rc));
#endif
        return 1;
    }

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_extract_all_with_directories(&arc_io,
                                         on_extract_open,
                                         on_extract_done,
                                         on_extract_directory,
                                         on_extract_metadata,
                                         &ctx);
    }
    if (rc == BP_OK && ctx.name_error) rc = BP_ERR_PARTIAL;
    {
        int dir_rc = bp_mini_directory_finish(&ctx.directories, rc == BP_OK || rc == BP_ERR_PARTIAL,
                                               finish_directory, &ctx);
        if (dir_rc != BP_OK && (rc == BP_OK || rc == BP_ERR_PARTIAL)) rc = dir_rc;
    }

#ifdef __WATCOMC__
    close(fd);
#else
    fclose(fp);
#endif

    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": extract failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": extract failed: %s\n",
                bp_mini_strerror(rc));
#endif
        return 1;
    }
    return 0;
}

/* ---- cmd_list ---------------------------------------------------------- */

static int cmd_list(const char *arc_path)
{
#ifdef __WATCOMC__
    int fd;
#else
    FILE *fp;
#endif
    bp_io_t arc_io;
    int rc;

#ifdef __WATCOMC__
    fd = open(arc_path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot open ");
        bp_wstr(2, arc_path); write(2, "\n", 1);
        return 1;
    }
    bp_io_from_handle(&arc_io, fd);
    write(1, "  Type  Size          Name\n"
             "  ----  ------------  ----\n", 54);
#else
    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);
    printf("  Type  Size          Name\n");
    printf("  ----  ------------  ----\n");
#endif

    rc = bp_mini_archive_list(&arc_io, on_list, NULL);

#ifdef __WATCOMC__
    close(fd);
#else
    fclose(fp);
#endif

    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": list failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": list failed: %s\n",
                bp_mini_strerror(rc));
#endif
        return 1;
    }
    return 0;
}

/* ---- cmd_test ---------------------------------------------------------- */

static int cmd_test(const char *arc_path)
{
#ifdef __WATCOMC__
    int fd;
#else
    FILE *fp;
#endif
    bp_io_t arc_io;
    int rc;
    BP_STACK_PROBE();

#ifdef __WATCOMC__
    fd = open(arc_path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot open ");
        bp_wstr(2, arc_path); write(2, "\n", 1);
        return 1;
    }
    bp_io_from_handle(&arc_io, fd);
#else
    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);
#endif

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_test(&arc_io, on_test, NULL);
    }

#ifdef __WATCOMC__
    close(fd);
#else
    fclose(fp);
#endif

    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": test failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": test failed: %s\n",
                bp_mini_strerror(rc));
#endif
        return 1;
    }
    return 0;
}

/* Restore only an explicitly selected, verified transaction backup.  Refuse
 * to replace any current target so recovery never discards a newer archive. */
static int cmd_recover(const char *arc_path, const char *backup_path)
{
#ifdef __WATCOMC__
    char expected_backup[BP_MAX_PATH];
    char stage_path[BP_MAX_PATH];
    unsigned int backup_id;
    unsigned int backup_attr;
    int stage_present;
    int stage_valid = 0;
    if (!dos16_path_is_83(arc_path, BP_TYPE_DATA) ||
        !dos16_path_is_83(backup_path, BP_TYPE_DATA) ||
        dos16_backup_id(backup_path, &backup_id) != 0 ||
        dos16_sibling_name(arc_path, expected_backup, sizeof(expected_backup),
                           "BK", ".BAK", backup_id) != 0 ||
        !dos16_path_equal(backup_path, expected_backup) ||
        dos16_sibling_name(arc_path, stage_path, sizeof(stage_path),
                           "BPK", ".TMP", backup_id) != 0) {
        bp_wstr(2, PROG_NAME ": recovery requires a same-directory BK####.BAK\n");
        return 1;
    }
    if (access(arc_path, 0) == 0) {
        bp_wstr(2, PROG_NAME ": recovery target already exists: ");
        bp_wstr(2, arc_path); write(2, "\n", 1);
        return 1;
    }
    if (errno != ENOENT || _dos_getfileattr(backup_path, &backup_attr) != 0 ||
        (backup_attr & _A_SUBDIR) != 0) {
        bp_wstr(2, PROG_NAME ": cannot inspect recovery backup: ");
        bp_wstr(2, backup_path); write(2, "\n", 1);
        return 1;
    }
    if (cmd_test(backup_path) != 0) {
        bp_wstr(2, PROG_NAME ": recovery backup failed integrity test\n");
        return 1;
    }
    stage_present = access(stage_path, 0) == 0;
    if (!stage_present && errno != ENOENT) {
        bp_wstr(2, PROG_NAME ": cannot inspect recovery stage: ");
        bp_wstr(2, stage_path); write(2, "\n", 1);
        return 1;
    }
    if (stage_present) stage_valid = cmd_test(stage_path) == 0;
    if (rename(backup_path, arc_path) != 0) {
        bp_wstr(2, PROG_NAME ": cannot restore recovery backup: ");
        bp_wstr(2, backup_path); write(2, "\n", 1);
        return 1;
    }
    if (stage_present && stage_valid && remove(stage_path) != 0) {
        bp_wstr(2, PROG_NAME ": archive restored; verified replacement stage remains: ");
        bp_wstr(2, stage_path); write(2, "\n", 1);
        return 1;
    }
    if (stage_present && !stage_valid) {
        bp_wstr(2, PROG_NAME ": archive restored; unverified stage retained: ");
        bp_wstr(2, stage_path); write(2, "\n", 1);
        return 1;
    }
    if (g_verbose) {
        bp_wstr(1, "Restored "); bp_wstr(1, arc_path);
        bp_wstr(1, " from "); bp_wstr(1, backup_path);
        write(1, "\n", 1);
    }
    return 0;
#else
    (void)arc_path;
    (void)backup_path;
    fprintf(stderr, PROG_NAME ": recovery is available only in the DOS build\n");
    return 1;
#endif
}

/* ---- cmd_info ---------------------------------------------------------- */

static const char *os_name(bp_uint8 os)
{
    switch (os) {
    case BP_OS_DOS:     return "DOS";
    case BP_OS_WIN16:   return "Win16";
    case BP_OS_OS2:     return "OS/2";
    case BP_OS_CPM86:   return "CP/M-86";
    case BP_OS_LINUX:   return "Linux";
    case BP_OS_MACOSX:  return "macOS";
    case BP_OS_WINDOWS: return "Windows";
    default:             return "unknown";
    }
}

static int cmd_info(const char *arc_path)
{
#ifdef __WATCOMC__
    int fd;
#else
    FILE *fp;
#endif
    bp_io_t arc_io;
    bp_mini_archive_info_t info;
    char buf[32];
    int rc;

#ifdef __WATCOMC__
    fd = open(arc_path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot open ");
        bp_wstr(2, arc_path); write(2, "\n", 1);
        return 1;
    }
    bp_io_from_handle(&arc_io, fd);
#else
    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);
#endif

    rc = bp_mini_archive_info(&arc_io, &info);

#ifdef __WATCOMC__
    close(fd);
#else
    fclose(fp);
#endif

    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": info failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": info failed: %s\n",
                bp_mini_strerror(rc));
#endif
        return 1;
    }

#ifdef __WATCOMC__
    {
        static const char * const mn[4] = {"stored","BoltLite","boltlz","fluff"};
        int j;
        int mfirst;
        write(1, "Archive:     ", 13); bp_wstr(1, arc_path); write(1, "\n", 1);
        write(1, "Version:     ", 13); bp_wulong(1, (unsigned long)info.version); write(1, "\n", 1);
        write(1, "Creator OS:  ", 13); bp_wstr(1, os_name(info.creator_os)); write(1, "\n", 1);
        write(1, "Methods:     ", 13);
        mfirst = 1;
        for (j = 0; j < 4; j++) {
            if (info.method_counts[j] > 0) {
                if (!mfirst) write(1, ", ", 2);
                bp_wstr(1, mn[j]); write(1, ": ", 2);
                bp_wulong(1, (unsigned long)info.method_counts[j]);
                mfirst = 0;
            }
        }
        if (mfirst) write(1, "none", 4);
        write(1, "\n", 1);
        write(1, "Entries:     ", 13); bp_wulong(1, (unsigned long)info.entry_count); write(1, "\n", 1);
        write(1, "Directories: ", 13); bp_wulong(1, (unsigned long)info.dir_count); write(1, "\n", 1);
        format_size64_mini(info.total_original, buf);
        write(1, "Orig size:   ", 13); bp_wstr(1, buf); write(1, "\n", 1);
        format_size64_mini(info.total_compressed, buf);
        write(1, "Comp size:   ", 13); bp_wstr(1, buf); write(1, "\n", 1);
        format_size64_mini(info.archive_size, buf);
        write(1, "Archive sz:  ", 13); bp_wstr(1, buf); write(1, "\n", 1);
    }
#else
    printf("Archive:     %s\n", arc_path);
    printf("Version:     %u\n", (unsigned)info.version);
    printf("Creator OS:  %s\n", os_name(info.creator_os));
    {
        static const char * const mn[4] = {"stored","BoltLite","boltlz","fluff"};
        int j;
        int mfirst;
        printf("Methods:     ");
        mfirst = 1;
        for (j = 0; j < 4; j++) {
            if (info.method_counts[j] > 0) {
                if (!mfirst) printf(", ");
                printf("%s: %lu", mn[j], (unsigned long)info.method_counts[j]);
                mfirst = 0;
            }
        }
        if (mfirst) printf("none");
        printf("\n");
    }
    printf("Entries:     %lu\n", (unsigned long)info.entry_count);
    printf("Directories: %lu\n", (unsigned long)info.dir_count);
    format_size64_mini(info.total_original, buf);
    printf("Orig size:   %s\n", buf);
    format_size64_mini(info.total_compressed, buf);
    printf("Comp size:   %s\n", buf);
    format_size64_mini(info.archive_size, buf);
    printf("Archive:     %s\n", buf);
#endif
    return 0;
}

/* ---- usage ------------------------------------------------------------- */

static void usage(void)
{
#ifdef __WATCOMC__
    bp_wstr(2,
        PROG_NAME " " BP_VER "  BoltPack mini for DOS (16-bit)\n"
        "Usage:\n"
        "  bpmini c <archive> <path>...   create archive\n"
        "  bpmini x <archive> [dest]      extract archive\n"
        "  bpmini l <archive>             list contents\n"
        "  bpmini t <archive>             test integrity\n"
        "  bpmini i <archive>             show info\n"
        "  bpmini r <archive> <backup>    restore verified BK####.BAK if target missing\n"
        "Options:\n"
        "  -b8k / -b16k / -b32k           block size (create only)\n"
        "  -m0                            store, no compression\n"
        "  -m1                            BoltLite compression (default)\n"
        "  -q                             quiet mode\n"
        "Compression:\n"
        "  create:   store (0), BoltLite (1)\n"
        "  extract:  store (0), BoltLite (1), boltlz (2)\n");
#else
    fputs(PROG_NAME " " BP_VER "  BoltPack mini for DOS (16-bit)\n"
          "Usage:\n"
          "  bpmini c <archive> <path>...   create archive\n"
          "  bpmini x <archive> [dest]      extract archive\n"
          "  bpmini l <archive>             list contents\n"
          "  bpmini t <archive>             test integrity\n"
          "  bpmini i <archive>             show info\n"
          "  bpmini r <archive> <backup>    restore verified BK####.BAK if target missing\n"
          "Options:\n"
          "  -b8k / -b16k / -b32k           block size (create only)\n"
          "  -m0                            store, no compression\n"
          "  -m1                            BoltLite compression (default)\n"
          "  -q                             quiet mode\n"
          "Compression:\n"
          "  create:   store (0), BoltLite (1)\n"
          "  extract:  store (0), BoltLite (1), boltlz (2)\n",
          stderr);
#endif
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int i;
    int files_start;
    char cmd;
    const char *arc_path;
    bp_uint32 block_size = BP_BLOCK_32K;
    bp_uint8  method     = BP_METHOD_BOLTLITE;

#ifdef BP_TEST_ALLOC_CLI
    if (argc >= 4 && strcmp(argv[1], "--fail-path-buffer") == 0) {
        g_test_alloc_index = atol(argv[2]);
        g_test_alloc_enabled = 1;
        argv += 2;
        argc -= 2;
    }
#endif

#ifdef BP_TEST_STACK_HIGHWATER
    BP_STACK_PROBE();
    atexit(bp_test_stack_report);
#endif

    if (argc < 3) {
        usage();
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if      (strcmp(argv[i], "-q")   == 0) { g_verbose = 0; }
            else if (strcmp(argv[i], "-b8k") == 0) { block_size = 8192U; }
            else if (strcmp(argv[i], "-b16k")== 0) { block_size = 16384U; }
            else if (strcmp(argv[i], "-b32k")== 0) { block_size = 32768U; }
            else if (strcmp(argv[i], "-m0")  == 0) { method = BP_METHOD_STORED; }
            else if (strcmp(argv[i], "-m1")  == 0) { method = BP_METHOD_BOLTLITE; }
        }
    }

    cmd      = argv[1][0];
    arc_path = NULL;
    files_start = argc;
    for (i = 2; i < argc; i++) {
        if (argv[i][0] != '-') {
            arc_path    = argv[i];
            files_start = i + 1;
            break;
        }
    }
    if (arc_path == NULL) {
        usage();
        return 1;
    }
    while (files_start < argc && argv[files_start][0] == '-') {
        files_start++;
    }

    switch (cmd) {
    case 'c':
    {
        int result = cmd_create(arc_path, argc - files_start, argv + files_start,
                                block_size, method);
#ifdef BP_TEST_ALLOC_CLI
        if (g_test_alloc_enabled) {
            char marker_name[16];
            FILE *marker;
            sprintf(marker_name, "AF%ld.TXT", g_test_alloc_index);
            marker = fopen(marker_name, "w");
            if (marker != NULL) {
                fprintf(marker, "result=%d error=%d live=%ld\n", result,
                        g_test_last_error, bp_test_alloc_live_count());
                fclose(marker);
            }
        }
#endif
        return result;
    }
    case 'x': {
        const char *dest = (files_start < argc) ? argv[files_start] : NULL;
        return cmd_extract(arc_path, dest);
    }
    case 'l':
        return cmd_list(arc_path);
    case 't':
        return cmd_test(arc_path);
    case 'i':
        return cmd_info(arc_path);
    case 'r':
        if (files_start >= argc) {
            usage();
            return 1;
        }
        return cmd_recover(arc_path, argv[files_start]);
    default:
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": unknown command '");
        write(2, &cmd, 1); write(2, "'\n", 2);
#else
        fprintf(stderr, PROG_NAME ": unknown command '%c'\n", cmd);
#endif
        usage();
        return 1;
    }
}
