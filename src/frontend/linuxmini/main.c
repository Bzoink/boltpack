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

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

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
#include <fcntl.h>
#include <utime.h>
#include "../posix/extract_paths.h"

/* ---- constants --------------------------------------------------------- */

#define BP_MAX_PATH   4096
#define PROG_NAME     "bpmini"
#define BP_XSTR(x)    BP_STR(x)
#define BP_STR(x)     #x
#define BP_VER        BP_XSTR(BP_VERSION_MAJOR) "." BP_XSTR(BP_VERSION_MINOR)

/* ---- globals ----------------------------------------------------------- */

static int g_verbose = 1;
static int g_create_partial = 0;

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

static int set_mtime_fd(int fd, bp_uint64 mtime)
{
    bp_off_t seconds = bp_u64_to_off(mtime);
    time_t t = (time_t)seconds;
    struct timespec times[2];
    if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
        (bp_off_t)t != seconds) return BP_ERR_OVERFLOW;
    times[0].tv_sec = 0; times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = t; times[1].tv_nsec = 0;
    return futimens(fd, times) == 0 ? BP_OK : BP_ERR_IO;
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

typedef struct mini_restore_dir_s {
    struct mini_restore_dir_s *next;
    char *path;
    bp_uint16 flags;
    bp_uint64 mtime;
} mini_restore_dir_t;

typedef struct {
    char dest[BP_MAX_PATH];
    int root_fd;
    mini_restore_dir_t *dirs;
    unsigned dir_count;
    int error;
    int partial;
} extract_ctx_t;

typedef struct {
    bp_io_t io;
    int parent_fd;
    char *temporary;
    char *destination;
} mini_extract_output_t;

static int queue_directory(extract_ctx_t *ctx, const char *path, const bp_mini_entry_t *e)
{
    mini_restore_dir_t *node, **at;
    size_t n = strlen(path);
    if (ctx->dir_count >= 1024U) return BP_ERR_OVERFLOW;
    node = (mini_restore_dir_t *)malloc(sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->path = (char *)malloc(n + 1);
    if (node->path == NULL) { free(node); return BP_ERR_NOMEM; }
    memcpy(node->path, path, n + 1);
    node->flags = e->flags;
    node->mtime = e->mtime;
    at = &ctx->dirs;
    while (*at != NULL && strlen((*at)->path) >= n) at = &(*at)->next;
    node->next = *at; *at = node; ctx->dir_count++;
    return BP_OK;
}

static int finish_directories(extract_ctx_t *ctx, int apply)
{
    int rc = BP_OK;
    while (ctx->dirs != NULL) {
        mini_restore_dir_t *node = ctx->dirs;
        ctx->dirs = node->next;
        if (apply) {
            int fd = extract_open_directory(ctx->root_fd, node->path, 0);
            if (fd < 0) rc = BP_ERR_IO;
            else {
                if (node->flags & BP_ENTRY_FLAG_MODE_VALID) {
                    if (node->flags & 06000U) {
                        fprintf(stderr, PROG_NAME ": set-ID bits not restored: %s\n", node->path);
                        ctx->partial = 1;
                    }
                    if (fchmod(fd, (mode_t)(node->flags & 01777U)) != 0) rc = BP_ERR_IO;
                }
                if ((node->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
                    set_mtime_fd(fd, node->mtime) != BP_OK) rc = BP_ERR_IO;
                close(fd);
            }
        }
        free(node->path); free(node);
    }
    return rc;
}

static bp_io_t *on_extract_open(const bp_mini_entry_t *e, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    mini_extract_output_t *output;
    char leaf[BP_MAX_PATH], temporary[64];
    int parent_fd, fd, tries;
    FILE *fp;
    static unsigned long temporary_counter;

    if (!bp_mini_name_is_safe(e->name)) { ctx->partial = 1; return NULL; }
    if (e->entry_type == BP_TYPE_DIR) {
        fd = extract_open_directory(ctx->root_fd, e->name, 1);
        if (fd < 0) {
            fprintf(stderr, PROG_NAME ": unsafe or inaccessible directory: %s\n", e->name);
            ctx->error = BP_ERR_IO;
        } else {
            int dir_rc;
            close(fd);
            dir_rc = queue_directory(ctx, e->name, e);
            if (dir_rc != BP_OK && ctx->error == BP_OK) ctx->error = dir_rc;
        }
        return NULL;
    }
    if (g_verbose) {
        printf("  x  "); print_utf8_safe(e->name, e->name_len); putchar('\n');
    }
    parent_fd = extract_open_parent(ctx->root_fd, e->name, leaf, sizeof(leaf));
    if (parent_fd < 0) {
        fprintf(stderr, PROG_NAME ": unsafe or inaccessible directory: %s\n", e->name);
        ctx->error = BP_ERR_IO;
        return NULL;
    }
    {
        struct stat st;
        if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
            fprintf(stderr, PROG_NAME ": file exists or cannot be inspected: %s\n", e->name);
            close(parent_fd);
            return NULL;
        }
    }
    output = (mini_extract_output_t *)calloc(1, sizeof(*output));
    if (output == NULL) { close(parent_fd); ctx->error = BP_ERR_NOMEM; return NULL; }
    output->parent_fd = parent_fd;
    output->destination = (char *)malloc(strlen(leaf) + 1);
    output->temporary = (char *)malloc(sizeof(temporary));
    if (output->destination == NULL || output->temporary == NULL) {
        close(parent_fd); free(output->destination); free(output->temporary); free(output);
        ctx->error = BP_ERR_NOMEM; return NULL;
    }
    strcpy(output->destination, leaf);
    fd = -1;
    for (tries = 0; tries < 256; tries++) {
        sprintf(temporary, ".boltpack-%08lx-%08lx", (unsigned long)getpid(), ++temporary_counter);
        fd = openat(parent_fd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        fprintf(stderr, PROG_NAME ": cannot create temporary file for %s\n", e->name);
        close(parent_fd); free(output->destination); free(output->temporary); free(output);
        ctx->error = BP_ERR_IO; return NULL;
    }
    strcpy(output->temporary, temporary);
    fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd); unlinkat(parent_fd, temporary, 0); close(parent_fd);
        free(output->destination); free(output->temporary); free(output);
        ctx->error = BP_ERR_IO; return NULL;
    }
    bp_io_from_file(&output->io, fp);
    return &output->io;
}

