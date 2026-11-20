/*
 * volume_files.h -- POSIX hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * Linux CLI and the POSIX CLIs built from it: temporary volumes come from
 * mkstemp() in the target directory, are fsync()ed, and are renamed into
 * place; the user is asked for media when standard input is a terminal.
 *
 * Included by src/frontend/linux/main.c after its FILE * stream adapters
 * (bp_io_from_file), g_verbosity and g_tmp_archive.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_POSIX_VOLUME_FILES_H
#define BP_POSIX_VOLUME_FILES_H

#define BP_VOL_PATH_MAX 4096

static FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
{
    const char *slash = strrchr(target, '/');
    size_t dir_len;
    int n, fd;
    FILE *fp;

    if (slash == NULL) {
        n = snprintf(tmp, tmp_size, "./bpvol_XXXXXX");
    } else {
        dir_len = slash == target ? 1U : (size_t)(slash - target);
        n = snprintf(tmp, tmp_size, "%.*s/bpvol_XXXXXX", (int)dir_len, target);
    }
    if (n <= 0 || (size_t)n >= tmp_size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    fd = mkstemp(tmp);
    if (fd < 0) return NULL;
    fp = fdopen(fd, "w+b");
    if (fp == NULL) {
        close(fd);
        unlink(tmp);
    }
    return fp;
}

static int vol_hook_publish(const char *tmp, const char *target)
{
    return rename(tmp, target);
}

static int vol_hook_sync(FILE *fp)
{
    return fsync(fileno(fp));
}

static int vol_hook_interactive(void)
{
    return isatty(STDIN_FILENO);
}

static int vol_hook_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void vol_hook_note_tmp(const char *tmp)
{
    /* the signal handler removes g_tmp_archive */
    size_t n = strlen(tmp);
    if (n >= sizeof(g_tmp_archive)) n = sizeof(g_tmp_archive) - 1;
    memcpy(g_tmp_archive, tmp, n);
    g_tmp_archive[n] = '\0';
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

#include "../common/volume_files.h"

#endif /* BP_POSIX_VOLUME_FILES_H */
