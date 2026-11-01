#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/core/archiver.h"
#include "../../src/core/compress.h"

typedef struct {
    bp_io_t io;
    unsigned char bytes[2];
    unsigned int length;
    unsigned int position;
} metadata_sink_t;

typedef struct {
    FILE *fp;
    bp_io_t io;
    bp_io_t data_io;
    metadata_sink_t sink;
    unsigned int expected_count;
    unsigned int actual_count;
    unsigned int primary_mode;
    char primary_name[512];
    int primary_valid;
    int failed;
} check_t;

static long file_read(void *handle, void *buffer, bp_uint32 count)
{
    return (long)fread(buffer, 1, (size_t)count, (FILE *)handle);
}

static long file_write(void *handle, const void *buffer, bp_uint32 count)
{
    return (long)fwrite(buffer, 1, (size_t)count, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static long sink_read(void *handle, void *buffer, bp_uint32 count)
{
    (void)handle;
    (void)buffer;
    (void)count;
    return -1L;
}

static long discard_write(void *handle, const void *buffer, bp_uint32 count)
{
    (void)handle;
    (void)buffer;
    return (long)count;
}

static int discard_seek(void *handle, bp_off_t offset, int origin)
{
    (void)handle;
    (void)offset;
    (void)origin;
    return 0;
}

static bp_off_t discard_tell(void *handle)
{
    (void)handle;
    return (bp_off_t)0;
}

static long sink_write(void *handle, const void *buffer, bp_uint32 count)
{
    metadata_sink_t *sink = (metadata_sink_t *)handle;
    if (count > 2U - sink->length) return -1L;
    memcpy(sink->bytes + sink->length, buffer, count);
    sink->length += count;
    return (long)count;
}

static int sink_seek(void *handle, bp_off_t offset, int origin)
{
    metadata_sink_t *sink = (metadata_sink_t *)handle;
    long position;
    if (origin == SEEK_SET) position = (long)offset;
    else if (origin == SEEK_CUR) position = (long)sink->position + (long)offset;
    else if (origin == SEEK_END) position = 2L + (long)offset;
    else return -1;
    if (position < 0 || position > 2L) return -1;
    sink->position = (unsigned int)position;
    return 0;
}

static bp_off_t sink_tell(void *handle)
{
    return (bp_off_t)((metadata_sink_t *)handle)->position;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    check_t *check = (check_t *)user_data;
    if (entry->entry_type != BP_TYPE_META) return &check->data_io;
    memset(&check->sink, 0, sizeof(check->sink));
    check->sink.io.read = sink_read;
    check->sink.io.write = sink_write;
    check->sink.io.seek = sink_seek;
    check->sink.io.tell = sink_tell;
    check->sink.io.handle = &check->sink;
    return &check->sink.io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    check_t *check = (check_t *)user_data;
    (void)out;
    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR) {
        size_t length = strlen(entry->name);
        if (length >= sizeof(check->primary_name) ||
            (entry->flags & BP_ENTRY_FLAG_MODE_VALID) == 0) {
            check->failed = 1;
            check->primary_valid = 0;
            return status;
        }
        memcpy(check->primary_name, entry->name, length + 1U);
        check->primary_mode = entry->flags & BP_ENTRY_FLAG_MODE_MASK;
        check->primary_valid = 1;
        return status;
    }
    if (entry->entry_type != BP_TYPE_META) return status;
    check->actual_count++;
    if (status != BP_OK || entry->platform != BP_OS_PLAN9 ||
        check->sink.length != 2U || !check->primary_valid ||
        strcmp(check->primary_name, entry->name) != 0 ||
        ((unsigned int)check->sink.bytes[0] |
         ((unsigned int)check->sink.bytes[1] << 8)) != check->primary_mode) {
        fprintf(stderr,
                "Plan 9 mode record %s: status=%d platform=%u bytes=%u mode=%04o expected=%04o\n",
                entry->name, status, entry->platform, check->sink.length,
                (unsigned int)check->sink.bytes[0] |
                    ((unsigned int)check->sink.bytes[1] << 8),
                check->primary_mode);
        check->failed = 1;
    }
    check->primary_valid = 0;
    return status;
}

int main(int argc, char **argv)
{
    check_t check;
    int rc;
    if (argc != 3) {
        fprintf(stderr, "usage: %s archive metadata-count\n", argv[0]);
        return 2;
    }
    memset(&check, 0, sizeof(check));
    check.expected_count = (unsigned int)strtoul(argv[2], NULL, 10);
    check.fp = fopen(argv[1], "rb");
    if (check.fp == NULL) return 2;
    check.data_io.read = sink_read;
    check.data_io.write = discard_write;
    check.data_io.seek = discard_seek;
    check.data_io.tell = discard_tell;
    check.data_io.handle = &check;
    check.io.read = file_read;
    check.io.write = file_write;
    check.io.seek = file_seek;
    check.io.tell = file_tell;
    check.io.handle = check.fp;
    rc = bp_compress_init();
    if (rc == BP_OK)
        rc = bp_archive_extract_all_ex(&check.io, extract_open, extract_done,
                                       NULL, &check);
    fclose(check.fp);
    if (rc != BP_OK || check.failed ||
        check.actual_count != check.expected_count) {
        fprintf(stderr,
                "Plan 9 metadata mismatch: rc=%d failed=%d count=%u expected=%u\n",
                rc, check.failed, check.actual_count, check.expected_count);
        return 1;
    }
    return 0;
}
