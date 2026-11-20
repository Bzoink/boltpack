/*
 * dos_volumes.h -- DOS hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * DOS CLI.  Temporary volumes are 8.3-safe BPxxxxxx.TMP siblings created
 * with O_EXCL, and are moved into place with a no-replace rename after any
 * old file of the same name is removed.  A volume set for diskettes
 * is typically BACKUP.000, BACKUP.001, ... BACKUP.BLT.
 *
 * Included by src/frontend/dos/main.c after bp_io_from_file(),
 * dos_archive_open_stage() and dos_rename_no_replace().
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_DOS_VOLUMES_H
#define BP_DOS_VOLUMES_H

#define BP_VOL_DIR_SEPS "/\\:"
#define BP_VOL_PATH_MAX 512

static FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
{
    return dos_archive_open_stage(target, tmp, tmp_size);
}

/* A volume replaces any file of its name: the set it belongs to is being
 * rewritten anyway, so the identity checks that guard a single archive's
 * replacement do not apply.  DOS rename does not replace, so remove first. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    struct stat st;
    if (lstat(target, &st) == 0) {
        if (!S_ISREG(st.st_mode) || remove(target) != 0) return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    return dos_rename_no_replace(tmp, target);
}

static int vol_hook_sync(FILE *fp)
{
    return fsync(fileno(fp));
}

static int vol_hook_interactive(void)
{
    return isatty(0);
}

static int vol_hook_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* DOS: no signal-time cleanup */
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

#include "../common/volume_files.h"

#endif /* BP_DOS_VOLUMES_H */
