#include <stdio.h>
#include <stdlib.h>
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

static int add_empty(bp_archive_t *archive, const char *name)
{
    return bp_archive_add_entry(archive, name, BP_TYPE_DATA, BP_OS_LINUX,
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
    io.read = file_read; io.write = file_write;
    io.seek = file_seek; io.tell = file_tell; io.handle = file;
    rc = bp_compress_init();
    if (rc == BP_OK)
        rc = bp_archive_create(&archive, &io, BP_BLOCK_8K, BP_METHOD_STORED);
    if (rc == BP_OK) {
        if (strcmp(argv[2], "case") == 0) {
            rc = add_empty(&archive, "Case.txt");
            if (rc == BP_OK) rc = add_empty(&archive, "case.TXT");
        } else if (strcmp(argv[2], "prefix-reverse") == 0) {
            rc = add_empty(&archive, "node/child");
            if (rc == BP_OK) rc = add_empty(&archive, "NODE");
        } else {
            rc = add_empty(&archive, "NODE");
            if (rc == BP_OK) rc = add_empty(&archive, "node/child");
        }
        if (rc == BP_OK) rc = bp_archive_finalize(&archive);
        bp_archive_close(&archive);
    }
    if (fclose(file) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
