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

static int bp_atari_path_separator(char c)
{
    return c == '\\' || c == '/';
}

static int bp_atari_path_component_safe(const char *path)
{
    size_t i = 0;
    if (path == NULL || path[0] == '\0') return 0;
    while (path[i] != '\0') {
        size_t start, length;
        if (bp_atari_path_separator(path[i])) {
            if (i != 0 && bp_atari_path_separator(path[i - 1])) return 0;
            i++;
            continue;
        }
        start = i;
        while (path[i] != '\0' && !bp_atari_path_separator(path[i])) {
            if (path[i] == ':' &&
                !(start == 0 && i == 1 &&
                  ((path[0] >= 'A' && path[0] <= 'Z') ||
                   (path[0] >= 'a' && path[0] <= 'z'))))
                return 0;
            i++;
        }
        length = i - start;
        if ((length == 1U && path[start] == '.') ||
            (length == 2U && path[start] == '.' && path[start + 1] == '.'))
            return 0;
        if (bp_atari_path_separator(path[i]) &&
            path[i + 1] != '\0' && bp_atari_path_separator(path[i + 1]))
            return 0;
    }
    return 1;
}

/* Check a journal path lexically before using it for recovery. GEMDOS paths
 * are case-insensitive, and both slash forms are accepted by the frontend. */
static int bp_atari_path_under_root(const char *root, const char *path)
{
    size_t root_length, path_length, i;
    if (!bp_atari_path_component_safe(root) ||
        !bp_atari_path_component_safe(path))
        return 0;
    root_length = strlen(root);
    path_length = strlen(path);
    if (path_length <= root_length ||
        bp_atari_path_separator(path[path_length - 1])) return 0;
    for (i = 0; i < root_length; i++) {
        unsigned char a = (unsigned char)root[i];
        unsigned char b = (unsigned char)path[i];
        if (bp_atari_path_separator((char)a) &&
            bp_atari_path_separator((char)b))
            continue;
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    if (bp_atari_path_separator(root[root_length - 1])) return 1;
    return bp_atari_path_separator(path[root_length]);
}

static int bp_atari_selector_dir(const char *pattern, size_t pattern_capacity,
                                 char *dir, size_t capacity)
{
    size_t length, directory_length, i;
    size_t separator = (size_t)-1;
    if (pattern == NULL || dir == NULL || pattern_capacity == 0 || capacity == 0)
        return BP_ERR_INVALID_ARG;
    for (length = 0; length < pattern_capacity && pattern[length] != '\0'; length++)
        ;
    if (length == pattern_capacity)
        return BP_ERR_OVERFLOW;
    for (i = 0; i < length; i++)
        if (pattern[i] == '\\' || pattern[i] == '/') separator = i;
    directory_length = separator != (size_t)-1 ? separator : length;
    if (directory_length > BP_ATARI_GEMDOS_PATH_MAX ||
        directory_length >= capacity)
        return BP_ERR_OVERFLOW;
    memcpy(dir, pattern, directory_length);
    dir[directory_length] = '\0';
    return BP_OK;
}

static int bp_atari_selector_join(const char *pattern,
                                  size_t pattern_capacity,
                                  const char *filename, char *path,
                                  size_t capacity)
{
    char dir[BP_ATARI_GEMDOS_PATH_MAX + 1U];
    size_t dir_length, name_length, separator_length;
    int rc;
    if (filename == NULL || path == NULL || capacity == 0)
        return BP_ERR_INVALID_ARG;
    rc = bp_atari_selector_dir(pattern, pattern_capacity, dir, sizeof(dir));
    if (rc != BP_OK) return rc;
    dir_length = strlen(dir);
    name_length = strlen(filename);
    separator_length = dir_length != 0 && dir[dir_length - 1] != '\\' &&
                       dir[dir_length - 1] != '/' ? 1U : 0U;
    if (dir_length + separator_length + name_length >
            BP_ATARI_GEMDOS_PATH_MAX ||
        dir_length + separator_length + name_length >= capacity)
        return BP_ERR_OVERFLOW;
    memcpy(path, dir, dir_length);
    if (separator_length != 0) path[dir_length++] = '\\';
    memcpy(path + dir_length, filename, name_length);
    path[dir_length + name_length] = '\0';
    return BP_OK;
}

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

/* Include implicit parent directories and retain their original spelling.
 * GEMDOS projection is ASCII, so exact and folded paths have equal lengths. */
typedef struct bp_atari_tree_node_s {
    struct bp_atari_tree_node_s *next;
    char *names; /* projected name, NUL, exact name, NUL */
    unsigned int length;
    int directory;
    int explicit_entry;
} bp_atari_tree_node_t;

typedef struct {
    bp_atari_tree_node_t *nodes;
    unsigned int count;
    unsigned long bytes;
} bp_atari_tree_t;

static int bp_atari_tree_add(bp_atari_tree_t *tree, const char *exact,
                             const char *projected, unsigned int length,
                             int directory)
{
    unsigned int pos;
    if (!tree || !exact || !projected || length == 0 ||
        strlen(projected) != length || strlen(exact) < length)
        return BP_ERR_INVALID_ARG;
    for (pos = 0; pos <= length; pos++) {
        bp_atari_tree_node_t *node;
        unsigned long needed;
        int final = pos == length;
        int is_dir = final ? directory : 1;
        if (!final && projected[pos] != '\\') continue;
        for (node = tree->nodes; node; node = node->next) {
            if (node->length == pos && memcmp(node->names, projected, pos) == 0)
                break;
        }
        if (node) {
            if (memcmp(node->names + pos + 1, exact, pos) != 0 ||
                node->directory != is_dir || (final && node->explicit_entry))
                return BP_ERR_CORRUPT;
            if (final) node->explicit_entry = 1;
            continue;
        }
        needed = ((unsigned long)pos + 1UL) * 2UL;
        if (tree->count >= BP_ATARI_PATH_MAX_ENTRIES ||
            needed > BP_ATARI_PATH_MAX_BYTES - tree->bytes) return BP_ERR_OVERFLOW;
        node = (bp_atari_tree_node_t *)malloc(sizeof(*node));
        if (!node) return BP_ERR_NOMEM;
        node->names = (char *)malloc((size_t)needed);
        if (!node->names) { free(node); return BP_ERR_NOMEM; }
        memcpy(node->names, projected, pos);
        node->names[pos] = '\0';
        memcpy(node->names + pos + 1, exact, pos);
        node->names[2U * pos + 1U] = '\0';
        node->length = pos;
        node->directory = is_dir;
        node->explicit_entry = final;
        node->next = tree->nodes;
        tree->nodes = node;
        tree->count++;
        tree->bytes += needed;
    }
    return BP_OK;
}

static void bp_atari_tree_free(bp_atari_tree_t *tree)
{
    bp_atari_tree_node_t *node, *next;
    if (!tree) return;
    for (node = tree->nodes; node; node = next) {
        next = node->next;
        free(node->names);
        free(node);
    }
    tree->nodes = NULL;
    tree->count = 0;
    tree->bytes = 0;
}

#endif
