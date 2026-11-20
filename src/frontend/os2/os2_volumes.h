/*
 * os2_volumes.h -- OS/2 hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * OS/2 CLI.  Volumes are native OS/2 file handles (DosOpen/DosRead/DosWrite),
 * not stdio streams.  A new volume is created exclusively under a BPxxxxxx.TMP
 * name beside its final name (os2_make_temp_path), flushed with
 * DosResetBuffer and moved into place with DosMove after any old file of that
 * name has been deleted.  A volume set for diskettes is typically
 * BACKUP.000, BACKUP.001, ... BACKUP.BLT.
 *
 * Only one volume stream is open at a time (a set is read or written one
 * volume after the other), so the stream is a single static record.
 *
 * Included by src/frontend/os2/main.c after os2_make_temp_path(),
 * os2_archive_io_t and bp_io_from_os2_handle().
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_OS2_VOLUMES_H
#define BP_OS2_VOLUMES_H

#define BP_VOL_OWN_STREAMS
#define BP_VOL_DIR_SEPS "/\\:"
#define BP_VOL_PATH_MAX 260

typedef os2_archive_io_t *bp_vol_stream_t;
#define BP_VOL_NO_STREAM ((bp_vol_stream_t)0)

static os2_archive_io_t g_os2_vol_stream;

static bp_vol_stream_t vol_hook_open_read(const char *path)
{
    HFILE handle;
    ULONG action = 0;
    APIRET rc = DosOpen((PSZ)path, &handle, &action, 0, FILE_NORMAL,
                        OPEN_ACTION_OPEN_IF_EXISTS | OPEN_ACTION_FAIL_IF_NEW,
                        OPEN_ACCESS_READONLY | OPEN_SHARE_DENYWRITE |
                        OPEN_FLAGS_SEQUENTIAL, NULL);
    if (rc != NO_ERROR) return BP_VOL_NO_STREAM;
    g_os2_vol_stream.handle = handle;
    return &g_os2_vol_stream;
}

static int vol_hook_close(bp_vol_stream_t s)
{
    return DosClose(s->handle) == NO_ERROR ? 0 : -1;
}

/* DosWrite is not buffered by the library */
static int vol_hook_flush(bp_vol_stream_t s)
{
    (void)s;
    return 0;
}

static long vol_hook_read(bp_vol_stream_t s, void *buf, unsigned n)
{
    ULONG got = 0;
    if (DosRead(s->handle, buf, (ULONG)n, &got) != NO_ERROR) return -1L;
    return (long)got;
}

static void vol_hook_io(bp_io_t *io, bp_vol_stream_t s)
{
    bp_io_from_os2_handle(io, s);
}

static int vol_hook_getline(char *buf, int size)
{
    return fgets(buf, size, stdin) != NULL;
}

static bp_vol_stream_t vol_hook_create(const char *target, char *tmp,
                                       size_t tmp_size)
{
    int handle = os2_make_temp_path(target, tmp, tmp_size);
    if (handle < 0) return BP_VOL_NO_STREAM;
    g_os2_vol_stream.handle = (HFILE)handle;
    return &g_os2_vol_stream;
}

/* A volume replaces a file of its name (its set is being rewritten), but
 * never a directory or a read-only file: DosDelete fails for those. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    FILESTATUS3 info;
    if (DosQueryPathInfo((PSZ)target, FIL_STANDARD, &info, sizeof(info)) ==
        NO_ERROR && DosDelete((PSZ)target) != NO_ERROR)
        return -1;
    return DosMove((PSZ)tmp, (PSZ)target) == NO_ERROR ? 0 : -1;
}

static int vol_hook_sync(bp_vol_stream_t s)
{
    return DosResetBuffer(s->handle) == NO_ERROR ? 0 : -1;
}

static int vol_hook_interactive(void)
{
    return isatty(0);
}

static int vol_hook_exists(const char *path)
{
    FILESTATUS3 info;
    return DosQueryPathInfo((PSZ)path, FIL_STANDARD, &info, sizeof(info)) ==
           NO_ERROR;
}

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* OS/2: no signal-time cleanup */
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

/* Size of a file in bytes; -1 if it cannot be queried. */
static long os2_vol_file_size(const char *path)
{
    FILESTATUS3 info;
    if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &info, sizeof(info)) !=
        NO_ERROR) return -1L;
    return (long)info.cbFile;
}

#include "../common/volume_size.h"
#include "../common/volume_files.h"

#endif /* BP_OS2_VOLUMES_H */
