/*
 * make_attr_archive.c -- build an archive with a chosen native attribute
 * container payload (test only)
 *
 * usage: make_attr_archive OUT.bp NAME PAYLOADFILE file|dir [PLATFORM]
 *
 * Writes a subject entry (a 5-byte file, or a directory) followed by one
 * BP_TYPE_META entry whose platform byte is PLATFORM (default 0xFD) and whose
 * payload is the bytes of PAYLOADFILE.  Lets the tests feed the macOS CLI
 * containers that its own writer would never produce (bad version, wrong
 * origin, malformed framing, oversize).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archiver.h"
#include "compress.h"

static long file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read = file_read;
    io->write = file_write;
    io->seek = file_seek;
    io->tell = file_tell;
    io->handle = fp;
}

int main(int argc, char **argv)
{
    FILE *out, *payload, *subject;
    bp_io_t out_io, payload_io, subject_io;
    bp_archive_t ar;
    char dirname[4096];
    long size;
    int is_dir, platform = 0xFD, rc;

    if (argc < 5) {
        fprintf(stderr, "usage: %s OUT.bp NAME PAYLOAD file|dir [PLATFORM]\n",
                argv[0]);
        return 2;
    }
    is_dir = strcmp(argv[4], "dir") == 0;
    if (argc > 5) platform = (int)strtol(argv[5], NULL, 0);

    payload = fopen(argv[3], "rb");
    if (payload == NULL) { perror(argv[3]); return 2; }
    fseek(payload, 0, SEEK_END);
    size = ftell(payload);
    fseek(payload, 0, SEEK_SET);

    out = fopen(argv[1], "w+b");
    if (out == NULL) { perror(argv[1]); return 2; }

    if (bp_compress_init() != BP_OK) return 2;
    bp_io_from_file(&out_io, out);
    rc = bp_archive_create(&ar, &out_io, BP_BLOCK_32K, BP_METHOD_STORED);
    if (rc != BP_OK) { fprintf(stderr, "create failed: %d\n", rc); return 2; }

    if (is_dir) {
        snprintf(dirname, sizeof(dirname), "%s/", argv[2]);
        rc = bp_archive_add_entry_ex(&ar, dirname, BP_TYPE_DIR, BP_OS_MACOSX,
                                     NULL, bp_u64_from_u32(0),
                                     bp_u64_from_u32(0), 0);
    } else {
        subject = tmpfile();
        if (subject == NULL) return 2;
        fwrite("data\n", 1, 5, subject);
        fseek(subject, 0, SEEK_SET);
        bp_io_from_file(&subject_io, subject);
        rc = bp_archive_add_entry_ex(&ar, argv[2], BP_TYPE_DATA, BP_OS_MACOSX,
                                     &subject_io, bp_u64_from_u32(5),
                                     bp_u64_from_u32(0), 0);
        fclose(subject);
    }
    if (rc != BP_OK) { fprintf(stderr, "subject failed: %d\n", rc); return 2; }

    bp_io_from_file(&payload_io, payload);
    rc = bp_archive_add_entry_ex(&ar, is_dir ? dirname : argv[2], BP_TYPE_META,
                                 (bp_uint8)platform, &payload_io,
                                 bp_u64_from_u32((bp_uint32)size),
                                 bp_u64_from_u32(0), 0);
    if (rc != BP_OK) { fprintf(stderr, "meta failed: %d\n", rc); return 2; }

    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) { fprintf(stderr, "finalize failed: %d\n", rc); return 2; }
    fclose(payload);
    fclose(out);
    return 0;
}
