/*
 * mini_test_compress.c -- unit tests for the mini compression layer
 *
 * Tests bp_mini_compress_block / bp_mini_decompress_block / bp_mini_checksum
 * from src/core/mini/compress.h.  Targets both 16-bit (Open Watcom) and
 * 32/64-bit (GCC) builds.
 *
 * On 16-bit (SIZEOF_INT < 4): only Stored (0) and BoltLite (1) are available;
 * boltlz (2) returns BP_ERR_UNSUPPORTED_METHOD.
 *
 * ANSI C (C89/C90) only.  No long long, no VLA, no <stdint.h>.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <string.h>
#include "test_runner.h"
#include "bp_types.h"       /* resolved via -Isrc/core */
#include "mini/compress.h"  /* resolved via -Isrc/core; internally includes ../bp_types.h */
#include "mini/boltlz_decomp.h"

/* Worst-case buffer sizes */
#define MINI_BOLTLZ_DST(n)  ((n) + (n)/63 + 16)
#define MINI_BOLTLITE_DST(n) ((n) + (n)/8  + 16)

/* ======================================================================== */
/* helpers                                                                   */
/* ======================================================================== */

static void mini_roundtrip(const char *label,
                             bp_uint8 method,
                             const bp_uint8 *src, bp_uint32 src_len,
                             bp_uint8 *comp_buf, bp_uint32 comp_cap,
                             bp_uint8 *decomp_buf, bp_uint32 decomp_cap)
{
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    int rc;

    if (src_len > decomp_cap) {
        UNIT_CHECK(0, "round-trip fixture output capacity covers its source");
        return;
    }
    comp_len = comp_cap;
    rc = bp_mini_compress_block(src, src_len, comp_buf, &comp_len, method);
    if (rc != BP_OK) {
        printf("  FAIL  %s: compress rc=%d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    /*
     * BoltLite and BoltLZ require the EXACT original size as dst_len.
     * Passing src_len is correct for all three: roundtrip guarantees
     * decompressed size == original size, and the buffer is always >=.
     */
    decomp_len = src_len;
    rc = bp_mini_decompress_block(comp_buf, comp_len,
                                   decomp_buf, &decomp_len, method);
    if (rc != BP_OK) {
        printf("  FAIL  %s: decompress rc=%d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    if (decomp_len != src_len) {
        printf("  FAIL  %s: decomp_len=%lu expected %lu  [%s:%d]\n",
               label, (unsigned long)decomp_len, (unsigned long)src_len,
               __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    if (src_len > 0 && memcmp(src, decomp_buf, src_len) != 0) {
        printf("  FAIL  %s: roundtrip data mismatch  [%s:%d]\n",
               label, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    printf("  PASS  %s\n", label);
    bp_unit_pass_count++;
}

/* ======================================================================== */
/* Stored (method 0)                                                         */
/* ======================================================================== */

static void test_stored(void)
{
    /* Static buffers: keep small to fit in 16-bit segment */
    static unsigned char src[4096];
    static unsigned char comp[4096];
    static unsigned char decomp[4096];
    int i;

    UNIT_SECTION("Mini: Stored (method 0)");

    /* single byte */
    src[0] = 0x42;
    mini_roundtrip("single byte 0x42", BP_METHOD_STORED,
                    src, 1, comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));

    /* all-zeros */
    memset(src, 0, sizeof(src));
    mini_roundtrip("4 KB all-zeros", BP_METHOD_STORED,
                    src, (bp_uint32)sizeof(src),
                    comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));

    /* sequential */
    for (i = 0; i < 4096; i++) src[i] = (unsigned char)(i & 0xFF);
    mini_roundtrip("4 KB sequential", BP_METHOD_STORED,
                    src, (bp_uint32)sizeof(src),
                    comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));
}

/* ======================================================================== */
/* BoltLite (method 1)                                                           */
/* ======================================================================== */

static void test_boltlite(void)
{
    static unsigned char src[4096];
    static unsigned char comp[MINI_BOLTLITE_DST(4096)];
    static unsigned char decomp[4096];
    int i;

    UNIT_SECTION("Mini: BoltLite (method 1)");

    src[0] = 'Z';
    mini_roundtrip("single byte Z", BP_METHOD_BOLTLITE,
                    src, 1, comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));

    memset(src, 0x20, sizeof(src)); /* space fill = BoltLite ring-init byte */
    mini_roundtrip("4 KB all-spaces", BP_METHOD_BOLTLITE,
                    src, (bp_uint32)sizeof(src),
                    comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));

    for (i = 0; i < 4096; i++) src[i] = (unsigned char)(i % 13);
    mini_roundtrip("4 KB mod-13 pattern", BP_METHOD_BOLTLITE,
                    src, (bp_uint32)sizeof(src),
                    comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));
}

/* ======================================================================== */
/* boltlz (method 2) -- 32/64-bit only                                      */
/* ======================================================================== */

static void test_boltlz(void)
{
    static unsigned char src[4096];
    static unsigned char comp[MINI_BOLTLZ_DST(4096)];
#if !defined(SIZEOF_INT) || SIZEOF_INT >= 4
    static unsigned char decomp[4096];
    int i;
#endif

    UNIT_SECTION("Mini: boltlz (method 2)");

#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
    /*
     * On 16-bit builds boltlz is excluded.  Verify the expected error code.
     */
    {
        bp_uint32 comp_len = (bp_uint32)sizeof(comp);
        int rc;
        src[0] = 0;
        rc = bp_mini_compress_block(src, 1, comp, &comp_len,
                                     BP_METHOD_BOLTLZ);
        UNIT_CHECK(rc == BP_ERR_UNSUPPORTED_METHOD,
                   "boltlz returns BP_ERR_UNSUPPORTED_METHOD on 16-bit");
    }
#else
    memset(src, 0, sizeof(src));
    mini_roundtrip("4 KB all-zeros", BP_METHOD_BOLTLZ,
                    src, (bp_uint32)sizeof(src),
                    comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));

    for (i = 0; i < 4096; i++) src[i] = (unsigned char)(i & 0xFF);
    mini_roundtrip("4 KB sequential", BP_METHOD_BOLTLZ,
                    src, (bp_uint32)sizeof(src),
                    comp, (bp_uint32)sizeof(comp),
                    decomp, (bp_uint32)sizeof(decomp));
#endif
}

