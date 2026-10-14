/*
 * main.c -- CP/M-86 CLI frontend for BoltPack mini
 *
 * Target: Aztec C 86, or any ANSI C89 compiler for CP/M-86.
 * Binary: BPMINI.CMD
 *
 * CP/M-86 constraints:
 *   - Flat filesystem only (no subdirectory support in standard CP/M-86).
 *   - File names are 8.3 uppercase.
 *   - No mtime (set to zero in created archives; ignored on extract).
 *   - No recursive directory scanning; filenames taken directly from argv.
 *   - On extract: archive path components are stripped; name is folded to
 *     8.3 uppercase; duplicate names in the same archive produce a warning
 *     and the later entry is skipped.
 *   - No utime or stat mtime field; st_size is available via fseek/ftell.
 *
 * Compiler:  cc -DSIZEOF_INT=2 -DSIZEOF_LONG=4  -DBP_MINI
 *               -DBP_CREATOR_OS=BP_OS_CPM86 -DBP_CREATOR_CPU=BP_CPU_8086
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/mini/archiver.h"
#include "../mini_shared/mini_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- constants --------------------------------------------------------- */

#define BP_MAX_PATH  14   /* 8 + '.' + 3 + NUL = 13 chars on CP/M */
#define PROG_NAME    "BPMINI"
#define BP_XSTR(x)   BP_STR(x)
#define BP_STR(x)    #x
#define BP_VER       BP_XSTR(BP_VERSION_MAJOR) "." BP_XSTR(BP_VERSION_MINOR)

/* ---- 8.3 name utilities ------------------------------------------------ */

/*
 * Fold an archive path to a flat CP/M 8.3 uppercase name.
 * Strips directory components and truncates base/ext to 8/3 chars.
 */
static void to_8dot3(const char *arc_name, char *out)
{
    const char *base;
    const char *dot;
    const char *p;
    int b_len;
    int e_len;
    int i;

    /* Find the last directory separator */
    base = arc_name;
    for (p = arc_name; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }

    /* Find extension */
    dot = NULL;
    for (p = base; *p; p++) {
        if (*p == '.') dot = p;
    }

    if (dot == NULL) {
        /* No extension */
        b_len = (int)(p - base);
        if (b_len > 8) b_len = 8;
        for (i = 0; i < b_len; i++) {
            out[i] = (char)toupper((unsigned char)base[i]);
        }
        out[b_len] = '\0';
    } else {
        b_len = (int)(dot - base);
        if (b_len > 8) b_len = 8;
        for (i = 0; i < b_len; i++) {
            out[i] = (char)toupper((unsigned char)base[i]);
        }
        out[b_len] = '.';
        e_len = (int)strlen(dot + 1);
        if (e_len > 3) e_len = 3;
        for (i = 0; i < e_len; i++) {
            out[b_len + 1 + i] = (char)toupper((unsigned char)dot[1 + i]);
        }
        out[b_len + 1 + e_len] = '\0';
    }
}

/* ---- get file size without stat ---------------------------------------- */

static long file_size_of(const char *path)
{
    FILE *fp;
    long sz;

    fp = fopen(path, "rb");
    if (fp == NULL) return -1;
    if (fseek(fp, 0L, 2) != 0) { fclose(fp); return -1; }
    sz = ftell(fp);
    fclose(fp);
    return sz;
}

/* ---- list callback ----------------------------------------------------- */

static int on_list(const bp_mini_entry_t *e, void *user_data)
{
    char size_buf[32];
    (void)user_data;
    format_size64_mini(e->original_size, size_buf);

    if (e->entry_type == BP_TYPE_DIR) {
        printf("D  %-12s  ", size_buf);
    } else {
        printf("   %-12s  ", size_buf);
    }
    print_utf8_safe(e->name, (unsigned int)e->name_len);
    putchar('\n');
    return 0;
}

/* ---- extract callbacks (flat -- no dir creation) ----------------------- */

/* Track extracted names to detect collisions */
#define MAX_EXTRACTED 256
static char g_extracted[MAX_EXTRACTED][BP_MAX_PATH];
static int  g_extracted_count = 0;

static int already_extracted(const char *name)
{
    int i;
    for (i = 0; i < g_extracted_count; i++) {
        if (strcmp(g_extracted[i], name) == 0) return 1;
    }
    return 0;
}

static void record_extracted(const char *name)
{
    if (g_extracted_count < MAX_EXTRACTED) {
        strncpy(g_extracted[g_extracted_count], name, BP_MAX_PATH - 1);
        g_extracted[g_extracted_count][BP_MAX_PATH - 1] = '\0';
        g_extracted_count++;
    }
}

static bp_io_t *on_extract_open(const bp_mini_entry_t *e, void *user_data)
{
    char flat[BP_MAX_PATH];
    FILE *fp;

    (void)user_data;

    if (e->entry_type == BP_TYPE_DIR) {
        return NULL; /* CP/M-86: no directories */
    }

    to_8dot3(e->name, flat);

    if (already_extracted(flat)) {
        fprintf(stderr, PROG_NAME ": skip duplicate %s (from %s)\n",
                flat, e->name);
        return NULL;
    }

    printf("  x  %s\n", flat);

    fp = fopen(flat, "wb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot create %s\n", flat);
        return NULL;
    }

    record_extracted(flat);

    {
        static bp_io_t io;
        bp_io_from_file(&io, fp);
        return &io;
    }
}

