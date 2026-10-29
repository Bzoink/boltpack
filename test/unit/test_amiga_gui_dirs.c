#include "../../src/frontend/amiga_gui/amiga_gui_dirs.h"

#include <stdio.h>
#include <string.h>

static char events[64];
static size_t event_count;
static int callback_failed;

static unsigned int depth_of(const char *path)
{
    unsigned int depth = 0;
    while (*path) if (*path++ == '/') depth++;
    return depth;
}

static void event(char kind, unsigned int depth)
{
    if (event_count + 2 < sizeof(events)) {
        events[event_count++] = kind;
        events[event_count++] = (char)('0' + depth);
        events[event_count] = '\0';
    } else callback_failed = 1;
}

static int restore_time(const char *path, bp_uint64 mtime, void *user_data)
{
    unsigned int depth = depth_of(path);
    (void)user_data;
    event('T', depth);
    if (mtime.hi != 0 || mtime.lo != 2000U + depth) callback_failed = 1;
    return depth == 2 ? -1 : 0;
}

static int restore_native(const char *path, unsigned long protection,
                          const char *comment, void *user_data)
{
    unsigned int depth = depth_of(path);
    (void)user_data;
    event('N', depth);
    if (protection != 100UL + depth || comment[0] != 'D' ||
        comment[1] != (char)('0' + depth) || comment[2] != '\0')
        callback_failed = 1;
    return 0;
}

static int test_deepest_first(void)
{
    bp_amiga_gui_dir_queue_t queue;
    bp_amiga_gui_dir_restore_t *node;
    int failed = 0;
    memset(&queue, 0, sizeof(queue));
    event_count = 0;
    events[0] = '\0';
    callback_failed = 0;

    node = bp_amiga_gui_dir_queue_add(&queue, "/VOL/root",
                                      bp_u64_from_u32(2002), 1);
    if (!node || bp_amiga_gui_dir_set_native(node, 102UL, "D2") != 0) failed++;
    node = bp_amiga_gui_dir_queue_add(&queue, "/VOL/root/nested",
                                      bp_u64_from_u32(2003), 1);
    if (!node || bp_amiga_gui_dir_set_native(node, 103UL, "D3") != 0) failed++;
    node = bp_amiga_gui_dir_queue_add(&queue, "/VOL/root/nested/empty",
                                      bp_u64_from_u32(2004), 1);
    if (!node || bp_amiga_gui_dir_set_native(node, 104UL, "D4") != 0) failed++;

    if (bp_amiga_gui_dir_queue_finalize(&queue, restore_time,
                                        restore_native, NULL) != 1)
        failed++;
    if (strcmp(events, "T4N4T3N3T2N2") != 0 || callback_failed) failed++;
    if (queue.head != NULL || queue.count != 0 || queue.path_bytes != 0) failed++;
    return failed;
}

static int test_queue_bounds(void)
{
    bp_amiga_gui_dir_queue_t queue;
    bp_uint64 unknown = {0, 0};
    char path[512];
    char long_path[501];
    unsigned int i;
    int failed = 0;

    memset(&queue, 0, sizeof(queue));
    for (i = 0; i < BP_AMIGA_GUI_DIR_QUEUE_MAX; ++i) {
        sprintf(path, "/d%03u", i);
        if (bp_amiga_gui_dir_queue_add(&queue, path, unknown, 0) == NULL)
            failed++;
    }
    if (queue.count != BP_AMIGA_GUI_DIR_QUEUE_MAX ||
        bp_amiga_gui_dir_queue_add(&queue, "/overflow", unknown, 0) != NULL)
        failed++;
    if (bp_amiga_gui_dir_queue_finalize(&queue, NULL, NULL, NULL) != 0)
        failed++;

    memset(&queue, 0, sizeof(queue));
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    for (i = 0; i < 65; ++i)
        if (bp_amiga_gui_dir_queue_add(&queue, long_path, unknown, 0) == NULL)
            failed++;
    if (queue.path_bytes != 65UL * sizeof(long_path) ||
        bp_amiga_gui_dir_queue_add(&queue, long_path, unknown, 0) != NULL)
        failed++;
    if (bp_amiga_gui_dir_queue_finalize(&queue, NULL, NULL, NULL) != 0)
        failed++;
    return failed;
}

int main(void)
{
    int failed = test_deepest_first() + test_queue_bounds();
    printf("Amiga GUI directory metadata queue: %s\n",
           failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
