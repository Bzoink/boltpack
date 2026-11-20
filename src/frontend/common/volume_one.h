/*
 * volume_one.h -- a volume source over one open archive stream
 *
 * Lets code written for the volume APIs (bp_archive_list_volumes and
 * friends) read an archive that is already open as a bp_io_t, such as one
 * received over the network.  Every open request rewinds the stream, so the
 * same source can serve a preflight pass and the extraction after it.
 *
 *   bp_volume_source_t src;
 *   bp_volume_source_one(&src, &io);
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_COMMON_VOLUME_ONE_H
#define BP_COMMON_VOLUME_ONE_H

static bp_io_t *bp_volume_one_open(void *user, bp_uint16 number, int reason)
{
    bp_io_t *io = (bp_io_t *)user;
    (void)number;
    if (reason != BP_OK || io->seek(io->handle, 0, SEEK_SET) != 0) return NULL;
    return io;
}

static void bp_volume_one_close(void *user, bp_io_t *io)
{
    (void)user;
    (void)io;
}

static void bp_volume_source_one(bp_volume_source_t *src, bp_io_t *io)
{
    src->open = bp_volume_one_open;
    src->close = bp_volume_one_close;
    src->user = io;
}

#endif /* BP_COMMON_VOLUME_ONE_H */
