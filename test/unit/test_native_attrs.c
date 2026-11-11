/*
 * test_native_attrs.c -- BPNA native attribute container codec tests
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_runner.h"
#include "../../src/frontend/common/native_attrs.h"

static unsigned char g_buf[BP_NATTR_MAX_PAYLOAD];
static unsigned char g_copy[BP_NATTR_MAX_PAYLOAD];

static int roundtrip_two_items(void)
{
    bp_nattr_builder_t b;
    bp_nattr_info_t info;
    bp_nattr_iter_t it;
    const char *name;
    const unsigned char *value;
    unsigned long nl, vl, len;
    int ok = 1;

    bp_nattr_begin(&b, g_buf, 3, 0);
    ok &= bp_nattr_add(&b, "com.example.a", 13, "xyz", 3) == BP_NATTR_OK;
    ok &= bp_nattr_add(&b, "b", 1, NULL, 0) == BP_NATTR_OK;
    len = bp_nattr_finish(&b);
    ok &= bp_nattr_parse(g_buf, len, &info) == BP_NATTR_OK;
    ok &= info.count == 2 && info.origin == 3 && info.subject_type == 0;
    bp_nattr_iter_begin(&info, &it);
    ok &= bp_nattr_iter_next(&it, &name, &nl, &value, &vl) == 1;
    ok &= nl == 13 && memcmp(name, "com.example.a", 13) == 0 &&
          vl == 3 && memcmp(value, "xyz", 3) == 0;
    ok &= bp_nattr_iter_next(&it, &name, &nl, &value, &vl) == 1;
    ok &= nl == 1 && name[0] == 'b' && vl == 0;
    ok &= bp_nattr_iter_next(&it, &name, &nl, &value, &vl) == 0;
    return ok;
}

static unsigned long make_valid(unsigned count)
{
    bp_nattr_builder_t b;
    char name[16];
    unsigned i;
    bp_nattr_begin(&b, g_buf, 3, 4);
    for (i = 0; i < count; i++) {
        sprintf(name, "attr%u", i);
        if (bp_nattr_add(&b, name, strlen(name), "v", 1) != BP_NATTR_OK)
            return 0;
    }
    return bp_nattr_finish(&b);
}

int main(void)
{
    bp_nattr_builder_t b;
    bp_nattr_info_t info;
    unsigned long len, i;
    unsigned char big[BP_NATTR_MAX_VALUE + 1];
    char longname[BP_NATTR_MAX_NAME + 2];
    int rc;

    UNIT_SECTION("builder and parser");
    UNIT_CHECK(roundtrip_two_items(), "two items round trip");
    bp_nattr_begin(&b, g_buf, 3, 4);
    len = bp_nattr_finish(&b);
    UNIT_CHECK(len == BP_NATTR_HEADER_SIZE &&
               bp_nattr_parse(g_buf, len, &info) == BP_NATTR_OK &&
               info.count == 0 && info.subject_type == 4,
               "empty directory container is valid");
    UNIT_CHECK(g_buf[0] == 'B' && g_buf[1] == 'P' && g_buf[2] == 'N' &&
               g_buf[3] == 'A' && g_buf[4] == 1 && g_buf[7] == 0,
               "header bytes are BPNA, version 1, reserved zero");

    UNIT_SECTION("builder limits");
    memset(big, 'v', sizeof(big));
    bp_nattr_begin(&b, g_buf, 3, 0);
    UNIT_CHECK(bp_nattr_add(&b, "k", 1, big, BP_NATTR_MAX_VALUE) == BP_NATTR_OK,
               "32 KiB value is accepted");
    UNIT_CHECK(bp_nattr_add(&b, "k2", 2, big, BP_NATTR_MAX_VALUE + 1) ==
               BP_NATTR_TOO_BIG, "value over 32 KiB is rejected");
    UNIT_CHECK(bp_nattr_add(&b, "", 0, "v", 1) == BP_NATTR_BAD_NAME,
               "empty name is rejected");
    UNIT_CHECK(bp_nattr_add(&b, "a\0b", 3, "v", 1) == BP_NATTR_BAD_NAME,
               "name with NUL is rejected");
    memset(longname, 'n', sizeof(longname));
    UNIT_CHECK(bp_nattr_add(&b, longname, BP_NATTR_MAX_NAME + 1, "v", 1) ==
               BP_NATTR_BAD_NAME, "256-byte name is rejected");
    UNIT_CHECK(bp_nattr_add(&b, longname, BP_NATTR_MAX_NAME, "v", 1) ==
               BP_NATTR_OK, "255-byte name is accepted");
    UNIT_CHECK(bp_nattr_add(&b, "k", 1, "v", 1) == BP_NATTR_DUPLICATE,
               "duplicate name is rejected");
    UNIT_CHECK(bp_nattr_add(&b, "k3", 2, big, BP_NATTR_MAX_VALUE) ==
               BP_NATTR_FULL, "a value that would pass 64 KiB reports full");
    UNIT_CHECK(bp_nattr_add(&b, "k3", 2, big,
                            BP_NATTR_MAX_PAYLOAD - b.len -
                            BP_NATTR_ITEM_OVERHEAD - 2) == BP_NATTR_OK,
               "an item that exactly fills the payload is accepted");
    UNIT_CHECK(bp_nattr_add(&b, "k4", 2, big, 1) == BP_NATTR_FULL,
               "payload over 64 KiB reports full");
    len = bp_nattr_finish(&b);
    UNIT_CHECK(len <= BP_NATTR_MAX_PAYLOAD &&
               bp_nattr_parse(g_buf, len, &info) == BP_NATTR_OK,
               "a container filled to the limit still parses");

    len = make_valid(BP_NATTR_MAX_ITEMS);
    UNIT_CHECK(len != 0 && bp_nattr_parse(g_buf, len, &info) == BP_NATTR_OK &&
               info.count == BP_NATTR_MAX_ITEMS, "64 items accepted");
    bp_nattr_begin(&b, g_buf, 3, 0);
    for (i = 0; i < BP_NATTR_MAX_ITEMS; i++) {
        char n[16];
        sprintf(n, "a%lu", i);
        bp_nattr_add(&b, n, strlen(n), "v", 1);
    }
    UNIT_CHECK(bp_nattr_add(&b, "extra", 5, "v", 1) == BP_NATTR_FULL,
               "65th item reports full");

    UNIT_SECTION("parser rejects malformed input");
    len = make_valid(2);
    memcpy(g_copy, g_buf, len);
    UNIT_CHECK(bp_nattr_parse(g_copy, len - 1, &info) == BP_NATTR_CORRUPT,
               "truncated by one byte");
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_OK,
               "intact copy parses");
    g_copy[len] = 0;
    UNIT_CHECK(bp_nattr_parse(g_copy, len + 1, &info) == BP_NATTR_CORRUPT,
               "one trailing byte");
    UNIT_CHECK(bp_nattr_parse(g_copy, 9, &info) == BP_NATTR_CORRUPT,
               "shorter than the header");
    UNIT_CHECK(bp_nattr_parse(NULL, 0, &info) == BP_NATTR_CORRUPT,
               "null input");
    memcpy(g_copy, g_buf, len); g_copy[0] = 'X';
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "bad magic");
    memcpy(g_copy, g_buf, len); g_copy[8] = 3;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "count larger than the items present");
    memcpy(g_copy, g_buf, len); g_copy[8] = 1;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "count smaller than the items present");
    memcpy(g_copy, g_buf, len); g_copy[8] = 65;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "count above 64");
    memcpy(g_copy, g_buf, len); g_copy[6] = 3;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "subject type other than file or directory");
    memcpy(g_copy, g_buf, len); g_copy[10] = 0; g_copy[11] = 0;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "zero-length name");
    memcpy(g_copy, g_buf, len); g_copy[12] = 0xFF; g_copy[13] = 0xFF;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "value length larger than the payload");
    memcpy(g_copy, g_buf, len); g_copy[16] = 0;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_CORRUPT,
               "NUL inside a name");
    {
        bp_nattr_builder_t d;
        unsigned long dl;
        bp_nattr_begin(&d, g_copy, 3, 0);
        bp_nattr_add(&d, "aa", 2, "1", 1);
        bp_nattr_add(&d, "bb", 2, "2", 1);
        dl = bp_nattr_finish(&d);
        /* second item starts at 10 + 6 + 2 + 1 = 19; its name at 25 */
        g_copy[25] = 'a';
        g_copy[26] = 'a';
        UNIT_CHECK(bp_nattr_parse(g_copy, dl, &info) == BP_NATTR_CORRUPT,
                   "duplicate names in a parsed container");
    }

    UNIT_SECTION("versions");
    memcpy(g_copy, g_buf, len); g_copy[4] = 2;
    rc = bp_nattr_parse(g_copy, len, &info);
    UNIT_CHECK(rc == BP_NATTR_UNSUPPORTED && info.version == 2,
               "unknown version is unsupported, not corrupt");
    memcpy(g_copy, g_buf, len); g_copy[4] = 2; g_copy[8] = 200;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_UNSUPPORTED,
               "unknown version is not interpreted further");
    memcpy(g_copy, g_buf, len); g_copy[7] = 0x55;
    UNIT_CHECK(bp_nattr_parse(g_copy, len, &info) == BP_NATTR_OK,
               "reserved byte is ignored by readers");

    UNIT_SECTION("known bytes");
    {
        static const unsigned char expect[] = {
            'B','P','N','A', 1, 3, 0, 0, 1, 0,
            3, 0, 2, 0, 0, 0, 'a','b','c', 'x','y'
        };
        bp_nattr_builder_t d;
        bp_nattr_begin(&d, g_copy, 3, 0);
        bp_nattr_add(&d, "abc", 3, "xy", 2);
        len = bp_nattr_finish(&d);
        UNIT_CHECK(len == sizeof(expect) &&
                   memcmp(g_copy, expect, sizeof(expect)) == 0,
                   "one item serialises to the documented bytes");
    }
    return UNIT_REPORT();
}
