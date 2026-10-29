#ifndef BOLTPACK_X68K_PATH_INDEX_H
#define BOLTPACK_X68K_PATH_INDEX_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/archiver.h"

#define X68K_INDEX_INITIAL_SLOTS 256U
#define X68K_INDEX_MAX_SLOTS 131072U
#define X68K_INDEX_MAX_NODES 65535U
#define X68K_INDEX_MAX_ENTRIES 65535U
#define X68K_INDEX_MAX_BYTES (4UL * 1024UL * 1024UL)
#define X68K_INDEX_MAX_PATH 256U

typedef struct {
    char *key;
    unsigned char is_directory;
    unsigned char explicit_directory;
} x68k_path_slot_t;

typedef struct {
    x68k_path_slot_t *slots;
    unsigned int slot_count;
    unsigned int nodes;
    unsigned int entries;
    unsigned long key_bytes;
} x68k_path_index_t;

static void x68k_path_index_free(x68k_path_index_t *index)
{
    unsigned int i;
    if (index == NULL) return;
    if (index->slots != NULL) {
        for (i = 0; i < index->slot_count; i++) free(index->slots[i].key);
        free(index->slots);
    }
    memset(index, 0, sizeof(*index));
}

static int x68k_path_index_init(x68k_path_index_t *index)
{
    if (index == NULL) return BP_ERR_INVALID_ARG;
    memset(index, 0, sizeof(*index));
    index->slot_count = X68K_INDEX_INITIAL_SLOTS;
    index->slots = (x68k_path_slot_t *)calloc(index->slot_count,
                                               sizeof(x68k_path_slot_t));
    if (index->slots == NULL) {
        index->slot_count = 0;
        return BP_ERR_NOMEM;
    }
    return BP_OK;
}

static unsigned long x68k_path_hash(const char *key)
{
    unsigned long hash = 2166136261UL;
    const unsigned char *p = (const unsigned char *)key;
    while (*p != 0) { hash ^= (unsigned long)*p++; hash *= 16777619UL; }
    return hash;
}

static x68k_path_slot_t *x68k_path_find(x68k_path_index_t *index,
                                        const char *key, int *found)
{
    unsigned int slot = (unsigned int)(x68k_path_hash(key) &
                                       (index->slot_count - 1U));
    unsigned int probes;
    for (probes = 0; probes < index->slot_count; probes++) {
        x68k_path_slot_t *item = &index->slots[slot];
        if (item->key == NULL) { *found = 0; return item; }
        if (strcmp(item->key, key) == 0) { *found = 1; return item; }
        slot = (slot + 1U) & (index->slot_count - 1U);
    }
    *found = 0;
    return NULL;
}

static int x68k_path_grow(x68k_path_index_t *index)
{
    unsigned int count, i;
    x68k_path_slot_t *slots;
    if (index->slot_count >= X68K_INDEX_MAX_SLOTS) return BP_ERR_PARTIAL;
    count = index->slot_count * 2U;
    slots = (x68k_path_slot_t *)calloc(count, sizeof(x68k_path_slot_t));
    if (slots == NULL) return BP_ERR_NOMEM;
    for (i = 0; i < index->slot_count; i++) {
        x68k_path_slot_t *old = &index->slots[i];
        unsigned int slot;
        if (old->key == NULL) continue;
        slot = (unsigned int)(x68k_path_hash(old->key) & (count - 1U));
        while (slots[slot].key != NULL) slot = (slot + 1U) & (count - 1U);
        slots[slot] = *old;
    }
    free(index->slots);
    index->slots = slots;
    index->slot_count = count;
    return BP_OK;
}

static int x68k_path_insert(x68k_path_index_t *index, const char *key,
                            int is_directory, int explicit_directory)
{
    int found;
    x68k_path_slot_t *slot = x68k_path_find(index, key, &found);
    size_t length;
    if (slot == NULL) return BP_ERR_PARTIAL;
    if (found) {
        if (!slot->is_directory) return BP_ERR_PARTIAL;
        if (explicit_directory) {
            if (slot->explicit_directory) return BP_ERR_PARTIAL;
            slot->explicit_directory = 1;
        }
        return is_directory ? BP_OK : BP_ERR_PARTIAL;
    }
    if (index->nodes >= X68K_INDEX_MAX_NODES) return BP_ERR_PARTIAL;
    if ((index->nodes + 1U) * 4U >= index->slot_count * 3U) {
        int rc = x68k_path_grow(index);
        if (rc != BP_OK) return rc;
        slot = x68k_path_find(index, key, &found);
        if (slot == NULL || found) return BP_ERR_PARTIAL;
    }
    length = strlen(key) + 1U;
    if (length > X68K_INDEX_MAX_BYTES - index->key_bytes)
        return BP_ERR_PARTIAL;
    slot->key = (char *)malloc(length);
    if (slot->key == NULL) return BP_ERR_NOMEM;
    memcpy(slot->key, key, length);
    slot->is_directory = (unsigned char)(is_directory != 0);
    slot->explicit_directory = (unsigned char)(explicit_directory != 0);
    index->nodes++;
    index->key_bytes += (unsigned long)length;
    return BP_OK;
}

static int x68k_index_sjis_pair_at(const char *p)
{
    unsigned char lead = (unsigned char)p[0];
    unsigned char trail;
    if (!((lead >= 0x81 && lead <= 0x9F) ||
          (lead >= 0xE0 && lead <= 0xEF)) || p[1] == '\0') return 0;
    trail = (unsigned char)p[1];
    return (trail >= 0x40 && trail <= 0x7E) ||
           (trail >= 0x80 && trail <= 0xFC);
}

/* Index a Human68k-projected path. ASCII is folded to uppercase to model
 * DOS case-insensitivity; Shift-JIS bytes outside ASCII remain unchanged. */
static int x68k_path_index_add(x68k_path_index_t *index,
                               const char *projected_name, int is_directory)
{
    char key[X68K_INDEX_MAX_PATH];
    size_t length, i;
    unsigned char ch;
    int rc;
    if (index == NULL || index->slots == NULL || projected_name == NULL)
        return BP_ERR_INVALID_ARG;
    length = strlen(projected_name);
    if (length == 0 || length >= sizeof(key)) return BP_ERR_NAME_TOO_LONG;
    if (index->entries >= X68K_INDEX_MAX_ENTRIES) return BP_ERR_PARTIAL;
    if (!is_directory && projected_name[length - 1U] == '/')
        return BP_ERR_PARTIAL;
    for (i = 0; i < length; i++) {
        if (x68k_index_sjis_pair_at(projected_name + i)) {
            key[i] = projected_name[i];
            key[i + 1U] = projected_name[i + 1U];
            i++;
            continue;
        }
        ch = (unsigned char)projected_name[i];
        if (ch == '\\') ch = '/';
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        key[i] = (char)ch;
    }
    key[length] = '\0';
    if (key[length - 1U] == '/') {
        is_directory = 1;
        key[--length] = '\0';
    }
    if (length == 0) return BP_ERR_PARTIAL;
    index->entries++;
    for (i = 0; i <= length; i++) {
        int final = i == length;
        char saved;
        if (!final && key[i] != '/') continue;
        if (i == 0 || key[i - 1U] == '/') return BP_ERR_PARTIAL;
        saved = key[i]; key[i] = '\0';
        rc = x68k_path_insert(index, key,
                              !final || is_directory,
                              final && is_directory);
        key[i] = saved;
        if (rc != BP_OK) return rc;
    }
    return BP_OK;
}

#endif
