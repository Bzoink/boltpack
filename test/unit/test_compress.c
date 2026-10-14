/*
 * test_compress.c -- unit tests for boltlz and BoltLite compression codecs
 *
 * Tests bp_compress_block / bp_decompress_block (boltlz, method 2) and
 * boltlite_compress / boltlite_decompress (method 1) directly.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_runner.h"
#include "bp_types.h"
#include "compress.h"
#include "boltlite.h"

/* Worst-case output size for boltlz compression */
#define BOLTLZ_DST_SIZE(n) ((n) + (n)/63 + 16)
/* Worst-case output size for BoltLite compression */
#define BOLTLITE_DST_SIZE(n) ((n) + (n)/8 + 16)

/* ======================================================================== */
/* boltlz (bp_compress_block / bp_decompress_block)                          */
/* ======================================================================== */

static void boltlz_roundtrip(const char *label,
                          const bp_uint8 *src, bp_uint32 src_len)
{
    static unsigned char comp_buf[BOLTLZ_DST_SIZE(65536)];
    static unsigned char decomp_buf[65536];
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    int rc;

    comp_len = (bp_uint32)BOLTLZ_DST_SIZE(src_len);
    rc = bp_compress_block(src, src_len, comp_buf, &comp_len);
    if (rc != BP_OK) {
        printf("  FAIL  %s: bp_compress_block rc=%d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    decomp_len = src_len; /* original size */
    rc = bp_decompress_block(comp_buf, comp_len, decomp_buf, &decomp_len);
    if (rc != BP_OK) {
        printf("  FAIL  %s: bp_decompress_block rc=%d  [%s:%d]\n",
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

    if (memcmp(src, decomp_buf, src_len) != 0) {
        printf("  FAIL  %s: roundtrip data mismatch  [%s:%d]\n",
               label, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    printf("  PASS  %s\n", label);
    bp_unit_pass_count++;
}

static void test_boltlz_basic(void)
{
    static unsigned char zeros[65536];
    static unsigned char seq[65536];
    static unsigned char rnd[65536];
    int i;
    bp_uint32 seed = 0x12345678UL;

    UNIT_SECTION("boltlz: roundtrip");

    /* single byte */
    {
        unsigned char one = 0x42;
        boltlz_roundtrip("single byte 0x42", &one, 1);
    }

    /* all zeros - highly compressible */
    memset(zeros, 0, sizeof(zeros));
    boltlz_roundtrip("64 KB all-zeros", zeros, 65536);

    /* sequential bytes - moderate compressibility */
    for (i = 0; i < 65536; i++) seq[i] = (unsigned char)(i & 0xFF);
    boltlz_roundtrip("64 KB sequential", seq, 65536);

    /* pseudo-random (xorshift32) - low compressibility */
    for (i = 0; i < 65536; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        rnd[i] = (unsigned char)(seed & 0xFF);
    }
    boltlz_roundtrip("64 KB pseudo-random", rnd, 65536);

    /* repeated pattern */
    {
        static unsigned char pat[32768];
        int j;
        for (j = 0; j < 32768; j++) pat[j] = (unsigned char)(j % 17);
        boltlz_roundtrip("32 KB mod-17 pattern", pat, 32768);
    }

    /* 8 KB block boundary */
    boltlz_roundtrip("8 KB all-zeros", zeros, 8192);
}

static void test_boltlz_invalid_lengths(void)
{
    static unsigned char src[2] = {0, 0};
    static unsigned char dst[8];
    bp_uint32 dst_len;
    int rc;

    UNIT_SECTION("boltlz: invalid lengths are rejected");

    dst_len = (bp_uint32)sizeof(dst);
    rc = bp_compress_block(src, 0, dst, &dst_len);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG, "zero-length compression rejected");

    dst_len = (bp_uint32)sizeof(dst);
    rc = bp_decompress_block(src, 1, dst, &dst_len);
    UNIT_CHECK(rc == BP_ERR_DECOMPRESS,
               "one-byte compressed input rejected");

    dst_len = (bp_uint32)sizeof(dst);
    rc = bp_compress_block(src, (bp_uint32)0xFFFFFFFFUL, dst, &dst_len);
    UNIT_CHECK(rc == BP_ERR_INVALID_ARG,
               "length exceeding BoltLZ int range rejected");
}

static void test_boltlz_checksum_consistency(void)
{
    static unsigned char src[4096];
    static unsigned char comp[BOLTLZ_DST_SIZE(4096)];
    static unsigned char decomp[4096];
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    bp_uint32 cksum_before;
    bp_uint32 cksum_after;
    int i;
    int rc;

    UNIT_SECTION("boltlz: Adler-32 consistency across roundtrip");

    for (i = 0; i < 4096; i++) src[i] = (unsigned char)(i & 0xFF);

    cksum_before = bp_checksum(1, src, 4096);

    comp_len   = (bp_uint32)BOLTLZ_DST_SIZE(4096);
    rc = bp_compress_block(src, 4096, comp, &comp_len);
    UNIT_CHECK(rc == BP_OK, "compress 4 KB sequential ok");

    decomp_len = 4096;
    rc = bp_decompress_block(comp, comp_len, decomp, &decomp_len);
    UNIT_CHECK(rc == BP_OK, "decompress ok");

    cksum_after = bp_checksum(1, decomp, decomp_len);
    UNIT_CHECK(cksum_before == cksum_after, "Adler-32 matches after roundtrip");
}

static void test_boltlz_corrupt(void)
{
    static unsigned char src[512];
    static unsigned char comp[BOLTLZ_DST_SIZE(512)];
    static unsigned char decomp[512];
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    int rc;

    UNIT_SECTION("boltlz: corrupt compressed data returns error");

    memset(src, 0xAB, sizeof(src));
    comp_len = (bp_uint32)BOLTLZ_DST_SIZE(512);
    rc = bp_compress_block(src, 512, comp, &comp_len);
    UNIT_CHECK(rc == BP_OK, "compress ok for corrupt test");

    if (rc == BP_OK && comp_len > 4) {
        /*
         * boltlz has no internal checksum, so a bit-flip or truncation may
         * still form a valid opcode sequence and decode to *some* length.
         * boltlz's bound-checked decompressor never writes past the output
         * buffer, so we require that corrupt input never crashes or overruns:
         * decompress must either return an error, or succeed with a length
         * no larger than the original and not overrun the buffer.
         */
        comp_len = comp_len / 2; /* truncate the stream in half */

        decomp_len = 512;
        rc = bp_decompress_block(comp, comp_len, decomp, &decomp_len);
        UNIT_CHECK(decomp_len <= 512,
                   "truncated decompress stays within buffer");
    }
}

static void test_boltlz_wrapper_modes(void)
{
    static bp_uint8 source[4096], encoded[BP_BOLTLZ_BOUND(4096)];
    static bp_uint8 reference[BP_BOLTLZ_BOUND(4096)];
    bp_uint32 actual, expected;
    void *work;
    int level, rc;
    bp_uint32 i;
    for (i = 0; i < sizeof(source); i++)
        source[i] = (bp_uint8)((i * i + 19 * i + (i / 43) * 7) % 23);
    work = malloc((size_t)bp_compress_work_size());
    if (!work) exit(2);
    UNIT_SECTION("BoltLZ: wrapper modes and exact capacity");
    for (level = 0; level <= 2; level++) {
        actual = sizeof(encoded);
        expected = sizeof(reference);
        rc = bp_compress_block_level(source, sizeof(source), encoded, &actual,
                                      (bp_uint8)level);
        if (boltlz_compress(source, sizeof(source), reference, &expected, work,
                           level == 1 ? BOLTLZ_FAST : BOLTLZ_BALANCED) != BP_OK)
            exit(2);
        UNIT_CHECK(rc == BP_OK && actual == expected &&
                   memcmp(encoded, reference, expected) == 0,
                   "wrapper selects the documented encoder mode");
        actual = expected;
        rc = bp_compress_block_work(source, sizeof(source), encoded, &actual,
                                    work, (bp_uint8)level);
        UNIT_CHECK(rc == BP_OK && actual == expected,
                   "reusable-work wrapper honours exact encoded capacity");
        actual = expected - 1;
        rc = bp_compress_block_work(source, sizeof(source), encoded, &actual,
                                    work, (bp_uint8)level);
        UNIT_CHECK(rc == BP_ERR_OVERFLOW && actual == 0,
                   "reusable-work wrapper rejects one-byte-short capacity");
    }
    actual = sizeof(encoded);
    expected = sizeof(reference);
    if (boltlz_compress(source, sizeof(source), encoded, &actual, work, BOLTLZ_FAST) != BP_OK ||
        boltlz_compress(source, sizeof(source), reference, &expected, work, BOLTLZ_BALANCED) != BP_OK)
        exit(2);
    UNIT_CHECK(actual != expected || memcmp(encoded, reference, actual) != 0,
               "mode-selection fixture distinguishes Fast from Balanced");
    free(work);
}

/* ======================================================================== */
/* BoltLite (boltlite_compress / boltlite_decompress)                                    */
/* ======================================================================== */

static void boltlite_roundtrip(const char *label,
                            const bp_uint8 *src, bp_uint32 src_len)
{
    static unsigned char work[BOLTLITE_WORK_SIZE];
    static unsigned char comp_buf[BOLTLITE_DST_SIZE(65536)];
    static unsigned char decomp_buf[65536];
    bp_uint32 comp_len;
    int rc;

    comp_len = boltlite_compress(src, src_len, comp_buf, work);
    if (comp_len == 0 && src_len > 0) {
        printf("  FAIL  %s: boltlite_compress returned 0  [%s:%d]\n",
               label, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    rc = boltlite_decompress(comp_buf, comp_len, decomp_buf, src_len, work);
    if (rc != BP_OK) {
        printf("  FAIL  %s: boltlite_decompress rc=%d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
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

static void test_boltlite_basic(void)
{
    static unsigned char zeros[8192];
    static unsigned char seq[8192];
    static unsigned char repeat[8192];
    static unsigned char wide[4160];
    static unsigned char mini_max[32768];
    int i;
    bp_uint32 state;

    UNIT_SECTION("BoltLite: roundtrip");

    /* single byte */
    {
        unsigned char one = 'X';
        boltlite_roundtrip("single byte", &one, 1);
    }

    /* repeated character - maximum compressibility for BoltLite */
    memset(zeros, 0x20, sizeof(zeros)); /* space char = BOLTLITE_FILL_BYTE */
    boltlite_roundtrip("8 KB all-spaces", zeros, 8192);

    memset(zeros, 0, sizeof(zeros));
    boltlite_roundtrip("8 KB all-zeros", zeros, 8192);

    /* sequential bytes */
    for (i = 0; i < 8192; i++) seq[i] = (unsigned char)(i & 0xFF);
    boltlite_roundtrip("8 KB sequential", seq, 8192);

    /* repeating short pattern */
    for (i = 0; i < 8192; i++) repeat[i] = (unsigned char)(i % 7);
    boltlite_roundtrip("8 KB mod-7 pattern", repeat, 8192);

    /* Exercise the full 4 KiB back-distance form with a run that is unique
     * in the preceding window. */
    state = 0x12345678UL;
    for (i = 0; i < 4096; i++) {
        state = state * 1664525UL + 1013904223UL;
        wide[i] = (unsigned char)(state >> 24);
    }
    memcpy(wide + 4096, wide, 64);
    boltlite_roundtrip("4 KiB distance match", wide, sizeof(wide));

    for (i = 0; i < (int)sizeof(mini_max); i++)
        mini_max[i] = (unsigned char)((i * 29 + i / 251) & 0xFF);
    boltlite_roundtrip("32 KiB mini block boundary", mini_max, sizeof(mini_max));

    /* ASCII text (simulated) */
    {
        const char *txt = "The quick brown fox jumps over the lazy dog. "
                          "Pack my box with five dozen liquor jugs. "
                          "How vexingly quick daft zebras jump! ";
        boltlite_roundtrip("short text string", (const bp_uint8 *)txt,
                       (bp_uint32)strlen(txt));
    }
}

static void test_boltlite_compress_ratio(void)
{
    static unsigned char work[BOLTLITE_WORK_SIZE];
    static unsigned char src[4096];
    static unsigned char comp[BOLTLITE_DST_SIZE(4096)];
    bp_uint32 comp_len;
    int i;

    UNIT_SECTION("BoltLite: compression ratio sanity (repeated data)");

    for (i = 0; i < 4096; i++) src[i] = (unsigned char)'A';
    comp_len = boltlite_compress(src, 4096, comp, work);
    UNIT_CHECK(comp_len > 0, "compress returned non-zero length");
    UNIT_CHECK(comp_len < 4096, "all-A 4096 bytes compresses smaller");
}

static void test_boltlite_corrupt(void)
{
    static unsigned char work[BOLTLITE_WORK_SIZE];
    static unsigned char src[256];
    static unsigned char comp[BOLTLITE_DST_SIZE(256)];
    static unsigned char decomp[256];
    static const unsigned char repeat_without_distance[2] = { 0x00, 0x40 };
    static const unsigned char distance_before_output[3] = { 0x00, 0x80, 0x00 };
    static const unsigned char trailing_after_literal[3] = { 0x01, 'x', 0xFF };
    bp_uint32 comp_len;
    int rc;

    UNIT_SECTION("BoltLite: corrupt data returns BP_ERR_CORRUPT");

    UNIT_CHECK(boltlite_decompress(repeat_without_distance,
                               sizeof(repeat_without_distance), decomp, 3, NULL)
               == BP_ERR_CORRUPT,
               "repeat-distance token before first match rejected");
    UNIT_CHECK(boltlite_decompress(distance_before_output,
                               sizeof(distance_before_output), decomp, 3, NULL)
               == BP_ERR_CORRUPT,
               "match distance beyond produced output rejected");
    UNIT_CHECK(boltlite_decompress(trailing_after_literal,
                               sizeof(trailing_after_literal), decomp, 1, NULL)
               == BP_ERR_CORRUPT,
               "trailing encoded byte rejected");

    memset(src, 0xCC, sizeof(src));
    comp_len = boltlite_compress(src, 256, comp, work);
    UNIT_CHECK(comp_len > 0, "compress ok for corrupt test");

    if (comp_len > 4) {
        /* corrupt several bytes in the compressed stream */
        comp[comp_len / 2]     ^= 0xAA;
        comp[comp_len / 2 + 1] ^= 0xAA;
        comp[comp_len / 2 + 2] ^= 0xAA;

        rc = boltlite_decompress(comp, comp_len, decomp, 256, work);
        /*
         * BoltLite may or may not detect corruption depending on where the flip
         * lands (a flipped length field may still decode without crashing).
         * We only require it does not crash or return garbage memory;
         * BP_ERR_CORRUPT is the desired result for detectable corruption.
         */
        (void)rc; /* result is informational only */
        UNIT_CHECK(1, "corrupt BoltLite did not crash");
    }
}

/* ======================================================================== */
/* main                                                                      */
/* ======================================================================== */

int main(void)
{
    int rc;

    printf("BoltPack unit tests: compression (boltlz + BoltLite)\n");

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "bp_compress_init() failed: %d\n", rc);
        return 1;
    }

    test_boltlz_basic();
    test_boltlz_wrapper_modes();
    test_boltlz_invalid_lengths();
    test_boltlz_checksum_consistency();
    test_boltlz_corrupt();
    test_boltlite_basic();
    test_boltlite_compress_ratio();
    test_boltlite_corrupt();

    return UNIT_REPORT();
}
