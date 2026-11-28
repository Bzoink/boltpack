/*
 * win_names.h -- how Windows sees the names in an archive.
 *
 * An archive name is a UTF-8 string with '/' separators.  Windows refuses or
 * silently changes some names that Unix accepts, and treats names that differ
 * only in case or Unicode normalisation form as the same file.  This header
 * decides, for one extraction, whether a name can be written, and keeps the
 * names already used so that collisions are found:
 *
 *   - characters Windows forbids (: < > " | ? * and controls), a trailing dot
 *     or space, a component longer than 255 or a path longer than MAX_PATH
 *   - reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9 and their
 *     superscript-digit forms, with or without an extension)
 *   - names equal under NFC and ordinal case-insensitive comparison
 *   - a file and a directory that would need the same path
 *
 * Shared by the Windows CLI and by boltpack-recover.  Windows only
 * (NormalizeString, CompareStringOrdinal).  win_names_register() returns
 * BP_OK, BP_ERR_INVALID_ARG (the name cannot exist on Windows) or
 * BP_ERR_PARTIAL (it collides with a name registered earlier); *why, if given,
 * is a short reason.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BP_WIN_NAMES_H
#define BP_WIN_NAMES_H

#include "../../core/bp_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef BP_MAX_PATH_WIDE
#define BP_MAX_PATH_WIDE 260
#endif

static int win_wide_is_valid_utf16(const wchar_t *wide)
{
    const wchar_t *p;
    if (wide == NULL) return 0;
    for (p = wide; *p != L'\0'; p++) {
        if (*p >= 0xD800 && *p <= 0xDBFF) {
            if (p[1] < 0xDC00 || p[1] > 0xDFFF) return 0;
            p++;
        } else if (*p >= 0xDC00 && *p <= 0xDFFF) {
            return 0;
        }
    }
    return 1;
}

typedef struct win_path_node_s {
    struct win_path_node_s *left;
    struct win_path_node_s *right;
    int height;
    bp_uint8 entry_type;
    wchar_t name[BP_MAX_PATH_WIDE];
} win_path_node_t;

static int win_path_height(const win_path_node_t *node)
{
    return node == NULL ? 0 : node->height;
}

static void win_path_update_height(win_path_node_t *node)
{
    int left = win_path_height(node->left);
    int right = win_path_height(node->right);
    node->height = (left > right ? left : right) + 1;
}

static win_path_node_t *win_path_rotate_right(win_path_node_t *root)
{
    win_path_node_t *top = root->left;
    root->left = top->right;
    top->right = root;
    win_path_update_height(root);
    win_path_update_height(top);
    return top;
}

static win_path_node_t *win_path_rotate_left(win_path_node_t *root)
{
    win_path_node_t *top = root->right;
    root->right = top->left;
    top->left = root;
    win_path_update_height(root);
    win_path_update_height(top);
    return top;
}

static int win_path_compare(const wchar_t *left, const wchar_t *right)
{
    int rc = CompareStringOrdinal(left, -1, right, -1, TRUE);
    if (rc == CSTR_LESS_THAN) return -1;
    if (rc == CSTR_GREATER_THAN) return 1;
    return rc == CSTR_EQUAL ? 0 : 2;
}

static win_path_node_t *win_path_find_exact(win_path_node_t *root,
                                            const wchar_t *name)
{
    while (root != NULL) {
        int cmp = win_path_compare(name, root->name);
        if (cmp == 2) return NULL;
        if (cmp == 0) return root;
        root = cmp < 0 ? root->left : root->right;
    }
    return NULL;
}

static win_path_node_t *win_path_lower_bound(win_path_node_t *root,
                                             const wchar_t *name)
{
    win_path_node_t *best = NULL;
    while (root != NULL) {
        int cmp = win_path_compare(root->name, name);
        if (cmp == 2) return NULL;
        if (cmp >= 0) {
            best = root;
            root = root->left;
        } else {
            root = root->right;
        }
    }
    return best;
}

static int win_path_has_prefix(const wchar_t *name, const wchar_t *prefix,
                               size_t prefix_length)
{
    int rc = CompareStringOrdinal(name, (int)prefix_length, prefix,
                                   (int)prefix_length, TRUE);
    return rc == CSTR_EQUAL;
}

static int win_path_compare_n(const wchar_t *left, size_t left_length,
                              const char *right, size_t right_length)
{
    wchar_t wide[16];
    size_t i;
    int rc;
    if (left_length != right_length || right_length >= 16U) return 1;
    for (i = 0; i < right_length; i++) wide[i] = (unsigned char)right[i];
    rc = CompareStringOrdinal(left, (int)left_length, wide,
                              (int)right_length, TRUE);
    if (rc == CSTR_EQUAL) return 0;
    return rc == CSTR_LESS_THAN ? -1 : 1;
}

static win_path_node_t *win_path_insert(win_path_node_t *root,
                                        const wchar_t *name,
                                        bp_uint8 entry_type, int *result)
{
    int cmp, balance;
    win_path_node_t *child;
    if (root == NULL) {
        root = (win_path_node_t *)calloc(1, sizeof(*root));
        if (root == NULL) { *result = BP_ERR_NOMEM; return NULL; }
        wcscpy(root->name, name);
        root->height = 1;
        root->entry_type = entry_type;
        *result = BP_OK;
        return root;
    }
    cmp = win_path_compare(name, root->name);
    if (cmp == 2) { *result = BP_ERR_INVALID_ARG; return root; }
    if (cmp == 0) { *result = BP_ERR_PARTIAL; return root; }
    if (cmp < 0) {
        child = win_path_insert(root->left, name, entry_type, result);
        if (*result != BP_OK) return root;
        root->left = child;
    } else {
        child = win_path_insert(root->right, name, entry_type, result);
        if (*result != BP_OK) return root;
        root->right = child;
    }
    win_path_update_height(root);
    balance = win_path_height(root->left) - win_path_height(root->right);
    if (balance > 1) {
        if (win_path_compare(name, root->left->name) > 0)
            root->left = win_path_rotate_left(root->left);
        return win_path_rotate_right(root);
    }
    if (balance < -1) {
        if (win_path_compare(name, root->right->name) < 0)
            root->right = win_path_rotate_right(root->right);
        return win_path_rotate_left(root);
    }
    return root;
}

static void win_path_free(win_path_node_t *root)
{
    if (root == NULL) return;
    win_path_free(root->left);
    win_path_free(root->right);
    free(root);
}

static int win_component_is_device(const wchar_t *component, size_t length)
{
    size_t base = 0;
    size_t i;
    static const char *const devices[] = {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"
    };
    while (base < length && component[base] != L'.') base++;
    while (base > 0 && (component[base - 1] == L' ' ||
                        component[base - 1] == L'.')) base--;
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        size_t n = strlen(devices[i]);
        if (base == n && win_path_compare_n(component, base, devices[i], n) == 0)
            return 1;
    }
    if (base == 4 &&
        (win_path_compare_n(component, 3, "COM", 3) == 0 ||
         win_path_compare_n(component, 3, "LPT", 3) == 0) &&
        ((component[3] >= L'1' && component[3] <= L'9') ||
         component[3] == 0x00B9 || component[3] == 0x00B2 ||
         component[3] == 0x00B3)) return 1;
    return 0;
}

/* Project a name to the key under which Windows would know it, or say why it
 * cannot be a Windows name.  *why is a short reason when the result is not BP_OK. */
