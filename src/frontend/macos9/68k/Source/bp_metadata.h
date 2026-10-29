/* Versioned, bounded portable metadata envelope. C89, including 16-bit int. */
#ifndef BP_METADATA_H
#define BP_METADATA_H
#include "bp_types.h"
#if !defined(__PLAN9__)
#include <stdlib.h>
#include <string.h>
#endif

#define BP_META_MAX_SIZE 256U
#define BP_META_FAT_MASK 0x27U
#define BP_META_FAT_KIND 1U
#define BP_META_FAT_SIZE 14U

typedef struct {
    bp_uint8 subject_type;
    bp_uint8 fat_valid;
    bp_uint8 fat_value;
    int has_fat;
    int has_unknown;
    int supported_version;
} bp_portable_metadata_t;

static int bp_metadata_matches_subject(const bp_portable_metadata_t *metadata,
                                       bp_uint8 subject_type)
{
    return metadata != NULL && metadata->supported_version &&
           metadata->subject_type == subject_type;
}

typedef struct bp_metadata_seen_s {
    struct bp_metadata_seen_s *next;
    char *name;
    size_t normalized_len;
    int owned;
} bp_metadata_seen_t;

/* Ownership of primary names is transferred to this tracker. */
typedef struct {
    char *name;
    bp_uint8 type;
    int portable_seen;
    bp_metadata_seen_t **seen;
    int resource_seen;
} bp_metadata_group_t;

#define BP_GROUP_BUCKETS 64U

static void bp_metadata_group_close(bp_metadata_group_t *group)
{
    unsigned i;
    if (group->seen != NULL) {
        for (i = 0; i < BP_GROUP_BUCKETS; i++) {
            bp_metadata_seen_t *node = group->seen[i];
            while (node != NULL) {
                bp_metadata_seen_t *next = node->next;
                if (node->owned) free(node->name);
                free(node);
                node = next;
            }
        }
        free(group->seen);
    }
    group->name = NULL;
    group->seen = NULL;
    group->portable_seen = 0;
    group->resource_seen = 0;
}

static int bp_metadata_group_record(bp_metadata_group_t *group, char **name,
                                    bp_uint8 type, bp_uint8 platform, int take_ownership)
{
    if (type == BP_TYPE_DATA || type == BP_TYPE_DIR || type == BP_TYPE_SYMLINK) {
        const unsigned char *p;
        size_t length = strlen(*name), i;
        bp_uint32 hash = 2166136261UL;
        unsigned bucket;
        bp_metadata_seen_t *node;
        while (length > 0 && (*name)[length - 1] == '/') length--;
        p = (const unsigned char *)*name;
        for (i = 0; i < length; i++)
            hash = (hash ^ p[i]) * 16777619UL;
        bucket = (unsigned)(hash % BP_GROUP_BUCKETS);
        if (group->seen == NULL) {
            group->seen = (bp_metadata_seen_t **)calloc(BP_GROUP_BUCKETS, sizeof(*group->seen));
            if (group->seen == NULL) return BP_ERR_NOMEM;
        }
        for (node = group->seen[bucket]; node != NULL; node = node->next)
            if (node->normalized_len == length && memcmp(node->name, *name, length) == 0)
                return BP_ERR_CORRUPT;
        node = (bp_metadata_seen_t *)malloc(sizeof(*node));
        if (node == NULL) return BP_ERR_NOMEM;
        node->name = *name;
        node->normalized_len = length;
        node->owned = take_ownership;
        node->next = group->seen[bucket];
        group->seen[bucket] = node;
        group->name = *name;
        if (take_ownership) *name = NULL;
        group->type = type;
        group->portable_seen = 0;
        group->resource_seen = 0;
        return BP_OK;
    }
    if (type != BP_TYPE_META && type != BP_TYPE_RSRC) return BP_ERR_CORRUPT;
    if (group->name == NULL || strcmp(group->name, *name) != 0)
        return BP_ERR_CORRUPT;
    if ((group->type == BP_TYPE_DIR) !=
        ((*name)[strlen(*name) - 1] == '/')) return BP_ERR_CORRUPT;
    if (type == BP_TYPE_RSRC) {
        if (group->type != BP_TYPE_DATA || group->resource_seen)
            return BP_ERR_CORRUPT;
        group->resource_seen = 1;
    }
    if (type == BP_TYPE_META) {
        if (group->type != BP_TYPE_DATA && group->type != BP_TYPE_DIR)
            return BP_ERR_CORRUPT;
        if (platform == BP_META_PORTABLE) {
            if (group->portable_seen) return BP_ERR_CORRUPT;
            group->portable_seen = 1;
        }
    }
    return BP_OK;
}

