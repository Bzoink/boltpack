#include <stdio.h>
#include <string.h>

#include "../../src/frontend/ql/ql_time.h"

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

static void test_offsets(void)
{
    long offset;
    CHECK(bp_ql_parse_tz_offset(NULL, &offset) && offset == 0);
    CHECK(bp_ql_parse_tz_offset("0", &offset) && offset == 0);
    CHECK(bp_ql_parse_tz_offset("+60", &offset) && offset == 3600L);
    CHECK(bp_ql_parse_tz_offset("-330", &offset) && offset == -19800L);
    CHECK(bp_ql_parse_tz_offset("1439", &offset) && offset == 86340L);
    CHECK(bp_ql_parse_tz_offset("-1439", &offset) && offset == -86340L);
    CHECK(!bp_ql_parse_tz_offset("", &offset));
    CHECK(!bp_ql_parse_tz_offset("1440", &offset));
    CHECK(!bp_ql_parse_tz_offset("-1440", &offset));
    CHECK(!bp_ql_parse_tz_offset("+", &offset));
    CHECK(!bp_ql_parse_tz_offset("1x", &offset));
    CHECK(!bp_ql_parse_tz_offset("--1", &offset));
    CHECK(!bp_ql_parse_tz_offset("1", NULL));
}

static void test_time_conversion(void)
{
    bp_uint64 stamp;
    bp_uint64 beyond;
    long qdos;

    CHECK(bp_ql_qdos_to_utc((long)(BP_QL_EPOCH_OFFSET + 3600UL),
                            3600L, &stamp));
    CHECK(stamp.hi == 0 && stamp.lo == 0);
    CHECK(bp_ql_utc_to_qdos(stamp, 3600L, &qdos));
    CHECK((unsigned long)qdos == BP_QL_EPOCH_OFFSET + 3600UL);

    stamp = bp_u64_from_u32(1700000000UL);
    CHECK(bp_ql_utc_to_qdos(stamp, -19800L, &qdos));
    CHECK(bp_ql_qdos_to_utc(qdos, -19800L, &beyond));
    CHECK(beyond.hi == stamp.hi && beyond.lo == stamp.lo);

    CHECK(!bp_ql_qdos_to_utc(-1L, 0L, &stamp));
    CHECK(!bp_ql_qdos_to_utc(2147483648L, 0L, &stamp));
    CHECK(!bp_ql_qdos_to_utc((long)BP_QL_EPOCH_OFFSET - 1L,
                             0L, &stamp));
    CHECK(!bp_ql_qdos_to_utc((long)BP_QL_EPOCH_OFFSET,
                             86400L, &stamp));
    CHECK(bp_ql_utc_to_qdos(bp_u64_from_u32(0), 0L, &qdos));
    CHECK((unsigned long)qdos == BP_QL_EPOCH_OFFSET);
    CHECK(!bp_ql_utc_to_qdos(bp_u64_from_u32(2147483647UL),
                             0L, &qdos));
    beyond = bp_u64_from_u32(1);
    beyond.hi = 1;
    CHECK(!bp_ql_utc_to_qdos(beyond, 0L, &qdos));
}

int main(void)
{
    test_offsets();
    test_time_conversion();
    if (failures != 0) {
        fprintf(stderr, "%d QL time test(s) failed\n", failures);
        return 1;
    }
    puts("QL time conversion tests passed");
    return 0;
}
