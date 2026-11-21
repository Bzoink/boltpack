/* bp_crc32 against an independent bitwise CRC-32: every length, every
 * alignment, chained calls and a buffer longer than 65536 rounds of the
 * assembly loop. Meant for the m68k build with BP_CRC32_M68K_ASM (run it in
 * an emulator or on a 68k machine); on other CPUs it checks the C loop.
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License. */
#include "compress.h"
#include "test_runner.h"
#include <stdlib.h>
#include <string.h>

static bp_uint32 bitwise(bp_uint32 crc, const bp_uint8 *p, bp_uint32 n)
{
    bp_uint32 c = crc ^ 0xFFFFFFFFUL, i, j;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (j = 0; j < 8; j++) c = (c & 1U) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFUL;
}

static bp_uint32 rng = 0x2545F491UL;
static bp_uint32 rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

#define BIG 300007UL

int main(void)
{
    static bp_uint8 buf[BIG + 16];
    bp_uint8 *big = buf;
    bp_uint32 n, off, i, a, b, c;
    int ok;

    UNIT_SECTION("CRC-32 (m68k assembly when built with BP_CRC32_M68K_ASM)");
    for (i = 0; i < BIG + 16; i++) buf[i] = (bp_uint8)rnd();

    UNIT_CHECK(bp_crc32(0, (const bp_uint8 *)"123456789", 9) == 0xCBF43926UL,
               "known answer: 123456789");
    UNIT_CHECK(bp_crc32(0, (const bp_uint8 *)"The quick brown fox jumps over the lazy dog", 43) == 0x414FA339UL,
               "known answer: 43-byte pangram (assembly path)");

    ok = 1;
    for (n = 0; n <= 700 && ok; n++)
        for (off = 0; off < 8 && ok; off++)
            if (bp_crc32(0, buf + off, n) != bitwise(0, buf + off, n)) {
                printf("mismatch: length %lu offset %lu\n", (unsigned long)n, (unsigned long)off);
                ok = 0;
            }
    UNIT_CHECK(ok, "lengths 0..700 at 8 alignments match the bitwise CRC");

    ok = 1;
    for (n = 0; n < 200 && ok; n++) {
        bp_uint32 len = rnd() % 5000, o = rnd() % 8;
        bp_uint32 seed = rnd(), x, y, cut1, cut2;
        cut1 = len ? rnd() % (len + 1) : 0;
        cut2 = cut1 + (len > cut1 ? rnd() % (len - cut1 + 1) : 0);
        x = bitwise(seed, buf + o, len);
        y = bp_crc32(seed, buf + o, cut1);
        y = bp_crc32(y, buf + o + cut1, cut2 - cut1);
        y = bp_crc32(y, buf + o + cut2, len - cut2);
        if (x != y) { printf("chain mismatch: len %lu cuts %lu %lu\n", (unsigned long)len, (unsigned long)cut1, (unsigned long)cut2); ok = 0; }
    }
    UNIT_CHECK(ok, "200 random chained splits with random seeds");

    a = bp_crc32(0, big, BIG);
    b = bitwise(0, big, BIG);
    c = bp_crc32(0, big + 1, BIG - 1);
    UNIT_CHECK(a == b, "300007 bytes (over 65536 rounds) match");
    UNIT_CHECK(c == bitwise(0, big + 1, BIG - 1), "the same from an odd address");

    ok = 1;
    for (i = 0; i < 4; i++) {            /* all-ones, zeros, runs */
        static const bp_uint8 fill[4] = {0x00, 0xFF, 0x80, 0x01};
        memset(buf, fill[i], 4096);
        if (bp_crc32(0, buf, 4096) != bitwise(0, buf, 4096)) ok = 0;
    }
    UNIT_CHECK(ok, "constant-byte buffers");
    return UNIT_REPORT();
}
