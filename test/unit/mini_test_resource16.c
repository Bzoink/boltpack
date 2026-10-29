/* Verify that the 16-bit writer stops before its size_t allocation wraps. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "archiver.h"
#include "mini_common.h"

#if !defined(SIZEOF_INT) || SIZEOF_INT != 2
#error This fixture must be compiled for a 16-bit size_t ABI
#endif
typedef char bp_resource_size_t_is_16[(sizeof(size_t) == 2) ? 1 : -1];
typedef char bp_resource_entry_size_is_54[(sizeof(bp_mini_entry_t) == 54) ? 1 : -1];

static int test_projected_index_bounds(void)
{
    bp_mini_projected_index_t index;
    char name[260];
    unsigned long i;
    int rc;
    if (bp_mini_projected_index_init(&index) != BP_OK) return 1;
    for (i = 0; i < BP_MINI_PROJECTED_INDEX_MAX; i++) {
        sprintf(name, "F%03lu.TXT", i);
        if (bp_mini_projected_index_add(&index, name, 0) != BP_OK) {
            bp_mini_projected_index_free(&index);
            return 1;
        }
    }
    rc = bp_mini_projected_index_add(&index, "OVERFLOW.TXT", 0);
    if (rc != BP_ERR_PARTIAL ||
        index.count != BP_MINI_PROJECTED_INDEX_MAX) {
        bp_mini_projected_index_free(&index);
        return 1;
    }
    bp_mini_projected_index_free(&index);

    if (bp_mini_projected_index_init(&index) != BP_OK) return 1;
    for (i = 0; i < 8U; i++) {
        sprintf(name, "P%03lu", i);
        memset(name + strlen(name), 'A', 250U - strlen(name));
        name[250] = '\0';
        if (bp_mini_projected_index_add(&index, name, 0) != BP_OK) {
            bp_mini_projected_index_free(&index);
            return 1;
        }
    }
    sprintf(name, "P%03lu", i);
    memset(name + strlen(name), 'A', 250U - strlen(name));
    name[250] = '\0';
    rc = bp_mini_projected_index_add(&index, name, 0);
    if (rc != BP_ERR_PARTIAL || index.count != 8U || index.used != 4016U) {
        bp_mini_projected_index_free(&index);
        return 1;
    }
    bp_mini_projected_index_free(&index);
    return 0;
}

static int test_directory_queue_bound(void)
{
    bp_mini_directory_queue_t queue;
    bp_mini_entry_t entry;
    char path[16];
    unsigned long i;
    int rc;
    memset(&queue, 0, sizeof(queue));
    memset(&entry, 0, sizeof(entry));
    for (i = 0; i < 256U; i++) {
        sprintf(path, "Q%03lu/", i);
        rc = bp_mini_directory_add(&queue, path, &entry);
        if (rc != BP_OK) {
            bp_mini_directory_finish(&queue, 0, NULL, NULL);
            return 1;
        }
    }
    sprintf(path, "Q%03lu/", i);
    rc = bp_mini_directory_add(&queue, path, &entry);
    if (rc != BP_ERR_OVERFLOW || queue.count != 256U) {
        bp_mini_directory_finish(&queue, 0, NULL, NULL);
        return 1;
    }
    return bp_mini_directory_finish(&queue, 0, NULL, NULL) == BP_OK ? 0 : 1;
}

int main(void)
{
    FILE *fp;
    FILE *result;
    bp_io_t io;
    bp_mini_archive_t archive;
    bp_uint64 zero = BP_UINT64_ZERO;
    size_t max_size = (size_t)-1;
    unsigned long max_entries;
    unsigned long i;
    char name[16];
    int rc;

    result = fopen("RESOURCE.TXT", "w");
    if (result == NULL) return 1;
    max_entries = (unsigned long)(max_size / sizeof(bp_mini_entry_t));
    if (max_entries == 0 || max_entries >= BP_MINI_MAX_ENTRIES) {
        fprintf(result, "FAIL: unexpected entry table bound %lu\n", max_entries);
        fclose(result);
        return 1;
    }

    fp = fopen("LIMIT.BLT", "wb+");
    if (fp == NULL) {
        fprintf(result, "FAIL: cannot create archive fixture\n");
        fclose(result);
        return 1;
    }
    bp_io_from_file(&io, fp);
    rc = bp_mini_archive_create(&archive, &io, BP_MINI_MAX_BLOCK_SIZE,
                                BP_METHOD_STORED);
    if (rc != BP_OK) {
        fprintf(result, "FAIL: create returned %d\n", rc);
        fclose(fp);
        fclose(result);
        return 1;
    }
    for (i = 0; i < max_entries; i++) {
        sprintf(name, "D%04lu/", i);
        rc = bp_mini_archive_add_entry(&archive, name, BP_TYPE_DIR, BP_OS_DOS,
                                       NULL, zero, zero);
        if (rc != BP_OK) {
            fprintf(result, "FAIL: add %lu returned %d\n", i, rc);
            bp_mini_archive_close(&archive);
            fclose(fp);
            fclose(result);
            return 1;
        }
    }
    sprintf(name, "D%04lu/", i);
    rc = bp_mini_archive_add_entry(&archive, name, BP_TYPE_DIR, BP_OS_DOS,
                                   NULL, zero, zero);
    if (rc != BP_ERR_OVERFLOW || archive.entries_written != max_entries) {
        fprintf(result, "FAIL: boundary returned %d with %lu entries\n",
                rc, (unsigned long)archive.entries_written);
        bp_mini_archive_close(&archive);
        fclose(fp);
        fclose(result);
        return 1;
    }
    rc = bp_mini_archive_finalize(&archive);
    bp_mini_archive_close(&archive);
    fclose(fp);
    if (rc != BP_OK) {
        fprintf(result, "FAIL: finalize returned %d\n", rc);
        fclose(result);
        return 1;
    }
    if (test_projected_index_bounds() != 0) {
        fprintf(result, "FAIL: projected-name index bound\n");
        fclose(result);
        return 1;
    }
    if (test_directory_queue_bound() != 0) {
        fprintf(result, "FAIL: deferred directory queue bound\n");
        fclose(result);
        return 1;
    }
    fprintf(result, "PASS: size_t=%u entry_size=%u max_entries=%lu table overflow refused\n",
            (unsigned)sizeof(size_t), (unsigned)sizeof(bp_mini_entry_t), max_entries);
    fprintf(result, "PASS: projected_index_entries=%u projected_key_bytes=%u\n",
            (unsigned)BP_MINI_PROJECTED_INDEX_MAX,
            (unsigned)BP_MINI_PROJECTED_INDEX_BYTES);
    fprintf(result, "PASS: deferred_directory_queue_entries=256 overflow refused\n");
    fclose(result);
    return 0;
}
