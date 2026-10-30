/* Allocation-size accounting used only by the DOS16 instrumented build.
 * The wrappers do not add per-allocation tracking records, so measured blocks
 * retain the production allocator's requested sizes and placement. */
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>

static unsigned long bp_alloc_successes;
static unsigned long bp_alloc_requested_bytes;
static unsigned long bp_alloc_usable_bytes;
static unsigned long bp_alloc_max_request;
static unsigned long bp_alloc_max_usable;
static unsigned long bp_alloc_max_slack;
static unsigned long bp_alloc_invalid_sizes;
static unsigned long bp_alloc_live_usable;
static unsigned long bp_alloc_peak_live_usable;

static unsigned long bp_alloc_usable_size(void *ptr)
{
    return ptr == NULL ? 0UL : (unsigned long)_msize(ptr);
}

static void bp_alloc_release(void *ptr)
{
    unsigned long usable = bp_alloc_usable_size(ptr);
    if (usable <= bp_alloc_live_usable)
        bp_alloc_live_usable -= usable;
    else
        bp_alloc_invalid_sizes++;
}

static void bp_alloc_record(unsigned long requested, void *ptr)
{
    unsigned long usable;
    if (ptr == NULL) return;
    usable = bp_alloc_usable_size(ptr);
    bp_alloc_successes++;
    bp_alloc_requested_bytes += requested;
    bp_alloc_usable_bytes += usable;
    if (requested > bp_alloc_max_request) bp_alloc_max_request = requested;
    if (usable > bp_alloc_max_usable) bp_alloc_max_usable = usable;
    if (usable < requested) {
        bp_alloc_invalid_sizes++;
    } else if (usable - requested > bp_alloc_max_slack) {
        bp_alloc_max_slack = usable - requested;
    }
    bp_alloc_live_usable += usable;
    if (bp_alloc_live_usable > bp_alloc_peak_live_usable)
        bp_alloc_peak_live_usable = bp_alloc_live_usable;
}

void *bp_test_malloc(size_t size)
{
    void *ptr = malloc(size);
    bp_alloc_record((unsigned long)size, ptr);
    return ptr;
}

void *bp_test_calloc(size_t count, size_t size)
{
    void *ptr = calloc(count, size);
    bp_alloc_record((unsigned long)count * (unsigned long)size, ptr);
    return ptr;
}

void *bp_test_realloc(void *old_ptr, size_t size)
{
    unsigned long old_usable = bp_alloc_usable_size(old_ptr);
    void *ptr = realloc(old_ptr, size);
    if (ptr != NULL && old_ptr != NULL) {
        if (old_usable <= bp_alloc_live_usable)
            bp_alloc_live_usable -= old_usable;
        else
            bp_alloc_invalid_sizes++;
    }
    bp_alloc_record((unsigned long)size, ptr);
    return ptr;
}

void bp_test_free(void *ptr)
{
    bp_alloc_release(ptr);
    free(ptr);
}

void bp_test_alloc_report(void)
{
    printf("BP_ALLOC_SUCCESSFUL_CALLS=%lu REQUESTED_BYTES=%lu "
           "USABLE_BYTES=%lu MAX_REQUEST=%lu MAX_USABLE=%lu "
           "MAX_BLOCK_SLACK=%lu PEAK_LIVE_USABLE=%lu "
           "LIVE_USABLE_AT_EXIT=%lu INVALID_SIZES=%lu\n",
           bp_alloc_successes, bp_alloc_requested_bytes,
           bp_alloc_usable_bytes, bp_alloc_max_request,
           bp_alloc_max_usable, bp_alloc_max_slack,
           bp_alloc_peak_live_usable, bp_alloc_live_usable,
           bp_alloc_invalid_sizes);
}
