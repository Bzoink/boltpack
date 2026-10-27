#include "../src/core/mini/archiver.h"
#include "../src/frontend/mini_shared/mini_common.h"

#include <stdio.h>
#include <string.h>

static int add_data(bp_mini_archive_t *archive, const char *name,
                    const char *contents)
{
    FILE *source;
    bp_io_t source_io;
    bp_uint64 size;
    bp_uint64 mtime;
    int rc;

    source = tmpfile();
    if (source == NULL) return BP_ERR_IO;
    size.lo = (bp_uint32)strlen(contents);
    size.hi = 0;
    mtime.lo = 0;
    mtime.hi = 0;
    if (fwrite(contents, 1, size.lo, source) != size.lo ||
        fseek(source, 0L, SEEK_SET) != 0) {
        fclose(source);
        return BP_ERR_IO;
    }
    bp_io_from_file(&source_io, source);
    rc = bp_mini_archive_add_entry_ex(archive, name, BP_TYPE_DATA,
                                      BP_OS_LINUX, &source_io, size, mtime,
                                      BP_ENTRY_FLAG_MTIME_VALID);
    if (fclose(source) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc;
}

int main(int argc, char **argv)
{
    FILE *fp;
    bp_io_t io;
    bp_mini_archive_t archive;
    int reverse;
    int created = 0;
    int rc;

    if (argc < 2 || argc > 3) return 2;
    reverse = argc == 3 && strcmp(argv[2], "reverse") == 0;
    fp = fopen(argv[1], "wb");
    if (fp == NULL) return 1;
    bp_io_from_file(&io, fp);
    rc = bp_mini_archive_create(&archive, &io, BP_BLOCK_8K,
                                BP_METHOD_STORED);
    if (rc == BP_OK) created = 1;
    if (rc == BP_OK && reverse)
        rc = add_data(&archive, "node/child", "child file");
    if (rc == BP_OK && !reverse)
        rc = add_data(&archive, "node", "file at node");
    if (rc == BP_OK) rc = add_data(&archive, "node!", "nearby name");
    if (rc == BP_OK && !reverse)
        rc = add_data(&archive, "node/child", "child file");
    if (rc == BP_OK && reverse)
        rc = add_data(&archive, "node", "file at node");
    if (rc == BP_OK) rc = bp_mini_archive_finalize(&archive);
    if (created) bp_mini_archive_close(&archive);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
