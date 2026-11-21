/*
 * atari_volumes.h -- Atari TOS hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * Atari CLI.  Volumes are stdio streams.  A new volume is created with
 * mkstemp() as a BPXXXXXX 8.3-safe sibling of its final name and moved into
 * place with Frename() after any older file of that name has been removed
 * (GEMDOS rename does not replace).  A volume set for diskettes is typically
 * BACKUP.000, BACKUP.001, ... BACKUP.BLT.
 *
 * Included by src/frontend/atari/main.c after atari_make_temp_template() and
 * atari_rename_noreplace().
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_ATARI_VOLUMES_H
#define BP_ATARI_VOLUMES_H

#define BP_VOL_DIR_SEPS "/\\:"
#define BP_VOL_PATH_MAX BP_MAX_PATH

static FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
{
    int fd;
    FILE *fp;

    if (atari_make_temp_template(target, tmp, tmp_size) != 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    fd = mkstemp(tmp);
    if (fd < 0) return NULL;
    fp = fdopen(fd, "w+b");
    if (fp == NULL) {
        close(fd);
        remove(tmp);
    }
    return fp;
}

/* A volume replaces any regular file of its name (its set is being
 * rewritten), but never a directory or a read-only file. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    struct stat st;
    unsigned char attributes;
    if (lstat(target, &st) == 0) {
        if (!S_ISREG(st.st_mode) ||
            atari_get_attrib(target, &attributes) != 0 ||
            (attributes & FA_RDONLY) != 0 || remove(target) != 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    return atari_rename_noreplace(tmp, target);
}

static int vol_hook_sync(FILE *fp)
{
    return fflush(fp);
}

#ifndef BP_VOL_OWN_PROMPT
static int vol_hook_interactive(void)
{
    return isatty(0);
}
#endif

static int vol_hook_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* Atari: no signal-time cleanup */
}

#ifndef BP_VOL_OWN_PROMPT
static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}
#endif

#include "../common/volume_size.h"
#include "../common/volume_files.h"

#endif /* BP_ATARI_VOLUMES_H */