/* ======================================================================== */
/* Fluff (method 3) -- should return unsupported on mini                     */
/* ======================================================================== */

static void test_fluff_unsupported(void)
{
    static unsigned char src[16];
    static unsigned char comp[64];
    bp_uint32 comp_len;
    int rc;

    UNIT_SECTION("Mini: Fluff (method 3) unsupported");

    memset(src, 0, sizeof(src));
    comp_len = (bp_uint32)sizeof(comp);
    rc = bp_mini_compress_block(src, (bp_uint32)sizeof(src), comp, &comp_len,
                                 BP_METHOD_FLUFF);
    UNIT_CHECK(rc == BP_ERR_UNSUPPORTED_METHOD,
               "Fluff compress returns BP_ERR_UNSUPPORTED_METHOD");

    comp_len = (bp_uint32)sizeof(comp);
    rc = bp_mini_decompress_block(src, (bp_uint32)sizeof(src),
                                   comp, &comp_len, BP_METHOD_FLUFF);
    UNIT_CHECK(rc == BP_ERR_UNSUPPORTED_METHOD,
               "Fluff decompress returns BP_ERR_UNSUPPORTED_METHOD");
}

static void test_boltlz_decoder_invalid(void)
{
    static const unsigned char valid[] = {2, 0x14, 'A', 1};
    static const unsigned char expected[] = "AAAAAAAAA";
    unsigned char output[9];
    static unsigned char src[1] = {0};
    static unsigned char dst[1];
    bp_uint32 dst_len;
    int rc;

    UNIT_SECTION("Mini: boltlz decoder rejects invalid input");

    dst_len = 9;
    rc = bp_boltlz_decompress(valid, sizeof(valid), output, &dst_len);
    UNIT_CHECK(rc == BOLTLZ_D_OK && dst_len == 9 &&
               memcmp(output, expected, 9) == 0,
               "shared decoder reconstructs golden overlapping match without padding");

    dst_len = 1;
    rc = bp_boltlz_decompress(src, 1, dst, &dst_len);
    UNIT_CHECK(rc == BOLTLZ_D_ERR && dst_len == 0,
               "one-byte boltlz input is rejected");

    dst_len = 1;
    rc = bp_boltlz_decompress(src, 65536UL, dst, &dst_len);
    UNIT_CHECK(rc == BOLTLZ_D_ERR && dst_len == 0,
               "oversized boltlz input is rejected");

    dst_len = 1;
    rc = bp_boltlz_decompress(NULL, 1, dst, &dst_len);
    UNIT_CHECK(rc == BOLTLZ_D_ERR && dst_len == 0,
               "null boltlz input is rejected");
}

