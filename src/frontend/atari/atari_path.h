/* Atari TOS filename projection and bounded collision index (C89). */
#ifndef BP_ATARI_PATH_H
#define BP_ATARI_PATH_H

#include "../../core/bp_types.h"
#include <stdlib.h>
#include <string.h>

#define BP_ATARI_PATH_MAX_ENTRIES 1024U
#define BP_ATARI_PATH_MAX_BYTES 32768UL
#define BP_ATARI_PATH_BUCKETS 64U
#define BP_ATARI_GEMDOS_PATH_MAX 125U

typedef struct bp_atari_name_node_s {
    struct bp_atari_name_node_s *next;
    char *key;
    unsigned int length;
    unsigned long hash;
} bp_atari_name_node_t;

typedef struct {
    bp_atari_name_node_t **buckets;
    unsigned int count;
    unsigned long bytes;
} bp_atari_name_set_t;

static int bp_atari_name_char_allowed(unsigned char c)
{
    if (c < 0x21U || c > 0x7EU) return 0;
    if (c == ':' || c == '/' || c == '\\' || c == '?' || c == '*') return 0;
    return 1;
}

/* Convert a local Atari byte path to its exact base-GEMDOS key.
 * TOS folds ASCII letters to uppercase and stores 8.3 path components. */
static int bp_atari_project_name(const char *local_name, int is_directory,
                                char *key, size_t capacity,
                                unsigned int *key_length)
{
    size_t length, pos = 0, component_start = 0, i;
    if (local_name == NULL || key == NULL || key_length == NULL)
        return BP_ERR_INVALID_ARG;
    length = strlen(local_name);
    if (is_directory && length > 0 && local_name[length - 1] == '/') length--;
    if (length == 0 || length > BP_ATARI_GEMDOS_PATH_MAX ||
        length + 1 > capacity) return BP_ERR_OVERFLOW;

    for (i = 0; i <= length; i++) {
        unsigned char c = (unsigned char)local_name[i];
        if (i == length || c == '/') {
            size_t component_length = i - component_start;
            size_t j, base_length = 0, extension_length = 0;
            int in_extension = 0;
            if (component_length == 0) return BP_ERR_INVALID_ARG;
            for (j = component_start; j < i; j++) {
                c = (unsigned char)local_name[j];
                if (c == '.') {
                    if (in_extension || base_length == 0) return BP_ERR_INVALID_ARG;
                    in_extension = 1;
                } else {
                    if (!bp_atari_name_char_allowed(c)) return BP_ERR_INVALID_ARG;
                    if (in_extension) extension_length++;
                    else base_length++;
                }
            }
            if (base_length == 0 || base_length > 8U ||
                extension_length > 3U || (in_extension && extension_length == 0))
                return BP_ERR_INVALID_ARG;
            if (i < length) key[pos++] = '\\';
            component_start = i + 1;
        } else {
            if (!bp_atari_name_char_allowed(c)) return BP_ERR_INVALID_ARG;
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
            key[pos++] = (char)c;
        }
    }
    key[pos] = '\0';
    *key_length = (unsigned int)pos;
    return BP_OK;
}

static void bp_atari_name_set_init(bp_atari_name_set_t *set)
{
    if (set == NULL) return;
    set->buckets = NULL;
    set->count = 0;
    set->bytes = 0;
}

/* BP_ERR_CORRUPT means another archive name projects to this destination. */
static int bp_atari_name_set_add(bp_atari_name_set_t *set,
                                 const char *key, unsigned int length)
{
    bp_atari_name_node_t *node;
    unsigned long hash = 2166136261UL;
    unsigned int i, bucket;
    if (set == NULL || key == NULL) return BP_ERR_INVALID_ARG;
    if (set->count >= BP_ATARI_PATH_MAX_ENTRIES ||
        (unsigned long)length + 1UL > BP_ATARI_PATH_MAX_BYTES - set->bytes)
        return BP_ERR_OVERFLOW;
    for (i = 0; i < length; i++)
        hash = (hash ^ (unsigned char)key[i]) * 16777619UL;
    bucket = (unsigned int)(hash % BP_ATARI_PATH_BUCKETS);
    if (set->buckets == NULL) {
        set->buckets = (bp_atari_name_node_t **)calloc(
            BP_ATARI_PATH_BUCKETS, sizeof(*set->buckets));
        if (set->buckets == NULL) return BP_ERR_NOMEM;
    }
    for (node = set->buckets[bucket]; node != NULL; node = node->next) {
        if (node->hash == hash && node->length == length &&
            memcmp(node->key, key, length) == 0) return BP_ERR_CORRUPT;
    }
    node = (bp_atari_name_node_t *)malloc(sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->key = (char *)malloc((size_t)length + 1U);
    if (node->key == NULL) { free(node); return BP_ERR_NOMEM; }
    memcpy(node->key, key, (size_t)length);
    node->key[length] = '\0';
    node->length = length;
    node->hash = hash;
    node->next = set->buckets[bucket];
    set->buckets[bucket] = node;
    set->count++;
    set->bytes += (unsigned long)length + 1UL;
    return BP_OK;
}

static void bp_atari_name_set_free(bp_atari_name_set_t *set)
{
    unsigned int i;
    if (set == NULL) return;
    if (set->buckets != NULL) {
        for (i = 0; i < BP_ATARI_PATH_BUCKETS; i++) {
            bp_atari_name_node_t *node = set->buckets[i];
            while (node != NULL) {
                bp_atari_name_node_t *next = node->next;
                free(node->key);
                free(node);
                node = next;
            }
        }
        free(set->buckets);
    }
    bp_atari_name_set_init(set);
}

#endif
