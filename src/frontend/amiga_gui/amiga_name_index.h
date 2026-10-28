/* Bounded exact and case-folded Amiga destination path preflight. */
#ifndef BP_AMIGA_GUI_NAME_INDEX_H
#define BP_AMIGA_GUI_NAME_INDEX_H

#include "../../core/archiver.h"
#include "../../core/archive_name.h"
#include "../../core/bp_codepage.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key; /* Folded path followed by its exact spelling. */
    int is_directory;
    int explicit_entry;
} gui_name_key_t;

typedef struct {
    const char *dest;
    gui_name_key_t names[GUI_NAME_INDEX_MAX];
    unsigned int count;
    unsigned long bytes;
    const char *error;
} gui_name_index_t;

static unsigned char gui_fold_byte(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c + ('a' - 'A'));
    if ((c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE))
        return (unsigned char)(c + 0x20);
    return c;
}

static void gui_fold_name(char *name)
{
    unsigned char *p = (unsigned char *)name;
    while (*p) { *p = gui_fold_byte(*p); p++; }
}

static int gui_local_name_safe(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    while (*p) { if (*p == ':') return 0; p++; }
    return 1;
}

static int gui_build_path(const char *dest, const char *local_name,
                          char *path, size_t cap)
{
    size_t dlen = strlen(dest), nlen = strlen(local_name), sep = 1, len;
    if (dlen == 0 || dest[dlen - 1] == '/' || dest[dlen - 1] == ':') sep = 0;
    if (dlen + sep + nlen + 1 > cap) return -1;
    memcpy(path, dest, dlen);
    if (sep) path[dlen++] = '/';
    memcpy(path + dlen, local_name, nlen + 1);
    len = strlen(path);
    while (len > 0 && path[len - 1] == '/') path[--len] = '\0';
    return 0;
}

static int gui_name_index_add(gui_name_index_t *index, const char *exact,
                              int is_directory, int explicit_entry)
{
    char folded[BP_MAX_PATH];
    size_t len = strlen(exact);
    unsigned int i;
    unsigned long needed = ((unsigned long)len + 1UL) * 2UL;
    char *key;
    if (len == 0 || len >= sizeof(folded)) return 1;
    memcpy(folded, exact, len + 1);
    gui_fold_name(folded);
    for (i = 0; i < index->count; i++) {
        gui_name_key_t *old = &index->names[i];
        if (strcmp(old->key, folded) != 0) continue;
        if (strcmp(old->key + strlen(old->key) + 1, exact) != 0 ||
            old->is_directory != is_directory ||
            (explicit_entry && old->explicit_entry)) {
            index->error = "Archive names collide on this Amiga destination";
            return 1;
        }
        if (explicit_entry) old->explicit_entry = 1;
        return 0;
    }
    if (index->count >= GUI_NAME_INDEX_MAX ||
        needed > GUI_NAME_INDEX_BYTES_MAX - index->bytes) {
        index->error = "Archive exceeds the Amiga name preflight limit";
        return 1;
    }
    key = (char *)malloc((size_t)needed);
    if (!key) { index->error = "Not enough memory for Amiga name checks"; return 1; }
    memcpy(key, folded, len + 1);
    memcpy(key + len + 1, exact, len + 1);
    index->names[index->count].key = key;
    index->names[index->count].is_directory = is_directory;
    index->names[index->count].explicit_entry = explicit_entry;
    index->count++;
    index->bytes += needed;
    return 0;
}

static int gui_preflight_name(const bp_entry_t *entry, void *ud)
{
    gui_name_index_t *index = (gui_name_index_t *)ud;
    char local[BP_MAX_PATH], full[BP_MAX_PATH];
    size_t len, pos, name_len;
    int is_dir;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    name_len = entry->name ? strlen(entry->name) : 0;
    if (name_len > BP_MAX_NAME_LEN ||
        !bp_archive_name_valid(entry->name, (bp_uint32)name_len) ||
        !bp_archive_name_type_valid(entry->name, (bp_uint32)name_len, entry->entry_type) ||
        bp_utf8_to_local(entry->name, local, sizeof(local), BP_CP_8859_1) != 0 ||
        !gui_local_name_safe(local) ||
        gui_build_path(index->dest, local, full, sizeof(full)) != 0) {
        index->error = "Destination cannot represent an archive name";
        return 1;
    }
    len = strlen(local);
    while (len && local[len - 1] == '/') local[--len] = '\0';
    if (!len) { index->error = "Empty destination name"; return 1; }
    is_dir = entry->entry_type == BP_TYPE_DIR;
    for (pos = 0; pos <= len; pos++) {
        char saved;
        int final_component, rc;
        if (pos < len && local[pos] != '/') continue;
        final_component = pos == len;
        saved = local[pos]; local[pos] = '\0';
        rc = gui_name_index_add(index, local,
                                final_component ? is_dir : 1,
                                final_component);
        local[pos] = saved;
        if (rc) return 1;
    }
    return 0;
}

static void gui_name_index_clear(gui_name_index_t *index)
{
    unsigned int i;
    for (i = 0; i < index->count; i++) free(index->names[i].key);
    index->count = 0; index->bytes = 0;
}
#endif /* BP_AMIGA_GUI_NAME_INDEX_H */
