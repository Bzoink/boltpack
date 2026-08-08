/*
 * main.c -- OS/2 1.x (16-bit) CLI frontend for BoltPack mini
 *
 * Target: Open Watcom C 16-bit, compact model, OS/2 system.
 * Compiler:  wcc -bt=os2 -mc -0 -za -DSIZEOF_INT=2 -DSIZEOF_LONG=4
 *            -DLZO_CFG_16BIT_OK -DBP_MINI
 *            -DBP_CREATOR_OS=BP_OS_OS2 -DBP_CREATOR_CPU=BP_CPU_8086
 * Linker:    wlink system os2
 * Binary:    BPMINI.EXE
 *
 * OS/2 1.x features used:
 *   - Standard C library (fopen/fclose/fread/fwrite/fseek/ftell)
 *   - mkdir() via direct.h (Watcom provides this for OS/2)
 *   - DosSetFileInfo() for setting mtime on extracted files
 *   - Directory scanning via _dos_findfirst/_dos_findnext (Watcom)
 *   - Full hierarchical extraction (HPFS and FAT supported)
 *   - Longer file names on HPFS (no 8.3 restriction)
 *
 * Commands: c, x, l, t, i  (same as dos16)
 *
 * ANSI C (C89/C90) with Watcom/OS2-specific extensions guarded by #ifdef.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include "../../core/mini/archiver.h"
#include "../mini_shared/mini_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef __WATCOMC__
#  include <direct.h>
#  include <dos.h>
#  ifdef __OS2__
#    define INCL_DOSFILEMGR
#    include <os2.h>   /* DosSetFileInfo, FILESTATUS */
#  endif
#endif

/* ---- constants --------------------------------------------------------- */

#define BP_MAX_PATH   260
#define PROG_NAME     "bpmini"
#define BP_XSTR(x)    BP_STR(x)
#define BP_STR(x)     #x
#define BP_VER        BP_XSTR(BP_VERSION_MAJOR) "." BP_XSTR(BP_VERSION_MINOR)

/* ---- globals ----------------------------------------------------------- */

static int g_verbose = 1;

/* ---- mtime helpers ----------------------------------------------------- */

/*
 * Set the mtime of a file using DosSetFileInfo on OS/2.
 * Falls back to doing nothing if the OS2 API is unavailable.
 *
 * OS/2 stores file times as FAT-style date/time packed fields.
 */
#if defined(__WATCOMC__) && defined(__OS2__)
static void set_mtime_os2(const char *path, bp_uint64 mtime)
{
    HFILE   hf;
    USHORT  action;
    FILESTATUS fst;
    time_t  t;
    struct tm *tm_p;

    if (mtime.hi == 0 && mtime.lo == 0) return;

    /* Open file to set date/time */
    if (DosOpen((PSZ)path, &hf, &action, 0L, FILE_NORMAL,
                FILE_OPEN, OPEN_SHARE_DENYWRITE | OPEN_ACCESS_WRITEONLY,
                0L) != 0) {
        return;
    }

    if (DosQFileInfo(hf, 1, (PBYTE)&fst, (USHORT)sizeof(fst)) == 0) {
        t    = (time_t)mtime.lo;
        tm_p = localtime(&t);
        if (tm_p) {
            fst.fdateLastWrite.day     = (USHORT)tm_p->tm_mday;
            fst.fdateLastWrite.month   = (USHORT)(tm_p->tm_mon + 1);
            fst.fdateLastWrite.year    = (USHORT)(tm_p->tm_year > 80
                                          ? tm_p->tm_year - 80 : 0);
            fst.ftimeLastWrite.twosecs = (USHORT)(tm_p->tm_sec / 2);
            fst.ftimeLastWrite.minutes = (USHORT)tm_p->tm_min;
            fst.ftimeLastWrite.hours   = (USHORT)tm_p->tm_hour;
            DosSetFileInfo(hf, 1, (PBYTE)&fst, (USHORT)sizeof(fst));
        }
    }

    DosClose(hf);
}
#else
static void set_mtime_os2(const char *path, bp_uint64 mtime)
{
    /* Fallback: no-op */
    (void)path; (void)mtime;
}
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
#endif
            *p = saved;
        }
    }
}

