/*
 * ql_volumes.h -- QDOS hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * QL CLI.  Volumes are stdio streams.  A new volume is created as
 * bpvNNN.tmp beside its final name and renamed into place after any older
 * file of that name has been removed.  Names follow the QDOS convention of
 * '_' between directories (win1_backup_dir_), and the numbered volumes take
 * the extension of the final name's stem: win1_backup.000, win1_backup.001,
 * ... win1_backup.blt.  QDOS paths are limited to 36 characters.
 *
 * Included by src/frontend/ql/main.c after bp_io_from_file() and
 * g_verbosity.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_QL_VOLUMES_H
#define BP_QL_VOLUMES_H

#include <errno.h>

#define BP_VOL_DIR_SEPS "_/"
#define BP_VOL_PATH_MAX BP_MAX_PATH

static int vol_hook_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return 0;
    fclose(fp);
    return 1;
}

static FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
{
    static unsigned long sequence = 0;
    const char *p, *sep = target;
    size_t prefix;
    int attempt;

    for (p = target; *p; p++)
        if (*p == '_' || *p == '/') sep = p + 1;
    prefix = (size_t)(sep - target);
    if (prefix + 12 >= tmp_size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    for (attempt = 0; attempt < 1000; attempt++) {
        memcpy(tmp, target, prefix);
        sprintf(tmp + prefix, "bpv%03lu.tmp", (sequence++) % 1000UL);
        if (!vol_hook_exists(tmp)) return fopen(tmp, "wb");
    }
    errno = EEXIST;
    return NULL;
}

/* A volume replaces any file of its name (its set is being rewritten);
 * QDOS rename does not replace, so remove the old file first. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    if (vol_hook_exists(target) && remove(target) != 0) return -1;
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

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* QL: no signal-time cleanup */
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

#include "../common/volume_size.h"
#include "../common/volume_files.h"

#endif /* BP_QL_VOLUMES_H */
