/* Counted UTF-8 validation shared by the full and mini archive readers. */
#ifndef BP_ARCHIVE_NAME_H
#define BP_ARCHIVE_NAME_H

#include "bp_types.h"

static int bp_archive_name_valid(const char *name, bp_uint32 len)
{
    bp_uint32 i;
    const bp_uint8 *s;
    if (name == NULL || len == 0) return 0;
    s = (const bp_uint8 *)name;
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
    return 1;
}
#endif