static int win_names_project(const char *local_name, wchar_t *key,
                             int key_capacity, const char **why)
{
    wchar_t normalized[BP_MAX_PATH_WIDE];
    int length, normalized_length, i, start;
    *why = "not-valid-on-windows";
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, local_name, -1,
                                 key, key_capacity);
    if (length <= 0) { *why = "path-too-long"; return BP_ERR_INVALID_ARG; }
    length--;
    if (!win_wide_is_valid_utf16(key)) { *why = "bad-encoding"; return BP_ERR_INVALID_ARG; }
    for (i = 0; i < length; i++)
        if (key[i] == L'/') key[i] = L'\\';
    normalized_length = NormalizeString(NormalizationC, key, length,
                                        normalized, BP_MAX_PATH_WIDE);
    if (normalized_length <= 0 || normalized_length >= key_capacity) {
        *why = "path-too-long";
        return BP_ERR_INVALID_ARG;
    }
    memcpy(key, normalized,
           (size_t)normalized_length * sizeof(wchar_t));
    length = normalized_length;
    key[length] = L'\0';
    if (length == 0 || key[0] == L'\\' || length >= BP_MAX_PATH_WIDE) {
        *why = "path-too-long";
        return BP_ERR_INVALID_ARG;
    }
    for (i = 0; i < length; i++) {
        if (key[i] == L':' || key[i] == L'<' || key[i] == L'>' ||
            key[i] == L'"' || key[i] == L'|' || key[i] == L'?' ||
            key[i] == L'*' || key[i] < 32) {
            *why = "forbidden-character";
            return BP_ERR_INVALID_ARG;
        }
    }
    while (length > 0 && key[length - 1] == L'\\') length--;
    if (length == 0) { *why = "empty-name"; return BP_ERR_INVALID_ARG; }
    key[length] = L'\0';
    start = 0;
    for (i = 0; i <= length; i++) {
        if (key[i] == L'\\' || key[i] == L'\0') {
            int component_length = i - start;
            if (component_length <= 0 ||
                (component_length == 1 && key[start] == L'.')) { *why = "empty-component"; return BP_ERR_INVALID_ARG; }
            if (key[i - 1] == L'.' || key[i - 1] == L' ') {
                *why = "trailing-dot-or-space";
                return BP_ERR_INVALID_ARG;
            }
            if (component_length > 255) {
                *why = "component-too-long";
                return BP_ERR_INVALID_ARG;
            }
            if (win_component_is_device(key + start, (size_t)component_length)) {
                *why = "reserved-device-name";
                return BP_ERR_INVALID_ARG;
            }
            start = i + 1;
        }
    }
    return BP_OK;
}

