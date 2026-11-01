/* Shared target alias allocation and report generation. */
#ifndef BP_TARGET_AUTO_H
#define BP_TARGET_AUTO_H

typedef struct {
    char *original_prefix;
    char *target_prefix;
} atari_alias_entry_t;

typedef struct {
    atari_alias_entry_t *entries;
    unsigned int count;
    unsigned int capacity;
    bp_atari_name_set_t reserved;
    target_profile_t profile;
} atari_alias_map_t;

static int dos_target_codepage(target_profile_t profile);
static int dos_target_component_83(const char *component, size_t length);
static int dos_lfn_component_valid(const char *component, size_t length);
static int target_auto_codepage(target_profile_t profile)
{
    if (profile == TARGET_PROFILE_LINUX_POSIX || profile == TARGET_PROFILE_POSIX_UTF8 ||
        profile == TARGET_PROFILE_LINUX_MINI || profile == TARGET_PROFILE_PLAN9_UTF8 ||
        profile == TARGET_PROFILE_HAIKU_BFS || profile == TARGET_PROFILE_CPM86_83)
        return BP_CP_UTF8;
    if (profile == TARGET_PROFILE_AMIGAOS_68K_LATIN1 ||
        profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1 ||
        profile == TARGET_PROFILE_RISCOS_LATIN1) return BP_CP_8859_1;
    if (profile == TARGET_PROFILE_MACOS_CLASSIC_HFS) return BP_CP_MACROMAN;
    if (profile == TARGET_PROFILE_X68000_SJIS) return BP_CP_SJIS;
    return dos_target_codepage(profile);
}

static int target_auto_long_component(target_profile_t profile,
                                      const char *local)
{
    size_t i, n = strlen(local), limit;
    if (profile == TARGET_PROFILE_X68000_SJIS) {
        size_t stem = 0, ext = 0;
        int in_ext = 0;
        for (i = 0; i < n;) {
            unsigned char c = (unsigned char)local[i];
            size_t width = x68k_index_sjis_pair_at(local + i) ? 2U : 1U;
            if (width == 1U) {
                if ((c >= 0x81U && c <= 0x9FU) || (c >= 0xE0U && c <= 0xEFU)) return 0;
                if (c == '.') {
                    if (in_ext || stem == 0 || i + 1U == n) return 0;
                    in_ext = 1;
                    i++;
                    continue;
                }
                if (c < 0x21U || c == 0x7FU || strchr("\"*+, :;<=>?[\\]|", c)) return 0;
            }
            if (in_ext) ext += width; else stem += width;
            i += width;
        }
        return stem > 0 && stem <= 18U && ext <= 3U;
    }
    if (target_auto_codepage(profile) == BP_CP_UTF8) {
        char prefixed[258];
        if (n == 0 || n > 255U) return 0;
        prefixed[0] = 'X'; prefixed[1] = '/';
        memcpy(prefixed + 2, local, n + 1U);
        if (!bp_archive_name_valid(prefixed, (bp_uint32)n + 2U) ||
            !bp_archive_name_type_valid(prefixed, (bp_uint32)n + 2U, BP_TYPE_DATA)) return 0;
        if (profile == TARGET_PROFILE_PLAN9_UTF8) {
            for (i = 0; i < n; i++) {
                unsigned char c = (unsigned char)local[i];
                if (c < 0x20U || c == 0x7fU ||
                    (c == 0xc2U && i + 1U < n &&
                     (unsigned char)local[i + 1U] >= 0x80U &&
                     (unsigned char)local[i + 1U] <= 0x9fU)) return 0;
            }
        }
        return 1;
    }
    if (profile == TARGET_PROFILE_DOS_CP850_LFN)
        return dos_lfn_component_valid(local, n);
    limit = profile == TARGET_PROFILE_RISCOS_LATIN1 ? 10U :
            profile == TARGET_PROFILE_AMIGAOS_68K_LATIN1 ? 30U :
            profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1 ? 36U : 31U;
    if (n == 0 || n > limit || strcmp(local, ".") == 0 ||
        strcmp(local, "..") == 0) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)local[i];
        if (c < 0x20U || c == 0x7fU || c == ':' || c == '\\' || c == '/')
            return 0;
        if (profile == TARGET_PROFILE_RISCOS_LATIN1 &&
            strchr(".*#$%&@\"|^", c) != NULL) return 0;
    }
    return 1;
}

