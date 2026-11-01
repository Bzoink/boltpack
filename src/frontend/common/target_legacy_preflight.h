/* Shared target profile writer preflights for legacy filesystems.
 * Included by the POSIX CLI and the Linux FLTK GUI after the caller has
 * defined add_job_t/add_job_list_t and included the core headers. */
#ifndef BP_TARGET_LEGACY_PREFLIGHT_H
#define BP_TARGET_LEGACY_PREFLIGHT_H

#include "../../core/archive_name.h"
#include "../../core/bp_codepage.h"
#include "../posix/archive_preflight.h"
#include "../atari/atari_path.h"
#include "../cpm86/cpm86_path.h"
#include "../x68k/x68k_path_index.h"
#include "x68000_target_name.h"

static int atari_target_add_name(bp_atari_name_set_t *projected_names,
                                 bp_posix_preflight_t *prefixes,
                                 const char *utf8_name, int is_directory)
{
    char local_name[BP_ATARI_GEMDOS_PATH_MAX + 2U];
    char key[BP_ATARI_GEMDOS_PATH_MAX + 1U];
    unsigned int key_length;
    size_t i;
    int rc;

    if (bp_utf8_to_local(utf8_name, local_name, sizeof(local_name),
                         BP_CP_ATARIST) != 0) {
        fprintf(stderr,
                "boltpack: Atari target profile cannot encode name: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    rc = bp_atari_project_name(local_name, is_directory, key, sizeof(key),
                               &key_length);
    if (rc != BP_OK) {
        fprintf(stderr,
                "boltpack: name is outside the Atari GEMDOS 8.3 profile: %s\n",
                utf8_name);
        return rc;
    }
    rc = bp_atari_name_set_add(projected_names, key, key_length);
    if (rc != BP_OK) {
        fprintf(stderr,
                "boltpack: Atari target name collision or profile index limit: %s\n",
                utf8_name);
        return rc;
    }

    /* The generic prefix checker handles file/parent conflicts.  The Atari
     * key has already been uppercased and normalized to GEMDOS separators. */
    for (i = 0; i < key_length; i++)
        if (key[i] == '\\') key[i] = '/';
    rc = bp_posix_preflight_add(prefixes, key, key_length, is_directory);
    if (rc != BP_OK) {
        fprintf(stderr,
                "boltpack: Atari target file/directory path conflict: %s\n",
                utf8_name);
        return rc;
    }
    return BP_OK;
}

static int dos_target_codepage(target_profile_t profile)
{
    switch (profile) {
    case TARGET_PROFILE_DOS_CP850_83:
    case TARGET_PROFILE_DOS_CP850_LFN:
    case TARGET_PROFILE_DOS16_CP850_MINI:
    case TARGET_PROFILE_OS2_16_CP850_MINI:
    case TARGET_PROFILE_OS2_FAT_CP850:
        return BP_CP_850;
    case TARGET_PROFILE_DOS_CP437_83:
    case TARGET_PROFILE_WIN16_CP437_MINI:
        return BP_CP_437;
    default:
        return BP_CP_INVALID;
    }
}

static const char *dos_target_label(target_profile_t profile)
{
    switch (profile) {
    case TARGET_PROFILE_DOS_CP850_83: return "DOS CP850 8.3";
    case TARGET_PROFILE_DOS_CP850_LFN: return "DOS CP850 LFN";
    case TARGET_PROFILE_DOS_CP437_83: return "DOS CP437 8.3";
    case TARGET_PROFILE_DOS16_CP850_MINI: return "DOS16 CP850 mini";
    case TARGET_PROFILE_WIN16_CP437_MINI: return "Win16 CP437 mini";
    case TARGET_PROFILE_OS2_16_CP850_MINI: return "OS/2-16 CP850 mini";
    case TARGET_PROFILE_OS2_FAT_CP850: return "OS/2 FAT CP850";
    default: return "legacy DOS-family";
    }
}

static bp_uint8 dos_target_metadata_platform(target_profile_t profile)
{
    switch (profile) {
    case TARGET_PROFILE_WIN16_CP437_MINI: return BP_OS_WIN16;
    case TARGET_PROFILE_OS2_16_CP850_MINI:
    case TARGET_PROFILE_OS2_FAT_CP850: return BP_OS_OS2;
    default: return BP_OS_DOS;
    }
}

static int dos_target_component_83(const char *component, size_t length)
{
    size_t stem = 0, extension = 0, i;
    char reserved[9];

    if (length == 0 || component[length - 1U] == ' ' ||
        component[length - 1U] == '.') return 0;
    while (stem < length && component[stem] != '.') stem++;
    if (stem == 0 || stem > 8U) return 0;
    if (stem < length) {
        if (stem + 1U == length) return 0;
        extension = length - stem - 1U;
        if (extension > 3U ||
            memchr(component + stem + 1U, '.', extension) != NULL) return 0;
    }
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)component[i];
        if (c == '.') continue;
        if (c < 0x20U || c == 0x7FU) return 0;
        if (c >= 0x80U) continue; /* CP437/CP850 bytes are target-defined. */
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c)))
            return 0;
    }
    if (stem <= 4U) {
        for (i = 0; i < stem; i++) {
            unsigned char c = (unsigned char)component[i];
            if (c >= 0x80U) return 1;
            reserved[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        }
        reserved[stem] = '\0';
        if (strcmp(reserved, "CON") == 0 || strcmp(reserved, "PRN") == 0 ||
            strcmp(reserved, "AUX") == 0 || strcmp(reserved, "NUL") == 0 ||
            (stem == 4U && reserved[3] >= '1' && reserved[3] <= '9' &&
             (memcmp(reserved, "COM", 3) == 0 ||
              memcmp(reserved, "LPT", 3) == 0))) return 0;
    }
    return 1;
}

