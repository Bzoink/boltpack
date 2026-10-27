/* Bounded, case-sensitive path collision preflight for POSIX frontends. */
#ifndef BP_POSIX_ARCHIVE_PREFLIGHT_H
#define BP_POSIX_ARCHIVE_PREFLIGHT_H

#include "../../core/bp_types.h"

#include <stdlib.h>
#include <string.h>

#ifndef BP_POSIX_PREFLIGHT_MAX_ENTRIES
#define BP_POSIX_PREFLIGHT_MAX_ENTRIES 1048576U
#endif
#ifndef BP_POSIX_PREFLIGHT_MAX_KEY_BYTES
#define BP_POSIX_PREFLIGHT_MAX_KEY_BYTES (64U * 1024U * 1024U)
#endif
#define BP_POSIX_PREFLIGHT_MAX_PATH 4096U
#define BP_POSIX_PREFLIGHT_MAX_COMPONENT 255U

typedef struct {
    char *name;
    int is_directory;
} bp_posix_projected_name_t;

typedef struct {
    bp_posix_projected_name_t *names;
    unsigned int count;
    unsigned int capacity;
    size_t key_bytes;
} bp_posix_preflight_t;

static int bp_posix_projected_name_compare(const void *left,
                                           const void *right)
{
    const bp_posix_projected_name_t *a =
        (const bp_posix_projected_name_t *)left;
    const bp_posix_projected_name_t *b =
        (const bp_posix_projected_name_t *)right;
    return strcmp(a->name, b->name);
}

static int bp_posix_preflight_add(bp_posix_preflight_t *preflight,
                                  const char *name, size_t name_len,
                                  int is_directory)
{
    size_t length = name_len, start, i;
    char *copy;
    bp_posix_projected_name_t *grown;

    if (preflight == NULL || name == NULL || name_len == 0 ||
        name_len >= BP_POSIX_PREFLIGHT_MAX_PATH || strlen(name) != name_len)
        return BP_ERR_PARTIAL;
    if (is_directory && name[length - 1] == '/') length--;
    if (length == 0 || length >= BP_POSIX_PREFLIGHT_MAX_PATH)
        return BP_ERR_NAME_TOO_LONG;

    for (start = 0, i = 0; i <= length; i++) {
        if (i == length || name[i] == '/') {
            size_t component_length = i - start;
            if (component_length == 0 ||
                component_length > BP_POSIX_PREFLIGHT_MAX_COMPONENT)
                return component_length > BP_POSIX_PREFLIGHT_MAX_COMPONENT
                    ? BP_ERR_NAME_TOO_LONG : BP_ERR_PARTIAL;
            if ((component_length == 1U && name[start] == '.') ||
                (component_length == 2U && name[start] == '.' &&
                 name[start + 1] == '.')) return BP_ERR_PARTIAL;
            start = i + 1;
        }
    }
    if (preflight->count >= BP_POSIX_PREFLIGHT_MAX_ENTRIES ||
        length + 1U > BP_POSIX_PREFLIGHT_MAX_KEY_BYTES ||
        preflight->key_bytes > BP_POSIX_PREFLIGHT_MAX_KEY_BYTES - length - 1U)
        return BP_ERR_OVERFLOW;

    if (preflight->count == preflight->capacity) {
        unsigned int capacity = preflight->capacity == 0 ? 64U :
                                preflight->capacity * 2U;
        if (capacity > BP_POSIX_PREFLIGHT_MAX_ENTRIES)
            capacity = BP_POSIX_PREFLIGHT_MAX_ENTRIES;
        grown = (bp_posix_projected_name_t *)realloc(
            preflight->names, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) return BP_ERR_NOMEM;
        preflight->names = grown;
        preflight->capacity = capacity;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) return BP_ERR_NOMEM;
    memcpy(copy, name, length);
    copy[length] = '\0';
    preflight->names[preflight->count].name = copy;
    preflight->names[preflight->count].is_directory = is_directory;
    preflight->count++;
    preflight->key_bytes += length + 1U;
    return BP_OK;
}

static unsigned int bp_posix_preflight_lower_bound(
    const bp_posix_preflight_t *preflight, const char *name)
{
    unsigned int low = 0, high = preflight->count;
    while (low < high) {
        unsigned int middle = low + (high - low) / 2U;
        if (strcmp(preflight->names[middle].name, name) < 0)
            low = middle + 1U;
        else
            high = middle;
    }
    return low;
}

static int bp_posix_preflight_finish(bp_posix_preflight_t *preflight)
{
    unsigned int i;
    int rc = BP_OK;
    if (preflight->count > 1U)
        qsort(preflight->names, preflight->count, sizeof(*preflight->names),
              bp_posix_projected_name_compare);
    for (i = 0; i < preflight->count; i++) {
        bp_posix_projected_name_t *current = &preflight->names[i];
        if (i > 0 &&
            strcmp(preflight->names[i - 1].name, current->name) == 0)
            return BP_ERR_PARTIAL;
        if (!current->is_directory) {
            size_t length = strlen(current->name);
            char *prefix = (char *)malloc(length + 2U);
            unsigned int child;
            if (prefix == NULL) return BP_ERR_NOMEM;
            memcpy(prefix, current->name, length);
            prefix[length] = '/';
            prefix[length + 1U] = '\0';
            child = bp_posix_preflight_lower_bound(preflight, prefix);
            if (child < preflight->count &&
                strncmp(preflight->names[child].name, prefix,
                        length + 1U) == 0)
                rc = BP_ERR_PARTIAL;
            free(prefix);
            if (rc != BP_OK) return rc;
        }
    }
    return rc;
}

static void bp_posix_preflight_free(bp_posix_preflight_t *preflight)
{
    unsigned int i;
    if (preflight == NULL) return;
    for (i = 0; i < preflight->count; i++) free(preflight->names[i].name);
    free(preflight->names);
    memset(preflight, 0, sizeof(*preflight));
}

#endif