/* ---- list callback ----------------------------------------------------- */

static int on_list(const bp_mini_entry_t *e, void *user_data)
{
    char size_buf[32];
    (void)user_data;
    format_size64_mini(e->original_size, size_buf);
    printf("  %c  %-12s  ",
           (e->entry_type == BP_TYPE_DIR) ? 'D' : ' ', size_buf);
    print_utf8_safe(e->name, (unsigned int)e->name_len);
    putchar('\n');
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
    FILE *fp;
    char *p;

    if (e->entry_type == BP_TYPE_DIR) {
        if (ctx->dest[0]) {
            snprintf(path, BP_MAX_PATH, "%s\\%s", ctx->dest, e->name);
        } else {
            snprintf(path, BP_MAX_PATH, "%s", e->name);
        }
        for (p = path; *p; p++) if (*p == '/') *p = '\\';
        make_parents(path);
#ifdef __WATCOMC__
        mkdir(path);
#endif
        return NULL;
    }

    if (ctx->dest[0]) {
        snprintf(path, BP_MAX_PATH, "%s\\%s", ctx->dest, e->name);
    } else {
        snprintf(path, BP_MAX_PATH, "%s", e->name);
    }
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    make_parents(path);

    if (g_verbose) {
        printf("  x  "); print_utf8_safe(e->name, e->name_len); putchar('\n');
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot create %s\n", path);
        return NULL;
    }
    {
        static bp_io_t io;
        bp_io_from_file(&io, fp);
        return &io;
    }
}

static void on_extract_done(const bp_mini_entry_t *e, bp_io_t *out,
                            void *user_data)
{
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
    set_mtime_os2(path, e->mtime);
}

/* ---- test callback ----------------------------------------------------- */

static void on_test(const bp_mini_entry_t *e, int result, void *user_data)
{
    (void)user_data;
    if (result == BP_OK) {
        printf("  OK   "); print_utf8_safe(e->name, e->name_len); putchar('\n');
    } else {
        printf("  FAIL "); print_utf8_safe(e->name, e->name_len);
        printf(" (%s)\n", bp_mini_strerror(result));
    }
}

/* ---- cmd_create -------------------------------------------------------- */

#ifdef __WATCOMC__

/* Progress-tracking I/O wrapper: intercepts each block read to update display */
typedef struct {
    FILE       *fp;
    bp_uint32   total;     /* original file size in bytes */
    bp_uint32   done;      /* bytes consumed so far */
    const char *arc_name;
} prog_ctx_t;

static long prog_read(void *handle, void *buf, bp_uint32 count)
{
    prog_ctx_t *ctx;
    long n;
    unsigned int pct;

    ctx = (prog_ctx_t *)handle;
    n = (long)fread(buf, 1, (size_t)count, ctx->fp);
    ctx->done += (bp_uint32)n;

    if (g_verbose && ctx->total > 0) {
        if (ctx->total >= 100UL) {
            pct = (unsigned int)(ctx->done / (ctx->total / 100UL));
        } else {
            pct = (unsigned int)((ctx->done * 100UL) / ctx->total);
        }
        if (pct > 99) pct = 99; /* stay below 100 during transfer */
        printf("  a  %s  %3u%%\r", ctx->arc_name, pct);
        fflush(stdout);
    }
    return n;
}

