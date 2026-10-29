#ifndef BOLTPACK_QL_PATH_H
#define BOLTPACK_QL_PATH_H

#include <stddef.h>
#include <string.h>

#define QL_QDOS_PATH_MAX 36
#define QL_STAGE_LEAF_MAX 8
#define QL_STAGE_CANDIDATE_MAX 4096UL

static size_t ql_device_prefix_length(const char *path)
{
    static const char *const devices[] = {
        "WIN", "MDV", "FLP", "RAM", "NFA", "SFA"
    };
    size_t i, path_length;
    if (path == NULL) return 0;
    path_length = strlen(path);
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        size_t n = strlen(devices[i]);
        if (path_length >= n + 2U &&
            ((path[0] >= 'a' && path[0] <= 'z') ? path[0] - 32 : path[0]) == devices[i][0] &&
            ((path[1] >= 'a' && path[1] <= 'z') ? path[1] - 32 : path[1]) == devices[i][1] &&
            ((path[2] >= 'a' && path[2] <= 'z') ? path[2] - 32 : path[2]) == devices[i][2] &&
            path[n] >= '1' && path[n] <= '8' && path[n + 1U] == '_')
            return n + 2U;
    }
    return 0;
}

/* Convert an archive member path (UTF-8 already projected to Latin-1) into
 * QDOS syntax. If no output root is supplied, inherit the archive's device. */
static int ql_native_output_path(const char *out_dir, const char *archive_path,
                                 const char *member_path, char *out,
                                 size_t out_size)
{
    size_t archive_device = ql_device_prefix_length(archive_path);
    size_t output_device = ql_device_prefix_length(out_dir);
    size_t used = 0;
    size_t i;
    size_t device_length;
    const char *base = out_dir;
    int need_separator = 0;

    if (member_path == NULL || out == NULL || out_size == 0) return -1;
    out[0] = '\0';
    if (out_dir != NULL && out_dir[0] != '\0') {
        if (output_device != 0) {
            base = out_dir;
        } else if (archive_device != 0) {
            if (archive_device >= out_size) return -1;
            memcpy(out, archive_path, archive_device);
            used = archive_device;
            base = out_dir;
            need_separator = used != 0 && out[used - 1U] != '_';
        }
        if (output_device != 0) {
            if (strlen(base) >= out_size) return -1;
            for (i = 0; base[i] != '\0'; i++) {
                out[used++] = base[i] == '/' ? '_' : base[i];
            }
            out[used] = '\0';
        } else if (used == 0) {
            if (strlen(base) >= out_size) return -1;
            for (i = 0; base[i] != '\0'; i++)
                out[used++] = base[i] == '/' ? '_' : base[i];
            out[used] = '\0';
        } else {
            if (need_separator) {
                if (used + 1U >= out_size) return -1;
                out[used++] = '_';
                out[used] = '\0';
            }
            for (i = 0; base[i] != '\0'; i++) {
                if (used + 1U >= out_size) return -1;
                out[used++] = base[i] == '/' ? '_' : base[i];
                out[used] = '\0';
            }
        }
    } else if (archive_device != 0) {
        if (archive_device >= out_size) return -1;
        memcpy(out, archive_path, archive_device);
        used = archive_device;
        out[used] = '\0';
    }

    if (used != 0 && out[used - 1U] != '_' && out[used - 1U] != '/') {
        if (used + 1U >= out_size) return -1;
        out[used++] = '_';
        out[used] = '\0';
    }
    for (i = 0; member_path[i] != '\0'; i++) {
        if (used + 1U >= out_size) return -1;
        out[used++] = member_path[i] == '/' ? '_' : member_path[i];
        out[used] = '\0';
    }
    device_length = ql_device_prefix_length(out);
    if (used < device_length || used - device_length > QL_QDOS_PATH_MAX)
        return -1;
    return 0;
}

static size_t ql_stage_leaf_length_for_target(const char *target,
                                              size_t target_leaf_length)
{
    size_t total_length, device_length, parent_length, available;
    if (target == NULL) return 0;
    total_length = strlen(target);
    device_length = ql_device_prefix_length(target);
    if (total_length < device_length + target_leaf_length) return 0;
    parent_length = total_length - device_length - target_leaf_length;
    if (parent_length >= QL_QDOS_PATH_MAX) return 0;
    available = QL_QDOS_PATH_MAX - parent_length;
    return available > QL_STAGE_LEAF_MAX ? QL_STAGE_LEAF_MAX : available;
}

/* Join path components without silently changing the destination name. */
static int ql_path_join2(char *out, size_t out_size, const char *left,
                         const char *right)
{
    size_t left_length, right_length;
    if (out == NULL || left == NULL || right == NULL || out_size == 0)
        return -1;
    left_length = strlen(left);
    right_length = strlen(right);
    if (left_length >= out_size || right_length >= out_size - left_length)
        return -1;
    memcpy(out, left, left_length);
    memcpy(out + left_length, right, right_length + 1U);
    return 0;
}

