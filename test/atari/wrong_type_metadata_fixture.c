/* Build a format-6 archive whose portable FAT record claims the wrong subject
   type. The Atari extractor must leave the data entry's attributes untouched. */
#include <stdio.h>
#include <string.h>
#include "archiver.h"
#include "bp_metadata.h"

typedef struct {
    FILE *file;
} file_io_t;

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
} memory_io_t;

static long file_write(void *handle, const void *buffer, bp_uint32 count)
{
    file_io_t *state = (file_io_t *)handle;
    return (long)fwrite(buffer, 1, (size_t)count, state->file);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    file_io_t *state = (file_io_t *)handle;
    return fseek(state->file, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    file_io_t *state = (file_io_t *)handle;
    return (bp_off_t)ftell(state->file);
}

static long memory_read(void *handle, void *buffer, bp_uint32 count)
{
    memory_io_t *state = (memory_io_t *)handle;
    size_t remaining = state->length - state->position;
    size_t amount = count < remaining ? (size_t)count : remaining;
    if (amount != 0) memcpy(buffer, state->data + state->position, amount);
    state->position += amount;
    return (long)amount;
}

static int memory_seek(void *handle, bp_off_t offset, int origin)
{
    memory_io_t *state = (memory_io_t *)handle;
    long base = origin == SEEK_SET ? 0L :
                origin == SEEK_CUR ? (long)state->position :
                (long)state->length;
    long position = base + (long)offset;
    if (position < 0 || (unsigned long)position > state->length) return -1;
    state->position = (size_t)position;
    return 0;
}

static bp_off_t memory_tell(void *handle)
{
    memory_io_t *state = (memory_io_t *)handle;
    return (bp_off_t)state->position;
}

int main(int argc, char **argv)
{
    static const unsigned char payload[] = "Wrong type metadata test\n";
    bp_uint8 metadata[BP_META_FAT_SIZE];
    memory_io_t payload_state, metadata_state;
    bp_io_t archive_io, payload_io, metadata_io;
    bp_archive_t archive;
    file_io_t file_state;
    int rc;
    int archive_open = 0;

    if (argc != 2) return 2;
    file_state.file = fopen(argv[1], "wb");
    if (file_state.file == NULL) return 2;
    archive_io.read = NULL;
    archive_io.write = file_write;
    archive_io.seek = file_seek;
    archive_io.tell = file_tell;
    archive_io.handle = &file_state;
    payload_state.data = payload;
    payload_state.length = sizeof(payload) - 1;
    payload_state.position = 0;
    payload_io.read = memory_read;
    payload_io.write = NULL;
    payload_io.seek = memory_seek;
    payload_io.tell = memory_tell;
    payload_io.handle = &payload_state;
    metadata_state.data = metadata;
    metadata_state.length = sizeof(metadata);
    metadata_state.position = 0;
    metadata_io.read = memory_read;
    metadata_io.write = NULL;
    metadata_io.seek = memory_seek;
    metadata_io.tell = memory_tell;
    metadata_io.handle = &metadata_state;

    rc = bp_metadata_fat_encode(metadata, sizeof(metadata), BP_TYPE_DIR,
                                 0x02, 0x02);
    if (rc == BP_OK) {
        rc = bp_archive_create(&archive, &archive_io, BP_BLOCK_16K,
                               BP_METHOD_STORED);
        if (rc == BP_OK) archive_open = 1;
    }
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&archive, "CONFLICT.TXT", BP_TYPE_DATA,
                                     BP_OS_ATARI, &payload_io,
                                     bp_u64_from_u32((bp_uint32)(sizeof(payload) - 1)),
                                     bp_u64_from_u32(0), 0);
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&archive, "CONFLICT.TXT", BP_TYPE_META,
                                     BP_META_PORTABLE, &metadata_io,
                                     bp_u64_from_u32((bp_uint32)sizeof(metadata)),
                                     bp_u64_from_u32(0), 0);
    if (rc == BP_OK) rc = bp_archive_finalize(&archive);
    if (archive_open) bp_archive_close(&archive);
    if (fclose(file_state.file) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
