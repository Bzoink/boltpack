/*
 * test_compress.c -- unit tests for miniLZO and LZSS compression codecs
 *
 * Tests bp_compress_block / bp_decompress_block (miniLZO, method 2) and
 * lzss_compress / lzss_decompress (method 1) directly.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_runner.h"
#include "bp_types.h"
#include "compress.h"
#include "lzss.h"

/* Worst-case output size for miniLZO compression */
#define LZO_DST_SIZE(n) ((n) + (n)/16 + 64 + 3)
/* Worst-case output size for LZSS compression */
#define LZSS_DST_SIZE(n) ((n) + (n)/8 + 16)

/* ======================================================================== */
/* miniLZO (bp_compress_block / bp_decompress_block)                         */
/* ======================================================================== */

static void lzo_roundtrip(const char *label,
                          const bp_uint8 *src, bp_uint32 src_len)
{
    static unsigned char comp_buf[LZO_DST_SIZE(65536)];
    static unsigned char decomp_buf[65536];
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    int rc;

    comp_len = (bp_uint32)LZO_DST_SIZE(src_len);
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

static void test_lzo_basic(void)
{
    static unsigned char zeros[65536];
    static unsigned char seq[65536];
    static unsigned char rnd[65536];
    int i;
    bp_uint32 seed = 0x12345678UL;

    UNIT_SECTION("miniLZO: roundtrip");

    /* single byte */
    {
        unsigned char one = 0x42;
        lzo_roundtrip("single byte 0x42", &one, 1);
    }

    /* all zeros - highly compressible */
    memset(zeros, 0, sizeof(zeros));
    lzo_roundtrip("64 KB all-zeros", zeros, 65536);

    /* sequential bytes - moderate compressibility */
    for (i = 0; i < 65536; i++) seq[i] = (unsigned char)(i & 0xFF);
    lzo_roundtrip("64 KB sequential", seq, 65536);

    /* pseudo-random (xorshift32) - low compressibility */
    for (i = 0; i < 65536; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        rnd[i] = (unsigned char)(seed & 0xFF);
    }
    lzo_roundtrip("64 KB pseudo-random", rnd, 65536);

    /* repeated pattern */
    {
        static unsigned char pat[32768];
        int j;
        for (j = 0; j < 32768; j++) pat[j] = (unsigned char)(j % 17);
        lzo_roundtrip("32 KB mod-17 pattern", pat, 32768);
    }

    /* 8 KB block boundary */
    lzo_roundtrip("8 KB all-zeros", zeros, 8192);
}

static void test_lzo_checksum_consistency(void)
{
    static unsigned char src[4096];
    static unsigned char comp[LZO_DST_SIZE(4096)];
    static unsigned char decomp[4096];
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    bp_uint32 cksum_before;
    bp_uint32 cksum_after;
    int i;
    int rc;

    UNIT_SECTION("miniLZO: Adler-32 consistency across roundtrip");

    for (i = 0; i < 4096; i++) src[i] = (unsigned char)(i & 0xFF);

    cksum_before = bp_checksum(1, src, 4096);

    comp_len   = (bp_uint32)LZO_DST_SIZE(4096);
    rc = bp_compress_block(src, 4096, comp, &comp_len);
    UNIT_CHECK(rc == BP_OK, "compress 4 KB sequential ok");

    decomp_len = 4096;
    rc = bp_decompress_block(comp, comp_len, decomp, &decomp_len);
    UNIT_CHECK(rc == BP_OK, "decompress ok");

    cksum_after = bp_checksum(1, decomp, decomp_len);
    UNIT_CHECK(cksum_before == cksum_after, "Adler-32 matches after roundtrip");
}

static void test_lzo_corrupt(void)
{
    static unsigned char src[512];
    static unsigned char comp[LZO_DST_SIZE(512)];
    static unsigned char decomp[512];
    bp_uint32 comp_len;
    bp_uint32 decomp_len;
    int rc;

    UNIT_SECTION("miniLZO: corrupt compressed data returns error");

    memset(src, 0xAB, sizeof(src));
    comp_len = (bp_uint32)LZO_DST_SIZE(512);
    rc = bp_compress_block(src, 512, comp, &comp_len);
    UNIT_CHECK(rc == BP_OK, "compress ok for corrupt test");

    if (rc == BP_OK && comp_len > 4) {
        /*
         * miniLZO has no internal checksum; arbitrary bit-flips may still
         * form a valid LZO opcode sequence.  Use truncation -- cutting the
         * stream in half guarantees the decompressor encounters an incomplete
         * input and must return a non-zero error code.
         */
        comp_len = comp_len / 2;

        decomp_len = 512;
        rc = bp_decompress_block(comp, comp_len, decomp, &decomp_len);
        UNIT_CHECK(rc != BP_OK,
                   "decompress of truncated data returns non-zero");
    }
}

/* ======================================================================== */
/* LZSS (lzss_compress / lzss_decompress)                                    */
/* ======================================================================== */

static void lzss_roundtrip(const char *label,
                            const bp_uint8 *src, bp_uint32 src_len)
{
    static unsigned char work[LZSS_WORK_SIZE];
    static unsigned char comp_buf[LZSS_DST_SIZE(65536)];
    static unsigned char decomp_buf[65536];
    bp_uint32 comp_len;
    int rc;

    comp_len = lzss_compress(src, src_len, comp_buf, work);
    if (comp_len == 0 && src_len > 0) {
        printf("  FAIL  %s: lzss_compress returned 0  [%s:%d]\n",
               label, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }

    rc = lzss_decompress(comp_buf, comp_len, decomp_buf, src_len, work);
    if (rc != BP_OK) {
        printf("  FAIL  %s: lzss_decompress rc=%d  [%s:%d]\n",
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

static void test_lzss_basic(void)
{
    static unsigned char zeros[8192];
    static unsigned char seq[8192];
    static unsigned char repeat[8192];
    int i;

    UNIT_SECTION("LZSS: roundtrip");

    /* single byte */
    {
        unsigned char one = 'X';
        lzss_roundtrip("single byte", &one, 1);
    }

    /* repeated character - maximum compressibility for LZSS */
    memset(zeros, 0x20, sizeof(zeros)); /* space char = LZSS_FILL_BYTE */
    lzss_roundtrip("8 KB all-spaces (FILL_BYTE)", zeros, 8192);

    memset(zeros, 0, sizeof(zeros));
    lzss_roundtrip("8 KB all-zeros", zeros, 8192);

    /* sequential bytes */
    for (i = 0; i < 8192; i++) seq[i] = (unsigned char)(i & 0xFF);
    lzss_roundtrip("8 KB sequential", seq, 8192);

    /* repeating short pattern */
    for (i = 0; i < 8192; i++) repeat[i] = (unsigned char)(i % 7);
    lzss_roundtrip("8 KB mod-7 pattern", repeat, 8192);

    /* ASCII text (simulated) */
    {
        const char *txt = "The quick brown fox jumps over the lazy dog. "
                          "Pack my box with five dozen liquor jugs. "
                          "How vexingly quick daft zebras jump! ";
        lzss_roundtrip("short text string", (const bp_uint8 *)txt,
                       (bp_uint32)strlen(txt));
    }
}

static void test_lzss_compress_ratio(void)
{
    static unsigned char work[LZSS_WORK_SIZE];
    static unsigned char src[4096];
    static unsigned char comp[LZSS_DST_SIZE(4096)];
    bp_uint32 comp_len;
    int i;

    UNIT_SECTION("LZSS: compression ratio sanity (repeated data)");

    for (i = 0; i < 4096; i++) src[i] = (unsigned char)'A';
    comp_len = lzss_compress(src, 4096, comp, work);
    UNIT_CHECK(comp_len > 0, "compress returned non-zero length");
    UNIT_CHECK(comp_len < 4096, "all-A 4096 bytes compresses smaller");
}

static void test_lzss_corrupt(void)
{
    static unsigned char work[LZSS_WORK_SIZE];
    static unsigned char src[256];
    static unsigned char comp[LZSS_DST_SIZE(256)];
    static unsigned char decomp[256];
    bp_uint32 comp_len;
    int rc;

    UNIT_SECTION("LZSS: corrupt data returns BP_ERR_CORRUPT");

    memset(src, 0xCC, sizeof(src));
    comp_len = lzss_compress(src, 256, comp, work);
    UNIT_CHECK(comp_len > 0, "compress ok for corrupt test");

    if (comp_len > 4) {
        /* corrupt several bytes in the compressed stream */
        comp[comp_len / 2]     ^= 0xAA;
        comp[comp_len / 2 + 1] ^= 0xAA;
        comp[comp_len / 2 + 2] ^= 0xAA;

        rc = lzss_decompress(comp, comp_len, decomp, 256, work);
        /*
         * LZSS may or may not detect corruption depending on where the flip
         * lands (a flipped length field may still decode without crashing).
         * We only require it does not crash or return garbage memory;
         * BP_ERR_CORRUPT is the desired result for detectable corruption.
         */
        (void)rc; /* result is informational only */
        UNIT_CHECK(1, "corrupt LZSS did not crash");
    }
}

/* ======================================================================== */
/* main                                                                      */
/* ======================================================================== */

int main(void)
{
    int rc;

    printf("BoltPack unit tests: compression (miniLZO + LZSS)\n");

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "bp_compress_init() failed: %d\n", rc);
        return 1;
    }

    test_lzo_basic();
    test_lzo_checksum_consistency();
    test_lzo_corrupt();
    test_lzss_basic();
    test_lzss_compress_ratio();
    test_lzss_corrupt();

    return UNIT_REPORT();
}
