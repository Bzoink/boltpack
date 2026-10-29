#include <stdio.h>
#include <string.h>

#include "archiver.h"
#include "compress.h"

static long file_read(void *handle, void *buffer, bp_uint32 size)
{
    return (long)fread(buffer, 1, (size_t)size, (FILE *)handle);
}

static long file_write(void *handle, const void *buffer, bp_uint32 size)
{
    return (long)fwrite(buffer, 1, (size_t)size, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static int add_empty(bp_archive_t *archive, const char *name,
                     bp_uint8 entry_type)
{
    return bp_archive_add_entry(archive, name, entry_type, BP_OS_LINUX,
                                NULL, bp_u64_from_u32(0),
                                bp_u64_from_u32(0));
}

int main(int argc, char **argv)
{
    FILE *file;
    bp_io_t io;
    bp_archive_t archive;
    int rc;

    if (argc != 3) return 2;
    file = fopen(argv[1], "wb+");
    if (file == NULL) return 1;
    io.read = file_read;
    io.write = file_write;
    io.seek = file_seek;
    io.tell = file_tell;
    io.handle = file;
    rc = bp_compress_init();
    if (rc == BP_OK)
        rc = bp_archive_create(&archive, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    if (rc == BP_OK) {
        if (strcmp(argv[2], "case") == 0) {
            rc = add_empty(&archive, "Folder/Readme.txt", BP_TYPE_DATA);
            if (rc == BP_OK)
                rc = add_empty(&archive, "folder/readme.TXT", BP_TYPE_DATA);
        } else if (strcmp(argv[2], "latin1-case") == 0) {
            rc = add_empty(&archive, "\xC3\x84.TXT", BP_TYPE_DATA);
            if (rc == BP_OK)
                rc = add_empty(&archive, "\xC3\xA4.txt", BP_TYPE_DATA);
        } else if (strcmp(argv[2], "prefix") == 0) {
            rc = add_empty(&archive, "node", BP_TYPE_DATA);
            if (rc == BP_OK)
                rc = add_empty(&archive, "node-file", BP_TYPE_DATA);
            if (rc == BP_OK)
                rc = add_empty(&archive, "node/child", BP_TYPE_DATA);
        } else if (strcmp(argv[2], "dir-alias") == 0) {
            rc = add_empty(&archive, "Node/", BP_TYPE_DIR);
            if (rc == BP_OK)
                rc = add_empty(&archive, "node", BP_TYPE_DATA);
        } else if (strcmp(argv[2], "unmappable") == 0) {
            rc = add_empty(&archive, "\xE6\xBC\xA2.txt", BP_TYPE_DATA);
        } else if (strcmp(argv[2], "overlong") == 0) {
            rc = add_empty(&archive,
                           "1234567890123456789012345678901234567",
                           BP_TYPE_DATA);
        } else if (strcmp(argv[2], "exact36") == 0) {
            rc = add_empty(&archive,
                           "123456789012345678901234567890123456",
                           BP_TYPE_DATA);
        } else {
            rc = BP_ERR_INVALID_ARG;
        }
        if (rc == BP_OK) rc = bp_archive_finalize(&archive);
        bp_archive_close(&archive);
    }
    if (fclose(file) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
