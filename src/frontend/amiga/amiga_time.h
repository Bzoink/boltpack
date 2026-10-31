/* Convert classic AmigaDOS local wall-clock seconds to/from format-6 UTC. */
#ifndef BP_AMIGA_TIME_H
#define BP_AMIGA_TIME_H

#include "../../core/bp_types.h"
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <utime.h>
#include "../../core/bp_time.h"

/* Parse an explicit timezone in minutes east of UTC. */
static int bp_amiga_parse_tz_offset(const char *text, long *offset_seconds)
{
    unsigned long magnitude = 0;
    int negative = 0;
    const char *p = text;

    if (offset_seconds == NULL || p == NULL || *p == '\0') return 0;
    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        ++p;
    }
    if (*p == '\0') return 0;
    while (*p != '\0') {
        unsigned int digit;
        if (*p < '0' || *p > '9') return 0;
        digit = (unsigned int)(*p - '0');
        if (magnitude > 143UL || magnitude * 10UL + digit > 1439UL)
            return 0;
        magnitude = magnitude * 10UL + digit;
        ++p;
    }
    *offset_seconds = (long)(magnitude * 60UL);
    if (negative) *offset_seconds = -*offset_seconds;
    return 1;
}

static int bp_amiga_get_tz_offset(long *offset_seconds)
{
    return bp_amiga_parse_tz_offset(getenv("BP_TZ_OFFSET"), offset_seconds);
}

/* AmigaDOS DateStamp has local wall-clock fields, while format 6 is UTC. */
static int bp_amiga_time_to_utc(time_t source, bp_uint64 *mtime)
{
    long offset_seconds;
    long local_seconds;
    long utc_seconds;

    if (mtime == NULL) return 0;
    *mtime = bp_u64_from_u32(0);
    if (!bp_amiga_get_tz_offset(&offset_seconds) ||
        offset_seconds < -86340L || offset_seconds > 86340L ||
        source < (time_t)0)
        return 0;
    local_seconds = (long)source;
    if ((time_t)local_seconds != source) return 0;
    if (offset_seconds > 0 && local_seconds < offset_seconds) return 0;
    if (offset_seconds < 0 &&
        local_seconds > LONG_MAX + offset_seconds) return 0;
    utc_seconds = local_seconds - offset_seconds;
    if (utc_seconds < 0) return 0;
    *mtime = bp_u64_from_ulong((unsigned long)utc_seconds);
    return 1;
}

static int bp_amiga_time_from_utc(bp_uint64 mtime, bp_uint64 *local_time)
{
    long offset_seconds;
    long utc_seconds;
    long local_seconds;

    if (local_time == NULL || mtime.hi != 0 ||
        mtime.lo > (bp_uint32)LONG_MAX ||
        !bp_amiga_get_tz_offset(&offset_seconds) ||
        offset_seconds < -86340L || offset_seconds > 86340L)
        return 0;
    utc_seconds = (long)mtime.lo;
    if (offset_seconds > 0 && utc_seconds > LONG_MAX - offset_seconds)
        return 0;
    if (offset_seconds < 0 && utc_seconds < -offset_seconds)
        return 0;
    local_seconds = utc_seconds + offset_seconds;
    if (local_seconds < 0) return 0;
    *local_time = bp_u64_from_ulong((unsigned long)local_seconds);
    return 1;
}

static int bp_amiga_restore_mtime(const char *path, bp_uint64 mtime)
{
    bp_uint64 local_time;
    if (!bp_amiga_time_from_utc(mtime, &local_time)) return BP_ERR_PARTIAL;
    return bp_restore_mtime(path, local_time);
}

#endif
