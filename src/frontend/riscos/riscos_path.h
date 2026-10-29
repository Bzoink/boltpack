/* Checked pathname projection for the RISC OS adapter. */
#ifndef BP_RISCOS_PATH_H
#define BP_RISCOS_PATH_H

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/bp_codepage.h"

static int bp_riscos_arc_to_path(const char *src, char *dst, size_t capacity)
{
    size_t i, n, component_length = 0;
    if (src == NULL || dst == NULL || capacity == 0 || capacity > INT_MAX)
        return -1;
    if (bp_utf8_to_local(src, dst, (int)capacity, BP_CP_8859_1) != 0) {
        dst[0] = '\0';
        return -1;
    }
    n = strlen(dst);
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)dst[i];
        if (ch == '/') {
            component_length = 0;
            dst[i] = '.';
            continue;
        }
        if (ch == '.' || ch == ':' || ch == '*' || ch == '#' || ch == '$' ||
            ch == '%' || ch == '&' || ch == '@' || ch == '"' || ch == '|' ||
            ch == '^' || ch == '\\' || ++component_length > 10U) {
            /* Refuse syntax aliases and names beyond FileCore's 10-byte
             * leaf bound instead of relying on filing-system truncation. */
            dst[0] = '\0';
            return -1;
        }
    }
    dst[n] = '\0';
    return 0;
}

static int bp_riscos_path_to_arc(const char *src, char *dst, size_t capacity)
{
    size_t i, n, temp_capacity;
    char *utf8;
    if (src == NULL || dst == NULL || capacity == 0) return -1;
    n = strlen(src);
    if (n > (((size_t)-1) - 1U) / 2U) { dst[0] = '\0'; return -1; }
    temp_capacity = n * 2U + 1U;
    if (temp_capacity > INT_MAX) { dst[0] = '\0'; return -1; }
    utf8 = (char *)malloc(temp_capacity);
    if (utf8 == NULL) { dst[0] = '\0'; return -1; }
    if (bp_local_to_utf8(src, utf8, (int)temp_capacity,
                         BP_CP_8859_1) != 0) {
        free(utf8);
        dst[0] = '\0';
        return -1;
    }
    n = strlen(utf8);
    if (n >= capacity) {
        free(utf8);
        dst[0] = '\0';
        return -1;
    }
    for (i = 0; i < n; i++) dst[i] = utf8[i] == '.' ? '/' : utf8[i];
    dst[n] = '\0';
    free(utf8);
    return 0;
}

static int bp_riscos_path_join(char *dst, size_t capacity,
                               const char *dir, const char *name)
{
    size_t dlen, nlen, separator;
    if (dst == NULL || dir == NULL || name == NULL || capacity == 0) return -1;
    dlen = strlen(dir);
    nlen = strlen(name);
    separator = dlen != 0 && dir[dlen - 1] != '.' ? 1U : 0U;
    if (dlen > capacity - 1U || nlen > capacity - 1U - dlen ||
        separator > capacity - 1U - dlen - nlen) {
        dst[0] = '\0';
        return -1;
    }
    memcpy(dst, dir, dlen);
    if (separator) dst[dlen++] = '.';
    memcpy(dst + dlen, name, nlen + 1U);
    return 0;
}

static int bp_riscos_arc_join(char *dst, size_t capacity,
                              const char *dir, const char *name)
{
    size_t dlen, nlen;
    if (dst == NULL || dir == NULL || name == NULL || capacity == 0) return -1;
    dlen = strlen(dir);
    nlen = strlen(name);
    if (dlen > capacity - 1U || nlen > capacity - 1U - dlen ||
        (dlen != 0 && nlen != 0 && 1U > capacity - 1U - dlen - nlen)) {
        dst[0] = '\0';
        return -1;
    }
    memcpy(dst, dir, dlen);
    if (dlen != 0 && nlen != 0) dst[dlen++] = '/';
    memcpy(dst + dlen, name, nlen + 1U);
    return 0;
}

static int bp_riscos_sibling_path(char *dst, size_t capacity,
                                  const char *target, const char *sibling)
{
    const char *last_dot;
    size_t parent_len;
    char parent[1024];
    if (dst == NULL || target == NULL || sibling == NULL || capacity == 0)
        return -1;
    last_dot = strrchr(target, '.');
    parent_len = last_dot == NULL ? 0U : (size_t)(last_dot - target);
    if (parent_len >= sizeof(parent)) { dst[0] = '\0'; return -1; }
    memcpy(parent, target, parent_len);
    parent[parent_len] = '\0';
    return bp_riscos_path_join(dst, capacity, parent, sibling);
}

#endif /* BP_RISCOS_PATH_H */
