/*
 * riscos_volumes.h -- RISC OS hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * RISC OS CLI.  Volumes are native RISC OS file handles (OS_Find, OS_GBPB),
 * not stdio streams.  A new volume is created under an unused BPVnnnn name
 * beside its final name and renamed into place (OS_FSControl 25) after any
 * older file of that name has been deleted.
 *
 * RISC OS separates directories with '.' (and the filing system from the
 * disc with ':'), and marks a file extension with '/', so a set is
 * ADFS::0.$.Backup/000, Backup/001, ... and the last Backup/blt.
 *
 * Included by src/frontend/riscos/main.c after riscos_archive_open_read(),
 * riscos_archive_open_write(), riscos_stat(), riscos_unused_sibling(),
 * riscos_delete() and riscos_rename().
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_RISCOS_VOLUMES_H
#define BP_RISCOS_VOLUMES_H

#include <errno.h>
#include <unistd.h>

#define BP_VOL_OWN_STREAMS
#define BP_VOL_OWN_GETLINE
#define BP_VOL_DIR_SEPS ".:"
#define BP_VOL_EXT_CHAR '/'
#define BP_VOL_EXT_STR "/"
#define BP_VOL_PATH_MAX 512

typedef int bp_vol_stream_t;
#define BP_VOL_NO_STREAM 0   /* OS_Find returns 0 for "no file" */

static bp_vol_stream_t vol_hook_open_read(const char *path)
{
    return riscos_archive_open_read(path);
}

static int vol_hook_close(bp_vol_stream_t s)
{
    return riscos_close(s);
}

/* OS_GBPB writes through; OS_Args 255 asks the filing system to flush */
static int vol_hook_flush(bp_vol_stream_t s)
{
    (void)s;
    return 0;
}

static int vol_hook_sync(bp_vol_stream_t s)
{
    _kernel_swi_regs r;
    r.r[0] = 255;
    r.r[1] = s;
    return _kernel_swi(OS_Args, &r, &r) == NULL ? 0 : -1;
}

static long vol_hook_read(bp_vol_stream_t s, void *buf, unsigned n)
{
    return riscos_io_read(riscos_fh_to_handle(s), buf, (bp_uint32)n);
}

static void vol_hook_io(bp_io_t *io, bp_vol_stream_t s)
{
    bp_io_from_riscos(io, s);
}

static int vol_hook_getline(char *buf, int size)
{
    return fgets(buf, size, stdin) != NULL;
}

static bp_vol_stream_t vol_hook_create(const char *target, char *tmp,
                                       size_t tmp_size)
{
    if (riscos_unused_sibling(target, "BPV", tmp, tmp_size) != 0)
        return BP_VOL_NO_STREAM;
    return riscos_archive_open_write(tmp);
}

/* A volume replaces a file of its name (its set is being rewritten), but
 * never a directory; OS_FSControl rename does not replace. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    riscos_stat_t st;
    int type = riscos_stat(target, &st);
    if (type != RISCOS_OBJ_NONE) {
        if (type != RISCOS_OBJ_FILE || riscos_delete(target) != 0) return -1;
    }
    return riscos_rename(tmp, target);
}

static int vol_hook_interactive(void)
{
    return isatty(0);
}

static int vol_hook_exists(const char *path)
{
    riscos_stat_t st;
    return riscos_stat(path, &st) != RISCOS_OBJ_NONE;
}

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* RISC OS: no signal-time cleanup */
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

#include "../common/volume_size.h"
#include "../common/volume_files.h"

#endif /* BP_RISCOS_VOLUMES_H */