static int on_extract_done(const bp_mini_entry_t *e, bp_io_t *out,
                           int status, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    mini_extract_output_t *output = (mini_extract_output_t *)out;
    FILE *fp = (FILE *)out->handle;
    int rc = BP_OK;
    if (status == BP_OK) {
        if (fflush(fp) != 0) rc = BP_ERR_IO;
        if (rc == BP_OK && (e->flags & BP_ENTRY_FLAG_MODE_VALID)) {
            if (e->flags & 06000U) {
                fprintf(stderr, PROG_NAME ": set-ID bits not restored: %s\n", e->name);
                ctx->partial = 1;
            }
            if (fchmod(fileno(fp), (mode_t)(e->flags & 01777U)) != 0) rc = BP_ERR_IO;
        }
        if (rc == BP_OK && (e->flags & BP_ENTRY_FLAG_MTIME_VALID))
            rc = set_mtime_fd(fileno(fp), e->mtime);
    }
    if (fclose(fp) != 0 && rc == BP_OK && status == BP_OK) rc = BP_ERR_IO;
    if (status == BP_OK && rc == BP_OK &&
        extract_commit_noreplace(output->parent_fd, output->temporary, output->destination) != 0)
        rc = BP_ERR_IO;
    if (status != BP_OK || rc != BP_OK) {
        if (rc != BP_OK) fprintf(stderr, PROG_NAME ": cannot finish extracting %s\n", e->name);
        (void)unlinkat(output->parent_fd, output->temporary, 0);
    }
    close(output->parent_fd);
    free(output->temporary); free(output->destination); free(output);
    return rc;
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
    bp_uint16 flags;
    int rc;

    if (stat(fs_path, &st) != 0) {
        fprintf(stderr, PROG_NAME ": cannot stat %s\n", fs_path);
        return BP_ERR_IO;
    }
    if (st.st_size < 0) return BP_ERR_OVERFLOW;

    fsize.lo = (bp_uint32)(unsigned long)st.st_size;
    fsize.hi = 0;

    flags = BP_ENTRY_FLAG_MODE_VALID | (bp_uint16)(st.st_mode & BP_ENTRY_FLAG_MODE_MASK);
    if (st.st_mtime < 0) {
        fprintf(stderr, PROG_NAME ": pre-1970 timestamp not representable: %s\n", fs_path);
        mtime = bp_u64_from_u32(0);
        g_create_partial = 1;
    } else {
        mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        flags |= BP_ENTRY_FLAG_MTIME_VALID;
    }

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
    rc = bp_mini_archive_add_entry_ex(ar, arc_name, BP_TYPE_DATA, platform,
                                      &file_io, fsize, mtime, flags);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, PROG_NAME ": error adding %s: %s\n",
                arc_name, bp_mini_strerror(rc));
    }
    return rc;
}