static int dos_target_add_name(bp_posix_preflight_t *paths,
                               const char *utf8_name, int is_directory,
                               target_profile_t profile)
{
    char local[261], folded[261];
    size_t length, i, start;
    int codepage = dos_target_codepage(profile);
    int rc;

    if (codepage == BP_CP_INVALID ||
        bp_utf8_to_local(utf8_name, local, (int)sizeof(local), codepage) != 0) {
        fprintf(stderr, "boltpack: %s cannot encode name: %s\n",
                dos_target_label(profile), utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (is_directory && length != 0 && local[length - 1U] == '/')
        local[--length] = '\0';
    if (length == 0 || length >= 260U || local[0] == '/') {
        fprintf(stderr, "boltpack: %s path is empty, absolute, or too long: %s\n",
                dos_target_label(profile), utf8_name);
        return BP_ERR_NAME_TOO_LONG;
    }
    start = 0;
    for (i = 0; i <= length; i++) {
        size_t component_length;
        if (i != length && local[i] != '/') {
            if (local[i] == '\\') {
                fprintf(stderr, "boltpack: %s path contains a backslash: %s\n",
                        dos_target_label(profile), utf8_name);
                return BP_ERR_PARTIAL;
            }
            continue;
        }
        component_length = i - start;
        if (!dos_target_component_83(local + start, component_length)) {
            fprintf(stderr,
                    "boltpack: %s component is not legal 8.3 or is a reserved device name: %s\n",
                    dos_target_label(profile), utf8_name);
            return BP_ERR_PARTIAL;
        }
        start = i + 1U;
    }
    if (bp_local_casefold(local, folded, (int)sizeof(folded), codepage) != 0)
        return BP_ERR_PARTIAL;
    rc = bp_posix_preflight_add(paths, folded, strlen(folded), is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: %s projected path collision or path limit: %s\n",
                dos_target_label(profile), utf8_name);
    return rc;
}

static int dos_target_preflight(const add_job_list_t *jobs,
                                const bp_archive_t *existing,
                                target_profile_t profile)
{
    bp_posix_preflight_t paths;
    bp_uint8 metadata_platform = dos_target_metadata_platform(profile);
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory;
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform == BP_META_PORTABLE ||
                    entry->platform == metadata_platform) continue;
                fprintf(stderr,
                        "boltpack: existing archive has metadata unsupported by %s: %s\n",
                        dos_target_label(profile), entry->name);
                rc = BP_ERR_PARTIAL;
                goto dos_target_done;
            }
            if (entry->entry_type == BP_TYPE_RSRC ||
                entry->entry_type == BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: existing archive has an unsupported entry type for %s: %s\n",
                        dos_target_label(profile), entry->name);
                rc = BP_ERR_PARTIAL;
                goto dos_target_done;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) continue;
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: existing file exceeds the %s signed-I/O limit: %s\n",
                        dos_target_label(profile), entry->name);
                rc = BP_ERR_OVERFLOW;
                goto dos_target_done;
            }
            is_directory = entry->entry_type == BP_TYPE_DIR;
            rc = dos_target_add_name(&paths, entry->name, is_directory,
                                     profile);
            if (rc != BP_OK) goto dos_target_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory;
            if (job->entry_type == BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: %s does not support symbolic links: %s\n",
                        dos_target_label(profile), job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto dos_target_done;
            }
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) continue;
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: source exceeds the %s signed-I/O limit: %s\n",
                        dos_target_label(profile), job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto dos_target_done;
            }
            is_directory = job->entry_type == BP_TYPE_DIR;
            rc = dos_target_add_name(&paths, job->arcname, is_directory,
                                     profile);
            if (rc != BP_OK) goto dos_target_done;
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr, "boltpack: %s file/directory path conflict\n",
                dos_target_label(profile));

