/*
 * main.c -- Windows 3.x (Win16) CLI frontend for BoltPack mini
 *
 * Target: Open Watcom C 16-bit, compact model, Windows system.
 * Compiler:  wcc -bt=windows -mc -0 -za -DSIZEOF_INT=2 -DSIZEOF_LONG=4
 *             -DBP_MINI
 *            -DBP_CREATOR_OS=BP_OS_WIN16 -DBP_CREATOR_CPU=BP_CPU_8086
 * Linker:    wlink system windows
 * Binary:    BPMINI.EXE
 *
 * Win16 runs in a console launched by COMMAND.COM under Windows 3.x or via
 * the WINSTUB stub.  The standard C stdio library is fully available.
 * Directory scanning uses the same Watcom _dos_findfirst/_dos_findnext API
 * as the DOS frontend.
 *
 * Commands: c, x, l, t, i  (same as dos16)
 *
 * ANSI C (C89/C90) with Watcom-specific directory scanning.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/mini/archiver.h"
#include "../mini_shared/mini_common.h"

#include <stdlib.h>
#include <string.h>

#ifndef __WATCOMC__
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <utime.h>
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
static void set_mtime(const char *path, bp_uint64 mtime)
{
    struct utimbuf ut;
    if (mtime.hi == 0 && mtime.lo == 0) return;
    ut.actime  = (time_t)mtime.lo;
    ut.modtime = (time_t)mtime.lo;
    utime(path, &ut);
}
#endif

/* ---- path join and output helpers (Watcom only) ------------------------ */

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
    write(1, (e->entry_type == BP_TYPE_DIR) ? "  D  " : "     ", 5);
    bp_wpad(1, size_buf, 12);
    write(1, "  ", 2);
    print_utf8_safe(e->name, (unsigned int)e->name_len);
    write(1, "\n", 1);
#else
    printf("  %c  %-12s  ",
           (e->entry_type == BP_TYPE_DIR) ? 'D' : ' ', size_buf);
    print_utf8_safe(e->name, (unsigned int)e->name_len);
    putchar('\n');
#endif
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct {
    char dest[BP_MAX_PATH];
} extract_ctx_t;

static bp_io_t *on_extract_open(const bp_mini_entry_t *e, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH];
    static bp_io_t io;

    if (e->entry_type == BP_TYPE_DIR) {
#ifdef __WATCOMC__
        join_sep(path, BP_MAX_PATH, ctx->dest, '\\', e->name);
#else
        if (ctx->dest[0]) {
            snprintf(path, BP_MAX_PATH, "%s\\%s", ctx->dest, e->name);
        } else {
            snprintf(path, BP_MAX_PATH, "%s", e->name);
        }
#endif
        {
            char *p;
            for (p = path; *p; p++) if (*p == '/') *p = '\\';
        }
        make_parents(path);
#ifdef __WATCOMC__
        mkdir(path);
#endif
        return NULL;
    }

#ifdef __WATCOMC__
    join_sep(path, BP_MAX_PATH, ctx->dest, '\\', e->name);
#else
    if (ctx->dest[0]) {
        snprintf(path, BP_MAX_PATH, "%s\\%s", ctx->dest, e->name);
    } else {
        snprintf(path, BP_MAX_PATH, "%s", e->name);
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
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0);
        if (fd < 0) {
            bp_wstr(2, PROG_NAME ": cannot create ");
            bp_wstr(2, path); write(2, "\n", 1);
            return NULL;
        }
        bp_io_from_handle(&io, fd);
    }
#else
    {
        FILE *fp = fopen(path, "wb");
        if (fp == NULL) {
            fprintf(stderr, PROG_NAME ": cannot create %s\n", path);
            return NULL;
        }
        bp_io_from_file(&io, fp);
    }
#endif
    return &io;
}

static void on_extract_done(const bp_mini_entry_t *e, bp_io_t *out,
                            void *user_data)
{
#ifdef __WATCOMC__
    int fd;
    unsigned dos_date;
    unsigned dos_time;
    (void)user_data;
    if (out == NULL) return;
    fd = (int)(long)out->handle;
    if (e->mtime.lo != 0 || e->mtime.hi != 0) {
        bp_epoch_to_dos(e->mtime.lo, &dos_date, &dos_time);
        _dos_setftime(fd, dos_date, dos_time);
    }
    close(fd);
#else
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH];
    char *p;
    if (out == NULL) return;
    fclose((FILE *)out->handle);
    if (ctx->dest[0]) {
        snprintf(path, BP_MAX_PATH, "%s\\%s", ctx->dest, e->name);
    } else {
        snprintf(path, BP_MAX_PATH, "%s", e->name);
    }
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    set_mtime(path, e->mtime);
#endif
}

/* ---- test callback ----------------------------------------------------- */

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

/* ---- cmd_create (with Watcom directory scanning) ----------------------- */

#ifdef __WATCOMC__

