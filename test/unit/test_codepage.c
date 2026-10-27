/*
 * test_codepage.c -- unit tests for bp_local_to_utf8 / bp_utf8_to_local
 *
 * Tests encoding round-trips for ISO-8859-1, ISO-8859-2, CP437, CP850,
 * Windows-1252, Mac Roman, Atari ST, and Shift-JIS.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <string.h>
#include "test_runner.h"
#include "bp_types.h"
#include "bp_codepage.h"

#define OUT_SIZE 512

/* --- helpers ------------------------------------------------------------ */

static void check_local_to_utf8(const char *label,
                                  int codepage,
                                  const char *src_bytes, int src_len,
                                  const char *expected_utf8)
{
    char out[OUT_SIZE];
    char src[OUT_SIZE];
    int rc;

    memcpy(src, src_bytes, (size_t)src_len);
    src[src_len] = '\0';

    rc = bp_local_to_utf8(src, out, OUT_SIZE, codepage);
    if (rc != 0) {
        printf("  FAIL  %s: bp_local_to_utf8 returned %d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    if (memcmp(out, expected_utf8, strlen(expected_utf8) + 1) != 0) {
        printf("  FAIL  %s: got unexpected UTF-8 bytes  [%s:%d]\n",
               label, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    printf("  PASS  %s\n", label);
    bp_unit_pass_count++;
}

static void check_utf8_to_local(const char *label,
                                  int codepage,
                                  const char *utf8,
                                  const char *expected_bytes, int expected_len)
{
    char out[OUT_SIZE];
    int rc;

    rc = bp_utf8_to_local(utf8, out, OUT_SIZE, codepage);
    if (rc != 0) {
        printf("  FAIL  %s: bp_utf8_to_local returned %d  [%s:%d]\n",
               label, rc, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    if (memcmp(out, expected_bytes, (size_t)(expected_len + 1)) != 0) {
        printf("  FAIL  %s: got unexpected local bytes  [%s:%d]\n",
               label, __FILE__, __LINE__);
        bp_unit_fail_count++;
        return;
    }
    printf("  PASS  %s\n", label);
    bp_unit_pass_count++;
}

/* --- ASCII passthrough -------------------------------------------------- */

static void test_ascii_passthrough(void)
{
    const char *ascii = "Hello, World!";
    char out[OUT_SIZE];
    int codepages[] = {
        BP_CP_437, BP_CP_850, BP_CP_1252,
        BP_CP_MACROMAN, BP_CP_8859_1, BP_CP_ATARIST,
        BP_CP_SJIS, BP_CP_UTF8
    };
    const char *names[] = {
        "CP437", "CP850", "CP1252",
        "MacRoman", "ISO-8859-1", "AtariST",
        "Shift-JIS", "UTF-8"
    };
    int n = 8;
    int i;
    int rc;

    UNIT_SECTION("Codepage: ASCII passthrough (0x00-0x7F)");

    for (i = 0; i < n; i++) {
        rc = bp_local_to_utf8(ascii, out, OUT_SIZE, codepages[i]);
        if (rc == 0 && strcmp(out, ascii) == 0) {
            printf("  PASS  ASCII via %s\n", names[i]);
            bp_unit_pass_count++;
        } else {
            printf("  FAIL  ASCII via %s: rc=%d  [%s:%d]\n",
                   names[i], rc, __FILE__, __LINE__);
            bp_unit_fail_count++;
        }
    }
}

/* --- ISO-8859-1 --------------------------------------------------------- */

static void test_iso8859_1(void)
{
    /*
     * ISO-8859-1 byte 0xC9 = U+00C9 LATIN CAPITAL LETTER E WITH ACUTE.
     * UTF-8: 0xC3 0x89.
     */
    const char e_acute_local[2] = { (char)0xC9, '\0' };
    const char e_acute_utf8[3]  = { (char)0xC3, (char)0x89, '\0' };

    /*
     * ISO-8859-1 byte 0xFC = U+00FC LATIN SMALL LETTER U WITH DIAERESIS.
     * UTF-8: 0xC3 0xBC.
     */
    const char u_umlaut_local[2] = { (char)0xFC, '\0' };
    const char u_umlaut_utf8[3]  = { (char)0xC3, (char)0xBC, '\0' };

    /*
     * ISO-8859-1 byte 0xE5 = U+00E5 LATIN SMALL LETTER A WITH RING ABOVE.
     * UTF-8: 0xC3 0xA5.
     */
    const char a_ring_local[2] = { (char)0xE5, '\0' };
    const char a_ring_utf8[3]  = { (char)0xC3, (char)0xA5, '\0' };

    UNIT_SECTION("Codepage: ISO-8859-1 -> UTF-8");

    check_local_to_utf8("0xC9 (E acute) -> UTF-8",
                         BP_CP_8859_1, e_acute_local, 1, e_acute_utf8);
    check_local_to_utf8("0xFC (u umlaut) -> UTF-8",
                         BP_CP_8859_1, u_umlaut_local, 1, u_umlaut_utf8);
    check_local_to_utf8("0xE5 (a ring) -> UTF-8",
                         BP_CP_8859_1, a_ring_local, 1, a_ring_utf8);

    UNIT_SECTION("Codepage: UTF-8 -> ISO-8859-1");

    check_utf8_to_local("U+00C9 (E acute) -> 0xC9",
                         BP_CP_8859_1, e_acute_utf8, e_acute_local, 1);
    check_utf8_to_local("U+00FC (u umlaut) -> 0xFC",
                         BP_CP_8859_1, u_umlaut_utf8, u_umlaut_local, 1);
    check_utf8_to_local("U+00E5 (a ring) -> 0xE5",
                         BP_CP_8859_1, a_ring_utf8, a_ring_local, 1);
}

/* --- ISO-8859-1 round-trip ---------------------------------------------- */

static void test_iso8859_1_roundtrip(void)
{
    /*
     * Round-trip: local -> UTF-8 -> local must recover the original byte.
     * Test all bytes 0x80-0xFF that are valid ISO-8859-1.
     * In ISO-8859-1 all 256 byte values are defined, so all should round-trip.
     */
    int byte_val;
    int failed = 0;

    UNIT_SECTION("Codepage: ISO-8859-1 full round-trip 0x80-0xFF");

    for (byte_val = 0x80; byte_val <= 0xFF; byte_val++) {
        char local_in[2];
        char utf8_buf[8];
        char local_out[4];
        int rc1;
        int rc2;

        local_in[0] = (char)byte_val;
        local_in[1] = '\0';

        rc1 = bp_local_to_utf8(local_in, utf8_buf, sizeof(utf8_buf),
                                BP_CP_8859_1);
        rc2 = bp_utf8_to_local(utf8_buf, local_out, sizeof(local_out),
                                BP_CP_8859_1);

        if (rc1 != 0 || rc2 != 0 ||
            (unsigned char)local_out[0] != (unsigned char)byte_val) {
            failed++;
        }
    }
    UNIT_CHECK(failed == 0, "all 128 high bytes round-trip in ISO-8859-1");
}

/* --- CP437 -------------------------------------------------------------- */

static void test_cp437(void)
{
    /*
     * CP437 byte 0x84 = U+00E4 LATIN SMALL LETTER A WITH DIAERESIS.
     * UTF-8: 0xC3 0xA4.
     */
    const char ae_local[2] = { (char)0x84, '\0' };
    const char ae_utf8[3]  = { (char)0xC3, (char)0xA4, '\0' };

    UNIT_SECTION("Codepage: CP437 spot checks");
    check_local_to_utf8("CP437 0x84 (a-umlaut) -> UTF-8",
                         BP_CP_437, ae_local, 1, ae_utf8);
}

/* --- Windows-1252 ------------------------------------------------------- */

static void test_cp1252(void)
{
    /*
     * Windows-1252 byte 0x80 = U+20AC EURO SIGN.
     * UTF-8: 0xE2 0x82 0xAC.
     */
    const char euro_local[2] = { (char)0x80, '\0' };
    const char euro_utf8[4]  = { (char)0xE2, (char)0x82, (char)0xAC, '\0' };

    UNIT_SECTION("Codepage: Windows-1252 Euro sign");
    check_local_to_utf8("CP1252 0x80 (Euro) -> UTF-8",
                         BP_CP_1252, euro_local, 1, euro_utf8);
}

/* --- Shift-JIS: ASCII and half-width katakana --------------------------- */

static void test_sjis(void)
{
    /*
     * Shift-JIS byte 0xA1 = U+FF61 HALFWIDTH IDEOGRAPHIC FULL STOP.
     * UTF-8: 0xEF 0xBD 0xA1.
     */
    const char hw_period_local[2] = { (char)0xA1, '\0' };
    const char hw_period_utf8[4]  = { (char)0xEF, (char)0xBD, (char)0xA1,
                                       '\0' };

    /*
     * Shift-JIS byte 0xB1 = U+FF71 HALFWIDTH KATAKANA LETTER A.
     * UTF-8: 0xEF 0xBD 0xB1.
     */
    const char kata_a_local[2] = { (char)0xB1, '\0' };
    const char kata_a_utf8[4]  = { (char)0xEF, (char)0xBD, (char)0xB1, '\0' };

    UNIT_SECTION("Codepage: Shift-JIS half-width katakana");
    check_local_to_utf8("SJIS 0xA1 (hw full-stop) -> UTF-8",
                         BP_CP_SJIS, hw_period_local, 1, hw_period_utf8);
    check_local_to_utf8("SJIS 0xB1 (hw katakana A) -> UTF-8",
                         BP_CP_SJIS, kata_a_local, 1, kata_a_utf8);
}

/* --- UTF-8 outside target codepage ------------------------------------- */

static void test_out_of_range_substitution(void)
{
    const char cjk_utf8[4] = { (char)0xE4, (char)0xB8, (char)0xAD, '\0' };
    const char overlong[3] = { (char)0xC0, (char)0xAF, '\0' };
    char out[OUT_SIZE];
    int rc;

    UNIT_SECTION("Codepage: strict conversion errors");
    rc = bp_utf8_to_local(cjk_utf8, out, OUT_SIZE, BP_CP_8859_1);
    UNIT_CHECK(rc == BP_CP_UNREPRESENTABLE,
               "unmappable CJK is refused without substitution");
    rc = bp_utf8_to_local(overlong, out, OUT_SIZE, BP_CP_8859_1);
    UNIT_CHECK(rc == BP_CP_INVALID, "overlong UTF-8 is rejected");
    rc = bp_utf8_to_local("\xF0\x9F\x98\x80", out, OUT_SIZE, BP_CP_8859_1);
    UNIT_CHECK(rc == BP_CP_UNREPRESENTABLE,
               "valid supplementary character is classified as unmappable");
    rc = bp_local_to_utf8(overlong, out, OUT_SIZE, BP_CP_UTF8);
    UNIT_CHECK(rc == BP_CP_INVALID, "UTF-8 passthrough rejects malformed input");
}

/* --- Buffer too small --------------------------------------------------- */

static void test_buffer_too_small(void)
{
    /*
     * Destination buffer of size 1 (only room for NUL) must fail or truncate
     * when the input is non-empty.
     */
    const char src_local[2] = { (char)0xC9, '\0' }; /* E-acute in ISO-8859-1 */
    char tiny[1];
    int rc;

    UNIT_SECTION("Codepage: buffer too small");

    rc = bp_local_to_utf8(src_local, tiny, 1, BP_CP_8859_1);
    /*
     * The API reports a distinct result when the destination is too small.
     */
    UNIT_CHECK(rc == BP_CP_NO_SPACE,
               "tiny buffer returns the distinct no-space result");
}

/* --- Empty string ------------------------------------------------------- */

static void test_empty_string(void)
{
    char out[OUT_SIZE];
    int rc;

    UNIT_SECTION("Codepage: empty string");

    rc = bp_local_to_utf8("", out, OUT_SIZE, BP_CP_8859_1);
    UNIT_CHECK(rc == 0 && out[0] == '\0',
               "empty local -> empty UTF-8");

    rc = bp_utf8_to_local("", out, OUT_SIZE, BP_CP_8859_1);
    UNIT_CHECK(rc == 0 && out[0] == '\0',
               "empty UTF-8 -> empty local");
}

/* --- UTF-8 passthrough -------------------------------------------------- */

static void test_utf8_passthrough(void)
{
    /*
     * BP_CP_UTF8 (codepage 0) must copy the source verbatim in both
     * directions without modification.
     */
    const char *msg = "Hello \xC3\xA9\xE4\xB8\xAD"; /* "Hello é中" in UTF-8 */
    char out[OUT_SIZE];
    int rc;

    UNIT_SECTION("Codepage: BP_CP_UTF8 passthrough");

    rc = bp_local_to_utf8(msg, out, OUT_SIZE, BP_CP_UTF8);
    UNIT_CHECK(rc == 0 && strcmp(out, msg) == 0,
               "local_to_utf8 with BP_CP_UTF8 copies verbatim");

    rc = bp_utf8_to_local(msg, out, OUT_SIZE, BP_CP_UTF8);
    UNIT_CHECK(rc == 0 && strcmp(out, msg) == 0,
               "utf8_to_local with BP_CP_UTF8 copies verbatim");
}

/* --- main --------------------------------------------------------------- */

int main(void)
{
    printf("BoltPack unit tests: codepage conversion\n");

    test_ascii_passthrough();
    test_iso8859_1();
    test_iso8859_1_roundtrip();
    test_cp437();
    test_cp1252();
    test_sjis();
    test_out_of_range_substitution();
    test_buffer_too_small();
    test_empty_string();
    test_utf8_passthrough();

    return UNIT_REPORT();
}
