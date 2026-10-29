#ifndef BOLTPACK_PLAN9_PATH_H
#define BOLTPACK_PLAN9_PATH_H

#if !defined(__PLAN9__)
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "../../core/archiver.h"

#define P9_PATH_INDEX_INITIAL_SLOTS 1024U
#define P9_PATH_INDEX_MAX_SLOTS  131072U
#define P9_PATH_INDEX_MAX_NODES    65535U
#define P9_PATH_INDEX_MAX_ENTRIES  65535U
#define P9_PATH_INDEX_MAX_BYTES   (16UL * 1024UL * 1024UL)

typedef struct {
    char *name;
    unsigned char is_directory;
    unsigned char explicit_directory;
} p9_path_slot_t;

typedef struct {
    p9_path_slot_t *slots;
    unsigned int slot_count;
    unsigned int nodes;
    unsigned int entries;
    unsigned long key_bytes;
} p9_path_index_t;

static void p9_path_index_free(p9_path_index_t *index)
{
    unsigned int i;
    if (index == NULL) return;
    if (index->slots != NULL) {
        for (i = 0; i < index->slot_count; i++)
            free(index->slots[i].name);
        free(index->slots);
    }
    memset(index, 0, sizeof(*index));
}

static int p9_path_index_init(p9_path_index_t *index)
{
    if (index == NULL) return BP_ERR_INVALID_ARG;
    memset(index, 0, sizeof(*index));
    index->slot_count = P9_PATH_INDEX_INITIAL_SLOTS;
    index->slots = (p9_path_slot_t *)calloc(index->slot_count,
                                             sizeof(p9_path_slot_t));
    if (index->slots == NULL) {
        index->slot_count = 0;
        return BP_ERR_NOMEM;
    }
    return BP_OK;
}

static unsigned long p9_path_hash(const char *name)
{
    unsigned long hash = 2166136261UL;
    const unsigned char *p = (const unsigned char *)name;
    while (*p != 0) {
        hash ^= (unsigned long)*p++;
        hash *= 16777619UL;
    }
    return hash;
}

static p9_path_slot_t *p9_path_find(p9_path_index_t *index, const char *name,
                                    int *found)
{
    unsigned int slot = (unsigned int)(p9_path_hash(name) &
                                       (index->slot_count - 1U));
    unsigned int probes;
    for (probes = 0; probes < index->slot_count; probes++) {
        p9_path_slot_t *item = &index->slots[slot];
        if (item->name == NULL) {
            *found = 0;
            return item;
        }
        if (strcmp(item->name, name) == 0) {
            *found = 1;
            return item;
        }
        slot = (slot + 1U) & (index->slot_count - 1U);
    }
    *found = 0;
    return NULL;
}

static int p9_path_grow(p9_path_index_t *index)
{
    unsigned int new_count;
    unsigned int i;
    p9_path_slot_t *new_slots;
    if (index->slot_count >= P9_PATH_INDEX_MAX_SLOTS) return BP_ERR_PARTIAL;
    new_count = index->slot_count * 2U;
    new_slots = (p9_path_slot_t *)calloc(new_count, sizeof(p9_path_slot_t));
    if (new_slots == NULL) return BP_ERR_NOMEM;
    for (i = 0; i < index->slot_count; i++) {
        p9_path_slot_t *old = &index->slots[i];
        unsigned int slot;
        if (old->name == NULL) continue;
        slot = (unsigned int)(p9_path_hash(old->name) & (new_count - 1U));
        while (new_slots[slot].name != NULL)
            slot = (slot + 1U) & (new_count - 1U);
        new_slots[slot] = *old;
    }
    free(index->slots);
    index->slots = new_slots;
    index->slot_count = new_count;
    return BP_OK;
}

static int p9_path_insert_prefix(p9_path_index_t *index, const char *name,
                                 int is_directory, int explicit_directory)
{
    int found;
    p9_path_slot_t *slot = p9_path_find(index, name, &found);
    size_t length;
    if (slot == NULL) return BP_ERR_PARTIAL;
    if (found) {
        if (!slot->is_directory) return BP_ERR_PARTIAL;
        if (explicit_directory) {
            if (slot->explicit_directory) return BP_ERR_PARTIAL;
            slot->explicit_directory = 1;
        }
        if (!is_directory) return BP_ERR_PARTIAL;
        return BP_OK;
    }
    if (index->nodes >= P9_PATH_INDEX_MAX_NODES) return BP_ERR_PARTIAL;
    if ((index->nodes + 1U) * 4U >= index->slot_count * 3U) {
        int rc = p9_path_grow(index);
        if (rc != BP_OK) return rc;
        slot = p9_path_find(index, name, &found);
        if (slot == NULL || found) return BP_ERR_PARTIAL;
    }
    length = strlen(name) + 1U;
    if (length > P9_PATH_INDEX_MAX_BYTES - index->key_bytes)
        return BP_ERR_PARTIAL;
    slot->name = (char *)malloc(length);
    if (slot->name == NULL) return BP_ERR_NOMEM;
    memcpy(slot->name, name, length);
    slot->is_directory = (unsigned char)(is_directory != 0);
    slot->explicit_directory = (unsigned char)(explicit_directory != 0);
    index->nodes++;
    index->key_bytes += (unsigned long)length;
    return BP_OK;
}

/* Index one archive path and all its implicit parent directories. */
static int p9_path_index_add(p9_path_index_t *index, const char *archive_name,
                             int is_directory)
{
    char path[512];
    size_t length;
    size_t i;
    int rc;
    if (index == NULL || index->slots == NULL || archive_name == NULL)
        return BP_ERR_INVALID_ARG;
    if (!bp_name_is_safe(archive_name)) return BP_ERR_PARTIAL;
    length = strlen(archive_name);
    if (length == 0 || length >= sizeof(path)) return BP_ERR_NAME_TOO_LONG;
    if (index->entries >= P9_PATH_INDEX_MAX_ENTRIES) return BP_ERR_PARTIAL;
    memcpy(path, archive_name, length + 1U);
    if (path[length - 1U] == '/') {
        is_directory = 1;
        path[--length] = '\0';
    }
    if (length == 0) return BP_ERR_PARTIAL;
    index->entries++;
    for (i = 0; i <= length; i++) {
        int final_component = i == length;
        char saved;
        if (!final_component && path[i] != '/') continue;
        if (i == 0 || path[i - 1U] == '/') return BP_ERR_PARTIAL;
        saved = path[i];
        path[i] = '\0';
        rc = p9_path_insert_prefix(index, path, !final_component || is_directory,
                                   final_component && is_directory);
        path[i] = saved;
        if (rc != BP_OK) return rc;
    }
    return BP_OK;
}

#endif