static int add_dir_entry(bp_mini_archive_t *ar, const char *arc_name,
                         bp_uint8 platform, const struct stat *st)
{
    bp_uint64 z;
    bp_uint64 mtime;
    bp_uint16 flags;
    size_t name_len;
    char *canonical_name;
    int rc;
    z.lo = 0; z.hi = 0;

    name_len = strlen(arc_name);
    if (name_len == 0) return BP_ERR_INVALID_ARG;
    if (name_len >= BP_MAX_PATH ||
        (arc_name[name_len - 1] != '/' && name_len + 1 >= BP_MAX_PATH))
        return BP_ERR_NAME_TOO_LONG;
    canonical_name = (char *)malloc(name_len + 2);
    if (canonical_name == NULL) return BP_ERR_NOMEM;
    memcpy(canonical_name, arc_name, name_len);
    if (canonical_name[name_len - 1] != '/')
        canonical_name[name_len++] = '/';
    canonical_name[name_len] = '\0';

    if (g_verbose) {
        printf("  a  %s\n", canonical_name);
    }

    flags = BP_ENTRY_FLAG_MODE_VALID |
            (bp_uint16)(st->st_mode & BP_ENTRY_FLAG_MODE_MASK);
    mtime = z;
    if (st->st_mtime >= 0) {
        mtime = bp_off_to_u64((bp_off_t)st->st_mtime);
        flags |= BP_ENTRY_FLAG_MTIME_VALID;
    } else {
        fprintf(stderr, PROG_NAME ": pre-1970 directory timestamp not representable: %s\n", arc_name);
        g_create_partial = 1;
    }
    rc = bp_mini_archive_add_entry_ex(ar, canonical_name, BP_TYPE_DIR,
                                      platform, NULL, z, mtime, flags);
    free(canonical_name);
    return rc;
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
        rc = add_dir_entry(ar, arc_prefix, platform, &st);
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
    return (rc == BP_OK && !g_create_partial) ? 0 : 1;
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
        if (strlen(dest) >= sizeof(ctx.dest)) {
            fprintf(stderr, PROG_NAME ": output directory is too long\n");
            fclose(fp);
            return 1;
        }
        strncpy(ctx.dest, dest, BP_MAX_PATH - 1);
    }
    if (ctx.dest[0]) {
        make_parents(ctx.dest);
        if (mkdir(ctx.dest, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, PROG_NAME ": cannot create output directory: %s\n", ctx.dest);
            fclose(fp);
            return 1;
        }
    }
    ctx.root_fd = open(ctx.dest[0] ? ctx.dest : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (ctx.root_fd < 0) {
        fprintf(stderr, PROG_NAME ": cannot open output directory: %s\n", ctx.dest);
        fclose(fp);
        return 1;
    }

    rc = bp_mini_compress_init();
    if (rc == BP_OK) {
        rc = bp_mini_archive_extract_all_ex(&arc_io,
                                         on_extract_open,
                                         on_extract_done,
                                         &ctx);
    }

    fclose(fp);
    if (rc == BP_OK && ctx.error != BP_OK) rc = ctx.error;
    {
        int dir_rc = finish_directories(&ctx, rc == BP_OK || rc == BP_ERR_PARTIAL);
        if (rc == BP_OK && dir_rc != BP_OK) rc = dir_rc;
    }
    if (rc == BP_OK && ctx.partial) rc = BP_ERR_PARTIAL;
    close(ctx.root_fd);

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
