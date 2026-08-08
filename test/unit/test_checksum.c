/*
 * test_checksum.c -- unit tests for bp_checksum (Adler-32)
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include <stdio.h>
#include <string.h>
#include "test_runner.h"
#include "bp_types.h"
#include "compress.h"

/* --- helper ------------------------------------------------------------- */

/*
 * Compute expected Adler-32 using a plain reference implementation that
 * matches the algorithm specified in RFC 1950.
 *
 * s1 = 1 + sum(Di) mod 65521
 * s2 = sum(s1 after each Di) mod 65521
 * result = (s2 << 16) | s1
 */
static bp_uint32 ref_adler32(const unsigned char *data, unsigned long len)
{
    unsigned long s1 = 1;
    unsigned long s2 = 0;
    unsigned long i;
    for (i = 0; i < len; i++) {
        s1 = (s1 + data[i]) % 65521UL;
        s2 = (s2 + s1)      % 65521UL;
    }
    return (bp_uint32)((s2 << 16) | s1);
}

/* --- tests -------------------------------------------------------------- */

static void test_empty(void)
{
    bp_uint32 ck;
    UNIT_SECTION("Adler-32: empty input");
    ck = bp_checksum(1, (const bp_uint8 *)"", 0);
    UNIT_CHECK(ck == 0x00000001UL, "empty string -> 0x00000001");
}

static void test_known_vectors(void)
{
    bp_uint32 ck;
    const char *abc = "ABC";

    UNIT_SECTION("Adler-32: known vectors");

    ck = bp_checksum(1, (const bp_uint8 *)"a", 1);
    UNIT_CHECK(ck == 0x00620062UL, "\"a\" -> 0x00620062");

    /*
     * "ABC": s1=199, s2=397 after the three bytes, result=0x018D00C7.
     * Pre-calculated by hand:
     *   After 'A'(65): s1=66, s2=66
     *   After 'B'(66): s1=132, s2=198
     *   After 'C'(67): s1=199, s2=397
     */
    ck = bp_checksum(1, (const bp_uint8 *)abc, 3);
    UNIT_CHECK(ck == 0x018D00C7UL, "\"ABC\" -> 0x018D00C7");

    /*
     * "abcdefghijklmnopqrstuvwxyz" is a well-known zlib test vector.
     */
    ck = bp_checksum(1, (const bp_uint8 *)"abcdefghijklmnopqrstuvwxyz", 26);
    UNIT_CHECK(ck == 0x90860B20UL, "26 lowercase letters -> 0x90860B20");
}

static void test_incremental(void)
{
    const char *msg = "Hello, BoltPack!";
    bp_uint32 full;
    bp_uint32 part;
    size_t split = 7;

    UNIT_SECTION("Adler-32: incremental chaining");

    full = bp_checksum(1, (const bp_uint8 *)msg, (bp_uint32)strlen(msg));

    /* split at byte 7 and chain */
    part = bp_checksum(1, (const bp_uint8 *)msg, (bp_uint32)split);
    part = bp_checksum(part, (const bp_uint8 *)msg + split,
                       (bp_uint32)(strlen(msg) - split));

    UNIT_CHECK(full == part, "incremental split matches one-shot");
}

static void test_all_zeros(void)
{
    unsigned char buf[1024];
    bp_uint32 expected;
    bp_uint32 actual;

    UNIT_SECTION("Adler-32: all-zero buffer 1024 bytes");

    memset(buf, 0, sizeof(buf));
    expected = ref_adler32(buf, 1024);
    actual   = bp_checksum(1, buf, 1024);
    UNIT_CHECK(actual == expected, "all-zero 1024 matches reference");
}

static void test_all_0xff(void)
{
    unsigned char buf[256];
    bp_uint32 expected;
    bp_uint32 actual;

    UNIT_SECTION("Adler-32: all-0xFF buffer 256 bytes");

    memset(buf, 0xFF, sizeof(buf));
    expected = ref_adler32(buf, 256);
    actual   = bp_checksum(1, buf, 256);
    UNIT_CHECK(actual == expected, "all-0xFF 256 matches reference");
}

static void test_single_byte_range(void)
{
    unsigned char buf[1];
    bp_uint32 expected;
    bp_uint32 actual;
    int byte_val;

    UNIT_SECTION("Adler-32: single byte 0x00..0xFF");
    for (byte_val = 0; byte_val <= 255; byte_val++) {
        buf[0] = (unsigned char)byte_val;
        expected = ref_adler32(buf, 1);
        actual   = bp_checksum(1, buf, 1);
        if (actual != expected) {
            printf("  FAIL  single byte 0x%02X: got 0x%08lX, expected 0x%08lX"
                   "  [%s:%d]\n",
                   byte_val, (unsigned long)actual, (unsigned long)expected,
                   __FILE__, __LINE__);
            bp_unit_fail_count++;
        }
    }
    UNIT_CHECK(1, "all single-byte values match reference");
}

static void test_65536_bytes(void)
{
    /*
     * Static array: 65536 bytes avoids VLA and large stack usage.
     * Pattern: byte[i] = i & 0xFF, which covers all byte values many times.
     */
    static unsigned char buf[65536];
    bp_uint32 expected;
    bp_uint32 actual;
    int i;

    UNIT_SECTION("Adler-32: 65536-byte boundary");
    for (i = 0; i < 65536; i++) {
        buf[i] = (unsigned char)(i & 0xFF);
    }
    expected = ref_adler32(buf, 65536);
    actual   = bp_checksum(1, buf, 65536);
    UNIT_CHECK(actual == expected, "65536-byte pattern matches reference");
}

static void test_modulo_wrap(void)
{
    /*
     * Feed exactly 65521 bytes of value 1 to force s1 modulo wrap.
     * After 65520 bytes of 1: s1 = (1 + 65520) % 65521 = 65521 % 65521 = 0.
     * This tests the modulo reduction path in the Adler-32 implementation.
     */
    static unsigned char buf[65521];
    bp_uint32 expected;
    bp_uint32 actual;

    UNIT_SECTION("Adler-32: modulo wrap (65521 ones)");
    memset(buf, 1, sizeof(buf));
    expected = ref_adler32(buf, 65521);
    actual   = bp_checksum(1, buf, 65521);
    UNIT_CHECK(actual == expected, "65521 ones matches reference (tests modulo wrap)");
}

/* --- main --------------------------------------------------------------- */

int main(void)
{
    printf("BoltPack unit tests: checksum (Adler-32)\n");
    test_empty();
    test_known_vectors();
    test_incremental();
    test_all_zeros();
    test_all_0xff();
    test_single_byte_range();
    test_65536_bytes();
    test_modulo_wrap();
    return UNIT_REPORT();
}