static int add_path(bp_mini_archive_t *ar, const char *arc_prefix,
                    const char *fs_path, bp_uint8 platform);

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
    int rc;

    fd = open(fs_path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot open ");
        bp_wstr(2, fs_path); write(2, "\n", 1);
        return BP_ERR_IO;
    }

    fsize.lo = (bp_uint32)(unsigned long)_filelength(fd);
    fsize.hi = 0;

    if (_dos_getftime(fd, &dos_date, &dos_time) == 0) {
        mtime.lo = bp_dos_to_epoch(dos_date, dos_time);
    } else {
        mtime.lo = 0;
    }
    mtime.hi = 0;

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

    rc = bp_mini_archive_add_entry(ar, arc_name, BP_TYPE_DATA, platform,
                                   &file_io, fsize, mtime);
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

    return rc;
}

static int add_dir_entry(bp_mini_archive_t *ar, const char *arc_name,
                         bp_uint8 platform)
{
    bp_uint64 z;
    z.lo = 0; z.hi = 0;
    if (g_verbose) {
        write(1, "  a  ", 5); bp_wstr(1, arc_name); write(1, "/\n", 2);
    }
    return bp_mini_archive_add_entry(ar, arc_name, BP_TYPE_DIR, platform,
                                     NULL, z, z);
}

static int add_path(bp_mini_archive_t *ar, const char *arc_prefix,
                    const char *fs_path, bp_uint8 platform)
{
    struct find_t ff;
    char *pattern;
    char *child_fs;
    char *child_arc;
    unsigned int ret;
    int rc;
    int plen;

    rc      = BP_OK;
    pattern   = (char *)malloc(BP_MAX_PATH);
    child_fs  = (char *)malloc(BP_MAX_PATH);
    child_arc = (char *)malloc(BP_MAX_PATH);
    if (!pattern || !child_fs || !child_arc) {
        free(pattern); free(child_fs); free(child_arc);
        return BP_ERR_IO;
    }

    plen = (int)strlen(fs_path);
    if (plen + 5 < BP_MAX_PATH) {
        memcpy(pattern, fs_path, (size_t)plen);
        memcpy(pattern + plen, "\\*.*", 5);
    } else {
        pattern[0] = '\0';
    }

    ret = _dos_findfirst(pattern, _A_NORMAL | _A_RDONLY | _A_HIDDEN |
                                  _A_SYSTEM | _A_SUBDIR, &ff);
    if (ret != 0) {
        free(pattern); free(child_fs); free(child_arc);
        return add_one_file(ar, arc_prefix, fs_path, platform);
    }

    if (arc_prefix[0]) {
        rc = add_dir_entry(ar, arc_prefix, platform);
        if (rc != BP_OK) goto done;
    }

    do {
        int nn;
        if (strcmp(ff.name, ".") == 0 || strcmp(ff.name, "..") == 0) {
            continue;
        }

        join_sep(child_fs, BP_MAX_PATH, fs_path, '\\', ff.name);

        nn = (int)strlen(ff.name);
        if (arc_prefix[0]) {
            join_sep(child_arc, BP_MAX_PATH, arc_prefix, '/', ff.name);
        } else if (nn < BP_MAX_PATH) {
            memcpy(child_arc, ff.name, (size_t)(nn + 1));
        } else {
            child_arc[0] = '\0';
        }

        if (ff.attrib & _A_SUBDIR) {
            rc = add_path(ar, child_arc, child_fs, platform);
        } else {
            rc = add_one_file(ar, child_arc, child_fs, platform);
        }
        if (rc != BP_OK) break;

    } while (_dos_findnext(&ff) == 0);

done:
    free(pattern); free(child_fs); free(child_arc);
    return rc;
}

#endif /* __WATCOMC__ */

