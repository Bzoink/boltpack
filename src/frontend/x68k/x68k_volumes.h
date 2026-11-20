/*
 * x68k_volumes.h -- Human68k hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * X68000 CLI.  Volumes are stdio streams.  A new volume is created as a
 * TMP-numbered sibling of its final name (x68k_create_archive_stage) and
 * renamed into place once complete, after any older regular file of that
 * name has been removed (Human68k rename does not replace).
 *
 * Included by src/frontend/x68k/main.c after x68k_create_archive_stage()
 * and x68k_path_exists().
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_X68K_VOLUMES_H
#define BP_X68K_VOLUMES_H

#include <unistd.h>

#define BP_VOL_DIR_SEPS "/\\:"
#define BP_VOL_PATH_MAX BP_MAX_PATH

static FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
{
    FILE *fp;
    if (x68k_create_archive_stage(target, tmp, tmp_size) != BP_OK) {
        errno = EACCES;
        return NULL;
    }
    fp = fopen(tmp, "wb");
    if (fp == NULL) remove(tmp);
    return fp;
}

/* A volume replaces a regular file of its name (its set is being
 * rewritten), but never a directory or a read-only file. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    struct dos_filbuf found;
    if (x68k_path_exists(target, &found)) {
        if ((found.atr & (FA_SUBDIR | FA_RDONLY)) != 0 || remove(target) != 0)
            return -1;
    }
    return rename(tmp, target);
}

static int vol_hook_sync(FILE *fp)
{
    return fflush(fp);
}

static int vol_hook_interactive(void)
{
    return isatty(0);
}

static int vol_hook_exists(const char *path)
{
    return x68k_path_exists(path, NULL);
}

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* X68000: no signal-time cleanup */
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

#include "../common/volume_size.h"
#include "../common/volume_files.h"

#endif /* BP_X68K_VOLUMES_H */