static int ql_path_join3(char *out, size_t out_size, const char *first,
                         const char *second, const char *third)
{
    size_t first_length, second_length, third_length;
    if (out == NULL || first == NULL || second == NULL || third == NULL ||
        out_size == 0)
        return -1;
    first_length = strlen(first);
    second_length = strlen(second);
    third_length = strlen(third);
    if (first_length >= out_size ||
        second_length >= out_size - first_length ||
        third_length >= out_size - first_length - second_length)
        return -1;
    memcpy(out, first, first_length);
    memcpy(out + first_length, second, second_length);
    memcpy(out + first_length + second_length, third, third_length + 1U);
    return 0;
}

/* Leave room for a same-directory stage name within QDOS's 36-byte relative
 * pathname limit. The name replaces the final component; it is not appended. */
static size_t ql_stage_leaf_length(const char *relative_path)
{
    const char *slash;
    size_t parent_length;
    size_t available;
    if (relative_path == NULL) return 0;
    slash = strrchr(relative_path, '/');
    parent_length = slash ? (size_t)(slash - relative_path + 1) : 0;
    if (parent_length >= QL_QDOS_PATH_MAX) return 0;
    available = QL_QDOS_PATH_MAX - parent_length;
    return available > QL_STAGE_LEAF_MAX ? QL_STAGE_LEAF_MAX : available;
}

static void ql_stage_leaf(char role, unsigned long serial,
                          size_t leaf_length, char *leaf)
{
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t i;
    if (leaf_length == 1U) {
        leaf[0] = alphabet[serial % 36UL];
    } else {
        leaf[0] = role;
        for (i = leaf_length; i > 1U; i--) {
            leaf[i - 1U] = alphabet[serial % 36UL];
            serial /= 36UL;
        }
    }
    leaf[leaf_length] = '\0';
}

static unsigned long ql_stage_candidate_count(size_t leaf_length)
{
    unsigned long count = 1UL;
    size_t i;
    size_t digits = leaf_length == 1U ? 1U : leaf_length - 1U;
    for (i = 0; i < digits; i++) {
        if (count > QL_STAGE_CANDIDATE_MAX / 36UL)
            return QL_STAGE_CANDIDATE_MAX;
        count *= 36UL;
    }
    return count > QL_STAGE_CANDIDATE_MAX
           ? QL_STAGE_CANDIDATE_MAX : count;
}

static int ql_ascii_equal_prefix(const char *value, const char *prefix,
                                 size_t length)
{
    size_t i;
    for (i = 0; i < length; i++) {
        char a = value[i];
        char b = prefix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

/* Only recognized disk device prefixes are stripped. An arbitrary first
 * underscore can be valid name data or a directory boundary, so unknown
 * prefixes stay intact. */
static const char *ql_strip_disk_device(const char *qdos_path)
{
    static const char *const device_names[] = {
        "WIN", "MDV", "FLP", "RAM", "NFA", "SFA"
    };
    size_t path_length;
    size_t i;
    if (qdos_path == NULL) return NULL;
    path_length = strlen(qdos_path);
    for (i = 0; i < sizeof(device_names) / sizeof(device_names[0]); i++) {
        size_t prefix_length = strlen(device_names[i]);
        if (path_length < prefix_length + 2U) continue;
        if (ql_ascii_equal_prefix(qdos_path, device_names[i], prefix_length) &&
            qdos_path[prefix_length] >= '1' &&
            qdos_path[prefix_length] <= '8' &&
            qdos_path[prefix_length + 1] == '_')
            return qdos_path + prefix_length + 2;
    }
    return qdos_path;
}

/* QDOS directory qdirect names carry a trailing '_' type marker.  Remove
 * that marker from archive components, while preserving embedded underscores
 * as part of the native component. */
static int ql_project_component(const char *native_name, int is_directory,
                                char *archive_name, size_t archive_size)
{
    size_t length;

    if (native_name == NULL || archive_name == NULL || archive_size == 0)
        return -1;
    length = strlen(native_name);
    if (is_directory && length != 0 && native_name[length - 1] == '_')
        length--;
    if (length == 0 || length >= archive_size) return -1;
    if ((length == 1 && native_name[0] == '.') ||
        (length == 2 && native_name[0] == '.' && native_name[1] == '.'))
        return -1;
    if (memchr(native_name, '/', length) != NULL ||
        memchr(native_name, '\\', length) != NULL ||
        memchr(native_name, ':', length) != NULL)
        return -1;
    memcpy(archive_name, native_name, length);
    archive_name[length] = '\0';
    return 0;
}

#endif
