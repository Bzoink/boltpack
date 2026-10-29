/* Allocation hooks used only by the 16-bit failure-injection executable. */
#ifdef malloc
#undef malloc
#endif
#ifdef calloc
#undef calloc
#endif
#ifdef realloc
#undef realloc
#endif
#ifdef free
#undef free
#endif
#include <stddef.h>
#include <stdlib.h>

static long bp_fail_at = -1;
static long bp_alloc_calls;
static long bp_live_allocations;
static size_t bp_fail_size;
static long bp_fail_size_at = -1;
static long bp_fail_size_calls;

static int bp_should_fail(size_t size)
{
    long call = bp_alloc_calls++;
    if (bp_fail_at >= 0 && call == bp_fail_at) return 1;
    if (bp_fail_size_at >= 0 && size == bp_fail_size &&
        bp_fail_size_calls++ == bp_fail_size_at) return 1;
    return 0;
}

void bp_test_alloc_fail_after(long successful_allocations)
{
    bp_fail_at = successful_allocations;
    bp_alloc_calls = 0;
    bp_fail_size_at = -1;
    bp_fail_size_calls = 0;
}

void bp_test_alloc_fail_size_after(size_t size, long matching_allocations)
{
    bp_fail_at = -1;
    bp_alloc_calls = 0;
    bp_fail_size = size;
    bp_fail_size_at = matching_allocations;
    bp_fail_size_calls = 0;
}

void bp_test_alloc_disable(void)
{
    bp_fail_at = -1;
    bp_alloc_calls = 0;
    bp_fail_size_at = -1;
    bp_fail_size_calls = 0;
}

long bp_test_alloc_live_count(void)
{
    return bp_live_allocations;
}

void *bp_test_malloc(size_t size)
{
    void *p;
    if (bp_should_fail(size)) return NULL;
    p = malloc(size);
    if (p != NULL) bp_live_allocations++;
    return p;
}

void *bp_test_calloc(size_t count, size_t size)
{
    void *p;
    if (bp_should_fail(count * size)) return NULL;
    p = calloc(count, size);
    if (p != NULL) bp_live_allocations++;
    return p;
}

void *bp_test_realloc(void *ptr, size_t size)
{
    void *p;
    if (bp_should_fail(size)) return NULL;
    p = realloc(ptr, size);
    if (p != NULL && ptr == NULL) bp_live_allocations++;
    return p;
}

void bp_test_free(void *ptr)
{
    if (ptr != NULL) bp_live_allocations--;
    free(ptr);
}