typedef struct {
    bp_io_t io;
    bp_uint8 data[BP_META_MAX_SIZE];
    unsigned length;
} bp_metadata_sink_t;

static long bp_metadata_sink_write(void *handle, const void *data, bp_uint32 size)
{
    bp_metadata_sink_t *sink = (bp_metadata_sink_t *)handle;
    const bp_uint8 *source = (const bp_uint8 *)data;
    bp_uint32 i;
    if (size > BP_META_MAX_SIZE - sink->length) return -1;
    for (i = 0; i < size; i++) sink->data[sink->length++] = source[i];
    return (long)size;
}

static void bp_metadata_sink_init(bp_metadata_sink_t *sink)
{
    sink->io.read = NULL; sink->io.write = bp_metadata_sink_write;
    sink->io.seek = NULL; sink->io.tell = NULL; sink->io.handle = sink;
    sink->length = 0;
}

/* Returns BP_OK for known metadata, BP_ERR_PARTIAL for unknown TLV kinds,
 * BP_ERR_PARTIAL for unknown versions and BP_ERR_CORRUPT for bad framing. */
static int bp_metadata_parse(const bp_uint8 *data, unsigned size,
                             bp_portable_metadata_t *result)
{
    unsigned pos, count, i;
    if (data == NULL || result == NULL) return BP_ERR_INVALID_ARG;
    result->subject_type = 0;
    result->fat_valid = 0; result->fat_value = 0;
    result->has_fat = 0; result->has_unknown = 0;
    result->supported_version = 0;
    if (size < 8U || size > BP_META_MAX_SIZE || data[0] != 'B' ||
        data[1] != 'P' || data[2] != 'M' || data[3] != 'T') return BP_ERR_CORRUPT;
    if (data[4] != 1U) return BP_ERR_PARTIAL;
    result->supported_version = 1;
    if (data[5] != BP_TYPE_DATA && data[5] != BP_TYPE_DIR) return BP_ERR_CORRUPT;
    result->subject_type = data[5];
    count = (unsigned)data[6] | ((unsigned)data[7] << 8);
    pos = 8U;
    for (i = 0; i < count; i++) {
        unsigned kind, length;
        if (size - pos < 4U) return BP_ERR_CORRUPT;
        kind = (unsigned)data[pos] | ((unsigned)data[pos + 1] << 8);
        length = (unsigned)data[pos + 2] | ((unsigned)data[pos + 3] << 8);
        pos += 4U;
        if (length > size - pos) return BP_ERR_CORRUPT;
        if (kind == BP_META_FAT_KIND) {
            unsigned valid, value;
            if (result->has_fat || length != 2U) return BP_ERR_CORRUPT;
            valid = data[pos]; value = data[pos + 1];
            if ((valid & ~BP_META_FAT_MASK) || (value & ~valid)) return BP_ERR_CORRUPT;
            result->fat_valid = (bp_uint8)valid;
            result->fat_value = (bp_uint8)value;
            result->has_fat = 1;
        } else result->has_unknown = 1;
        pos += length;
    }
    if (pos != size) return BP_ERR_CORRUPT;
    return result->has_unknown ? BP_ERR_PARTIAL : BP_OK;
}

static int bp_metadata_fat_encode(bp_uint8 *out, unsigned size,
                                  bp_uint8 subject_type,
                                  bp_uint8 valid, bp_uint8 value)
{
    if (out == NULL || size < BP_META_FAT_SIZE ||
        (subject_type != BP_TYPE_DATA && subject_type != BP_TYPE_DIR) ||
        (valid & ~BP_META_FAT_MASK) || (value & ~valid)) return BP_ERR_INVALID_ARG;
    out[0] = 'B'; out[1] = 'P'; out[2] = 'M'; out[3] = 'T';
    out[4] = 1; out[5] = subject_type; out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0; out[10] = 2; out[11] = 0;
    out[12] = valid; out[13] = value;
    return BP_OK;
}
#endif