static int cmd_create(const char *arc_path, int argc, char **argv,
                      bp_uint32 block_size, bp_uint8 method)
{
#ifdef __WATCOMC__
    int fd;
#else
    FILE *fp;
#endif
    bp_io_t arc_io;
    bp_mini_archive_t ar;
    int rc;
    int i;

#ifdef __WATCOMC__
    fd = open(arc_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0);
    if (fd < 0) {
        bp_wstr(2, PROG_NAME ": cannot create ");
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
        bp_wstr(2, PROG_NAME ": compress init: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
        close(fd);
#else
        fprintf(stderr, PROG_NAME ": compress init: %s\n",
                bp_mini_strerror(rc));
        fclose(fp);
#endif
        return 1;
    }

    rc = bp_mini_archive_create(&ar, &arc_io, block_size, method);
    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": create: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
        close(fd);
#else
        fprintf(stderr, PROG_NAME ": create: %s\n", bp_mini_strerror(rc));
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
        rc = add_path(&ar, arc_name, argv[i], BP_OS_WIN16);
#else
        fprintf(stderr, PROG_NAME ": directory scanning requires Watcom C\n");
        rc = BP_ERR_IO;
#endif
        if (rc != BP_OK) {
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
    close(fd);
#else
    fclose(fp);
#endif

    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": failed: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": failed: %s\n", bp_mini_strerror(rc));
#endif
        return 1;
    }
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------- */

static int cmd_extract(const char *arc_path, const char *dest)
{
#ifdef __WATCOMC__
    int fd;
#else
    FILE *fp;
#endif
    bp_io_t arc_io;
    extract_ctx_t ctx;
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

    memset(&ctx, 0, sizeof(ctx));
    if (dest) strncpy(ctx.dest, dest, BP_MAX_PATH - 1);

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_extract_all(&arc_io, on_extract_open,
                                         on_extract_done, &ctx);
    }

#ifdef __WATCOMC__
    close(fd);
#else
    fclose(fp);
#endif

    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": extract: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": extract: %s\n", bp_mini_strerror(rc));
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
        bp_wstr(2, PROG_NAME ": list: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": list: %s\n", bp_mini_strerror(rc));
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
    if (rc == BP_OK) rc = bp_mini_archive_test(&arc_io, on_test, NULL);

#ifdef __WATCOMC__
    close(fd);
#else
    fclose(fp);
#endif

    if (rc != BP_OK) {
#ifdef __WATCOMC__
        bp_wstr(2, PROG_NAME ": test: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": test: %s\n", bp_mini_strerror(rc));
#endif
        return 1;
    }
    return 0;
}

/* ---- cmd_info ---------------------------------------------------------- */

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
        bp_wstr(2, PROG_NAME ": info: ");
        bp_wstr(2, bp_mini_strerror(rc)); write(2, "\n", 1);
#else
        fprintf(stderr, PROG_NAME ": info: %s\n", bp_mini_strerror(rc));
#endif
        return 1;
    }

#ifdef __WATCOMC__
    {
        static const char * const mn[4] = {"stored","BoltLite","boltlz","fluff"};
        int j;
        int mfirst;
        write(1, "Entries:  ", 10); bp_wulong(1, (unsigned long)info.entry_count); write(1, "\n", 1);
        format_size64_mini(info.total_original, buf);
        write(1, "Orig:     ", 10); bp_wstr(1, buf); write(1, "\n", 1);
        format_size64_mini(info.total_compressed, buf);
        write(1, "Comp:     ", 10); bp_wstr(1, buf); write(1, "\n", 1);
        write(1, "Methods:  ", 10);
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
    }
#else
    printf("Entries:  %lu\n", (unsigned long)info.entry_count);
    format_size64_mini(info.total_original, buf);
    printf("Orig:     %s\n", buf);
    format_size64_mini(info.total_compressed, buf);
    printf("Comp:     %s\n", buf);
    {
        static const char * const mn[4] = {"stored","BoltLite","boltlz","fluff"};
        int j;
        int mfirst;
        printf("Methods:  ");
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
#endif
    return 0;
}

/* ---- usage ------------------------------------------------------------- */

static void usage(void)
{
#ifdef __WATCOMC__
    bp_wstr(2,
        PROG_NAME " " BP_VER "  BoltPack mini for Windows 3.x (Win16)\n"
        "Usage:\n"
        "  bpmini c <archive> <path>...   create archive\n"
        "  bpmini x <archive> [dest]      extract archive\n"
        "  bpmini l <archive>             list contents\n"
        "  bpmini t <archive>             test integrity\n"
        "  bpmini i <archive>             show info\n"
        "Options:\n"
        "  -b8k / -b16k / -b32k           block size (create only)\n"
        "  -m0                            store, no compression\n"
        "  -m1                            BoltLite compression (default)\n"
        "  -q                             quiet mode\n"
        "Compression:\n"
        "  create:   store (0), BoltLite (1)\n"
        "  extract:  store (0), BoltLite (1), boltlz (2)\n");
#else
    fputs(PROG_NAME " " BP_VER "  BoltPack mini for Windows 3.x (Win16)\n"
          "Usage:\n"
          "  bpmini c <archive> <path>...   create archive\n"
          "  bpmini x <archive> [dest]      extract archive\n"
          "  bpmini l <archive>             list contents\n"
          "  bpmini t <archive>             test integrity\n"
          "  bpmini i <archive>             show info\n"
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

    if (argc < 3) { usage(); return 1; }

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
    if (arc_path == NULL) { usage(); return 1; }
    while (files_start < argc && argv[files_start][0] == '-') files_start++;

    switch (cmd) {
    case 'c':
        return cmd_create(arc_path, argc - files_start, argv + files_start,
                          block_size, method);
    case 'x': {
        const char *dest = (files_start < argc) ? argv[files_start] : NULL;
        return cmd_extract(arc_path, dest);
    }
    case 'l': return cmd_list(arc_path);
    case 't': return cmd_test(arc_path);
    case 'i': return cmd_info(arc_path);
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
