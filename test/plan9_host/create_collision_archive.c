#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archiver.h"
#include "compress.h"

typedef struct {
    const unsigned char *data;
    bp_uint32 size;
    bp_uint32 pos;
} memory_t;

static long memory_read(void *handle, void *buffer, bp_uint32 size)
{
    memory_t *memory = (memory_t *)handle;
    if (size > memory->size - memory->pos) size = memory->size - memory->pos;
    memcpy(buffer, memory->data + memory->pos, size);
    memory->pos += size;
    return (long)size;
}

static void memory_io(bp_io_t *io, memory_t *memory)
{
    io->read = memory_read;
    io->write = NULL;
    io->seek = NULL;
    io->tell = NULL;
    io->handle = memory;
}

static long file_read(void *handle, void *buffer, bp_uint32 size)
{
    return (long)fread(buffer, 1, size, (FILE *)handle);
}

static long file_write(void *handle, const void *buffer, bp_uint32 size)
{
    return (long)fwrite(buffer, 1, size, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static int add_empty(bp_archive_t *archive, const char *name)
{
    bp_uint64 zero;
    zero.hi = 0;
    zero.lo = 0;
    return bp_archive_add_entry(archive, name, BP_TYPE_DATA, BP_OS_LINUX,
                                NULL, zero, zero);
}

static int add_metadata(bp_archive_t *archive, const char *name,
                        unsigned int mode)
{
    unsigned char bytes[2];
    memory_t memory;
    bp_io_t io;
    bytes[0] = (unsigned char)(mode & 0xffU);
    bytes[1] = (unsigned char)((mode >> 8) & 0xffU);
    memory.data = bytes;
    memory.size = sizeof(bytes);
    memory.pos = 0;
    memory_io(&io, &memory);
    return bp_archive_add_entry(archive, name, BP_TYPE_META, BP_OS_PLAN9,
                                &io, bp_u64_from_u32(sizeof(bytes)),
                                bp_u64_from_u32(0));
}

static int add_metadata_fixture(bp_archive_t *archive)
{
    static const unsigned char payload[] = "payload";
    memory_t memory;
    bp_io_t io;
    int rc;
    rc = bp_archive_add_entry(archive, "locked/", BP_TYPE_DIR, BP_OS_PLAN9,
                              NULL, bp_u64_from_u32(0),
                              bp_u64_from_u32(1700000000U));
    if (rc == BP_OK) rc = add_metadata(archive, "locked/", 0550);
    memory.data = payload;
    memory.size = sizeof(payload) - 1U;
    memory.pos = 0;
    memory_io(&io, &memory);
    if (rc == BP_OK)
        rc = bp_archive_add_entry(archive, "locked/child.txt", BP_TYPE_DATA,
                                  BP_OS_PLAN9, &io,
                                  bp_u64_from_u32(sizeof(payload) - 1U),
                                  bp_u64_from_u32(1700000001U));
    if (rc == BP_OK) rc = add_metadata(archive, "locked/child.txt", 0640);
    return rc;
}

int main(int argc, char **argv)
{
    FILE *file;
    bp_io_t io;
    bp_archive_t archive;
    int rc;
    int reverse;

    if (argc != 3) return 2;
    reverse = atoi(argv[2]);
    file = fopen(argv[1], "wb+");
    if (file == NULL) return 1;
    io.read = file_read;
    io.write = file_write;
    io.seek = file_seek;
    io.tell = file_tell;
    io.handle = file;
    rc = bp_compress_init();
    if (rc == BP_OK) rc = bp_archive_create(&archive, &io, BP_BLOCK_8K,
                                             BP_METHOD_STORED);
    if (rc == BP_OK) {
        if (!strcmp(argv[2], "metadata")) {
            rc = add_metadata_fixture(&archive);
        } else if (reverse) {
            rc = add_empty(&archive, "node/child");
            if (rc == BP_OK) rc = add_empty(&archive, "node");
        } else {
            rc = add_empty(&archive, "node");
            if (rc == BP_OK) rc = add_empty(&archive, "node/child");
        }
        if (rc == BP_OK) rc = bp_archive_finalize(&archive);
        bp_archive_close(&archive);
    }
    if (fclose(file) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
