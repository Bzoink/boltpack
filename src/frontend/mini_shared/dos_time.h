/* DOS packed local time and UTC conversion using an explicit fixed offset. */
#ifndef BP_MINI_DOS_TIME_H
#define BP_MINI_DOS_TIME_H
#include "../../core/bp_types.h"
#include <stdlib.h>

/* Minutes east of UTC. An absent or invalid setting means unknown timezone. */
static int bp_dos_timezone(long *minutes)
{
    const char *s = getenv("BP_TZ_OFFSET");
    char *end;
    long value;
    if (s == NULL || *s == '\0') return -1;
    value = strtol(s, &end, 10);
    if (*end != '\0' || end == s || value < -1440L || value > 1440L) return -1;
    *minutes = value;
    return 0;
}

static int bp_dos_leap(unsigned y)
{
    return y % 4U == 0 && (y % 100U != 0 || y % 400U == 0);
}

static unsigned bp_dos_month_days(unsigned y, unsigned m)
{
    static const unsigned char days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[m - 1] + (m == 2 && bp_dos_leap(y) ? 1U : 0U);
}

static bp_uint64 bp_dos_seconds_days(bp_uint32 days)
{
    bp_uint64 total = bp_u64_from_u32(0), factor = bp_u64_from_u32(86400UL);
    while (days) {
        if (days & 1U) total = bp_u64_add(total, factor);
        factor = bp_u64_add(factor, factor);
        days >>= 1;
    }
    return total;
}

static int bp_dos_apply_offset(bp_uint64 *value, long minutes)
{
    bp_uint64 offset = bp_u64_from_u32((bp_uint32)(minutes < 0 ? -minutes : minutes) * 60UL);
    if (minutes < 0) {
        if (bp_u64_cmp(*value, offset) < 0) return -1;
        *value = bp_u64_sub(*value, offset);
    } else *value = bp_u64_add(*value, offset);
    return 0;
}

static int bp_dos_local_to_utc(unsigned date, unsigned time, long offset,
                               bp_uint64 *utc)
{
    unsigned year = 1980U + ((date >> 9) & 127U);
    unsigned month = (date >> 5) & 15U, day = date & 31U;
    unsigned hour = (time >> 11) & 31U, minute = (time >> 5) & 63U;
    unsigned second = (time & 31U) * 2U, y, m;
    bp_uint32 days = 0;
    if (offset < -1440L || offset > 1440L || month < 1 || month > 12 ||
        day < 1 || day > bp_dos_month_days(year, month) || hour > 23 ||
        minute > 59 || second > 59) return -1;
    for (y = 1970; y < year; y++) days += bp_dos_leap(y) ? 366UL : 365UL;
    for (m = 1; m < month; m++) days += bp_dos_month_days(year, m);
    days += day - 1;
    *utc = bp_dos_seconds_days(days);
    *utc = bp_u64_add(*utc, bp_u64_from_u32((bp_uint32)hour * 3600UL +
                              (bp_uint32)minute * 60UL + second));
    return bp_dos_apply_offset(utc, -offset);
}

/* Returns 1 if odd seconds were rounded down to FAT's two-second resolution. */
static int bp_dos_utc_to_local(bp_uint64 utc, long offset,
                               unsigned *date, unsigned *time)
{
    unsigned y = 1970, m = 1, day, hour, minute, second;
    bp_uint64 period;
    int rounded;
    if (offset < -1440L || offset > 1440L ||
        bp_dos_apply_offset(&utc, offset) != 0) return -1;
    while (y <= 2107U) {
        period = bp_dos_seconds_days(bp_dos_leap(y) ? 366UL : 365UL);
        if (bp_u64_cmp(utc, period) < 0) break;
        utc = bp_u64_sub(utc, period); y++;
    }
    if (y < 1980U || y > 2107U) return -1;
    while (m <= 12U) {
        period = bp_dos_seconds_days(bp_dos_month_days(y, m));
        if (bp_u64_cmp(utc, period) < 0) break;
        utc = bp_u64_sub(utc, period); m++;
    }
    if (utc.hi != 0) return -1;
    day = (unsigned)(utc.lo / 86400UL) + 1U;
    utc.lo %= 86400UL;
    hour = (unsigned)(utc.lo / 3600UL); utc.lo %= 3600UL;
    minute = (unsigned)(utc.lo / 60UL); second = (unsigned)(utc.lo % 60UL);
    rounded = (int)(second & 1U);
    *date = ((y - 1980U) << 9) | (m << 5) | day;
    *time = (hour << 11) | (minute << 5) | (second / 2U);
    return rounded;
}
#endif
