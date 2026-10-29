/* Deterministic allocation-failure checks for the 16-bit mini core. */
#include <stdio.h>
#include <string.h>
#include "archiver.h"
#include "mini_common.h"

void bp_test_alloc_fail_after(long successful_allocations);
void bp_test_alloc_disable(void);
long bp_test_alloc_live_count(void);

static int run_archive_failure(const char *archive_name, long fail_after,
                               int use_data, int expected_live_delta)
{
    FILE *archive_file;
    FILE *source_file = NULL;
    bp_io_t archive_io;
    bp_io_t source_io;
    bp_mini_archive_t archive;
    bp_uint64 size;
    bp_uint64 zero = BP_UINT64_ZERO;
    long live_before;
    int rc;

    archive_file = fopen(archive_name, "wb+");
    if (archive_file == NULL) return 1;
    if (use_data) {
        source_file = fopen("ALLOC.DAT", "rb");
        if (source_file == NULL) { fclose(archive_file); return 1; }
        bp_io_from_file(&source_io, source_file);
    }
    bp_io_from_file(&archive_io, archive_file);
    rc = bp_mini_archive_create(&archive, &archive_io, 8192U,
                                BP_METHOD_BOLTLITE);
    if (rc != BP_OK) {
        if (source_file != NULL) fclose(source_file);
        fclose(archive_file);
        return 1;
    }

    live_before = bp_test_alloc_live_count();
    bp_test_alloc_fail_after(fail_after);
    if (use_data) {
        size = bp_u64_from_u32(1024U);
        rc = bp_mini_archive_add_entry(&archive, "DATA.BIN", BP_TYPE_DATA,
                                       BP_OS_DOS, &source_io, size, zero);
    } else {
        rc = bp_mini_archive_add_entry(&archive, "EMPTY/", BP_TYPE_DIR,
                                       BP_OS_DOS, NULL, zero, zero);
    }
    bp_test_alloc_disable();
    bp_mini_archive_close(&archive);
    if (source_file != NULL) fclose(source_file);
    fclose(archive_file);
    remove(archive_name);

    if (rc != BP_ERR_NOMEM ||
        bp_test_alloc_live_count() != live_before + expected_live_delta)
        return 1;
    return 0;
}

static int run_common_failure(int kind, long fail_after)
{
    bp_mini_projected_index_t index;
    bp_mini_directory_queue_t queue;
    bp_mini_entry_t entry;
    long live_before;
    int rc;
    memset(&index, 0, sizeof(index));
    memset(&queue, 0, sizeof(queue));
    memset(&entry, 0, sizeof(entry));
    live_before = bp_test_alloc_live_count();
    bp_test_alloc_fail_after(fail_after);
    if (kind == 0) {
        rc = bp_mini_projected_index_init(&index);
        bp_mini_projected_index_free(&index);
    } else {
        rc = bp_mini_directory_add(&queue, "FAILED/", &entry);
        bp_mini_directory_finish(&queue, 0, NULL, NULL);
    }
    bp_test_alloc_disable();
    if (rc != BP_ERR_NOMEM || bp_test_alloc_live_count() != live_before)
        return 1;
    return 0;
}

static int run_name_conversion_failure(void)
{
    bp_mini_projected_index_t index;
    bp_mini_projected_preflight_t preflight;
    bp_mini_entry_t entry;
    long live_before;
    int rc;
    memset(&index, 0, sizeof(index));
    memset(&entry, 0, sizeof(entry));
    live_before = bp_test_alloc_live_count();
    if (bp_mini_projected_index_init(&index) != BP_OK) return 1;
    entry.name = "caf\303\251.txt";
    entry.entry_type = BP_TYPE_DATA;
    preflight.index = &index;
    preflight.result = BP_OK;
    bp_test_alloc_fail_after(0);
    rc = bp_mini_projected_name_preflight(&entry, &preflight);
    bp_test_alloc_disable();
    bp_mini_projected_index_free(&index);
    if (rc != 1 || preflight.result != BP_ERR_NOMEM ||
        bp_test_alloc_live_count() != live_before) {
        printf("name conversion allocation result=%d status=%d live=%ld expected=%ld\n",
               rc, preflight.result, bp_test_alloc_live_count(), live_before);
        return 1;
    }
    return 0;
}

