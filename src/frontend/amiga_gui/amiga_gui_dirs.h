#ifndef BP_AMIGA_GUI_DIRS_H
#define BP_AMIGA_GUI_DIRS_H

#include "../../core/bp_types.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BP_AMIGA_GUI_DIR_QUEUE_MAX       256U
#define BP_AMIGA_GUI_DIR_QUEUE_BYTES_MAX 32768UL

typedef struct bp_amiga_gui_dir_restore_s {
    struct bp_amiga_gui_dir_restore_s *next;
    char *path;
    size_t path_len;
    bp_uint64 mtime;
    int mtime_valid;
    int native_seen;
    unsigned long protection;
    char comment[80];
} bp_amiga_gui_dir_restore_t;

typedef struct {
    bp_amiga_gui_dir_restore_t *head;
    unsigned int count;
    unsigned long path_bytes;
} bp_amiga_gui_dir_queue_t;

typedef int (*bp_amiga_gui_restore_time_fn)(const char *path,
                                            bp_uint64 mtime, void *user_data);
typedef int (*bp_amiga_gui_restore_native_fn)(const char *path,
                                              unsigned long protection,
                                              const char *comment,
                                              void *user_data);

static bp_amiga_gui_dir_restore_t *bp_amiga_gui_dir_queue_add(
    bp_amiga_gui_dir_queue_t *queue, const char *path,
    bp_uint64 mtime, int mtime_valid)
{
    bp_amiga_gui_dir_restore_t *node;
    size_t len;

    if (queue == NULL || path == NULL) return NULL;
    len = strlen(path);
    if (queue->count >= BP_AMIGA_GUI_DIR_QUEUE_MAX ||
        queue->path_bytes > BP_AMIGA_GUI_DIR_QUEUE_BYTES_MAX ||
        (unsigned long)len + 1UL >
            BP_AMIGA_GUI_DIR_QUEUE_BYTES_MAX - queue->path_bytes)
        return NULL;
    node = (bp_amiga_gui_dir_restore_t *)calloc(1, sizeof(*node));
    if (node == NULL) return NULL;
    node->path = (char *)malloc(len + 1);
    if (node->path == NULL) { free(node); return NULL; }
    memcpy(node->path, path, len + 1);
    node->path_len = len;
    node->mtime = mtime;
    node->mtime_valid = mtime_valid;
    node->next = queue->head;
    queue->head = node;
    queue->count++;
    queue->path_bytes += (unsigned long)len + 1UL;
    return node;
}

static int bp_amiga_gui_dir_set_native(bp_amiga_gui_dir_restore_t *node,
                                       unsigned long protection,
                                       const char *comment)
{
    size_t len;
    if (node == NULL || comment == NULL) return -1;
    len = strlen(comment);
    if (len > sizeof(node->comment) - 1) return -1;
    node->native_seen = 1;
    node->protection = protection;
    memcpy(node->comment, comment, len + 1);
    return 0;
}

static unsigned int bp_amiga_gui_path_depth(const char *path)
{
    unsigned int depth = 0;
    while (*path != '\0') if (*path++ == '/') depth++;
    return depth;
}

/* Apply children before parents so a parent operation cannot disturb a
 * descendant's timestamp or protection. Return nonzero if any restore fails. */
static int bp_amiga_gui_dir_queue_finalize(
    bp_amiga_gui_dir_queue_t *queue,
    bp_amiga_gui_restore_time_fn restore_time,
    bp_amiga_gui_restore_native_fn restore_native,
    void *user_data)
{
    int partial = 0;
    if (queue == NULL) return 1;
    while (queue->head != NULL) {
        bp_amiga_gui_dir_restore_t **best = &queue->head, **link;
        bp_amiga_gui_dir_restore_t *node;
        unsigned int best_depth = 0;
        int have_best = 0;
        for (link = &queue->head; *link != NULL; link = &(*link)->next) {
            unsigned int depth = bp_amiga_gui_path_depth((*link)->path);
            if (!have_best || depth > best_depth) {
                best_depth = depth;
                best = link;
                have_best = 1;
            }
        }
        node = *best;
        *best = node->next;
        if (node->mtime_valid &&
            (restore_time == NULL ||
             restore_time(node->path, node->mtime, user_data) != 0))
            partial = 1;
        if (node->native_seen &&
            (restore_native == NULL ||
             restore_native(node->path, node->protection, node->comment,
                            user_data) != 0))
            partial = 1;
        free(node->path);
        free(node);
        queue->count--;
    }
    queue->path_bytes = 0;
    return partial;
}

#endif
