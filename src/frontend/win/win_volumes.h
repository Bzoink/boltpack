/*
 * win_volumes.h -- Windows hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * Windows CLI.  Temporary volumes are created with GetTempFileName in the
 * directory of the volume's final name, flushed to the device with _commit
 * and moved into place with MoveFileEx (replacing any older file of the
 * same name).  All names are UTF-8: fopen, DeleteFileA, MoveFileExA,
 * GetFileAttributesA and GetTempFileNameA are the wide-character wrappers
 * defined in main.c.
 *
 * Included by src/frontend/win/main.c after bp_io_from_file(), g_verbosity
 * and g_tmp_archive.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_WIN_VOLUMES_H
#define BP_WIN_VOLUMES_H

#define BP_VOL_DIR_SEPS "/\\:"
#define BP_VOL_PATH_MAX 1024

/* volume_files.h removes files with remove(), which would use the ANSI
 * code page */
static int win_vol_remove(const char *path)
{
    return DeleteFileA(path) ? 0 : -1;
}
#define remove win_vol_remove

static FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
{
    char dir[BP_VOL_PATH_MAX + 4];
    const char *p, *sep = NULL;
    size_t n;

    for (p = target; *p; p++)
        if (strchr(BP_VOL_DIR_SEPS, *p) != NULL) sep = p;
    if (sep == NULL) {
        strcpy(dir, ".");
    } else {
        n = (size_t)(sep - target);
        if (n >= BP_VOL_PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
        memcpy(dir, target, n);
        dir[n] = '\0';
        if (n == 0) strcpy(dir, "\\");                 /* \name: the root */
        else if (dir[n - 1] == ':') strcat(dir, *sep == ':' ? "." : "\\");
    }
    if (!GetTempFileNameA(dir, "bpv", 0, tmp)) {       /* creates the file */
        errno = EACCES;
        return NULL;
    }
    (void)tmp_size;
    {
        FILE *fp = fopen(tmp, "w+b");
        if (fp == NULL) DeleteFileA(tmp);
        return fp;
    }
}

static int vol_hook_publish(const char *tmp, const char *target)
{
    return MoveFileExA(tmp, target,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
           ? 0 : -1;
}

static int vol_hook_sync(FILE *fp)
{
    return _commit(_fileno(fp));
}

static int vol_hook_interactive(void)
{
    return _isatty(_fileno(stdin));
}

static int vol_hook_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void vol_hook_note_tmp(const char *tmp)
{
    /* the console handler removes g_tmp_archive on Ctrl+C */
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

#undef remove

#endif /* BP_WIN_VOLUMES_H */
