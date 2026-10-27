/* Checked conversion of a format-6 UTC timestamp to the host time_t. */
#ifndef BP_TIME_H
#define BP_TIME_H

#include "bp_types.h"
#include <time.h>
#include <sys/stat.h>
#include <utime.h>

static int bp_mtime_to_time_t(bp_uint64 value, time_t *result)
{
    bp_off_t seconds;
    time_t t;
    if (result == NULL) return BP_ERR_INVALID_ARG;
    seconds = bp_u64_to_off(value);
    if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), value) != 0)
        return BP_ERR_OVERFLOW;
    t = (time_t)seconds;
    if ((bp_off_t)t != seconds) return BP_ERR_OVERFLOW;
    *result = t;
    return BP_OK;
}

/* Preserve the destination's access time when only mtime was archived. */
static int bp_restore_mtime(const char *path, bp_uint64 value)
{
    struct stat st;
    struct utimbuf ut;
    time_t t;
    int rc = bp_mtime_to_time_t(value, &t);
    if (rc != BP_OK) return rc;
    if (stat(path, &st) != 0) return BP_ERR_IO;
    ut.actime = st.st_atime;
    ut.modtime = t;
    return utime(path, &ut) == 0 ? BP_OK : BP_ERR_IO;
}
#endif
