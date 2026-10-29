/* Build archives whose native TOS and portable FAT metadata conflict. */
#include "../../src/core/archiver.h"
#include "../../src/core/bp_metadata.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const unsigned char *data;
    bp_uint32 size;
    bp_uint32 pos;
} fixture_input_t;

static long fixture_read(void *handle, void *buffer, bp_uint32 count)
{
    fixture_input_t *input = (fixture_input_t *)handle;
    if (count > input->size - input->pos) count = input->size - input->pos;
    if (count == 0) return 0;
    memcpy(buffer, input->data + input->pos, count);
    input->pos += count;
    return (long)count;
}

static long fixture_file_read(void *handle, void *buffer, bp_uint32 count)
{
    return (long)fread(buffer, 1, count, (FILE *)handle);
}

static long fixture_file_write(void *handle, const void *buffer,
                               bp_uint32 count)
{
    return (long)fwrite(buffer, 1, count, (FILE *)handle);
}

static int fixture_memory_seek(void *handle, bp_off_t offset, int origin)
{
    fixture_input_t *input = (fixture_input_t *)handle;
    long position;
    if (origin == SEEK_SET) position = (long)offset;
    else if (origin == SEEK_CUR) position = (long)input->pos + (long)offset;
    else if (origin == SEEK_END) position = (long)input->size + (long)offset;
    else return -1;
    if (position < 0 || (unsigned long)position > input->size) return -1;
    input->pos = (bp_uint32)position;
    return 0;
}

static bp_off_t fixture_memory_tell(void *handle)
{
    return (bp_off_t)((fixture_input_t *)handle)->pos;
}

static int fixture_file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t fixture_file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static void fixture_file_io(bp_io_t *io, FILE *fp)
{
    io->read = fixture_file_read;
    io->write = fixture_file_write;
    io->seek = fixture_file_seek;
    io->tell = fixture_file_tell;
    io->handle = fp;
}

static void fixture_memory_io(bp_io_t *io, fixture_input_t *input)
{
    io->read = fixture_read;
    io->write = NULL;
    io->seek = fixture_memory_seek;
    io->tell = fixture_memory_tell;
    io->handle = input;
}

static int add_payload(bp_archive_t *archive, const char *name,
                       bp_uint8 platform, const unsigned char *data,
                       bp_uint32 length)
{
    fixture_input_t input;
    bp_io_t io;
    input.data = data;
    input.size = length;
    input.pos = 0;
    fixture_memory_io(&io, &input);
    return bp_archive_add_entry(archive, name, BP_TYPE_META, platform, &io,
                                bp_u64_from_u32(length),
                                bp_u64_from_u32(0));
}

int main(int argc, char **argv)
{
    static const unsigned char payload[] = "native metadata wins\n";
    static const unsigned char native_tos_attributes[] = {0x02};
    unsigned char portable[BP_META_FAT_SIZE];
    bp_archive_t archive;
    bp_io_t archive_io, data_io;
    fixture_input_t data;
    FILE *fp;
    int native_first, rc;

    if (argc != 3) return 2;
    native_first = strcmp(argv[2], "native-first") == 0;
    if (!native_first && strcmp(argv[2], "portable-first") != 0) return 2;
    fp = fopen(argv[1], "wb");
    if (fp == NULL) return 3;
    fixture_file_io(&archive_io, fp);
    rc = bp_metadata_fat_encode(portable, sizeof(portable), BP_TYPE_DATA,
                                (bp_uint8)(0x02U | 0x20U), 0x20U);
    if (rc == BP_OK)
        rc = bp_archive_create(&archive, &archive_io, 16384,
                               BP_METHOD_STORED);
    if (rc != BP_OK) {
        fclose(fp);
        return 1;
    }
    data.data = payload;
    data.size = (bp_uint32)(sizeof(payload) - 1U);
    data.pos = 0;
    fixture_memory_io(&data_io, &data);
    rc = bp_archive_add_entry(&archive, "CONFLICT.TXT", BP_TYPE_DATA,
                              BP_OS_ATARI, &data_io,
                              bp_u64_from_u32(data.size),
                              bp_u64_from_u32(0));
    if (rc == BP_OK && native_first)
        rc = add_payload(&archive, "CONFLICT.TXT", BP_OS_ATARI,
                         native_tos_attributes,
                         (bp_uint32)sizeof(native_tos_attributes));
    if (rc == BP_OK && !native_first)
        rc = add_payload(&archive, "CONFLICT.TXT", BP_META_PORTABLE,
                         portable, (bp_uint32)sizeof(portable));
    if (rc == BP_OK && native_first)
        rc = add_payload(&archive, "CONFLICT.TXT", BP_META_PORTABLE,
                         portable, (bp_uint32)sizeof(portable));
    if (rc == BP_OK && !native_first)
        rc = add_payload(&archive, "CONFLICT.TXT", BP_OS_ATARI,
                         native_tos_attributes,
                         (bp_uint32)sizeof(native_tos_attributes));
    if (rc == BP_OK) rc = bp_archive_finalize(&archive);
    bp_archive_close(&archive);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
