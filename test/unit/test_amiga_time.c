#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../src/frontend/amiga/amiga_time.h"

static int check(int ok, const char *what)
{
    if (!ok) fprintf(stderr, "FAIL: %s\n", what);
    return ok;
}

int main(void)
{
    long offset = 0;
    bp_uint64 utc, local;
    int failed = 0;

    failed += !check(bp_amiga_parse_tz_offset("60", &offset) &&
                     offset == 3600L, "positive timezone offset");
    failed += !check(bp_amiga_parse_tz_offset("-330", &offset) &&
                     offset == -19800L, "negative timezone offset");
    failed += !check(bp_amiga_parse_tz_offset("1439", &offset) &&
                     offset == 86340L, "maximum timezone offset");
    failed += !check(!bp_amiga_parse_tz_offset("1440", &offset),
                     "reject offset beyond 23:59");
    failed += !check(!bp_amiga_parse_tz_offset("60x", &offset),
                     "reject malformed timezone offset");

    failed += !check(setenv("BP_TZ_OFFSET", "60", 1) == 0,
                     "set positive timezone environment");
    failed += !check(bp_amiga_time_to_utc((time_t)1700003700L, &utc) &&
                     utc.hi == 0 && utc.lo == 1700000100UL,
                     "local wall time to UTC");
    failed += !check(bp_amiga_time_from_utc(utc, &local) &&
                     local.hi == 0 && local.lo == 1700003700UL,
                     "UTC to local wall time");

    failed += !check(setenv("BP_TZ_OFFSET", "-330", 1) == 0,
                     "set negative timezone environment");
    failed += !check(bp_amiga_time_to_utc((time_t)1699980300L, &utc) &&
                     utc.hi == 0 && utc.lo == 1700000100UL,
                     "negative-offset local time to UTC");
    failed += !check(bp_amiga_time_from_utc(utc, &local) &&
                     local.hi == 0 && local.lo == 1699980300UL,
                     "negative-offset UTC to local time");

    failed += !check(unsetenv("BP_TZ_OFFSET") == 0,
                     "remove timezone environment");
    failed += !check(!bp_amiga_time_to_utc((time_t)1700000100L, &utc),
                     "missing timezone refuses archive timestamp");
    failed += !check(bp_amiga_time_from_utc(bp_u64_from_ulong(1700000100UL),
                                             &local) == 0,
                     "missing timezone refuses restoration");
    failed += !check(setenv("BP_TZ_OFFSET", "invalid", 1) == 0 &&
                     !bp_amiga_time_to_utc((time_t)1700000100L, &utc),
                     "invalid timezone refuses archive timestamp");

    printf("Amiga timezone conversion: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
