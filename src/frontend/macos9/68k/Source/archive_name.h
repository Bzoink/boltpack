/* Counted UTF-8 validation shared by the full and mini archive readers. */
#ifndef BP_ARCHIVE_NAME_H
#define BP_ARCHIVE_NAME_H

#include "bp_types.h"
#include <string.h>

static int bp_archive_name_valid(const char *name, bp_uint32 len)
{
    bp_uint32 i;
    const bp_uint8 *s;
    if (name == NULL || len == 0) return 0;
    s = (const bp_uint8 *)name;
    if (s[0] == '/' || (len > 1 && s[1] == ':')) return 0;
    i = 0;
    while (i < len) {
        bp_uint32 n, j;
        bp_uint8 c = s[i];
        if (c == 0) return 0;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) n = 2;
        else if (c >= 0xE0 && c <= 0xEF) n = 3;
        else if (c >= 0xF0 && c <= 0xF4) n = 4;
        else return 0;
        if (n > len - i) return 0;
        for (j = 1; j < n; j++)
            if ((s[i + j] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[i + 1] < 0xA0) return 0;
        if (c == 0xED && s[i + 1] >= 0xA0) return 0;
        if (c == 0xF0 && s[i + 1] < 0x90) return 0;
        if (c == 0xF4 && s[i + 1] >= 0x90) return 0;
        i += n;
    }

    /* Keep archive paths canonical so distinct UTF-8 strings cannot resolve
     * to the same filesystem object through empty or dot components. A single
     * trailing slash is retained for directory entry names. */
    if (memchr(s, '\\', (size_t)len) != NULL) return 0;
    {
        bp_uint32 start = 0;
        for (i = 0; i <= len; i++) {
            bp_uint32 component_len;
            if (i < len && s[i] != '/') continue;
            component_len = i - start;
            if (component_len == 0) {
                if (i == len && len > 0 && s[len - 1] == '/') break;
                return 0;
            }
            if ((component_len == 1 && s[start] == '.') ||
                (component_len == 2 && s[start] == '.' &&
                 s[start + 1] == '.')) return 0;
            start = i + 1;
        }
    }
    return 1;
}

static int bp_archive_name_type_valid(const char *name, bp_uint32 len,
                                      bp_uint8 entry_type)
{
    int trailing_slash;
    if (name == NULL || len == 0) return 0;
    trailing_slash = name[len - 1] == '/';
    if (entry_type == BP_TYPE_DIR) return trailing_slash;
    if (entry_type == BP_TYPE_DATA || entry_type == BP_TYPE_SYMLINK)
        return !trailing_slash;
    if (entry_type == BP_TYPE_RSRC || entry_type == BP_TYPE_META) return 1;
    return 0;
}
#endif
