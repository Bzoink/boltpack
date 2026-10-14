/*
 * main.c -- Linux CLI frontend for BoltPack mini
 *
 * Provides a native 32-bit Linux binary for testing the mini archiver
 * without requiring DOS/Windows emulation.  Uses the same mini core as
 * dos16/win16/os2-16 but with POSIX filesystem access.
 *
 * Commands:
 *   c <archive> <files...>   create archive
 *   x <archive> [dest]       extract archive
 *   l <archive>              list archive contents
 *   t <archive>              test archive integrity
 *   i <archive>              show archive information
 *
 * ANSI C (C89/C90) with POSIX extensions for directory scanning.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _POSIX_C_SOURCE 200112L

#include "../../core/mini/archiver.h"
#include "../mini_shared/mini_common.h"

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

/* ---- constants --------------------------------------------------------- */

#define BP_MAX_PATH   4096
#define PROG_NAME     "bpmini"
#define BP_XSTR(x)    BP_STR(x)
#define BP_STR(x)     #x
#define BP_VER        BP_XSTR(BP_VERSION_MAJOR) "." BP_XSTR(BP_VERSION_MINOR)

/* ---- globals ----------------------------------------------------------- */

static int g_verbose = 1;

/* ---- platform I/O ------------------------------------------------------ */

static void make_parents(const char *path)
{
    char buf[BP_MAX_PATH];
    char *p;
    int  n;

    n = (int)strlen(path);
    if (n <= 0 || n >= BP_MAX_PATH) return;
    memcpy(buf, path, (size_t)n + 1);

    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            char saved = *p;
            *p = '\0';
            mkdir(buf, 0755);
            *p = saved;
        }
    }
}

static void set_mtime(const char *path, bp_uint64 mtime)
{
    struct utimbuf ut;
    if (mtime.hi == 0 && mtime.lo == 0) return;
    ut.actime  = (time_t)mtime.lo;
    ut.modtime = (time_t)mtime.lo;
    utime(path, &ut);
}

/* ---- list callback ----------------------------------------------------- */

static int on_list(const bp_mini_entry_t *e, void *user_data)
{
    char size_buf[32];
    (void)user_data;

    format_size64_mini(e->original_size, size_buf);

    switch (e->entry_type) {
    case BP_TYPE_DIR:
        printf("  D  ");
        break;
    case BP_TYPE_SYMLINK:
        printf("  L  ");
        break;
    default:
        printf("     ");
        break;
    }

    printf("%-12s  ", size_buf);
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

    if (e->entry_type == BP_TYPE_DIR) {
        int n;
        if (ctx->dest[0]) {
            n = snprintf(path, BP_MAX_PATH, "%s/%s", ctx->dest, e->name);
        } else {
            n = snprintf(path, BP_MAX_PATH, "%s", e->name);
        }
        (void)n;
        make_parents(path);
        mkdir(path, 0755);
        return NULL;
    }

    if (ctx->dest[0]) {
        snprintf(path, BP_MAX_PATH, "%s/%s", ctx->dest, e->name);
    } else {
        snprintf(path, BP_MAX_PATH, "%s", e->name);
    }

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
    FILE *fp;
    char path[BP_MAX_PATH];

    (void)ctx;

    if (out == NULL) return;
    fp = (FILE *)out->handle;
    fclose(fp);

    if (ctx->dest[0]) {
        snprintf(path, BP_MAX_PATH, "%s/%s", ctx->dest, e->name);
    } else {
        snprintf(path, BP_MAX_PATH, "%s", e->name);
    }
    set_mtime(path, e->mtime);
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

/* ---- cmd_create helpers ------------------------------------------------ */

static int add_path(bp_mini_archive_t *ar, const char *arc_prefix,
                    const char *fs_path, bp_uint8 platform);

static int add_one_file(bp_mini_archive_t *ar, const char *arc_name,
                        const char *fs_path, bp_uint8 platform)
{
    struct stat st;
    bp_uint64 fsize;
    bp_uint64 mtime;
    FILE *fp;
    bp_io_t file_io;
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

    if (g_verbose) {
        printf("  a  %s\n", arc_name);
        fflush(stdout);
    }

    bp_io_from_file(&file_io, fp);
    rc = bp_mini_archive_add_entry(ar, arc_name, BP_TYPE_DATA, platform,
                                   &file_io, fsize, mtime);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": error adding %s: %s\n",
                arc_name, bp_mini_strerror(rc));
    }
    return rc;
}

static int add_dir_entry(bp_mini_archive_t *ar, const char *arc_name,
                         bp_uint8 platform)
{
    bp_uint64 z;
    z.lo = 0; z.hi = 0;

    if (g_verbose) {
        printf("  a  %s/\n", arc_name);
    }

    return bp_mini_archive_add_entry(ar, arc_name, BP_TYPE_DIR, platform,
                                     NULL, z, z);
}

static int add_path(bp_mini_archive_t *ar, const char *arc_prefix,
                    const char *fs_path, bp_uint8 platform)
{
    struct stat st;
    DIR *dp;
    struct dirent *de;
    char *child_fs;
    char *child_arc;
    int rc = BP_OK;

    if (stat(fs_path, &st) != 0) {
        fprintf(stderr, PROG_NAME ": cannot stat %s\n", fs_path);
        return BP_ERR_IO;
    }

    if (!S_ISDIR(st.st_mode)) {
        return add_one_file(ar, arc_prefix, fs_path, platform);
    }

    /* It's a directory */
    if (arc_prefix[0]) {
        rc = add_dir_entry(ar, arc_prefix, platform);
        if (rc != BP_OK) return rc;
    }

    dp = opendir(fs_path);
    if (dp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open directory: %s\n", fs_path);
        return BP_ERR_IO;
    }

    child_fs  = (char *)malloc(BP_MAX_PATH);
    child_arc = (char *)malloc(BP_MAX_PATH);
    if (!child_fs || !child_arc) {
        free(child_fs); free(child_arc);
        closedir(dp);
        return BP_ERR_IO;
    }

    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        snprintf(child_fs, BP_MAX_PATH, "%s/%s", fs_path, de->d_name);
        if (arc_prefix[0]) {
            snprintf(child_arc, BP_MAX_PATH, "%s/%s", arc_prefix, de->d_name);
        } else {
            snprintf(child_arc, BP_MAX_PATH, "%s", de->d_name);
        }

        rc = add_path(ar, child_arc, child_fs, platform);
        if (rc != BP_OK) break;
    }

    free(child_fs);
    free(child_arc);
    closedir(dp);
    return rc;
}