static int atari_job_name_compare(const void *left, const void *right)
{
    const add_job_t *a = (const add_job_t *)left;
    const add_job_t *b = (const add_job_t *)right;
    return strcmp(a->original_arcname, b->original_arcname);
}

static void atari_alias_map_free(atari_alias_map_t *map)
{
    unsigned int i;
    for (i = 0; i < map->count; i++) {
        free(map->entries[i].original_prefix);
        free(map->entries[i].target_prefix);
    }
    free(map->entries);
    bp_atari_name_set_free(&map->reserved);
    memset(map, 0, sizeof(*map));
}

static const char *atari_alias_lookup(const atari_alias_map_t *map,
                                      const char *original_prefix)
{
    unsigned int i;
    for (i = 0; i < map->count; i++)
        if (strcmp(map->entries[i].original_prefix, original_prefix) == 0)
            return map->entries[i].target_prefix;
    return NULL;
}

static int target_auto_name_set_add(atari_alias_map_t *map,
                                    const char *key, unsigned int length)
{
    bp_atari_name_set_t *set = &map->reserved;
    bp_atari_name_node_t *node;
    unsigned long hash = 2166136261UL;
    unsigned int i, bucket;
    if (map->profile == TARGET_PROFILE_ATARI_ST)
        return bp_atari_name_set_add(set, key, length);
    if (set->count >= BP_POSIX_PREFLIGHT_MAX_ENTRIES ||
        (unsigned long)length + 1UL > BP_POSIX_PREFLIGHT_MAX_KEY_BYTES - set->bytes)
        return BP_ERR_OVERFLOW;
    for (i = 0; i < length; i++) hash = (hash ^ (unsigned char)key[i]) * 16777619UL;
    bucket = (unsigned int)(hash % BP_ATARI_PATH_BUCKETS);
    if (set->buckets == NULL) {
        set->buckets = (bp_atari_name_node_t **)calloc(BP_ATARI_PATH_BUCKETS,
                                                       sizeof(*set->buckets));
        if (set->buckets == NULL) return BP_ERR_NOMEM;
    }
    for (node = set->buckets[bucket]; node != NULL; node = node->next)
        if (node->hash == hash && node->length == length &&
            memcmp(node->key, key, length) == 0) return BP_ERR_CORRUPT;
    node = (bp_atari_name_node_t *)malloc(sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->key = (char *)malloc((size_t)length + 1U);
    if (node->key == NULL) { free(node); return BP_ERR_NOMEM; }
    memcpy(node->key, key, length);
    node->key[length] = '\0';
    node->length = length;
    node->hash = hash;
    node->next = set->buckets[bucket];
    set->buckets[bucket] = node;
    set->count++;
    set->bytes += (unsigned long)length + 1UL;
    return BP_OK;
}

static int atari_alias_remember(atari_alias_map_t *map,
                                const char *original_prefix,
                                const char *target_prefix)
{
    atari_alias_entry_t *grown;
    unsigned int capacity;
    char *original_copy, *target_copy;

    if (map->count >= (map->profile == TARGET_PROFILE_ATARI_ST
                      ? BP_ATARI_PATH_MAX_ENTRIES
                      : BP_POSIX_PREFLIGHT_MAX_ENTRIES)) return BP_ERR_OVERFLOW;
    if (map->count == map->capacity) {
        capacity = map->capacity == 0 ? 32U : map->capacity * 2U;
        if (capacity > (map->profile == TARGET_PROFILE_ATARI_ST
                        ? BP_ATARI_PATH_MAX_ENTRIES : BP_POSIX_PREFLIGHT_MAX_ENTRIES))
            capacity = map->profile == TARGET_PROFILE_ATARI_ST
                     ? BP_ATARI_PATH_MAX_ENTRIES : BP_POSIX_PREFLIGHT_MAX_ENTRIES;
        grown = (atari_alias_entry_t *)realloc(
            map->entries, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) return BP_ERR_NOMEM;
        map->entries = grown;
        map->capacity = capacity;
    }
    original_copy = dup_string(original_prefix);
    target_copy = dup_string(target_prefix);
    if (original_copy == NULL || target_copy == NULL) {
        free(original_copy);
        free(target_copy);
        return BP_ERR_NOMEM;
    }
    map->entries[map->count].original_prefix = original_copy;
    map->entries[map->count].target_prefix = target_copy;
    map->count++;
    return BP_OK;
}

static int atari_alias_reserve(atari_alias_map_t *map,
                               const char *parent_target,
                               const char *component,
                               char *target_path, size_t target_capacity)
{
    size_t parent_length = strlen(parent_target);
    size_t component_length = strlen(component);
    size_t path_length = parent_length +
                         (parent_length != 0 ? 1U : 0U) + component_length;
    int rc;

    if ((map->profile == TARGET_PROFILE_ATARI_ST &&
         path_length > BP_ATARI_GEMDOS_PATH_MAX) ||
        path_length + 1U > target_capacity)
        return BP_ERR_NAME_TOO_LONG;
    if (parent_length != 0) {
        memcpy(target_path, parent_target, parent_length);
        target_path[parent_length] = '/';
        memcpy(target_path + parent_length + 1U, component,
               component_length + 1U);
    } else {
        memcpy(target_path, component, component_length + 1U);
    }
    if (map->profile != TARGET_PROFILE_ATARI_ST) {
        char local[4097], folded[4097];
        int cp = target_auto_codepage(map->profile);
        size_t max_path = map->profile == TARGET_PROFILE_PLAN9_UTF8 ? 511U :
                          map->profile == TARGET_PROFILE_HAIKU_BFS ? 1023U :
                          cp == BP_CP_UTF8 ? 4095U :
                          map->profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1
                        ? 35U : (map->profile == TARGET_PROFILE_AMIGAOS_68K_LATIN1 ||
                           map->profile == TARGET_PROFILE_RISCOS_LATIN1 ||
                           map->profile == TARGET_PROFILE_X68000_SJIS ||
                           map->profile == TARGET_PROFILE_MACOS_CLASSIC_HFS)
                        ? 255U : 259U;
        if (bp_utf8_to_local(target_path, local, sizeof(local), cp) != 0 ||
            strlen(local) > max_path)
            return BP_ERR_NAME_TOO_LONG;
        if (map->profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1) {
            char *p;
            for (p = local; *p; p++) if (*p == '/') *p = '_';
        }
        if (cp == BP_CP_UTF8) {
            if (!bp_archive_name_valid(local, (bp_uint32)strlen(local)))
                return BP_ERR_NAME_TOO_LONG;
            strcpy(folded, local);
        }
        else if (cp == BP_CP_SJIS) {
            size_t i;
            for (i = 0; local[i]; i++) {
                unsigned char c = (unsigned char)local[i];
                if (x68k_index_sjis_pair_at(local + i)) {
                    folded[i] = local[i]; folded[i + 1U] = local[i + 1U]; i++;
                } else folded[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
            }
            folded[i] = '\0';
        }
        else if (bp_local_casefold(local, folded, sizeof(folded), cp) != 0)
            return BP_ERR_NAME_TOO_LONG;
        rc = target_auto_name_set_add(map, folded,
                                   (unsigned int)strlen(folded));
    } else {
        rc = target_auto_name_set_add(map, target_path,
                                   (unsigned int)path_length);
    }
    return rc;
}

static void atari_alias_hash_prefix(const char *component, char *prefix,
                                    char *extension)
{
    const unsigned char *p = (const unsigned char *)component;
    const char *dot = strrchr(component, '.');
    size_t stem_length = dot != NULL && dot != component
                       ? (size_t)(dot - component) : strlen(component);
    size_t ext_length = dot != NULL ? strlen(dot + 1) : 0;
    size_t prefix_length = 0, extension_length = 0, i;

    for (i = 0; i < stem_length && prefix_length < 2U; i++) {
        unsigned char c = p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
            prefix[prefix_length++] = (char)c;
        }
    }
    if (prefix_length == 0) prefix[prefix_length++] = 'F';
    prefix[prefix_length] = '\0';

    if (dot != NULL) {
        for (i = 0; i < ext_length && extension_length < 3U; i++) {
            unsigned char c = (unsigned char)dot[1 + i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9')) {
                if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
                extension[extension_length++] = (char)c;
            }
        }
    }
    extension[extension_length] = '\0';
}

static int atari_alias_component(atari_alias_map_t *map,
                                 const char *parent_target,
                                 const char *original_component,
                                 int force_alias,
                                 int portable_alias,
                                 char *target_path, size_t target_capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    char local[4097];
    char direct[BP_ATARI_GEMDOS_PATH_MAX + 1U];
    char prefix[3], extension[4], candidate[1025];
    unsigned int direct_length, attempt;
    unsigned long hash = 2166136261UL;
    const unsigned char *p;
    int rc;

    if (map->profile != TARGET_PROFILE_ATARI_ST &&
        map->profile != TARGET_PROFILE_CPM86_83 &&
        target_auto_codepage(map->profile) != BP_CP_INVALID &&
        bp_utf8_to_local(original_component, local, sizeof(local),
                         target_auto_codepage(map->profile)) == 0 &&
        (target_auto_codepage(map->profile) == BP_CP_UTF8 ||
         map->profile == TARGET_PROFILE_DOS_CP850_LFN ||
         map->profile == TARGET_PROFILE_AMIGAOS_68K_LATIN1 ||
         map->profile == TARGET_PROFILE_MACOS_CLASSIC_HFS ||
         map->profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1 ||
         map->profile == TARGET_PROFILE_RISCOS_LATIN1 ||
         map->profile == TARGET_PROFILE_X68000_SJIS
         ? target_auto_long_component(map->profile, local)
         : dos_target_component_83(local, strlen(local)))) {
        rc = atari_alias_reserve(map, parent_target, original_component,
                                 target_path, target_capacity);
        if (rc == BP_OK) return BP_OK;
        if (rc != BP_ERR_CORRUPT && rc != BP_ERR_NAME_TOO_LONG) return rc;
    }

    if (map->profile == TARGET_PROFILE_ATARI_ST && !force_alias &&
        bp_utf8_to_local(original_component, local, sizeof(local),
                         BP_CP_ATARIST) == 0 &&
        bp_atari_project_name(local, 0, direct, sizeof(direct),
                              &direct_length) == BP_OK) {
        rc = atari_alias_reserve(map, parent_target, direct, target_path,
                                 target_capacity);
        if (rc == BP_OK) return BP_OK;
        if (rc != BP_ERR_CORRUPT && rc != BP_ERR_NAME_TOO_LONG)
            return rc;
    }

    atari_alias_hash_prefix(original_component, prefix, extension);
    for (p = (const unsigned char *)original_component; *p != '\0'; p++)
        hash = (hash ^ *p) * 16777619UL;
    for (attempt = 0; attempt <= (portable_alias ? 0xFFFFFFU : 0xFFFFFU);
         attempt++) {
        unsigned long mask = portable_alias ? 0xFFFFFFUL : 0xFFFFFUL;
        unsigned long suffix = (hash + attempt) & mask;
        size_t pos = 0, i;
        if ((target_auto_codepage(map->profile) == BP_CP_UTF8 &&
             map->profile != TARGET_PROFILE_CPM86_83) ||
            map->profile == TARGET_PROFILE_DOS_CP850_LFN ||
            map->profile == TARGET_PROFILE_AMIGAOS_68K_LATIN1 ||
            map->profile == TARGET_PROFILE_MACOS_CLASSIC_HFS ||
            map->profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1 ||
            map->profile == TARGET_PROFILE_RISCOS_LATIN1 ||
            map->profile == TARGET_PROFILE_X68000_SJIS) {
            char parent_local[4097];
            const char *dot = strrchr(original_component, '.');
            size_t parent_len, limit, stem_len, j, native_used = 0;
            size_t ext_len = map->profile == TARGET_PROFILE_RISCOS_LATIN1
                           ? 0U : strlen(extension);
            size_t path_limit = map->profile == TARGET_PROFILE_PLAN9_UTF8 ? 511U :
                                map->profile == TARGET_PROFILE_HAIKU_BFS ? 1023U :
                                target_auto_codepage(map->profile) == BP_CP_UTF8 ? 4095U :
                                map->profile == TARGET_PROFILE_DOS_CP850_LFN
                              ? 259U : map->profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1
                              ? 35U : 255U;
            size_t component_limit = target_auto_codepage(map->profile) == BP_CP_UTF8 ? 255U :
                                    map->profile == TARGET_PROFILE_DOS_CP850_LFN
                                   ? 255U : map->profile == TARGET_PROFILE_RISCOS_LATIN1
                                   ? 10U : map->profile == TARGET_PROFILE_X68000_SJIS
                                   ? 18U + (ext_len ? ext_len + 1U : 0U) : map->profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1
                                   ? 36U : (map->profile == TARGET_PROFILE_AMIGAOS_68K_LATIN1 ? 30U : 31U);
            if (bp_utf8_to_local(parent_target, parent_local,
                                 sizeof(parent_local), target_auto_codepage(map->profile)) != 0)
                return BP_ERR_NAME_TOO_LONG;
            parent_len = strlen(parent_local);
            if (parent_len >= path_limit) return BP_ERR_NAME_TOO_LONG;
            limit = path_limit - parent_len - (parent_len ? 1U : 0U);
            if (limit > component_limit) limit = component_limit;
            if (limit < 7U + (ext_len ? ext_len + 1U : 0U))
                return BP_ERR_NAME_TOO_LONG;
            stem_len = dot && dot != original_component
                     ? (size_t)(dot - original_component)
                     : strlen(original_component);
            for (j = 0; j < stem_len;) {
                unsigned char c = (unsigned char)original_component[j];
                char character[5], encoded[8];
                size_t width, local_width, budget;
                if (c < 0x80U) {
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9'))) { j++; continue; }
                    width = 1U;
                } else if (c >= 0xC2U && c <= 0xDFU) width = 2U;
                else if (c >= 0xE0U && c <= 0xEFU) width = 3U;
                else if (c >= 0xF0U && c <= 0xF4U) width = 4U;
                else { j++; continue; }
                if (width > stem_len - j) break;
                memcpy(character, original_component + j, width);
                character[width] = '\0';
                if (!bp_archive_name_valid(character, (bp_uint32)width) ||
                    (map->profile == TARGET_PROFILE_PLAN9_UTF8 && c == 0xC2U &&
                     width == 2U && (unsigned char)character[1] <= 0x9FU) ||
                    bp_utf8_to_local(character, encoded, sizeof(encoded),
                                     target_auto_codepage(map->profile)) != 0) {
                    j += width;
                    continue;
                }
                local_width = strlen(encoded);
                budget = limit - 6U - (ext_len ? ext_len + 1U : 0U);
                if (native_used + local_width > budget || pos + width >= sizeof(candidate)) break;
                memcpy(candidate + pos, character, width);
                pos += width;
                native_used += local_width;
                j += width;
            }
            if (pos == 0) candidate[pos++] = 'F';
            if (map->profile == TARGET_PROFILE_RISCOS_LATIN1) extension[0] = '\0';
        } else if (map->profile != TARGET_PROFILE_ATARI_ST &&
                   map->profile != TARGET_PROFILE_CPM86_83) {
            const char *dot = strrchr(original_component, '.');
            size_t j = 0, native_used = 0;
            size_t stem_len = dot && dot != original_component
                            ? (size_t)(dot - original_component)
                            : strlen(original_component);
            while (j < stem_len && native_used < 2U) {
                unsigned char c = (unsigned char)original_component[j];
                char character[5], encoded[8];
                size_t width;
                if (c < 0x80U) {
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9'))) { j++; continue; }
                    width = 1U;
                } else if (c >= 0xC2U && c <= 0xDFU) width = 2U;
                else if (c >= 0xE0U && c <= 0xEFU) width = 3U;
                else if (c >= 0xF0U && c <= 0xF4U) width = 4U;
                else { j++; continue; }
                if (width > stem_len - j) break;
                memcpy(character, original_component + j, width);
                character[width] = '\0';
                if (bp_archive_name_valid(character, (bp_uint32)width) &&
                    bp_utf8_to_local(character, encoded, sizeof(encoded),
                                     target_auto_codepage(map->profile)) == 0 &&
                    strlen(encoded) == 1U) {
                    memcpy(candidate + pos, character, width);
                    pos += width;
                    native_used++;
                }
                j += width;
            }
            if (pos == 0) candidate[pos++] = 'F';
        } else {
            memcpy(candidate + pos, prefix, strlen(prefix));
            pos += strlen(prefix);
        }
        if (!portable_alias) candidate[pos++] = '~';
        for (i = 0; i < (portable_alias ? 6U : 5U); i++)
            candidate[pos++] = hex[(suffix >>
                (((portable_alias ? 5U : 4U) - i) * 4U)) & 0x0FU];
        if (extension[0] != '\0') {
            candidate[pos++] = '.';
            memcpy(candidate + pos, extension, strlen(extension));
            pos += strlen(extension);
        }
        candidate[pos] = '\0';
        rc = atari_alias_reserve(map, parent_target, candidate,
                                 target_path, target_capacity);
        if (rc == BP_OK) return BP_OK;
        if (rc == BP_ERR_NAME_TOO_LONG) return rc;
        if (rc != BP_ERR_CORRUPT) return rc;
    }
    return BP_ERR_OVERFLOW;
}

static int atari_alias_map_name(atari_alias_map_t *map, const char *original,
                                int is_directory, int force_nonportable,
                                char *target,
                                size_t target_capacity)
{
    char original_prefix[4096];
    char target_prefix[4096];
    size_t original_length = strlen(original), pos = 0;

    if (is_directory && original_length != 0 &&
        original[original_length - 1U] == '/') original_length--;
    if (original_length == 0 || original_length >= sizeof(original_prefix))
        return BP_ERR_NAME_TOO_LONG;
    original_prefix[0] = '\0';
    target_prefix[0] = '\0';
    /* Absolute Linux source operands have no leading-root representation on
     * Atari.  Treat the remaining components as a relative archive path. */
    while (pos < original_length && original[pos] == '/') pos++;
    if (pos == original_length) return BP_ERR_PARTIAL;
    while (pos < original_length) {
        size_t start = pos, component_length;
        char component[4096];
        char next_target[4096];
        const char *mapped;
        int rc;

        while (pos < original_length && original[pos] != '/') pos++;
        component_length = pos - start;
        if (component_length == 0 || component_length >= sizeof(component))
            return BP_ERR_PARTIAL;
        component[component_length] = '\0';
        memcpy(component, original + start, component_length);
        if (strlen(original_prefix) + (original_prefix[0] ? 1U : 0U) +
                component_length >= sizeof(original_prefix))
            return BP_ERR_NAME_TOO_LONG;
        if (original_prefix[0] != '\0') strcat(original_prefix, "/");
        strcat(original_prefix, component);

        mapped = atari_alias_lookup(map, original_prefix);
        if (mapped == NULL) {
            rc = atari_alias_component(map, target_prefix, component,
                                       0, force_nonportable,
                                       next_target, sizeof(next_target));
            if (rc != BP_OK) return rc;
            rc = atari_alias_remember(map, original_prefix, next_target);
            if (rc != BP_OK) return rc;
            mapped = atari_alias_lookup(map, original_prefix);
        }
        if (strlen(mapped) > sizeof(target_prefix) - 1U)
            return BP_ERR_NAME_TOO_LONG;
        strcpy(target_prefix, mapped);
        if (pos < original_length) pos++;
    }
    if (is_directory) {
        if (strlen(target_prefix) + 2U > target_capacity)
            return BP_ERR_NAME_TOO_LONG;
        strcpy(target, target_prefix);
        strcat(target, "/");
    } else {
        if (strlen(target_prefix) + 1U > target_capacity)
            return BP_ERR_NAME_TOO_LONG;
        strcpy(target, target_prefix);
    }
    return BP_OK;
}

/* Project names using the selected target's encoding, component and path
 * limits, and collision rules. CP/M-86 files use a flat 8.3 namespace. */
static int target_auto_rename_jobs(add_job_list_t *jobs,
                                   target_profile_t profile)
{
    atari_alias_map_t map;
    int i, rc = BP_OK;
    memset(&map, 0, sizeof(map));
    map.profile = profile;
    bp_atari_name_set_init(&map.reserved);
    {
        const char *report_name = profile == TARGET_PROFILE_RISCOS_LATIN1
                                ? "BPNAMESTXT" : "BPNAMES.TXT";
        rc = target_auto_name_set_add(&map, report_name,
                                   (unsigned int)strlen(report_name));
    }
    if (rc != BP_OK) {
        atari_alias_map_free(&map);
        return rc;
    }
    if (jobs->count > 1)
        qsort(jobs->items, (size_t)jobs->count, sizeof(*jobs->items),
              atari_job_name_compare);

    i = 0;
    while (i < jobs->count) {
        add_job_t *job = &jobs->items[i];
        char renamed[4096];
        char *renamed_copy;
        if (job->entry_type == BP_TYPE_SYMLINK) {
            fprintf(stderr,
                    "boltpack: --target-auto-rename does not support symbolic links: %s\n",
                    job->original_arcname);
            rc = BP_ERR_PARTIAL;
            break;
        }
        if (job->entry_type != BP_TYPE_DATA && job->entry_type != BP_TYPE_DIR) {
            i++;
            continue;
        }
        if (profile == TARGET_PROFILE_CPM86_83 &&
            job->entry_type == BP_TYPE_DIR) {
            free_add_job(job);
            memmove(job, job + 1,
                    (size_t)(jobs->count - i - 1) * sizeof(*job));
            jobs->count--;
            continue;
        }
        if (profile == TARGET_PROFILE_CPM86_83) {
            char alias[13];
            rc = atari_alias_component(&map, "", job->original_arcname, 1, 1,
                                       alias, sizeof(alias));
            if (rc == BP_OK) strcpy(renamed, alias);
        } else {
            rc = atari_alias_map_name(&map, job->original_arcname,
                                      job->entry_type == BP_TYPE_DIR,
                                      profile != TARGET_PROFILE_ATARI_ST,
                                      renamed, sizeof(renamed));
        }
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: cannot create a target-safe alias for path: %s\n",
                    job->original_arcname);
            break;
        }
        renamed_copy = dup_string(renamed);
        if (renamed_copy == NULL) {
            rc = BP_ERR_NOMEM;
            break;
        }
        free(job->arcname);
        job->arcname = renamed_copy;
        i++;
    }
    atari_alias_map_free(&map);
    return rc;
}