/* ======================================================================== */
/* Adler-32 via bp_mini_checksum                                             */
/* ======================================================================== */

static void test_mini_checksum(void)
{
    bp_uint32 ck;

    UNIT_SECTION("Mini: bp_mini_checksum (Adler-32)");

    ck = bp_mini_checksum(1, (const bp_uint8 *)"", 0);
    UNIT_CHECK(ck == 0x00000001UL, "empty -> 0x00000001");

    ck = bp_mini_checksum(1, (const bp_uint8 *)"a", 1);
    UNIT_CHECK(ck == 0x00620062UL, "\"a\" -> 0x00620062");

    ck = bp_mini_checksum(1, (const bp_uint8 *)"ABC", 3);
    UNIT_CHECK(ck == 0x018D00C7UL, "\"ABC\" -> 0x018D00C7");

    /* incremental chaining */
    {
        const char *msg = "Hello!";
        bp_uint32 full;
        bp_uint32 part;
        full = bp_mini_checksum(1, (const bp_uint8 *)msg, 6);
        part = bp_mini_checksum(1, (const bp_uint8 *)msg, 3);
        part = bp_mini_checksum(part, (const bp_uint8 *)msg + 3, 3);
        UNIT_CHECK(full == part, "incremental split matches one-shot");
    }
}

/* ======================================================================== */
/* Roundtrip + checksum consistency                                          */
/* ======================================================================== */

static void test_checksum_consistency(void)
{
    static unsigned char src[1024];
    static unsigned char comp[MINI_BOLTLITE_DST(1024)];
    static unsigned char decomp[1024];
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    bp_uint32 ck_before;
    bp_uint32 ck_after;
    int i;
    int rc;

    UNIT_SECTION("Mini: Adler-32 consistency after BoltLite roundtrip");

    for (i = 0; i < 1024; i++) src[i] = (unsigned char)(i & 0xFF);
    ck_before = bp_mini_checksum(1, src, 1024);

    comp_len = (bp_uint32)sizeof(comp);
    rc = bp_mini_compress_block(src, 1024, comp, &comp_len, BP_METHOD_BOLTLITE);
    UNIT_CHECK(rc == BP_OK, "BoltLite compress 1 KB ok");

    decomp_len = 1024;
    rc = bp_mini_decompress_block(comp, comp_len, decomp, &decomp_len,
                                   BP_METHOD_BOLTLITE);
    UNIT_CHECK(rc == BP_OK, "BoltLite decompress ok");

    ck_after = bp_mini_checksum(1, decomp, decomp_len);
    UNIT_CHECK(ck_before == ck_after, "Adler-32 matches after roundtrip");
}

/* ======================================================================== */
/* main                                                                      */
/* ======================================================================== */

int main(void)
{
    int rc;

    printf("BoltPack mini unit tests: compress + checksum\n");

    rc = bp_mini_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "bp_mini_compress_init() failed: %d\n", rc);
        return 1;
    }

    test_stored();
    test_boltlite();
    test_boltlz();
    test_boltlz_decoder_invalid();
    test_fluff_unsupported();
    test_mini_checksum();
    test_checksum_consistency();
    return UNIT_REPORT();
}