dos_target_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

static int dos_lfn_component_valid(const char *component, size_t length)
{
    size_t stem = 0, i;
    char reserved[9];

    if (length == 0 || length > 255U || component[length - 1U] == '.' ||
        component[length - 1U] == ' ') return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)component[i];
        if (c < 0x20U || c == 0x7FU || c == '\\' ||
            strchr("\"*:<>?|", c) != NULL) return 0;
        if (c == ' ' && (i + 1U == length || component[i + 1U] == '.'))
            return 0;
    }
    while (stem < length && component[stem] != '.') stem++;
    if (stem <= 8U) {
        for (i = 0; i < stem; i++) {
            unsigned char c = (unsigned char)component[i];
            reserved[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        }
        reserved[stem] = '\0';
        if (strcmp(reserved, "CON") == 0 || strcmp(reserved, "PRN") == 0 ||
            strcmp(reserved, "AUX") == 0 || strcmp(reserved, "NUL") == 0 ||
            (stem == 4U && reserved[3] >= '1' && reserved[3] <= '9' &&
             (memcmp(reserved, "COM", 3) == 0 ||
              memcmp(reserved, "LPT", 3) == 0))) return 0;
    }
    return 1;
}

static int dos_lfn_add_name(bp_posix_preflight_t *paths,
                            const char *utf8_name, int is_directory)
{
    char local[260], folded[260];
    size_t length, start, i;
    int rc;

    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local), BP_CP_850) != 0) {
        fprintf(stderr, "boltpack: DOS CP850 LFN cannot encode name: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (is_directory && length != 0 && local[length - 1U] == '/')
        local[--length] = '\0';
    if (length == 0 || length >= 260U || local[0] == '/') {
        fprintf(stderr,
                "boltpack: DOS CP850 LFN path is empty, absolute, or exceeds 259 bytes: %s\n",
                utf8_name);
        return BP_ERR_NAME_TOO_LONG;
    }
    for (start = 0, i = 0; i <= length; i++) {
        if (i == length || local[i] == '/') {
            if (!dos_lfn_component_valid(local + start, i - start)) {
                fprintf(stderr,
                        "boltpack: DOS CP850 LFN component is illegal or exceeds 255 bytes: %s\n",
                        utf8_name);
                return BP_ERR_PARTIAL;
            }
            start = i + 1U;
        }
    }
    if (bp_local_casefold(local, folded, (int)sizeof(folded), BP_CP_850) != 0)
        return BP_ERR_PARTIAL;
    rc = bp_posix_preflight_add(paths, folded, strlen(folded), is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: DOS CP850 LFN projected path collision or limit: %s\n",
                utf8_name);
    return rc;
}

static int dos_lfn_target_preflight(const add_job_list_t *jobs,
                                    const bp_archive_t *existing)
{
    bp_posix_preflight_t paths;
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory;
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform == BP_META_PORTABLE ||
                    entry->platform == BP_OS_DOS) continue;
                fprintf(stderr,
                        "boltpack: existing archive has foreign metadata for DOS LFN: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto dos_lfn_done;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: DOS CP850 LFN does not support this entry type: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto dos_lfn_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                rc = BP_ERR_OVERFLOW;
                goto dos_lfn_done;
            }
            is_directory = entry->entry_type == BP_TYPE_DIR;
            rc = dos_lfn_add_name(&paths, entry->name, is_directory);
            if (rc != BP_OK) goto dos_lfn_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory;
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: DOS CP850 LFN does not support source links or special entries: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto dos_lfn_done;
            }
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                rc = BP_ERR_OVERFLOW;
                goto dos_lfn_done;
            }
            is_directory = job->entry_type == BP_TYPE_DIR;
            rc = dos_lfn_add_name(&paths, job->arcname, is_directory);
            if (rc != BP_OK) goto dos_lfn_done;
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr, "boltpack: DOS CP850 LFN file/directory path conflict\n");

dos_lfn_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

