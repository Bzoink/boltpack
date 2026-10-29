/* Build a valid archive with a file/directory prefix conflict. */
#include "../src/core/archiver.h"
#include "../src/core/compress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long fixture_read(void *handle, void *buffer, bp_uint32 count)
{
    return (long)fread(buffer, 1, (size_t)count, (FILE *)handle);
}

static long fixture_write(void *handle, const void *buffer, bp_uint32 count)
{
    return (long)fwrite(buffer, 1, (size_t)count, (FILE *)handle);
}

static int fixture_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t fixture_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static void fixture_io(bp_io_t *io, FILE *fp)
{
    io->read = fixture_read;
    io->write = fixture_write;
    io->seek = fixture_seek;
    io->tell = fixture_tell;
    io->handle = fp;
}

static int mutate_archive_name(FILE *fp, const char *replacement,
                               unsigned int name_length)
{
    long file_size;
    unsigned char *bytes;
    bp_uint64 table_offset, table_size;
    unsigned long table_pos, table_len;
    int ok = 0;

    if (fflush(fp) != 0 || fseek(fp, 0, SEEK_END) != 0) return -1;
    file_size = ftell(fp);
    if (file_size < (long)(BP_HEADER_SIZE + BP_FOOTER_SIZE) ||
        fseek(fp, 0, SEEK_SET) != 0) return -1;
    bytes = (unsigned char *)malloc((size_t)file_size);
    if (bytes == NULL) return -1;
    if (fread(bytes, 1, (size_t)file_size, fp) != (size_t)file_size)
        goto done;
    table_offset = bp_read_le64(bytes + file_size - BP_FOOTER_SIZE);
    table_size = bp_read_le64(bytes + file_size - BP_FOOTER_SIZE + 8);
    if (table_offset.hi != 0 || table_size.hi != 0 ||
        table_offset.lo > (bp_uint32)file_size ||
        table_size.lo > (bp_uint32)file_size - table_offset.lo ||
        table_offset.lo + BP_TABLE_HDR_V3 + 2U + name_length >
            table_offset.lo + table_size.lo ||
        bp_read_le16(bytes + table_offset.lo + BP_TABLE_HDR_V3) != name_length ||
        name_length != 4U || strlen(replacement) != name_length)
        goto done;
    table_pos = table_offset.lo + BP_TABLE_HDR_V3 + 2U;
    table_len = table_size.lo;
    memcpy(bytes + BP_HEADER_SIZE + 4U, replacement, name_length);
    memcpy(bytes + table_pos, replacement, name_length);
    bp_write_le32(bytes + BP_HEADER_TABLE_CRC_OFF,
                  bp_crc32(0, bytes + table_offset.lo, table_len));
    bp_write_le32(bytes + BP_HEADER_CRC_OFF,
                  bp_crc32(0, bytes, BP_HEADER_CRC_OFF));
    if (fseek(fp, 0, SEEK_SET) != 0 ||
        fwrite(bytes, 1, (size_t)file_size, fp) != (size_t)file_size ||
        fflush(fp) != 0)
        goto done;
    ok = 1;
done:
    free(bytes);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    FILE *archive_fp;
    FILE *empty_fp;
    bp_io_t archive_io, empty_io;
    bp_archive_t archive;
    bp_uint64 zero_size = {0, 0};
    int rc, created = 0, unsafe;

    if (argc < 2 || argc > 3) return 2;
    unsafe = argc == 3 && strcmp(argv[2], "unsafe") == 0;
    if (argc == 3 && !unsafe) return 2;
    archive_fp = fopen(argv[1], "wb+");
    empty_fp = tmpfile();
    if (archive_fp == NULL || empty_fp == NULL) return 3;
    fixture_io(&archive_io, archive_fp);
    fixture_io(&empty_io, empty_fp);
    rc = bp_archive_create(&archive, &archive_io, 16384, BP_METHOD_STORED);
    if (rc == BP_OK) {
        created = 1;
        if (unsafe) {
            rc = bp_archive_add_entry(&archive, "safe", BP_TYPE_DATA,
                                      BP_OS_HAIKU, &empty_io, zero_size, zero_size);
        } else {
            rc = bp_archive_add_entry(&archive, "node", BP_TYPE_DATA,
                                      BP_OS_HAIKU, &empty_io, zero_size, zero_size);
        }
    }
    if (rc == BP_OK && fseek(empty_fp, 0, SEEK_SET) != 0) rc = BP_ERR_IO;
    if (rc == BP_OK && !unsafe)
        rc = bp_archive_add_entry(&archive, "node/child", BP_TYPE_DATA,
                                  BP_OS_HAIKU, &empty_io, zero_size, zero_size);
    if (rc == BP_OK) rc = bp_archive_finalize(&archive);
    if (created) bp_archive_close(&archive);
    if (rc == BP_OK && unsafe && mutate_archive_name(archive_fp, "../x", 4U) != 0)
        rc = BP_ERR_IO;
    if (fclose(empty_fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (fclose(archive_fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc == BP_OK ? 0 : 1;
}
