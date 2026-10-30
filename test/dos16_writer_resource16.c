/* Measure the maximum mini writer table together with BoltLite buffers in DOS. */
#include <stdio.h>
#include <string.h>
#include "archiver.h"
#include "mini_common.h"

#if !defined(SIZEOF_INT) || SIZEOF_INT != 2
#error This fixture must be compiled for a 16-bit size_t ABI
#endif
typedef char bp_resource_size_t_is_16[(sizeof(size_t) == 2) ? 1 : -1];
typedef char bp_resource_entry_size_is_54[(sizeof(bp_mini_entry_t) == 54) ? 1 : -1];

#define WRITER_MAX_ENTRIES ((unsigned long)(((size_t)-1) / sizeof(bp_mini_entry_t)))
#define WRITER_DATA_BYTES 32768UL

typedef struct {
    const unsigned char *data;
    unsigned long length;
    unsigned long position;
} writer_source_t;

static unsigned char writer_data[WRITER_DATA_BYTES];

extern void bp_test_alloc_report(void);

/* The shared core objects also contain optional stack samples. This separate
   writer executable measures heap only; the main DOS16 runner measures stack. */
void bp_test_stack_probe(const char *source_file, unsigned int source_line)
{
    (void)source_file;
    (void)source_line;
}

static long writer_read(void *handle, void *buffer, bp_uint32 count)
{
    writer_source_t *source = (writer_source_t *)handle;
    if ((unsigned long)count > source->length - source->position) return -1;
    memcpy(buffer, source->data + source->position, (size_t)count);
    source->position += (unsigned long)count;
    return (long)count;
}

int main(void)
{
    FILE *fp;
    bp_io_t archive_io, data_io;
    bp_mini_archive_t archive;
    writer_source_t source;
    bp_uint64 zero = BP_UINT64_ZERO;
    char name[16];
    unsigned long i;
    int rc;

    memset(writer_data, 'A', sizeof(writer_data));
    source.data = writer_data;
    source.length = sizeof(writer_data);
    source.position = 0;
    memset(&data_io, 0, sizeof(data_io));
    data_io.read = writer_read;
    data_io.handle = &source;

    fp = fopen("WRITMAX.BLT", "wb+");
    if (fp == NULL) {
        printf("FAIL: cannot create WRITMAX.BLT\n");
        return 1;
    }
    bp_io_from_file(&archive_io, fp);
    rc = bp_mini_archive_create(&archive, &archive_io,
                                BP_MINI_MAX_BLOCK_SIZE, BP_METHOD_BOLTLITE);
    if (rc != BP_OK) goto fail;

    /* Reserve all but one maximum-bound row with directories, then add one
       full-size compressed file while the maximum table remains live. */
    for (i = 0; i + 1 < WRITER_MAX_ENTRIES; i++) {
        sprintf(name, "D%04lu/", i);
        rc = bp_mini_archive_add_entry(&archive, name, BP_TYPE_DIR,
                                       BP_OS_DOS, NULL, zero, zero);
        if (rc != BP_OK) goto fail_archive;
    }
    rc = bp_mini_archive_add_entry(&archive, "PAYLOAD.DAT", BP_TYPE_DATA,
                                   BP_OS_DOS, &data_io,
                                   bp_u64_from_u32(WRITER_DATA_BYTES), zero);
    if (rc != BP_OK) goto fail_archive;
    if (archive.entries_written != WRITER_MAX_ENTRIES) {
        rc = BP_ERR_CORRUPT;
        goto fail_archive;
    }
    rc = bp_mini_archive_finalize(&archive);
    if (rc != BP_OK) goto fail_archive;
    bp_mini_archive_close(&archive);
    fclose(fp);
    printf("PASS: writer_entries=%lu data_bytes=%lu method=boltlite\n",
           WRITER_MAX_ENTRIES, WRITER_DATA_BYTES);
    bp_test_alloc_report();
    return 0;

fail_archive:
    bp_mini_archive_close(&archive);
fail:
    fclose(fp);
    printf("FAIL: writer resource probe rc=%d\n", rc);
    return 1;
}