/* ---- cmd_create -------------------------------------------------------- */

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
        fprintf(stderr, PROG_NAME ": cannot create %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);

    rc = bp_mini_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": compress init failed: %s\n",
                bp_mini_strerror(rc));
        fclose(fp);
        return 1;
    }

    rc = bp_mini_archive_create(&ar, &arc_io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": create failed: %s\n", bp_mini_strerror(rc));
        fclose(fp);
        return 1;
    }

    for (i = 0; i < argc; i++) {
        char arc_name[BP_MAX_PATH];
        char *p;

        strncpy(arc_name, argv[i], BP_MAX_PATH - 1);
        arc_name[BP_MAX_PATH - 1] = '\0';

        /* Strip leading "./" */
        while (arc_name[0] == '.' && arc_name[1] == '/') {
            memmove(arc_name, arc_name + 2, strlen(arc_name) - 1);
        }

        /* Strip trailing "/" */
        p = arc_name + strlen(arc_name) - 1;
        while (p > arc_name && *p == '/') { *p-- = '\0'; }

        rc = add_path(&ar, arc_name, argv[i], BP_OS_LINUX);
        if (rc != BP_OK) break;
    }

    if (rc == BP_OK) {
        rc = bp_mini_archive_finalize(&ar);
        if (rc != BP_OK) {
            fprintf(stderr, PROG_NAME ": finalize failed: %s\n",
                    bp_mini_strerror(rc));
        }
    }

    bp_mini_archive_close(&ar);
    fclose(fp);

    if (rc == BP_OK && g_verbose) {
        printf("Archive: %s (%u entr%s)\n", arc_path,
               (unsigned)ar.entries_written,
               ar.entries_written == 1 ? "y" : "ies");
    }
    return (rc == BP_OK) ? 0 : 1;
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
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);

    memset(&ctx, 0, sizeof(ctx));
    if (dest) {
        strncpy(ctx.dest, dest, BP_MAX_PATH - 1);
    }

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_extract_all(&arc_io,
                                         on_extract_open,
                                         on_extract_done,
                                         &ctx);
    }

    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": extract failed: %s\n",
                bp_mini_strerror(rc));
        return 1;
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
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);

    printf("  Type  Size          Name\n");
    printf("  ----  ------------  ----\n");
    rc = bp_mini_archive_list(&arc_io, on_list, NULL);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": list failed: %s\n", bp_mini_strerror(rc));
        return 1;
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
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_test(&arc_io, on_test, NULL);
    }
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": test failed: %s\n", bp_mini_strerror(rc));
        return 1;
    }
    return 0;
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
    FILE *fp;
    bp_io_t arc_io;
    bp_mini_archive_info_t info;
    char buf[32];
    int rc;

    fp = fopen(arc_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);
    rc = bp_mini_archive_info(&arc_io, &info);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": info failed: %s\n", bp_mini_strerror(rc));
        return 1;
    }

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
    return 0;
}

/* ---- usage ------------------------------------------------------------- */

static void usage(void)
{
    fputs(PROG_NAME " " BP_VER "  BoltPack mini for Linux (32-bit)\n"
          "Usage:\n"
          "  bpmini c <archive> <path>...   create archive\n"
          "  bpmini x <archive> [dest]      extract archive\n"
          "  bpmini l <archive>             list contents\n"
          "  bpmini t <archive>             test integrity\n"
          "  bpmini i <archive>             show info\n"
          "Options:\n"
          "  -b8k / -b16k / -b32k           block size (create only)\n"
          "  -m0                            store, no compression\n"
          "  -m1                            BoltLite compression\n"
          "  -m2                            boltlz compression (default)\n"
          "  -q                             quiet mode\n"
          "Compression:\n"
          "  create:   stored (0), BoltLite (1), boltlz (2)\n"
          "  extract:  stored (0), BoltLite (1), boltlz (2)\n",
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
    bp_uint8  method     = BP_METHOD_BOLTLZ;

    if (argc < 3) {
        usage();
        return 1;
    }

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-q") == 0) {
                g_verbose = 0;
            } else if (strcmp(argv[i], "-b8k") == 0) {
                block_size = 8192U;
            } else if (strcmp(argv[i], "-b16k") == 0) {
                block_size = 16384U;
            } else if (strcmp(argv[i], "-b32k") == 0) {
                block_size = 32768U;
            } else if (strcmp(argv[i], "-m0") == 0) {
                method = BP_METHOD_STORED;
            } else if (strcmp(argv[i], "-m1") == 0) {
                method = BP_METHOD_BOLTLITE;
            } else if (strcmp(argv[i], "-m2") == 0) {
                method = BP_METHOD_BOLTLZ;
            }
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
        return cmd_create(arc_path, argc - files_start, argv + files_start,
                          block_size, method);
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
    default:
        fprintf(stderr, PROG_NAME ": unknown command '%c'\n", cmd);
        usage();
        return 1;
    }
}