static void on_extract_done(const bp_mini_entry_t *e, bp_io_t *out,
                            void *user_data)
{
    (void)e;
    (void)user_data;
    if (out == NULL) return;
    fclose((FILE *)out->handle);
    /* No mtime on CP/M-86 */
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

static int cmd_create(const char *arc_path, int argc, char **argv,
                      bp_uint32 block_size, bp_uint8 method)
{
    FILE *fp;
    bp_io_t arc_io;
    bp_mini_archive_t ar;
    int i;
    int rc;

    fp = fopen(arc_path, "wb");
    if (fp == NULL) {
        fprintf(stderr, PROG_NAME ": cannot create %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);

    rc = bp_mini_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": compress init: %s\n",
                bp_mini_strerror(rc));
        fclose(fp);
        return 1;
    }

    rc = bp_mini_archive_create(&ar, &arc_io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": create: %s\n", bp_mini_strerror(rc));
        fclose(fp);
        return 1;
    }

    for (i = 0; i < argc; i++) {
        FILE *in;
        bp_io_t file_io;
        long sz;
        bp_uint64 fsize;
        bp_uint64 mtime;
        char arc_name[BP_MAX_PATH];

        /* CP/M: filenames are flat; normalise to 8.3 in the archive too */
        to_8dot3(argv[i], arc_name);

        sz = file_size_of(argv[i]);
        if (sz < 0) {
            fprintf(stderr, PROG_NAME ": cannot size %s\n", argv[i]);
            continue;
        }
        fsize.lo = (bp_uint32)(unsigned long)sz;
        fsize.hi = 0;
        mtime.lo = 0;
        mtime.hi = 0;

        printf("  a  %s\n", arc_name);

        in = fopen(argv[i], "rb");
        if (in == NULL) {
            fprintf(stderr, PROG_NAME ": cannot open %s\n", argv[i]);
            continue;
        }

        bp_io_from_file(&file_io, in);
        rc = bp_mini_archive_add_entry(&ar, arc_name, BP_TYPE_DATA,
                                       BP_OS_CPM86, &file_io, fsize, mtime);
        fclose(in);

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
        fprintf(stderr, PROG_NAME ": finalize: %s\n", bp_mini_strerror(rc));
        return 1;
    }
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------- */

static int cmd_extract(const char *arc_path)
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
    g_extracted_count = 0;

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_extract_all(&arc_io, on_extract_open,
                                         on_extract_done, NULL);
    }
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": extract: %s\n", bp_mini_strerror(rc));
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
        fprintf(stderr, PROG_NAME ": list: %s\n", bp_mini_strerror(rc));
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
        fprintf(stderr, PROG_NAME ": test: %s\n", bp_mini_strerror(rc));
        return 1;
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
        fprintf(stderr, PROG_NAME ": cannot open %s\n", arc_path);
        return 1;
    }
    bp_io_from_file(&arc_io, fp);
    rc = bp_mini_archive_info(&arc_io, &info);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": info: %s\n", bp_mini_strerror(rc));
        return 1;
    }

    printf("Entries: %lu\n", (unsigned long)info.entry_count);
    format_size64_mini(info.total_original, buf);
    printf("Total:   %s\n", buf);
    {
        static const char * const mn[4] = {"stored","BoltLite","boltlz","fluff"};
        int j;
        int mfirst;
        printf("Methods: ");
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
    fputs(PROG_NAME " " BP_VER "  BoltPack mini for CP/M-86\n"
          "Usage:\n"
          "  BPMINI c <archive> <file>...   create archive (files only, no dirs)\n"
          "  BPMINI x <archive>             extract (flat, 8.3 names)\n"
          "  BPMINI l <archive>             list contents\n"
          "  BPMINI t <archive>             test integrity\n"
          "  BPMINI i <archive>             show info\n"
          "Options:\n"
          "  -b8k / -b16k / -b32k           block size (create only)\n"
          "  -m0                            store, no compression\n"
          "  -m1                            BoltLite compression (default)\n"
          "Compression:\n"
          "  create:   store (0), BoltLite (1)\n"
          "  extract:  store (0), BoltLite (1), boltlz (2)\n",
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
    bp_uint8  method     = BP_METHOD_BOLTLITE;

    if (argc < 3) { usage(); return 1; }

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-b8k") == 0)  { block_size = 8192U; }
            else if (strcmp(argv[i], "-b16k") == 0) { block_size = 16384U; }
            else if (strcmp(argv[i], "-b32k") == 0) { block_size = 32768U; }
            else if (strcmp(argv[i], "-m0") == 0)   { method = BP_METHOD_STORED; }
            else if (strcmp(argv[i], "-m1") == 0)   { method = BP_METHOD_BOLTLITE;   }
            else if (strcmp(argv[i], "-m2") == 0)   {
                fputs("bpmini: boltlz not supported on 16-bit\n", stderr);
                return 1;
            }
        }
    }

    cmd      = argv[1][0];
    arc_path = argv[2];
    files_start = 3;
    while (files_start < argc && argv[files_start][0] == '-') files_start++;

    switch (cmd) {
    case 'c':
        return cmd_create(arc_path, argc - files_start, argv + files_start,
                          block_size, method);
    case 'x':
        return cmd_extract(arc_path);
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