static void amiga_latin1_casefold(const char *src, char *dst, size_t size)
{
    size_t i, length = strlen(src);
    if (size == 0) return;
    if (length >= size) length = size - 1U;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        else if ((c >= 0xE0U && c <= 0xF6U) ||
                 (c >= 0xF8U && c <= 0xFEU))
            c = (unsigned char)(c - 0x20U);
        dst[i] = (char)c;
    }
    dst[length] = '\0';
}

static int amiga_target_add_name(bp_posix_preflight_t *paths,
                                 const char *utf8_name, int is_directory)
{
    char local[257], folded[257];
    size_t length, start, i;
    int rc;

    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_8859_1) != 0) {
        fprintf(stderr,
                "boltpack: AmigaOS 68k profile cannot encode name as Latin-1: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (is_directory && length != 0 && local[length - 1U] == '/')
        local[--length] = '\0';
    if (length == 0 || length > 255U || local[0] == '/') {
        fprintf(stderr,
                "boltpack: AmigaOS 68k path is empty, absolute, or exceeds 255 Latin-1 bytes: %s\n",
                utf8_name);
        return BP_ERR_NAME_TOO_LONG;
    }
    for (start = 0, i = 0; i <= length; i++) {
        if (i == length || local[i] == '/') {
            size_t component_length = i - start;
            size_t j;
            if (component_length == 0 || component_length > 30U ||
                (component_length == 1U && local[start] == '.') ||
                (component_length == 2U && local[start] == '.' &&
                 local[start + 1U] == '.')) {
                fprintf(stderr,
                        "boltpack: AmigaOS 68k component is empty, reserved, or exceeds 30 bytes: %s\n",
                        utf8_name);
                return BP_ERR_NAME_TOO_LONG;
            }
            for (j = start; j < i; j++) {
                unsigned char c = (unsigned char)local[j];
                if (c < 0x20U || c == 0x7FU || c == ':') {
                    fprintf(stderr,
                            "boltpack: AmigaOS 68k name contains a control character or volume separator: %s\n",
                            utf8_name);
                    return BP_ERR_PARTIAL;
                }
            }
            start = i + 1U;
        } else if (local[i] == '\\') {
            fprintf(stderr,
                    "boltpack: AmigaOS 68k profile rejects backslashes in paths: %s\n",
                    utf8_name);
            return BP_ERR_PARTIAL;
        }
    }
    amiga_latin1_casefold(local, folded, sizeof(folded));
    rc = bp_posix_preflight_add(paths, folded, strlen(folded), is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: AmigaOS 68k projected path collision or path conflict: %s\n",
                utf8_name);
    return rc;
}

static int amiga_target_preflight(const add_job_list_t *jobs,
                                  const bp_archive_t *existing)
{
    bp_posix_preflight_t paths;
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory;
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform == BP_OS_AMIGA_68K) continue;
                fprintf(stderr,
                        "boltpack: existing archive has metadata unsupported by AmigaOS 68k: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto amiga_target_done;
            }
            if (entry->entry_type == BP_TYPE_RSRC ||
                entry->entry_type == BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: existing archive has an entry type unsupported by AmigaOS 68k: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto amiga_target_done;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: existing archive has an unknown entry type for AmigaOS 68k: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto amiga_target_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: existing file exceeds AmigaOS 68k signed-I/O limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto amiga_target_done;
            }
            is_directory = entry->entry_type == BP_TYPE_DIR;
            rc = amiga_target_add_name(&paths, entry->name, is_directory);
            if (rc != BP_OK) goto amiga_target_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory;
            if (job->entry_type == BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: AmigaOS 68k does not support symbolic links: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto amiga_target_done;
            }
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: unsupported source entry type for AmigaOS 68k: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto amiga_target_done;
            }
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: source exceeds AmigaOS 68k signed-I/O limit: %s\n",
                        job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto amiga_target_done;
            }
            is_directory = job->entry_type == BP_TYPE_DIR;
            rc = amiga_target_add_name(&paths, job->arcname, is_directory);
            if (rc != BP_OK) goto amiga_target_done;
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr, "boltpack: AmigaOS 68k file/directory path conflict\n");

amiga_target_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