static int run_projected_scratch_failure(void)
{
    bp_mini_projected_index_t index;
    long live_before;
    int rc;
    memset(&index, 0, sizeof(index));
    if (bp_mini_projected_index_init(&index) != BP_OK) return 1;
    live_before = bp_test_alloc_live_count();
    bp_test_alloc_fail_after(0);
    rc = bp_mini_projected_index_add(&index, "DIRECTORY/FILE.TXT", 0);
    bp_test_alloc_disable();
    bp_mini_projected_index_free(&index);
    if (rc != BP_ERR_NOMEM || bp_test_alloc_live_count() != live_before - 1)
        return 1;
    return 0;
}

static int run_path_buffer_failure(long fail_after)
{
    bp_mini_path_buffers_t buffers;
    long live_before = bp_test_alloc_live_count();
    int rc;
    memset(&buffers, 0, sizeof(buffers));
    bp_test_alloc_fail_after(fail_after);
    rc = bp_mini_path_buffers_alloc_sized(&buffers, 128U, 160U, 192U);
    bp_test_alloc_disable();
    if (rc != BP_ERR_NOMEM || buffers.pattern != NULL ||
        buffers.child_fs != NULL || buffers.child_arc != NULL ||
        bp_test_alloc_live_count() != live_before)
        return 1;
    return 0;
}

static int run_path_buffer_failures(void)
{
    bp_mini_path_buffers_t buffers;
    long live_before;
    int rc;
    if (run_path_buffer_failure(0) != 0 ||
        run_path_buffer_failure(1) != 0 ||
        run_path_buffer_failure(2) != 0)
        return 1;
    memset(&buffers, 0, sizeof(buffers));
    live_before = bp_test_alloc_live_count();
    rc = bp_mini_path_buffers_alloc(&buffers, 260U);
    if (rc != BP_OK || buffers.pattern == NULL || buffers.child_fs == NULL ||
        buffers.child_arc == NULL ||
        bp_test_alloc_live_count() != live_before + 3)
        return 1;
    bp_mini_path_buffers_free(&buffers);
    return bp_test_alloc_live_count() == live_before ? 0 : 1;
}

static int make_read_fixture(void)
{
    FILE *archive_file;
    FILE *source_file;
    bp_io_t archive_io;
    bp_io_t source_io;
    bp_mini_archive_t archive;
    int rc;

    archive_file = fopen("READ.BLT", "wb+");
    source_file = fopen("ALLOC.DAT", "rb");
    if (archive_file == NULL || source_file == NULL) {
        if (archive_file != NULL) fclose(archive_file);
        if (source_file != NULL) fclose(source_file);
        return 1;
    }
    bp_io_from_file(&archive_io, archive_file);
    bp_io_from_file(&source_io, source_file);
    rc = bp_mini_archive_create(&archive, &archive_io, 1024U,
                                BP_METHOD_STORED);
    if (rc == BP_OK) {
        rc = bp_mini_archive_add_entry(&archive, "READ.DAT", BP_TYPE_DATA,
                                       BP_OS_DOS, &source_io,
                                       bp_u64_from_u32(1024U),
                                       bp_u64_from_u32(0));
        if (rc == BP_OK) rc = bp_mini_archive_finalize(&archive);
        bp_mini_archive_close(&archive);
    }
    fclose(source_file);
    fclose(archive_file);
    return rc == BP_OK ? 0 : 1;
}

static int run_test_read_failure(long fail_after, int expect_nomem)
{
    FILE *archive_file;
    bp_io_t archive_io;
    long live_before;
    int rc;

    archive_file = fopen("READ.BLT", "rb");
    if (archive_file == NULL) return 1;
    bp_io_from_file(&archive_io, archive_file);
    live_before = bp_test_alloc_live_count();
    bp_test_alloc_fail_after(fail_after);
    rc = bp_mini_archive_test(&archive_io, NULL, NULL);
    bp_test_alloc_disable();
    fclose(archive_file);
    if (expect_nomem) {
        if (rc != BP_ERR_NOMEM) return 1;
    } else if (rc != BP_OK) return 1;
    if (bp_test_alloc_live_count() != live_before) return 1;
    return 0;
}

typedef struct {
    FILE *file;
    bp_io_t io;
    int done_count;
    int last_status;
} bp_alloc_extract_context_t;

static bp_io_t *open_extract_output(const bp_mini_entry_t *entry,
                                    void *user_data)
{
    bp_alloc_extract_context_t *context;
    (void)entry;
    context = (bp_alloc_extract_context_t *)user_data;
    context->file = fopen("OUT.DAT", "wb");
    if (context->file == NULL) return NULL;
    bp_io_from_file(&context->io, context->file);
    return &context->io;
}

