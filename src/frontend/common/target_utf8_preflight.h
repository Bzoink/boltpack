/* Shared preflight for UTF-8 target profiles. */
#ifndef BP_TARGET_UTF8_PREFLIGHT_H
#define BP_TARGET_UTF8_PREFLIGHT_H

static int posix_target_validate_existing_links(bp_archive_t *archive,
                                                bp_uint32 max_target);

#define BP_LINUX_MINI_TARGET_MAX_ENTRIES 65535UL
#define BP_LINUX_MINI_TARGET_MAX_ARCHIVE_SIZE 2147483647UL

/* Linux full and Linux mini share UTF-8 POSIX name projection. The mini
 * reader has narrower codec, entry-type, count, per-file and archive-size
 * capabilities, all checked here for the full Linux writer. */
static int linux_target_preflight(const add_job_list_t *jobs,
                                  bp_archive_t *existing,
                                  int mini_profile, bp_uint8 method,
                                  bp_uint32 block_size)
{
    bp_posix_preflight_t paths;
    unsigned long existing_count = 0;
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (mini_profile &&
        (method > BP_METHOD_BOLTLZ || block_size > BP_BLOCK_32K)) {
        fprintf(stderr,
                "boltpack: Linux mini profile supports Stored, BoltLite, or BoltLZ with blocks no larger than 32 KiB\n");
        rc = BP_ERR_UNSUPPORTED_METHOD;
        goto linux_target_done;
    }
    if (existing != NULL) {
        existing_count = (unsigned long)existing->entries_written;
        if (mini_profile && existing_count > BP_LINUX_MINI_TARGET_MAX_ENTRIES) {
            fprintf(stderr,
                    "boltpack: existing archive exceeds the Linux mini 65,535-entry limit\n");
            rc = BP_ERR_OVERFLOW;
            goto linux_target_done;
        }
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory = entry->entry_type == BP_TYPE_DIR;
            size_t name_len = entry->name_len;

            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR &&
                (!(!mini_profile && entry->entry_type == BP_TYPE_SYMLINK))) {
                fprintf(stderr,
                        "boltpack: existing archive has an entry type unsupported by Linux%s: %s\n",
                        mini_profile ? " mini" : " POSIX", entry->name);
                rc = BP_ERR_PARTIAL;
                goto linux_target_done;
            }
            if (mini_profile &&
                (entry->method > BP_METHOD_BOLTLZ ||
                 entry->block_size > BP_BLOCK_32K ||
                 (entry->block_size == 0 &&
                  (entry->original_size.hi != 0 ||
                   entry->original_size.lo > BP_BLOCK_32K)))) {
                fprintf(stderr,
                        "boltpack: existing archive uses a codec or block size unsupported by Linux mini: %s\n",
                        entry->name);
                rc = BP_ERR_UNSUPPORTED_METHOD;
                goto linux_target_done;
            }
            if (mini_profile && entry->entry_type == BP_TYPE_DATA &&
                entry->original_size.hi != 0) {
                fprintf(stderr,
                        "boltpack: existing file exceeds the Linux mini 4 GiB limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto linux_target_done;
            }
            if (name_len == 0 ||
                !bp_archive_name_valid(entry->name, (bp_uint32)name_len) ||
                !bp_archive_name_type_valid(entry->name, (bp_uint32)name_len,
                                            entry->entry_type)) {
                fprintf(stderr,
                        "boltpack: existing archive has a noncanonical Linux UTF-8 path: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto linux_target_done;
            }
            rc = bp_posix_preflight_add(&paths, entry->name, name_len,
                                        is_directory);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing Linux path is too long or collides: %s\n",
                        entry->name);
                goto linux_target_done;
            }
        }
    }
    if (mini_profile && jobs != NULL &&
        (unsigned long)jobs->count >
            BP_LINUX_MINI_TARGET_MAX_ENTRIES - existing_count) {
        fprintf(stderr,
                "boltpack: Linux mini profile exceeds the 65,535-entry limit\n");
        rc = BP_ERR_OVERFLOW;
        goto linux_target_done;
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            int is_directory = job->entry_type == BP_TYPE_DIR;
            size_t name_len = strlen(job->arcname);

            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR &&
                (!(!mini_profile && job->entry_type == BP_TYPE_SYMLINK))) {
                fprintf(stderr,
                        "boltpack: source entry type is unsupported by Linux%s: %s\n",
                        mini_profile ? " mini" : " POSIX",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto linux_target_done;
            }
            if (mini_profile && job->entry_type == BP_TYPE_DATA &&
                job->file_size.hi != 0) {
                fprintf(stderr,
                        "boltpack: source exceeds the Linux mini 4 GiB limit: %s\n",
                        job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto linux_target_done;
            }
            if (name_len == 0 || name_len > 0xFFFFFFFFUL ||
                !bp_archive_name_valid(job->arcname, (bp_uint32)name_len) ||
                !bp_archive_name_type_valid(job->arcname,
                                            (bp_uint32)name_len,
                                            job->entry_type)) {
                fprintf(stderr,
                        "boltpack: source path is not canonical UTF-8 for Linux: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto linux_target_done;
            }
            rc = bp_posix_preflight_add(&paths, job->arcname, name_len,
                                        is_directory);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: Linux path is too long or collides: %s\n",
                        job->original_arcname);
                goto linux_target_done;
            }
            if (job->entry_type == BP_TYPE_SYMLINK &&
                (job->symlink_target == NULL ||
                 strlen(job->symlink_target) >=
                     BP_POSIX_PREFLIGHT_MAX_PATH ||
                 !bp_symlink_target_is_safe(job->arcname,
                                            job->symlink_target))) {
                fprintf(stderr,
                        "boltpack: POSIX target profile refuses an unsafe or overlong symlink target: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto linux_target_done;
            }
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr, "boltpack: Linux profile has a file/directory path conflict\n");
    if (rc == BP_OK && existing != NULL && !mini_profile) {
        rc = posix_target_validate_existing_links(
            existing, BP_POSIX_PREFLIGHT_MAX_PATH - 1U);
        if (rc != BP_OK)
            fprintf(stderr,
                    "boltpack: existing POSIX archive has an invalid symlink or entry payload\n");
    }

linux_target_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

#define BP_PLAN9_TARGET_MAX_PATH 511U
#define BP_PLAN9_TARGET_MAX_ENTRIES 65535UL

/* 9P names are UTF-8, slash-separated, and case-sensitive. Plan 9's 9P
 * documentation excludes control characters; this conservative projection
 * also rejects DEL and C1 controls. The Plan 9 frontend's path buffers cap
 * the complete relative archive path at 511 bytes. */
static int plan9_target_name_valid(const char *name, size_t length,
                                   bp_uint8 entry_type)
{
    size_t i = 0;
    size_t component = 0;
    size_t path_length = length;
    int is_directory = entry_type == BP_TYPE_DIR;

    if (length == 0 || length > BP_PLAN9_TARGET_MAX_PATH ||
        !bp_archive_name_valid(name, (bp_uint32)length) ||
        !bp_archive_name_type_valid(name, (bp_uint32)length, entry_type))
        return 0;
    if (is_directory && name[length - 1U] == '/') path_length--;
    while (i < path_length) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/') {
            if (component == 0) return 0;
            component = 0;
            i++;
            continue;
        }
        if (c < 0x20U || c == 0x7fU) return 0;
        if (c == 0xc2U && i + 1U < path_length &&
            (unsigned char)name[i + 1U] >= 0x80U &&
            (unsigned char)name[i + 1U] <= 0x9fU)
            return 0;
        component++;
        if (component > 255U) return 0;
        i++;
    }
    return component != 0;
}