static int add_one_file(bp_mini_archive_t *ar, const char *arc_name,
                        const char *fs_path, bp_uint8 platform)
{
    struct stat st;
    bp_uint64 fsize;
    bp_uint64 mtime;
    FILE *fp;
    bp_io_t file_io;
    prog_ctx_t prog;
    int rc;

    if (stat(fs_path, &st) != 0) {
        fprintf(stderr, PROG_NAME ": cannot stat %s\n", fs_path);
        return BP_ERR_IO;
    }
    fsize.lo = (bp_uint32)(unsigned long)st.st_size;
    fsize.hi = 0;
    mtime.lo = (bp_uint32)(unsigned long)st.st_mtime;
    mtime.hi = 0;

    fp = fopen(fs_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", fs_path);
        return BP_ERR_IO;
    }

    if (g_verbose && fsize.lo > 0) {
        printf("  a  %s  %3u%%\r", arc_name, 0U);
        fflush(stdout);
    }

    prog.fp       = fp;
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
    fclose(fp);

    if (g_verbose) {
        unsigned int pct;
        if (rc == BP_OK && ar->entries_written > 0) {
            bp_mini_entry_t *ent = &ar->entry_list[ar->entries_written - 1];
            if (ent->original_size.lo > 0 && ent->original_size.hi == 0) {
                unsigned long orig = ent->original_size.lo;
                unsigned long comp = ent->compressed_size.lo;
                /* Divide first to avoid overflow when orig > ~42 MB */
                if (orig >= 100UL) {
                    pct = (unsigned int)(comp / (orig / 100UL));
                } else {
                    pct = (unsigned int)((comp * 100UL) / orig);
                }
                if (pct > 100) pct = 100;
            } else {
                pct = 0;
            }
        } else {
            pct = 0;
        }
        printf("  a  %s  %3u%%\n", arc_name, pct);
        fflush(stdout);
    }

    return rc;
}

static int add_dir_entry(bp_mini_archive_t *ar, const char *arc_name,
                         bp_uint8 platform)
{
    bp_uint64 z; z.lo = 0; z.hi = 0;
    if (g_verbose) printf("  a  %s/\n", arc_name);
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
    int rc = BP_OK;

    pattern   = (char *)malloc(BP_MAX_PATH);
    child_fs  = (char *)malloc(BP_MAX_PATH);
    child_arc = (char *)malloc(BP_MAX_PATH);
    if (!pattern || !child_fs || !child_arc) {
        free(pattern); free(child_fs); free(child_arc);
        return BP_ERR_IO;
    }

    snprintf(pattern, BP_MAX_PATH, "%s\\*.*", fs_path);
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
        if (strcmp(ff.name, ".") == 0 || strcmp(ff.name, "..") == 0) continue;
        snprintf(child_fs, BP_MAX_PATH, "%s\\%s", fs_path, ff.name);
        if (arc_prefix[0]) {
            snprintf(child_arc, BP_MAX_PATH, "%s/%s", arc_prefix, ff.name);
        } else {
            snprintf(child_arc, BP_MAX_PATH, "%s", ff.name);
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
    FILE *fp;
    bp_io_t arc_io;
    bp_mini_archive_t ar;
    int rc;
    int i;

    fp = fopen(arc_path, "wb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot create %s\n", arc_path); return 1;
    }
    bp_io_from_file(&arc_io, fp);

    rc = bp_mini_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": compress init: %s\n", bp_mini_strerror(rc));
        fclose(fp); return 1;
    }

    rc = bp_mini_archive_create(&ar, &arc_io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": create: %s\n", bp_mini_strerror(rc));
        fclose(fp); return 1;
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
        rc = add_path(&ar, arc_name, argv[i], BP_OS_OS2);
#else
        fprintf(stderr, PROG_NAME ": directory scanning requires Watcom C\n");
        rc = BP_ERR_IO;
#endif
        if (rc != BP_OK) {
            fprintf(stderr, PROG_NAME ": error adding %s: %s\n",
                    argv[i], bp_mini_strerror(rc));
            break;
        }
    }

    if (rc == BP_OK) rc = bp_mini_archive_finalize(&ar);
    bp_mini_archive_close(&ar);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": failed: %s\n", bp_mini_strerror(rc)); return 1;
    }
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------- */

static int cmd_extract(const char *arc_path, const char *dest)
{
    FILE *fp;
    bp_io_t arc_io;
    extract_ctx_t ctx;
    int rc;

    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path); return 1;
    }
    bp_io_from_file(&arc_io, fp);
    memset(&ctx, 0, sizeof(ctx));
    if (dest) strncpy(ctx.dest, dest, BP_MAX_PATH - 1);

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_extract_all(&arc_io, on_extract_open,
                                         on_extract_done, &ctx);
    }
    fclose(fp);
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": extract: %s\n", bp_mini_strerror(rc)); return 1;
    }
    return 0;
}

