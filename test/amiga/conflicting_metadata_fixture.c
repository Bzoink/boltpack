/* Build Amiga archives with conflicting native and portable metadata order. */
#include "../../src/core/archiver.h"
#include "../../src/core/bp_metadata.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const unsigned char *data;
    bp_uint32 size;
    bp_uint32 pos;
} fixture_input_t;

static long memory_read(void *handle, void *buffer, bp_uint32 count)
{
    fixture_input_t *input = (fixture_input_t *)handle;
    if (count > input->size - input->pos) count = input->size - input->pos;
    if (count == 0) return 0;
    memcpy(buffer, input->data + input->pos, count);
    input->pos += count;
    return (long)count;
}

static int memory_seek(void *handle, bp_off_t offset, int origin)
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

static bp_off_t memory_tell(void *handle)
{
    return (bp_off_t)((fixture_input_t *)handle)->pos;
}

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

static void memory_io(bp_io_t *io, fixture_input_t *input)
{
    io->read = memory_read;
    io->write = NULL;
    io->seek = memory_seek;
    io->tell = memory_tell;
    io->handle = input;
}

static void file_io(bp_io_t *io, FILE *file)
{
    io->read = file_read;
    io->write = file_write;
    io->seek = file_seek;
    io->tell = file_tell;
    io->handle = file;
}

static int add_entry(bp_archive_t *archive, const char *name,
                     bp_uint8 type, bp_uint8 platform,
                     const unsigned char *payload, bp_uint32 size)
{
    fixture_input_t input;
    bp_io_t io;
    input.data = payload;
    input.size = size;
    input.pos = 0;
    memory_io(&io, &input);
    return bp_archive_add_entry(archive, name, type, platform, &io,
                                bp_u64_from_u32(size), bp_u64_from_u32(0));
}

int main(int argc, char **argv)
{
    static const unsigned char data[] = "Amiga metadata precedence fixture\n";
    static const unsigned char amiga_meta[] = {0x40, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char malformed_meta[] = {
        0x40, 0x00, 0x00, 0x00, 0x03, 'A', 0x00, 'B'
    };
    bp_uint8 portable[BP_META_FAT_SIZE];
    bp_archive_t archive;
    bp_io_t io;
    FILE *file;
    int native_first, malformed, rc;

    if (argc != 3) return 2;
    native_first = strcmp(argv[2], "native-first") == 0;
    malformed = strcmp(argv[2], "embedded-nul") == 0;
    if (!native_first && !malformed && strcmp(argv[2], "portable-first") != 0) return 2;
    file = fopen(argv[1], "wb");
    if (file == NULL) return 3;
    file_io(&io, file);
    rc = bp_metadata_fat_encode(portable, sizeof(portable), BP_TYPE_DATA,
                                0x02U, 0x02U);
    if (rc == BP_OK)
        rc = bp_archive_create(&archive, &io, 16384, BP_METHOD_STORED);
    if (rc != BP_OK) {
        fclose(file);
        return 1;
    }
    rc = add_entry(&archive, "CONFLICT.TXT", BP_TYPE_DATA, BP_OS_LINUX,
                   data, (bp_uint32)(sizeof(data) - 1U));
    if (rc == BP_OK && malformed)
        rc = add_entry(&archive, "CONFLICT.TXT", BP_TYPE_META,
                       BP_OS_AMIGA_68K, malformed_meta, sizeof(malformed_meta));
    if (rc == BP_OK && native_first)
        rc = add_entry(&archive, "CONFLICT.TXT", BP_TYPE_META,
                       BP_OS_AMIGA_68K, amiga_meta, sizeof(amiga_meta));
    if (rc == BP_OK && !malformed && !native_first)
        rc = add_entry(&archive, "CONFLICT.TXT", BP_TYPE_META,
                       BP_META_PORTABLE, portable, sizeof(portable));
    if (rc == BP_OK && !malformed && native_first)
        rc = add_entry(&archive, "CONFLICT.TXT", BP_TYPE_META,
                       BP_META_PORTABLE, portable, sizeof(portable));
    if (rc == BP_OK && !native_first)
        rc = add_entry(&archive, "CONFLICT.TXT", BP_TYPE_META,
                       BP_OS_AMIGA_68K, amiga_meta, sizeof(amiga_meta));
    if (rc == BP_OK) rc = bp_archive_finalize(&archive);
    bp_archive_close(&archive);
    if (fclose(file) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