static int macos_classic_add_name(bp_posix_preflight_t *paths,
                                  const char *utf8_name, int is_directory)
{
    char local[256], folded[256];
    size_t length, start, i;
    int rc;

    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_MACROMAN) != 0) {
        fprintf(stderr,
                "boltpack: Classic Mac HFS profile cannot encode name as MacRoman: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (is_directory && length != 0 && local[length - 1U] == '/')
        local[--length] = '\0';
    if (length == 0 || length > 255U || local[0] == '/') {
        fprintf(stderr,
                "boltpack: Classic Mac HFS path is empty, absolute, or exceeds 255 bytes: %s\n",
                utf8_name);
        return BP_ERR_NAME_TOO_LONG;
    }
    for (start = 0, i = 0; i <= length; i++) {
        if (i == length || local[i] == '/') {
            size_t component_length = i - start;
            size_t j;
            if (component_length == 0 || component_length > 31U ||
                (component_length == 1U && local[start] == '.') ||
                (component_length == 2U && local[start] == '.' &&
                 local[start + 1U] == '.')) {
                fprintf(stderr,
                        "boltpack: Classic Mac HFS component is empty, reserved, or exceeds 31 MacRoman bytes: %s\n",
                        utf8_name);
                return BP_ERR_NAME_TOO_LONG;
            }
            for (j = start; j < i; j++) {
                unsigned char c = (unsigned char)local[j];
                if (c < 0x20U || c == 0x7FU || c == ':') {
                    fprintf(stderr,
                            "boltpack: Classic Mac HFS name contains a control character or colon: %s\n",
                            utf8_name);
                    return BP_ERR_PARTIAL;
                }
            }
            start = i + 1U;
        } else if (local[i] == '\\') {
            fprintf(stderr,
                    "boltpack: Classic Mac HFS profile rejects backslashes in paths: %s\n",
                    utf8_name);
            return BP_ERR_PARTIAL;
        }
    }
    if (bp_local_casefold(local, folded, (int)sizeof(folded),
                          BP_CP_MACROMAN) != 0) {
        fprintf(stderr,
                "boltpack: cannot safely case-fold Classic Mac HFS name: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    rc = bp_posix_preflight_add(paths, folded, strlen(folded), is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: Classic Mac HFS projected path collision or conflict: %s\n",
                utf8_name);
    return rc;
}

static int macos_classic_target_preflight(const add_job_list_t *jobs,
                                         const bp_archive_t *existing)
{
    bp_posix_preflight_t paths;
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory;
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Classic Mac HFS profile cannot preserve this entry type or metadata: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto macos_classic_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: Classic Mac HFS file exceeds signed 32-bit I/O limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto macos_classic_done;
            }
            is_directory = entry->entry_type == BP_TYPE_DIR;
            rc = macos_classic_add_name(&paths, entry->name, is_directory);
            if (rc != BP_OK) goto macos_classic_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory;
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Classic Mac HFS profile does not support source links or special entries: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto macos_classic_done;
            }
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: source exceeds Classic Mac HFS signed 32-bit I/O limit: %s\n",
                        job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto macos_classic_done;
            }
            is_directory = job->entry_type == BP_TYPE_DIR;
            rc = macos_classic_add_name(&paths, job->arcname, is_directory);
            if (rc != BP_OK) goto macos_classic_done;
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr, "boltpack: Classic Mac HFS file/directory path conflict\n");

macos_classic_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

static int ql_target_add_name(bp_posix_preflight_t *paths,
                              const char *utf8_name, int is_directory)
{
    char local[256], projected[256], folded[256];
    size_t length, component_scan_length, start, i;
    int rc;

    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_8859_1) != 0) {
        fprintf(stderr, "boltpack: Sinclair QL profile cannot encode name as Latin-1: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (length == 0 || length > 36U || local[0] == '/') {
        fprintf(stderr,
                "boltpack: Sinclair QL relative path is empty, absolute, or exceeds 36 bytes: %s\n",
                utf8_name);
        return BP_ERR_NAME_TOO_LONG;
    }
    component_scan_length = length;
    if (is_directory && local[length - 1U] == '/') component_scan_length--;
    for (start = 0, i = 0; i <= component_scan_length; i++) {
        if (i == component_scan_length || local[i] == '/') {
            size_t component_length = i - start;
            size_t j;
            if (component_length == 0 ||
                (component_length == 1U && local[start] == '.') ||
                (component_length == 2U && local[start] == '.' &&
                 local[start + 1U] == '.')) {
                fprintf(stderr,
                        "boltpack: Sinclair QL path has an empty or dot component: %s\n",
                        utf8_name);
                return BP_ERR_PARTIAL;
            }
            for (j = start; j < i; j++) {
                unsigned char c = (unsigned char)local[j];
                if (c < 0x20U || c == 0x7FU || c == ':' || c == '\\') {
                    fprintf(stderr,
                            "boltpack: Sinclair QL component contains a control, colon, or backslash: %s\n",
                            utf8_name);
                    return BP_ERR_PARTIAL;
                }
            }
            start = i + 1U;
        }
    }
    for (i = 0; i < length; i++)
        projected[i] = local[i] == '/' ? '_' : local[i];
    projected[length] = '\0';
    amiga_latin1_casefold(projected, folded, sizeof(folded));
    rc = bp_posix_preflight_add(paths, folded, strlen(folded), is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: Sinclair QL projected path collision: %s\n",
                utf8_name);
    return rc;
}