static int plan9_target_budget(const char *name, size_t name_len,
                               int is_directory,
                               unsigned long *path_nodes,
                               unsigned long *path_bytes)
{
    size_t effective_len = name_len;
    size_t i;
    unsigned long components = 1UL;
    unsigned long bytes = (unsigned long)name_len + 1UL;
    if (is_directory && effective_len > 0) effective_len--;
    for (i = 0; i < effective_len; i++)
        if (name[i] == '/') components++;
    if (*path_nodes > BP_PLAN9_TARGET_MAX_ENTRIES - components ||
        bytes > 16UL * 1024UL * 1024UL - *path_bytes)
        return 0;
    *path_nodes += components;
    *path_bytes += bytes;
    return 1;
}

static int plan9_target_preflight(const add_job_list_t *jobs,
                                  bp_archive_t *existing,
                                  bp_uint8 method, bp_uint32 block_size)
{
    bp_posix_preflight_t paths;
    unsigned long path_entries = 0;
    unsigned long path_nodes = 0;
    unsigned long path_bytes = 0;
    int rc = BP_OK;
    int i;

    memset(&paths, 0, sizeof(paths));
    if (method > BP_METHOD_FLUFF || block_size > BP_BLOCK_256K) {
        fprintf(stderr,
                "boltpack: Plan 9 profile supports Stored, BoltLite, BoltLZ, or Fluff with blocks no larger than 256 KiB\n");
        return BP_ERR_UNSUPPORTED_METHOD;
    }
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory = entry->entry_type == BP_TYPE_DIR;
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform != BP_OS_PLAN9 ||
                    entry->original_size.hi != 0 ||
                    entry->original_size.lo != 2U || i == 0 ||
                    (existing->entry_list[i - 1].entry_type != BP_TYPE_DATA &&
                     existing->entry_list[i - 1].entry_type != BP_TYPE_DIR) ||
                    strcmp(existing->entry_list[i - 1].name,
                           entry->name) != 0) {
                    fprintf(stderr,
                            "boltpack: existing archive has metadata Plan 9 cannot restore: %s\n",
                            entry->name);
                    rc = BP_ERR_PARTIAL;
                    goto plan9_target_done;
                }
                if (entry->method > BP_METHOD_FLUFF ||
                    entry->block_size > BP_BLOCK_256K) {
                    fprintf(stderr,
                            "boltpack: existing Plan 9 metadata uses an unsupported codec: %s\n",
                            entry->name);
                    rc = BP_ERR_UNSUPPORTED_METHOD;
                    goto plan9_target_done;
                }
                continue;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: existing archive has an entry type unsupported by Plan 9: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto plan9_target_done;
            }
            if (i + 1 >= (int)existing->entries_written ||
                existing->entry_list[i + 1].entry_type != BP_TYPE_META ||
                existing->entry_list[i + 1].platform != BP_OS_PLAN9 ||
                existing->entry_list[i + 1].original_size.hi != 0 ||
                existing->entry_list[i + 1].original_size.lo != 2U ||
                strcmp(existing->entry_list[i + 1].name, entry->name) != 0) {
                fprintf(stderr,
                        "boltpack: existing Plan 9 profile entry lacks its native mode metadata: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto plan9_target_done;
            }
            if (entry->method > BP_METHOD_FLUFF ||
                entry->block_size > BP_BLOCK_256K) {
                fprintf(stderr,
                        "boltpack: existing archive uses a codec or block size unsupported by Plan 9: %s\n",
                        entry->name);
                rc = BP_ERR_UNSUPPORTED_METHOD;
                goto plan9_target_done;
            }
            if ((entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0 &&
                entry->mtime.hi != 0) {
                fprintf(stderr,
                        "boltpack: existing Plan 9 entry has a timestamp outside the target range: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto plan9_target_done;
            }
            if (!plan9_target_name_valid(entry->name, entry->name_len,
                                         entry->entry_type)) {
                fprintf(stderr,
                        "boltpack: existing archive has a path Plan 9 cannot represent: %s\n",
                        entry->name);
                rc = BP_ERR_NAME_TOO_LONG;
                goto plan9_target_done;
            }
            if (++path_entries > BP_PLAN9_TARGET_MAX_ENTRIES) {
                rc = BP_ERR_OVERFLOW;
                goto plan9_target_done;
            }
            if (!plan9_target_budget(entry->name, entry->name_len,
                                     is_directory, &path_nodes, &path_bytes)) {
                rc = BP_ERR_OVERFLOW;
                goto plan9_target_done;
            }
            rc = bp_posix_preflight_add(&paths, entry->name,
                                        entry->name_len, is_directory);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing Plan 9 path is duplicated or conflicts: %s\n",
                        entry->name);
                goto plan9_target_done;
            }
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            size_t name_len = strlen(job->arcname);
            int is_directory = job->entry_type == BP_TYPE_DIR;
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: source entry type is unsupported by Plan 9: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto plan9_target_done;
            }
            if (!plan9_target_name_valid(job->arcname, name_len,
                                         job->entry_type)) {
                fprintf(stderr,
                        "boltpack: source path is not representable by Plan 9 (UTF-8 controls, component, or path limit): %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto plan9_target_done;
            }
            if ((job->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0 &&
                job->mtime.hi != 0) {
                fprintf(stderr,
                        "boltpack: source timestamp is outside the Plan 9 range: %s\n",
                        job->original_arcname);
                rc = BP_ERR_OVERFLOW;
                goto plan9_target_done;
            }
            if ((job->flags & BP_ENTRY_FLAG_MODE_VALID) != 0 &&
                (job->flags & BP_ENTRY_FLAG_MODE_MASK & ~0x01ffU) != 0) {
                fprintf(stderr,
                        "boltpack: Plan 9 cannot preserve special POSIX mode bits: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto plan9_target_done;
            }
            if (++path_entries > BP_PLAN9_TARGET_MAX_ENTRIES) {
                fprintf(stderr,
                        "boltpack: Plan 9 profile exceeds the 65,535-entry path limit\n");
                rc = BP_ERR_OVERFLOW;
                goto plan9_target_done;
            }
            if (!plan9_target_budget(job->arcname, name_len, is_directory,
                                     &path_nodes, &path_bytes)) {
                fprintf(stderr,
                        "boltpack: Plan 9 profile exceeds the path-index memory limit\n");
                rc = BP_ERR_OVERFLOW;
                goto plan9_target_done;
            }
            rc = bp_posix_preflight_add(&paths, job->arcname, name_len,
                                        is_directory);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: Plan 9 path is duplicated or conflicts: %s\n",
                        job->original_arcname);
                goto plan9_target_done;
            }
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: Plan 9 profile has a file/directory path conflict\n");

plan9_target_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

#define BP_HAIKU_BFS_MAX_PATH 1023U

typedef struct {
    bp_io_t io;
    char target[BP_POSIX_PREFLIGHT_MAX_PATH];
    size_t length;
} haiku_link_sink_t;

typedef struct {
    haiku_link_sink_t link;
    int error;
    bp_uint32 max_target;
} haiku_link_scan_t;

static long haiku_link_sink_write(void *handle, const void *buffer,
                                  bp_uint32 count)
{
    haiku_link_sink_t *sink = (haiku_link_sink_t *)handle;
    if ((size_t)count > sizeof(sink->target) - 1U - sink->length) return -1;
    memcpy(sink->target + sink->length, buffer, (size_t)count);
    sink->length += (size_t)count;
    return (long)count;
}

static bp_io_t *haiku_link_scan_open(const bp_entry_t *entry, void *user_data)
{
    haiku_link_scan_t *scan = (haiku_link_scan_t *)user_data;
    if (entry->entry_type != BP_TYPE_SYMLINK)
        return bp_archive_dry_run_stream();
    if (entry->original_size.hi != 0 ||
        entry->original_size.lo > scan->max_target) {
        scan->error = BP_ERR_NAME_TOO_LONG;
        return bp_archive_dry_run_stream();
    }
    memset(&scan->link, 0, sizeof(scan->link));
    scan->link.io.write = haiku_link_sink_write;
    scan->link.io.handle = &scan->link;
    return &scan->link.io;
}

static int haiku_link_scan_done(const bp_entry_t *entry, bp_io_t *output,
                                int status, void *user_data)
{
    haiku_link_scan_t *scan = (haiku_link_scan_t *)user_data;
    haiku_link_sink_t *sink = (haiku_link_sink_t *)output->handle;
    if (status != BP_OK) return status;
    if (sink->length != entry->original_size.lo ||
        memchr(sink->target, '\0', sink->length) != NULL) {
        scan->error = BP_ERR_PARTIAL;
        return BP_ERR_PARTIAL;
    }
    sink->target[sink->length] = '\0';
    if (!bp_symlink_target_is_safe(entry->name, sink->target)) {
        scan->error = BP_ERR_PARTIAL;
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

/* Existing symlink payloads are part of the target profile too. Decode and
 * checksum the staged archive only when it contains links, validating their
 * targets before append, then restore the append cursor. */
static int posix_target_validate_existing_links(bp_archive_t *archive,
                                                bp_uint32 max_target)
{
    haiku_link_scan_t scan;
    bp_off_t position;
    bp_uint32 i;
    int has_link = 0, rc, restore_rc;
    for (i = 0; i < archive->entries_written; i++)
        if (archive->entry_list[i].entry_type == BP_TYPE_SYMLINK) {
            has_link = 1;
            break;
        }
    if (!has_link) return BP_OK;
    position = archive->io->tell(archive->io->handle);
    if (position < 0) return BP_ERR_IO;
    if (archive->io->seek(archive->io->handle, 0, SEEK_SET) != 0)
        return BP_ERR_IO;
    memset(&scan, 0, sizeof(scan));
    scan.max_target = max_target;
    rc = bp_archive_extract_all_ex(archive->io, haiku_link_scan_open,
                                   haiku_link_scan_done, NULL, &scan);
    restore_rc = archive->io->seek(archive->io->handle, position, SEEK_SET);
    if (restore_rc != 0) return BP_ERR_IO;
    if (scan.error != BP_OK) return scan.error;
    if (rc != BP_OK) return rc;
    return BP_OK;
}

/* Haiku BFS stores UTF-8 byte names and looks them up byte-for-byte (case
 * sensitive). The Haiku CLI accepts data, directories, symlinks, and its own
 * BEOS metadata records; resource forks and foreign metadata are skipped. */
static int haiku_target_add_name(bp_posix_preflight_t *paths,
                                 const char *name, size_t name_len,
                                 int is_directory)
{
    char utf8[BP_HAIKU_BFS_MAX_PATH + 1U];
    if (name_len == 0 || name_len > BP_HAIKU_BFS_MAX_PATH ||
        !bp_archive_name_valid(name, (bp_uint32)name_len) ||
        bp_utf8_to_local(name, utf8, (int)sizeof(utf8), BP_CP_UTF8) != 0) {
        fprintf(stderr,
                "boltpack: Haiku BFS requires a canonical UTF-8 path no longer than 1,023 bytes\n");
        return BP_ERR_PARTIAL;
    }
    return bp_posix_preflight_add(paths, name, name_len, is_directory);
}

static int haiku_target_preflight(const add_job_list_t *jobs,
                                  bp_archive_t *existing,
                                  bp_uint8 method)
{
    bp_posix_preflight_t paths;
    int rc = BP_OK;
    int i;
    memset(&paths, 0, sizeof(paths));

    /* Haiku's standard CLI build has no Fluff reader. Target profiles also
     * select Interchange Profile 1, so creation is further limited to
     * Stored/BoltLite and its 16 KiB blocks. */
    if (method > BP_METHOD_BOLTLITE) {
        fprintf(stderr,
                "boltpack: Haiku BFS profile supports only Stored or BoltLite\n");
        rc = BP_ERR_UNSUPPORTED_METHOD;
        goto haiku_target_done;
    }
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            int is_directory = entry->entry_type == BP_TYPE_DIR;
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform != BP_OS_BEOS) {
                    fprintf(stderr,
                            "boltpack: existing archive has metadata Haiku cannot restore: %s\n",
                            entry->name);
                    rc = BP_ERR_PARTIAL;
                    goto haiku_target_done;
                }
                continue;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR &&
                entry->entry_type != BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: existing archive has an entry type unsupported by Haiku BFS: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto haiku_target_done;
            }
            rc = haiku_target_add_name(&paths, entry->name,
                                       entry->name_len, is_directory);
            if (rc != BP_OK) goto haiku_target_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            size_t name_len = strlen(job->arcname);
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR &&
                job->entry_type != BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: source entry type is unsupported by Haiku BFS: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto haiku_target_done;
            }
            rc = haiku_target_add_name(&paths, job->arcname, name_len,
                                       job->entry_type == BP_TYPE_DIR);
            if (rc != BP_OK) {
                fprintf(stderr, "boltpack: invalid Haiku BFS name: %s\n",
                        job->original_arcname);
                goto haiku_target_done;
            }
            if (job->entry_type == BP_TYPE_SYMLINK &&
                !bp_symlink_target_is_safe(job->arcname,
                                           job->symlink_target)) {
                fprintf(stderr,
                        "boltpack: Haiku BFS profile refuses an unsafe symlink target: %s\n",
                        job->original_arcname);
                rc = BP_ERR_PARTIAL;
                goto haiku_target_done;
            }
        }
    }
    if (existing != NULL) {
        rc = posix_target_validate_existing_links(existing,
                                                   BP_HAIKU_BFS_MAX_PATH);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: existing Haiku BFS archive has an invalid symlink or entry payload\n");
            goto haiku_target_done;
        }
    }
    rc = bp_posix_preflight_finish(&paths);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: Haiku BFS profile found a duplicate or file/directory path conflict\n");

haiku_target_done:
    bp_posix_preflight_free(&paths);
    return rc;
}

#endif