static int finish_extract_output(const bp_mini_entry_t *entry, bp_io_t *out,
                                 int status, void *user_data)
{
    bp_alloc_extract_context_t *context;
    (void)entry;
    (void)out;
    context = (bp_alloc_extract_context_t *)user_data;
    if (context->file != NULL) {
        fclose(context->file);
        context->file = NULL;
    }
    context->done_count++;
    context->last_status = status;
    return BP_OK;
}

static int run_extract_read_failure(long fail_after, int expect_nomem,
                                   int expect_output)
{
    FILE *archive_file;
    bp_io_t archive_io;
    bp_alloc_extract_context_t context;
    long live_before;
    int rc;

    memset(&context, 0, sizeof(context));
    context.last_status = BP_OK;
    archive_file = fopen("READ.BLT", "rb");
    if (archive_file == NULL) return 1;
    bp_io_from_file(&archive_io, archive_file);
    live_before = bp_test_alloc_live_count();
    bp_test_alloc_fail_after(fail_after);
    rc = bp_mini_archive_extract_all_ex(&archive_io, open_extract_output,
                                        finish_extract_output, &context);
    bp_test_alloc_disable();
    if (context.file != NULL) fclose(context.file);
    fclose(archive_file);
    if (expect_nomem) {
        if (rc != BP_ERR_NOMEM) return 1;
        if (expect_output &&
            (context.done_count != 1 || context.last_status != BP_ERR_NOMEM))
            return 1;
        if (!expect_output && context.done_count != 0) return 1;
    } else if (rc != BP_OK || context.done_count != 1 ||
               context.last_status != BP_OK) return 1;
    if (bp_test_alloc_live_count() != live_before) return 1;
    remove("OUT.DAT");
    return 0;
}

static int run_read_failure_sweeps(void)
{
    long fail_after;
    if (make_read_fixture() != 0) return 1;
    for (fail_after = 0; fail_after < 6; fail_after++) {
        if (run_test_read_failure(fail_after, 1) != 0) return 1;
    }
    if (run_test_read_failure(6, 0) != 0) return 1;
    for (fail_after = 0; fail_after < 4; fail_after++) {
        if (run_extract_read_failure(fail_after, 1, 0) != 0) return 1;
    }
    for (fail_after = 4; fail_after < 6; fail_after++) {
        if (run_extract_read_failure(fail_after, 1, 1) != 0) return 1;
    }
    if (run_extract_read_failure(6, 0, 1) != 0) return 1;
    remove("READ.BLT");
    return 0;
}

int main(void)
{
    FILE *source;
    FILE *result;
    unsigned char data[1024];
    unsigned i;
    int failures = 0;

    result = fopen("ALLOCFL.TXT", "w");
    if (result == NULL) return 1;
    for (i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i * 37U);
    source = fopen("ALLOC.DAT", "wb");
    if (source == NULL) { fclose(result); return 1; }
    if (fwrite(data, 1, sizeof(data), source) != sizeof(data)) {
        fclose(source); fclose(result); return 1;
    }
    fclose(source);

    failures += run_archive_failure("FAIL0.BLT", 0, 0, 0); /* entry table */
    failures += run_archive_failure("FAIL1.BLT", 1, 0, 0); /* entry name */
    failures += run_archive_failure("FAIL2.BLT", 1, 1, 0); /* input block */
    failures += run_archive_failure("FAIL3.BLT", 2, 1, 0); /* output block */
    failures += run_archive_failure("FAIL4.BLT", 3, 1, 0); /* BoltLite workspace */
    failures += run_archive_failure("FAIL5.BLT", 4, 1, 1); /* retained workspace, then name */
    failures += run_common_failure(0, 0); /* projected-index allocation */
    failures += run_common_failure(1, 0); /* directory queue node */
    failures += run_common_failure(1, 1); /* directory queue path */
    failures += run_name_conversion_failure(); /* name round-trip buffer */
    failures += run_projected_scratch_failure(); /* projected-index scratch */
    failures += run_path_buffer_failures(); /* shared 16-bit directory-walker buffers */
    failures += run_read_failure_sweeps(); /* reader and extraction allocations */

    remove("ALLOC.DAT");
    if (failures != 0 || bp_test_alloc_live_count() != 1) {
        fprintf(result, "FAIL: %d failures, live allocations=%ld\n",
                failures, bp_test_alloc_live_count());
        fclose(result);
        return 1;
    }
    fprintf(result, "PASS: 26 writer/reader/extraction/index/queue/path-buffer/name allocation failures; temporary allocations released\n");
    fclose(result);
    return 0;
}