static int win_project_windows_path(const char *local_name, wchar_t *key,
                                    int key_capacity)
{
    const char *why;
    return win_names_project(local_name, key, key_capacity, &why);
}

static int win_names_claim(win_path_node_t **tree, const char *local_name,
                           bp_uint8 entry_type, const char **why_out, int insert)
{
    wchar_t key[BP_MAX_PATH_WIDE];
    size_t i, length;
    const char *why_local = NULL;
    const char **why = why_out != NULL ? why_out : &why_local;
    int rc = win_names_project(local_name, key, BP_MAX_PATH_WIDE, why);
    if (rc != BP_OK) return rc;
    length = wcslen(key);
    for (i = 0; i < length; i++) {
        if (key[i] == L'\\') {
            wchar_t saved = key[i];
            key[i] = L'\0';
            {
                win_path_node_t *ancestor =
                    win_path_find_exact(*tree, key);
                if (ancestor != NULL && ancestor->entry_type == BP_TYPE_DATA) {
                    key[i] = saved;
                    *why = "file-and-directory-conflict";
                    return BP_ERR_PARTIAL;
                }
            }
            key[i] = saved;
        }
    }
    {
        wchar_t descendant_prefix[BP_MAX_PATH_WIDE];
        win_path_node_t *next;
        if (entry_type == BP_TYPE_DATA && length + 1 < BP_MAX_PATH_WIDE) {
            memcpy(descendant_prefix, key, length * sizeof(wchar_t));
            descendant_prefix[length] = L'\\';
            descendant_prefix[length + 1] = L'\0';
            next = win_path_lower_bound(*tree,
                                        descendant_prefix);
            if (next != NULL &&
                win_path_has_prefix(next->name, descendant_prefix,
                                    length + 1)) {
                *why = "file-and-directory-conflict";
                return BP_ERR_PARTIAL;
            }
        }
    }
    if (!insert) {
        /* a probe: would this exact name collide with one already registered? */
        if (win_path_find_exact(*tree, key) != NULL) {
            *why = "name-collision";
            return BP_ERR_PARTIAL;
        }
        return BP_OK;
    }
    *tree = win_path_insert(*tree, key, entry_type, &rc);
    if (rc != BP_OK) *why = "name-collision";
    return rc;
}

static int win_names_register(win_path_node_t **tree, const char *local_name,
                              bp_uint8 entry_type, const char **why_out)
{
    return win_names_claim(tree, local_name, entry_type, why_out, 1);
}

/* The same checks without registering the name. */
static int win_names_probe(win_path_node_t **tree, const char *local_name,
                           bp_uint8 entry_type, const char **why_out)
{
    return win_names_claim(tree, local_name, entry_type, why_out, 0);
}

#endif /* BP_WIN_NAMES_H */
