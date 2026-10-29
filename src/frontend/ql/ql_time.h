#ifndef BP_QL_TIME_H
#define BP_QL_TIME_H

#include "../../core/bp_types.h"

/* QDOS FS.DATE seconds use the 1961-01-01 epoch and local wall time. */
#define BP_QL_EPOCH_OFFSET 283996800UL
#define BP_QL_MAX_QDOS_SECONDS 2147483647UL

/* Parse BP_TZ_OFFSET, in minutes east of UTC, to signed seconds. A missing
 * value keeps the documented UTC default; a present malformed value fails. */
static int bp_ql_parse_tz_offset(const char *text, long *offset_seconds)
{
    unsigned long magnitude = 0;
    int negative = 0;
    const char *p = text;

    if (offset_seconds == NULL) return 0;
    if (p == NULL) {
        *offset_seconds = 0;
        return 1;
    }
    if (*p == '\0') return 0;
    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        ++p;
    }
    if (*p == '\0') return 0;
    while (*p != '\0') {
        unsigned int digit;
        if (*p < '0' || *p > '9') return 0;
        digit = (unsigned int)(*p - '0');
        if (magnitude > 143UL ||
            magnitude * 10UL + digit > 1439UL)
            return 0;
        magnitude = magnitude * 10UL + digit;
        ++p;
    }
    *offset_seconds = (long)(magnitude * 60UL);
    if (negative) *offset_seconds = -*offset_seconds;
    return 1;
}

/* Convert local QDOS seconds since 1961 to UTC Unix seconds. */
static int bp_ql_qdos_to_utc(long qdos_seconds, long offset_seconds,
                             bp_uint64 *mtime)
{
    long utc_seconds;
    if (mtime == NULL || qdos_seconds < 0 ||
        (unsigned long)qdos_seconds > BP_QL_MAX_QDOS_SECONDS ||
        offset_seconds < -86340L || offset_seconds > 86340L)
        return 0;
    utc_seconds = qdos_seconds - (long)BP_QL_EPOCH_OFFSET - offset_seconds;
    if (utc_seconds < 0) return 0;
    *mtime = bp_u64_from_ulong((unsigned long)utc_seconds);
    return 1;
}

/* Convert UTC Unix seconds to local QDOS seconds, rejecting signed-long
 * overflow before constructing the target date value. */
static int bp_ql_utc_to_qdos(bp_uint64 mtime, long offset_seconds,
                             long *qdos_seconds)
{
    unsigned long max_unix;
    unsigned long local_seconds;
    unsigned long shift;

    if (qdos_seconds == NULL || mtime.hi != 0 ||
        offset_seconds < -86340L || offset_seconds > 86340L)
        return 0;
    if (offset_seconds >= 0) {
        shift = (unsigned long)offset_seconds;
        max_unix = BP_QL_MAX_QDOS_SECONDS - BP_QL_EPOCH_OFFSET - shift;
        if ((unsigned long)mtime.lo > max_unix) return 0;
        local_seconds = (unsigned long)mtime.lo + BP_QL_EPOCH_OFFSET + shift;
    } else {
        shift = (unsigned long)(-offset_seconds);
        max_unix = BP_QL_MAX_QDOS_SECONDS - BP_QL_EPOCH_OFFSET + shift;
        if ((unsigned long)mtime.lo > max_unix) return 0;
        local_seconds = (unsigned long)mtime.lo + BP_QL_EPOCH_OFFSET - shift;
    }
    *qdos_seconds = (long)local_seconds;
    return 1;
}

#endif
