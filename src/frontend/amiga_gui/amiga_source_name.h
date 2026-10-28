/* Project selected Amiga local paths to canonical UTF-8 archive names. */
#ifndef BP_AMIGA_GUI_SOURCE_NAME_H
#define BP_AMIGA_GUI_SOURCE_NAME_H

#include "../../core/archiver.h"
#include "../../core/archive_name.h"
#include "../../core/bp_codepage.h"

static int gui_source_parent(const char *path, char *base, size_t cap)
{
    const char *slash, *colon;
    size_t n;
    if (!path || !base || cap == 0) return -1;
    slash = strrchr(path, '/');
    colon = strrchr(path, ':');
    if (colon && (!slash || colon > slash)) n = (size_t)(colon - path) + 1;
    else if (slash) n = slash == path ? 1 : (size_t)(slash - path);
    else n = 0;
    if (n >= cap) return -1;
    memcpy(base, path, n); base[n] = '\0';
    return 0;
}

static int gui_source_archive_name(const char *base, const char *path,
                                   bp_uint8 entry_type, char *name, size_t cap)
{
    const char *rel;
    size_t n, length;
    if (!base || !path || !name || cap == 0 || cap > 32767U) return -1;
    n = strlen(base);
    if (n && strncmp(base, path, n) != 0) return -1;
    rel = path + n;
    if (n && base[n - 1] != ':' && base[n - 1] != '/') {
        if (*rel != '/') return -1;
        rel++;
    }
    if (!*rel || strchr(rel, ':') != NULL) return -1;
    if (bp_local_to_utf8(rel, name, (int)cap, BP_CP_8859_1) != 0) return -1;
    length = strlen(name);
    if (entry_type == BP_TYPE_DIR && (length == 0 || name[length - 1] != '/')) {
        if (length + 2 > cap) return -1;
        name[length++] = '/'; name[length] = '\0';
    }
    if (length > BP_MAX_NAME_LEN ||
        !bp_archive_name_valid(name, (bp_uint32)length) ||
        !bp_archive_name_type_valid(name, (bp_uint32)length, entry_type))
        return -1;
    return 0;
}
#endif /* BP_AMIGA_GUI_SOURCE_NAME_H */
