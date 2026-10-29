#ifndef BOLTPACK_POSIX_RESTORE_MODE_H
#define BOLTPACK_POSIX_RESTORE_MODE_H

#include <sys/stat.h>

#include "bp_types.h"

/* POSIX extraction restores ordinary permissions and sticky, but never
   restores set-user-ID or set-group-ID bits from an untrusted archive. */
static int bp_posix_restore_mode_fd(int fd, bp_uint16 flags)
{
    mode_t mode;
    int partial = 0;
    if ((flags & BP_ENTRY_FLAG_MODE_VALID) == 0) return BP_OK;
    mode = (mode_t)(flags & BP_ENTRY_FLAG_MODE_MASK);
    if ((mode & (mode_t)06000) != 0) {
        mode &= (mode_t)~06000;
        partial = 1;
    }
    if (fchmod(fd, mode) != 0) return BP_ERR_IO;
    return partial ? BP_ERR_PARTIAL : BP_OK;
}

static int bp_posix_restore_mode_path(const char *path, bp_uint16 flags)
{
    mode_t mode;
    int partial = 0;
    if ((flags & BP_ENTRY_FLAG_MODE_VALID) == 0) return BP_OK;
    mode = (mode_t)(flags & BP_ENTRY_FLAG_MODE_MASK);
    if ((mode & (mode_t)06000) != 0) {
        mode &= (mode_t)~06000;
        partial = 1;
    }
    if (chmod(path, mode) != 0) return BP_ERR_IO;
    return partial ? BP_ERR_PARTIAL : BP_OK;
}

#endif
