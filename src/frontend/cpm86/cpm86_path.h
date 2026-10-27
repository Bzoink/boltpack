/*
 * cpm86_path.h -- strict CP/M-86 flat 8.3 filename projection
 *
 * ANSI C89 only. Source names may use one drive prefix and lowercase ASCII;
 * directories, truncation, and characters outside the conservative CP/M
 * interchange set are refused.
 */

#ifndef BP_CPM86_PATH_H
#define BP_CPM86_PATH_H

#include <string.h>

#define BP_CPM86_NAME_SIZE 14

static int bp_cpm86_name_char(unsigned char ch)
{
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9'))
        return 1;
    switch (ch) {
    case '$': case '#': case '@': case '!': case '%': case '&':
    case '\'': case '(': case ')': case '-': case '^': case '_':
    case '{': case '}': case '~':
        return 1;
    default:
        return 0;
    }
}

static int bp_cpm86_project_name(const char *source, char *projected)
{
    const unsigned char *p;
    unsigned int base_length = 0;
    unsigned int extension_length = 0;
    unsigned int out = 0;
    int in_extension = 0;
    int have_base = 0;

    if (source == NULL || projected == NULL) return 0;
    p = (const unsigned char *)source;
    if (((p[0] >= 'A' && p[0] <= 'Z') ||
         (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        p += 2;
    if (*p == '\0') return 0;

    while (*p != '\0') {
        unsigned char ch = *p++;
        if (ch == '.') {
            if (in_extension || !have_base) return 0;
            in_extension = 1;
            projected[out++] = '.';
            continue;
        }
        if (!bp_cpm86_name_char(ch)) return 0;
        if (in_extension) {
            if (++extension_length > 3U) return 0;
        } else {
            if (++base_length > 8U) return 0;
            have_base = 1;
        }
        if (out >= 12U) return 0;
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        projected[out++] = (char)ch;
    }
    if (!have_base || (in_extension && extension_length == 0U)) return 0;
    projected[out] = '\0';
    return 1;
}

static int bp_cpm86_names_unique(char *const *names, unsigned int count)
{
    char current[BP_CPM86_NAME_SIZE];
    char previous[BP_CPM86_NAME_SIZE];
    unsigned int i, j;

    if (names == NULL && count != 0U) return 0;
    for (i = 0; i < count; i++) {
        if (!bp_cpm86_project_name(names[i], current)) return 0;
        for (j = 0; j < i; j++) {
            if (!bp_cpm86_project_name(names[j], previous) ||
                strcmp(current, previous) == 0)
                return 0;
        }
    }
    return 1;
}

static int bp_cpm86_create_names_safe(const char *archive_name,
                                      char *const *sources,
                                      unsigned int count)
{
    char archive_projected[BP_CPM86_NAME_SIZE];
    char source_projected[BP_CPM86_NAME_SIZE];
    unsigned int i;

    if (!bp_cpm86_project_name(archive_name, archive_projected) ||
        !bp_cpm86_names_unique(sources, count))
        return 0;
    for (i = 0; i < count; i++) {
        if (!bp_cpm86_project_name(sources[i], source_projected) ||
            strcmp(source_projected, archive_projected) == 0)
            return 0;
    }
    return 1;
}

#endif /* BP_CPM86_PATH_H */