/* ---- cmd_list ---------------------------------------------------------- */

static int cmd_list(const char *arc_path)
{
    FILE *fp;
    bp_io_t arc_io;
    int rc;

    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path); return 1;
    }
    bp_io_from_file(&arc_io, fp);
    printf("  Type  Size          Name\n");
    printf("  ----  ------------  ----\n");
    rc = bp_mini_archive_list(&arc_io, on_list, NULL);
    fclose(fp);
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": list: %s\n", bp_mini_strerror(rc)); return 1;
    }
    return 0;
}

/* ---- cmd_test ---------------------------------------------------------- */

static int cmd_test(const char *arc_path)
{
    FILE *fp;
    bp_io_t arc_io;
    int rc;

    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path); return 1;
    }
    bp_io_from_file(&arc_io, fp);
    rc = bp_mini_compress_init();
    if (rc == BP_OK) rc = bp_mini_archive_test(&arc_io, on_test, NULL);
    fclose(fp);
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": test: %s\n", bp_mini_strerror(rc)); return 1;
    }
    return 0;
}

/* ---- cmd_info ---------------------------------------------------------- */

static int cmd_info(const char *arc_path)
{
    FILE *fp;
    bp_io_t arc_io;
    bp_mini_archive_info_t info;
    char buf[32];
    int rc;

    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path); return 1;
    }
    bp_io_from_file(&arc_io, fp);
    rc = bp_mini_archive_info(&arc_io, &info);
    fclose(fp);
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": info: %s\n", bp_mini_strerror(rc)); return 1;
    }

    printf("Entries:     %lu\n", (unsigned long)info.entry_count);
    printf("Directories: %lu\n", (unsigned long)info.dir_count);
    format_size64_mini(info.total_original, buf);
    printf("Orig size:   %s\n", buf);
    format_size64_mini(info.total_compressed, buf);
    printf("Comp size:   %s\n", buf);
    {
        static const char * const mn[4] = {"stored","LZSS","miniLZO","fluff"};
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
    return 0;
}

/* ---- usage ------------------------------------------------------------- */

static void usage(void)
{
    fputs(PROG_NAME " " BP_VER "  BoltPack mini for OS/2 1.x (16-bit)\n"
          "Usage:\n"
          "  bpmini c <archive> <path>...   create archive\n"
          "  bpmini x <archive> [dest]      extract archive\n"
          "  bpmini l <archive>             list contents\n"
          "  bpmini t <archive>             test integrity\n"
          "  bpmini i <archive>             show info\n"
          "Options:\n"
          "  -b8k / -b16k / -b32k           block size (create only)\n"
          "  -m0                            store, no compression\n"
          "  -m1                            LZSS compression (default)\n"
          "  -q                             quiet mode\n"
          "Compression:\n"
          "  create:   store (0), LZSS (1)\n"
          "  extract:  store (0), LZSS (1), miniLZO (2)\n",
          stderr);
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int i;
    int files_start;
    char cmd;
    const char *arc_path;
    bp_uint32 block_size = BP_BLOCK_32K;
    bp_uint8  method     = BP_METHOD_LZSS;

    if (argc < 3) { usage(); return 1; }

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if      (strcmp(argv[i], "-q")   == 0) g_verbose = 0;
            else if (strcmp(argv[i], "-b8k") == 0) block_size = 8192U;
            else if (strcmp(argv[i], "-b16k")== 0) block_size = 16384U;
            else if (strcmp(argv[i], "-b32k")== 0) block_size = 32768U;
            else if (strcmp(argv[i], "-m0")  == 0) method = BP_METHOD_STORED;
            else if (strcmp(argv[i], "-m1")  == 0) method = BP_METHOD_LZSS;
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
        fprintf(stderr, PROG_NAME ": unknown command '%c'\n", cmd);
        usage(); return 1;
    }
}
