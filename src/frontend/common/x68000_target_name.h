/* Shared Human68k target name validation. */
#ifndef BP_X68000_TARGET_NAME_H
#define BP_X68000_TARGET_NAME_H

static int x68000_target_add_name(x68k_path_index_t *paths,
                                  const char *utf8_name, int is_directory)
{
    char local[X68K_INDEX_MAX_PATH * 3U];
    size_t length, scan_length, start, i;
    int rc;

    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_SJIS) != 0) {
        fprintf(stderr,
                "boltpack: X68000 profile cannot encode name as standard Shift-JIS: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (length == 0 || length > 255U || local[0] == '/') {
        fprintf(stderr,
                "boltpack: X68000 relative path is empty, absolute, or exceeds 255 Shift-JIS bytes: %s\n",
                utf8_name);
        return BP_ERR_NAME_TOO_LONG;
    }
    scan_length = length;
    if (is_directory && local[length - 1U] == '/') scan_length--;
    for (start = 0, i = 0; i <= scan_length; i++) {
        if (i == scan_length || local[i] == '/') {
            size_t component_length = i - start;
            size_t stem_bytes = 0, extension_bytes = 0;
            size_t j;
            int in_extension = 0, has_dot = 0;
            if (component_length == 0) {
                fprintf(stderr,
                        "boltpack: X68000 path contains an empty component: %s\n",
                        utf8_name);
                return BP_ERR_PARTIAL;
            }
            for (j = start; j < i; ) {
                unsigned char c = (unsigned char)local[j];
                size_t char_bytes = 1U;
                if (x68k_index_sjis_pair_at(local + j)) char_bytes = 2U;
                else if (c >= 0x81U && c <= 0x9FU) {
                    fprintf(stderr,
                            "boltpack: X68000 component has an invalid Shift-JIS sequence: %s\n",
                            utf8_name);
                    return BP_ERR_PARTIAL;
                } else if (c >= 0xE0U && c <= 0xEFU) {
                    fprintf(stderr,
                            "boltpack: X68000 component has an invalid Shift-JIS sequence: %s\n",
                            utf8_name);
                    return BP_ERR_PARTIAL;
                }
                if (char_bytes == 1U) {
                    if (c == '.') {
                        if (has_dot || j == start || j + 1U == i) {
                            fprintf(stderr,
                                    "boltpack: X68000 component has invalid 18.3 extension syntax: %s\n",
                                    utf8_name);
                            return BP_ERR_PARTIAL;
                        }
                        has_dot = 1;
                        in_extension = 1;
                        j++;
                        continue;
                    }
                    if (c < 0x21U || c == 0x7FU || c == '"' || c == '*' ||
                        c == '+' || c == ',' || c == ':' || c == ';' ||
                        c == '<' || c == '=' || c == '>' || c == '?' ||
                        c == '[' || c == '\\' || c == ']' || c == '|') {
                        fprintf(stderr,
                                "boltpack: X68000 component contains a forbidden character: %s\n",
                                utf8_name);
                        return BP_ERR_PARTIAL;
                    }
                }
                if (in_extension) extension_bytes += char_bytes;
                else stem_bytes += char_bytes;
                j += char_bytes;
            }
            if (stem_bytes == 0 || stem_bytes > 18U ||
                extension_bytes > 3U) {
                fprintf(stderr,
                        "boltpack: X68000 component exceeds Human68k 18.3 byte limits: %s\n",
                        utf8_name);
                return BP_ERR_NAME_TOO_LONG;
            }
            start = i + 1U;
        }
    }
    rc = x68k_path_index_add(paths, local, is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: X68000 projected path collision or path limit: %s\n",
                utf8_name);
    return rc;
}

#endif
