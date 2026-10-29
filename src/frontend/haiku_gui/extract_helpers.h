/* Bounded path preflight and same-directory staging for the Haiku GUI. */
#ifndef BP_HAIKU_GUI_EXTRACT_HELPERS_H
#define BP_HAIKU_GUI_EXTRACT_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif
#include "../../core/archiver.h"
#ifdef __cplusplus
}
#endif
#include "../posix/archive_preflight.h"
#include "../posix/extract_paths.h"
#include "../posix/restore_mode.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

typedef struct {
    int parent_fd;
    char leaf[BP_POSIX_PREFLIGHT_MAX_COMPONENT + 1U];
    char temporary[64];
    FILE *stream;
} bp_haiku_extract_output_t;

static int bp_haiku_preflight_add(bp_posix_preflight_t *preflight,
                                  const bp_entry_t *entry)
{
    char folded[BP_POSIX_PREFLIGHT_MAX_PATH];
    size_t i, length;
    if (entry == NULL || entry->name == NULL ||
        !bp_name_is_safe(entry->name)) return BP_ERR_PARTIAL;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR)
        return BP_OK;
    length = entry->name_len;
    if (length == 0 || length >= sizeof(folded)) return BP_ERR_NAME_TOO_LONG;
    memcpy(folded, entry->name, length);
    folded[length] = '\0';
    for (i = 0; i < length; i++)
        if (folded[i] >= 'A' && folded[i] <= 'Z')
            folded[i] = (char)(folded[i] + ('a' - 'A'));
    return bp_posix_preflight_add(preflight, folded, length,
                                  entry->entry_type == BP_TYPE_DIR);
}

static int bp_haiku_extract_output_open(int root_fd, const char *name,
                                        bp_haiku_extract_output_t *output)
{
    static unsigned long counter = 0;
    int tries, fd;
    memset(output, 0, sizeof(*output));
    output->parent_fd = extract_open_parent(root_fd, name, output->leaf,
                                            sizeof(output->leaf));
    if (output->parent_fd < 0) return BP_ERR_IO;
    for (tries = 0; tries < 256; tries++) {
        snprintf(output->temporary, sizeof(output->temporary),
                 ".boltpack-gui-%08lx-%08lx",
                 (unsigned long)getpid(), ++counter);
        fd = openat(output->parent_fd, output->temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    0666);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        close(output->parent_fd);
        output->parent_fd = -1;
        return BP_ERR_IO;
    }
    output->stream = fdopen(fd, "wb");
    if (output->stream == NULL) {
        close(fd);
        unlinkat(output->parent_fd, output->temporary, 0);
        close(output->parent_fd);
        output->parent_fd = -1;
        return BP_ERR_IO;
    }
    return BP_OK;
}

/* Publish only after the caller has verified the stream and applied metadata. */
static int bp_haiku_extract_output_finish(bp_haiku_extract_output_t *output,
                                          int publish)
{
    int rc = BP_OK;
    if (output->stream != NULL) {
        if (publish && fflush(output->stream) != 0) rc = BP_ERR_IO;
        if (fclose(output->stream) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        output->stream = NULL;
    }
    if (publish && rc == BP_OK) {
        if (renameat(output->parent_fd, output->temporary,
                     output->parent_fd, output->leaf) != 0)
            rc = BP_ERR_IO;
    }
    if (rc != BP_OK || !publish)
        if (output->temporary[0] != '\0' && output->parent_fd >= 0)
            (void)unlinkat(output->parent_fd, output->temporary, 0);
    if (output->parent_fd >= 0) {
        close(output->parent_fd);
        output->parent_fd = -1;
    }
    return rc;
}

/* The core calls this only after the stream checksum has been evaluated. */
static int bp_haiku_extract_output_complete(bp_haiku_extract_output_t *output,
                                            int status, bp_uint16 flags,
                                            bp_uint64 mtime)
{
    int partial = 0;
    int rc;
    FILE *stream = output->stream;
    if (status != BP_OK) {
        (void)bp_haiku_extract_output_finish(output, 0);
        return BP_OK;
    }
    if (fflush(stream) != 0) {
        (void)bp_haiku_extract_output_finish(output, 0);
        return BP_ERR_IO;
    }
    if (bp_posix_restore_mode_fd(fileno(stream), flags) != BP_OK)
        partial = 1;
    if (flags & BP_ENTRY_FLAG_MTIME_VALID) {
        bp_off_t seconds = bp_u64_to_off(mtime);
        time_t timestamp = (time_t)seconds;
        struct timespec times[2];
        if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
            (bp_off_t)timestamp != seconds) {
            partial = 1;
        } else {
            times[0].tv_sec = 0;
            times[0].tv_nsec = UTIME_OMIT;
            times[1].tv_sec = timestamp;
            times[1].tv_nsec = 0;
            if (futimens(fileno(stream), times) != 0) partial = 1;
        }
    }
    rc = bp_haiku_extract_output_finish(output, 1);
    if (rc != BP_OK) return rc;
    return partial ? BP_ERR_PARTIAL : BP_OK;
}

static int bp_haiku_extract_root_open(const char *path)
{
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

#endif