static int ql_target_preflight(const add_job_list_t *jobs,
                               const bp_archive_t *existing)
{
    bp_posix_preflight_t paths;
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory;
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Sinclair QL profile cannot preserve this entry type or metadata: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto ql_target_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: Sinclair QL file exceeds signed 32-bit I/O limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto ql_target_done;
            }
            is_directory = entry->entry_type == BP_TYPE_DIR;
            rc = ql_target_add_name(&paths, entry->name, is_directory);
            if (rc != BP_OK) goto ql_target_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory;
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Sinclair QL profile does not support source links or special entries: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto ql_target_done;
            }
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                rc = BP_ERR_OVERFLOW;
                goto ql_target_done;
            }
            is_directory = job->entry_type == BP_TYPE_DIR;
            rc = ql_target_add_name(&paths, job->arcname, is_directory);
            if (rc != BP_OK) goto ql_target_done;
        }
    }
    rc = bp_posix_preflight_finish(&paths);

ql_target_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

static int riscos_target_add_name(bp_posix_preflight_t *paths,
                                 const char *utf8_name, int is_directory)
{
    char local[256], folded[256];
    size_t length, scan_length, start, i;
    int rc;

    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_8859_1) != 0) {
        fprintf(stderr,
                "boltpack: RISC OS profile cannot encode name as Latin-1: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (length == 0 || length > 255U || local[0] == '/') {
        fprintf(stderr,
                "boltpack: RISC OS relative path is empty, absolute, or exceeds 255 bytes: %s\n",
                utf8_name);
        return BP_ERR_NAME_TOO_LONG;
    }
    scan_length = length;
    if (is_directory && local[length - 1U] == '/') scan_length--;
    for (start = 0, i = 0; i <= scan_length; i++) {
        if (i == scan_length || local[i] == '/') {
            size_t component_length = i - start;
            size_t j;
            if (component_length == 0 || component_length > 10U ||
                (component_length == 1U && local[start] == '.') ||
                (component_length == 2U && local[start] == '.' &&
                 local[start + 1U] == '.')) {
                fprintf(stderr,
                        "boltpack: RISC OS component is empty, dot syntax, or exceeds 10 bytes: %s\n",
                        utf8_name);
                return BP_ERR_NAME_TOO_LONG;
            }
            for (j = start; j < i; j++) {
                unsigned char c = (unsigned char)local[j];
                if (c < 0x20U || c == 0x7FU || c == '.' || c == ':' ||
                    c == '*' || c == '#' || c == '$' || c == '%' ||
                    c == '&' || c == '@' || c == '"' || c == '|' ||
                    c == '^' || c == '\\') {
                    fprintf(stderr,
                            "boltpack: RISC OS component contains reserved syntax: %s\n",
                            utf8_name);
                    return BP_ERR_PARTIAL;
                }
            }
            start = i + 1U;
        }
    }
    amiga_latin1_casefold(local, folded, sizeof(folded));
    rc = bp_posix_preflight_add(paths, folded, strlen(folded), is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: RISC OS projected path collision or file/directory conflict: %s\n",
                utf8_name);
    return rc;
}

static int riscos_target_preflight(const add_job_list_t *jobs,
                                   const bp_archive_t *existing)
{
    bp_posix_preflight_t paths;
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory;
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: RISC OS profile cannot preserve this entry type or metadata: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto riscos_target_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: RISC OS file exceeds the signed 32-bit I/O limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto riscos_target_done;
            }
            is_directory = entry->entry_type == BP_TYPE_DIR;
            rc = riscos_target_add_name(&paths, entry->name, is_directory);
            if (rc != BP_OK) goto riscos_target_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory;
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: RISC OS profile does not support links or special entries: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto riscos_target_done;
            }
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: source exceeds the RISC OS signed 32-bit I/O limit: %s\n",
                        job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto riscos_target_done;
            }
            is_directory = job->entry_type == BP_TYPE_DIR;
            rc = riscos_target_add_name(&paths, job->arcname, is_directory);
            if (rc != BP_OK) goto riscos_target_done;
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr, "boltpack: RISC OS file/directory path conflict\n");