static int write_atari_report_original(FILE *fp, const char *name)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)name;
    while (*p != '\0') {
        unsigned char c = *p++;
        if (c >= 0x20U && c <= 0x7EU && c != '\\') {
            if (fputc((int)c, fp) == EOF) return 0;
        } else if (c == '\\') {
            if (fputs("\\\\", fp) == EOF) return 0;
        } else {
            if (fputs("\\x", fp) == EOF ||
                fputc(hex[c >> 4], fp) == EOF ||
                fputc(hex[c & 0x0FU], fp) == EOF)
                return 0;
        }
    }
    return 1;
}

/* Add an ASCII-readable mapping as a regular archive member.  Non-ASCII
 * original UTF-8 bytes are escaped so the report is usable on legacy targets. */
static int add_target_name_report(bp_archive_t *ar,
                                  const add_job_list_t *jobs,
                                  target_profile_t profile)
{
    FILE *fp;
    bp_io_t io;
    long report_size;
    int i, rc = BP_OK;

    fp = tmpfile();
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot create temporary target name report: %s\n",
                strerror(errno));
        return BP_ERR_IO;
    }
    if (profile == TARGET_PROFILE_ATARI_ST) {
        if (fprintf(fp,
                    "BoltPack Atari ST filename report v1\r\n"
                    "Original UTF-8 path (non-ASCII bytes escaped as \\xHH)\tAtari GEMDOS path\r\n") < 0) {
            rc = BP_ERR_IO;
            goto report_done;
        }
    } else if (fprintf(fp,
                       "BoltPack target filename report v1\r\n"
                       "Original UTF-8 path (non-ASCII bytes escaped as \\xHH)\tTarget archive path\r\n") < 0) {
        rc = BP_ERR_IO;
        goto report_done;
    }
    for (i = 0; i < jobs->count; i++) {
        const add_job_t *job = &jobs->items[i];
        const char *p;
        char local_target[4097];
        char atari_target[BP_ATARI_GEMDOS_PATH_MAX + 1U];
        unsigned int atari_length;

        if (job->entry_type != BP_TYPE_DATA && job->entry_type != BP_TYPE_DIR)
            continue;
        if (!write_atari_report_original(fp, job->original_arcname) ||
            fputc('\t', fp) == EOF) {
            rc = BP_ERR_IO;
            goto report_done;
        }
        if (profile == TARGET_PROFILE_ATARI_ST) {
            if (bp_utf8_to_local(job->arcname, local_target, sizeof(local_target),
                                 BP_CP_ATARIST) != 0 ||
                bp_atari_project_name(local_target, job->entry_type == BP_TYPE_DIR,
                                      atari_target, sizeof(atari_target),
                                      &atari_length) != BP_OK) {
                rc = BP_ERR_PARTIAL;
                goto report_done;
            }
        } else if (bp_utf8_to_local(job->arcname, local_target, sizeof(local_target),
                                    target_auto_codepage(profile)) != 0) {
            rc = BP_ERR_PARTIAL;
            goto report_done;
        }
        for (p = profile == TARGET_PROFILE_ATARI_ST ? atari_target : local_target;
             *p != '\0'; p++) {
            int ch = (unsigned char)*p;
            if (target_auto_codepage(profile) == BP_CP_UTF8 &&
                (ch < 0x20 || ch == 0x7f || ch == '\\')) {
                if (fprintf(fp, "\\x%02X", ch) < 0) {
                    rc = BP_ERR_IO;
                    goto report_done;
                }
                continue;
            }
            if (*p == '/') {
                if (profile == TARGET_PROFILE_MACOS_CLASSIC_HFS) ch = ':';
                else if (profile == TARGET_PROFILE_SINCLAIR_QL_LATIN1) ch = '_';
                else if (profile == TARGET_PROFILE_RISCOS_LATIN1) ch = '.';
                else if (profile != TARGET_PROFILE_AMIGAOS_68K_LATIN1 &&
                         target_auto_codepage(profile) != BP_CP_UTF8) ch = '\\';
            }
            if (fputc(ch, fp) == EOF) {
                rc = BP_ERR_IO;
                goto report_done;
            }
        }
        if (fputs("\r\n", fp) == EOF) {
            rc = BP_ERR_IO;
            goto report_done;
        }
    }
    if (fflush(fp) != 0 || fseek(fp, 0, SEEK_END) != 0 ||
        (report_size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        rc = BP_ERR_IO;
        goto report_done;
    }
    if ((unsigned long)report_size > 0xFFFFFFFFUL) {
        rc = BP_ERR_OVERFLOW;
        goto report_done;
    }
    bp_io_from_file(&io, fp);
    rc = bp_archive_add_entry_ex(ar,
                                 profile == TARGET_PROFILE_RISCOS_LATIN1
                                 ? "BPNAMESTXT" : "BPNAMES.TXT", BP_TYPE_DATA,
                                 BP_CREATOR_OS, &io,
                                 bp_u64_from_u32((bp_uint32)report_size),
                                 bp_u64_from_u32(0), 0);
    if (rc == BP_OK && profile == TARGET_PROFILE_PLAN9_UTF8) {
        FILE *mode_file = tmpfile();
        unsigned char mode[2];
        mode[0] = (unsigned char)(0644U & 0xffU);
        mode[1] = (unsigned char)((0644U >> 8) & 0xffU);
        if (mode_file == NULL) rc = BP_ERR_IO;
        else {
            if (fwrite(mode, 1, sizeof(mode), mode_file) != sizeof(mode) ||
                fflush(mode_file) != 0 || fseek(mode_file, 0, SEEK_SET) != 0)
                rc = BP_ERR_IO;
            if (rc == BP_OK) {
                bp_io_t mode_io;
                bp_io_from_file(&mode_io, mode_file);
                rc = bp_archive_add_entry_ex(ar, "BPNAMES.TXT", BP_TYPE_META,
                                              BP_OS_PLAN9, &mode_io,
                                              bp_u64_from_u32(2U),
                                              bp_u64_from_u32(0), 0);
            }
            if (fclose(mode_file) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        }
    }
    if (rc != BP_OK)
        fprintf(stderr, "boltpack: cannot add target name report: %s\n",
                bp_strerror(rc));

report_done:
    fclose(fp);
    return rc;
}

#endif