riscos_target_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

static int x68000_target_preflight(const add_job_list_t *jobs,
                                   const bp_archive_t *existing)
{
    x68k_path_index_t paths;
    int rc = x68k_path_index_init(&paths);
    int i;
    if (rc != BP_OK) return rc;

    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: X68000 profile cannot preserve this entry type or metadata: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto x68000_target_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: X68000 file exceeds the signed 32-bit I/O limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto x68000_target_done;
            }
            rc = x68000_target_add_name(&paths, entry->name,
                                         entry->entry_type == BP_TYPE_DIR);
            if (rc != BP_OK) goto x68000_target_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: X68000 profile does not support links or special entries: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto x68000_target_done;
            }
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: source exceeds the X68000 signed 32-bit I/O limit: %s\n",
                        job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto x68000_target_done;
            }
            rc = x68000_target_add_name(&paths, job->arcname,
                                         job->entry_type == BP_TYPE_DIR);
            if (rc != BP_OK) goto x68000_target_done;
        }
    }

x68000_target_done:
    x68k_path_index_free(&paths);
    return rc;
}

#define BP_CPM86_TARGET_MAX_ENTRIES 256UL
#define BP_CPM86_TARGET_MAX_FILE_SIZE 2147483647UL
#define BP_CPM86_TARGET_MAX_ARCHIVE_SIZE 2147483647UL

/* CP/M-86 has a flat, uppercase ASCII 8.3 namespace. Its current frontend
 * keeps a 256-name extraction collision table and uses signed 32-bit file
 * sizes, so this writer profile enforces those reader limits rather than the
 * larger generic mini-core ceilings. */
static int cpm86_target_preflight(add_job_list_t *jobs,
                                  const bp_archive_t *existing,
                                  bp_uint8 method, bp_uint32 block_size)
{
    bp_posix_preflight_t names;
    unsigned long existing_count = 0;
    int rc = BP_OK;
    int i;

    memset(&names, 0, sizeof(names));
    if ((method != BP_METHOD_STORED && method != BP_METHOD_BOLTLITE) ||
        block_size > BP_BLOCK_32K) {
        fprintf(stderr,
                "boltpack: CP/M-86 supports Stored or BoltLite with blocks no larger than 32 KiB\n");
        rc = BP_ERR_UNSUPPORTED_METHOD;
        goto cpm86_target_done;
    }
    if (existing != NULL) {
        existing_count = (unsigned long)existing->entries_written;
        if (existing_count > BP_CPM86_TARGET_MAX_ENTRIES) {
            fprintf(stderr,
                    "boltpack: existing archive exceeds the CP/M-86 256-entry extraction limit\n");
            rc = BP_ERR_OVERFLOW;
            goto cpm86_target_done;
        }
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            char projected[BP_CPM86_NAME_SIZE];

            if (entry->entry_type != BP_TYPE_DATA) {
                fprintf(stderr,
                        "boltpack: CP/M-86 supports regular files only: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto cpm86_target_done;
            }
            if (entry->flags != 0 || !bp_u64_is_zero(entry->mtime)) {
                fprintf(stderr,
                        "boltpack: existing archive carries attributes that CP/M-86 cannot restore: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto cpm86_target_done;
            }
            if (entry->method != BP_METHOD_STORED &&
                entry->method != BP_METHOD_BOLTLITE) {
                fprintf(stderr,
                        "boltpack: existing entry uses a codec unsupported by CP/M-86: %s\n",
                        entry->name);
                rc = BP_ERR_UNSUPPORTED_METHOD;
                goto cpm86_target_done;
            }
            if (entry->block_size > BP_BLOCK_32K ||
                (entry->block_size == 0 &&
                 entry->original_size.lo > BP_BLOCK_32K) ||
                entry->original_size.hi != 0 ||
                entry->original_size.lo > BP_CPM86_TARGET_MAX_FILE_SIZE) {
                fprintf(stderr,
                        "boltpack: existing entry exceeds CP/M-86 size or block limits: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto cpm86_target_done;
            }
            if (!bp_cpm86_project_name(entry->name, projected) ||
                strcmp(entry->name, projected) != 0) {
                fprintf(stderr,
                        "boltpack: existing entry is not uppercase CP/M-86 8.3: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto cpm86_target_done;
            }
            rc = bp_posix_preflight_add(&names, projected,
                                        strlen(projected), 0);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing CP/M-86 name collision: %s\n",
                        entry->name);
                goto cpm86_target_done;
            }
        }
    }
    if (jobs != NULL &&
        (unsigned long)jobs->count >
            BP_CPM86_TARGET_MAX_ENTRIES - existing_count) {
        fprintf(stderr,
                "boltpack: CP/M-86 profile exceeds the 256-entry extraction limit\n");
        rc = BP_ERR_OVERFLOW;
        goto cpm86_target_done;
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            add_job_t *job = &jobs->items[i];
            char projected[BP_CPM86_NAME_SIZE];
            char *projected_copy;

            if (job->entry_type != BP_TYPE_DATA) {
                fprintf(stderr,
                        "boltpack: CP/M-86 does not support directories or symbolic links: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto cpm86_target_done;
            }
            if (job->file_size.hi != 0 ||
                job->file_size.lo > BP_CPM86_TARGET_MAX_FILE_SIZE) {
                fprintf(stderr,
                        "boltpack: source exceeds the CP/M-86 signed 32-bit file-size limit: %s\n",
                        job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto cpm86_target_done;
            }
            if (strchr(job->arcname, '/') != NULL ||
                strchr(job->arcname, '\\') != NULL ||
                strchr(job->arcname, ':') != NULL ||
                !bp_cpm86_project_name(job->arcname, projected)) {
                fprintf(stderr,
                        "boltpack: source name is not a flat CP/M-86 8.3 name: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto cpm86_target_done;
            }
            rc = bp_posix_preflight_add(&names, projected,
                                        strlen(projected), 0);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: CP/M-86 projected name collision: %s\n",
                        job->original_arcname);
                goto cpm86_target_done;
            }
            projected_copy = dup_string(projected);
            if (projected_copy == NULL) {
                rc = BP_ERR_NOMEM;
                goto cpm86_target_done;
            }
            free(job->arcname);
            job->arcname = projected_copy;
            /* CP/M-86 has no timestamp or POSIX mode restoration. Do not
             * serialize source-host attributes that its reader cannot apply. */
            job->mtime = bp_u64_from_u32(0);
            job->flags = 0;
        }
    }
    rc = bp_posix_preflight_finish(&names);

cpm86_target_done:
    bp_posix_preflight_free(&names);
    return rc;
}

static int atari_target_preflight(const add_job_list_t *jobs,
                                  const bp_archive_t *existing,
                                  int include_name_report)
{
    bp_atari_name_set_t projected_names;
    bp_posix_preflight_t prefixes;
    int rc = BP_OK;
    int i;

    memset(&projected_names, 0, sizeof(projected_names));
    memset(&prefixes, 0, sizeof(prefixes));

    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory;

            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform == BP_META_PORTABLE ||
                    entry->platform == BP_OS_ATARI)
                    continue;
                fprintf(stderr,
                        "boltpack: existing archive has metadata not supported by the Atari target profile: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto atari_preflight_done;
            }
            if (entry->entry_type == BP_TYPE_RSRC ||
                entry->entry_type == BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: existing archive contains an entry type unsupported by the Atari target profile: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto atari_preflight_done;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR)
                continue;
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: existing entry exceeds the Atari signed-I/O size limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto atari_preflight_done;
            }
            is_directory = entry->entry_type == BP_TYPE_DIR;
            rc = atari_target_add_name(&projected_names, &prefixes,
                                       entry->name, is_directory);
            if (rc != BP_OK) goto atari_preflight_done;
        }
    }

    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory;

            if (job->entry_type == BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: Atari target profile does not support symbolic links: %s\n",
                        job->arcname);
                rc = BP_ERR_PARTIAL;
                goto atari_preflight_done;
            }
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR)
                continue;
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: source exceeds the Atari signed-I/O size limit: %s\n",
                        job->arcname);
                rc = BP_ERR_OVERFLOW;
                goto atari_preflight_done;
            }
            is_directory = job->entry_type == BP_TYPE_DIR;
            rc = atari_target_add_name(&projected_names, &prefixes,
                                       job->arcname, is_directory);
            if (rc != BP_OK) goto atari_preflight_done;
        }
    }

    if (include_name_report) {
        rc = atari_target_add_name(&projected_names, &prefixes,
                                   "BPNAMES.TXT", 0);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: Atari name report path BPNAMES.TXT conflicts with an input name\n");
            goto atari_preflight_done;
        }
    }

    rc = bp_posix_preflight_finish(&prefixes);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: Atari target profile has a file/directory prefix conflict\n");

atari_preflight_done:
    bp_atari_name_set_free(&projected_names);
    bp_posix_preflight_free(&prefixes);
    return rc;
}

#endif /* BP_TARGET_LEGACY_PREFLIGHT_H */
