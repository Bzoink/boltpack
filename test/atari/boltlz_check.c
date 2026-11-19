/*
 * boltlz_check.c -- BoltLZ encoder and decoder check for 68k systems
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 *
 * Writes BOLTLZ.TXT (C:\ on Atari TOS, win1_ on the Sinclair QL): the length
 * and CRC-32 of every compressed stream for many generated inputs and both
 * levels, and the result code and output CRC of decoding many corrupted
 * streams, so builds with the C codec, with boltlz_m68k.S and from different
 * compilers can be compared byte for byte; plus round-trip failures and
 * compression and decompression times. On TOS the times are 200 Hz ticks of
 * emulated time; on the QL they are not measured (0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boltlz.h"

#define MAXB 262144UL

#if defined(QDOS)
#define CHECK_OUTPUT "win1_boltlz_txt"
static long now(void) { return 0; }
#else
#include <osbind.h>
#define CHECK_OUTPUT "C:\\BOLTLZ.TXT"
static long ticks;
static long read_hz200(void) { ticks = *(volatile long *)0x4baL; return 0; }
static long now(void) { Supexec(read_hz200); return ticks; }
#endif

static unsigned long seed;
static unsigned long rnd(void) { seed = seed * 1103515245UL + 12345UL; return (seed >> 8) & 0xFFFFFFUL; }

static unsigned long crc32(const unsigned char *p, unsigned long n)
{
    unsigned long c = 0xFFFFFFFFUL;
    int k;
    while (n--) { c ^= *p++; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (0UL - (c & 1))); }
    return c ^ 0xFFFFFFFFUL;
}

static void fill(unsigned char *d, unsigned long n, int kind)
{
    unsigned long i, j;
    static const char *words[] = { "the ", "BoltPack ", "Atari ", "archive ", "of ", "and ", "68000 ", "\n" };
    switch (kind) {
    case 0: memset(d, 0, n); break;
    case 1: for (i = 0; i < n; i++) d[i] = (unsigned char)rnd(); break;
    case 2: for (i = 0; i < n; ) { const char *w = words[rnd() & 7]; while (*w && i < n) d[i++] = (unsigned char)*w++; } break;
    case 3: for (i = 0; i < n; i++) d[i] = (unsigned char)("abc"[i % 3]); break;
    case 4: for (i = 0; i < n; i++) d[i] = (unsigned char)(i < 1000 ? rnd() : d[i - 1000]); break;
    case 5: /* random with copies at random distances and lengths */
        for (i = 0; i < n; ) {
            if (i > 16 && (rnd() & 1)) {
                unsigned long dist = 1 + rnd() % (i < 70000UL ? i : 70000UL), len = 1 + (rnd() % 300);
                for (j = 0; j < len && i < n; j++, i++) d[i] = d[i - dist];
            } else d[i++] = (unsigned char)(rnd() & 15);
        }
        break;
    default: for (i = 0; i < n; i++) d[i] = (unsigned char)(i & 1 ? 'x' : rnd() & 3); break;
    }
}

int main(void)
{
    static const unsigned long sizes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 18, 19, 20,
        31, 33, 63, 64, 65, 127, 128, 129, 1000, 4097, 16384, 65535, 65536, 65537, 200001, 262144 };
    FILE *out = fopen(CHECK_OUTPUT, "w");
    unsigned char *buf = malloc(MAXB + 2), *enc = malloc(boltlz_compress_bound(MAXB)),
                  *dec = malloc(MAXB), *work = malloc(boltlz_work_size(BOLTLZ_BALANCED));
    unsigned int si, kind, level, odd;
    unsigned long n, failures = 0, cases = 0;
    bp_uint32 len;
    if (!out) return 1;
    if (!buf || !enc || !dec || !work) { fputs("alloc failed\n", out); fclose(out); return 1; }
    for (kind = 0; kind < 7; kind++)
        for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++)
            for (odd = 0; odd < 2; odd++) {
                n = sizes[si];
                if (odd && n > 70000UL) continue;
                seed = kind * 7919UL + si;
                fill(buf + odd, n, (int)kind);
                for (level = 1; level <= 2; level++) {
                    len = boltlz_compress_bound(n);
                    cases++;
                    if (boltlz_compress(buf + odd, n, enc, &len, work, (int)level) != BP_OK ||
                        boltlz_decompress(enc, len, dec, n) != BP_OK ||
                        memcmp(dec, buf + odd, n) != 0) {
                        fprintf(out, "FAIL kind %u size %lu odd %u level %u\n", kind, n, odd, level);
                        failures++;
                        continue;
                    }
                    fprintf(out, "k%u n%lu o%u l%u len %lu crc %08lx\n", kind, n, odd, level,
                            (unsigned long)len, crc32(enc, len));
                }
            }
    {   /* Corrupted streams: the result code and every output byte must
         * not depend on the build. */
        static const unsigned long fsizes[] = { 300, 5000, 65536 };
        unsigned long f, trial, pos;
        int rc;
        for (f = 0; f < 3 * 3; f++) {
            n = fsizes[f % 3];
            seed = 7 + f;
            fill(buf, n, f / 3 == 0 ? 2 : f / 3 == 1 ? 5 : 4);
            len = boltlz_compress_bound(n);
            if (boltlz_compress(buf, n, enc, &len, work, BOLTLZ_BALANCED) != BP_OK) {
                fprintf(out, "FAIL fuzz source %lu\n", f); failures++; continue;
            }
            for (trial = 0; trial < 60; trial++) {
                unsigned char *bad = buf + MAXB / 2;
                bp_uint32 bad_len = len, out_len = (bp_uint32)n;
                memcpy(bad, enc, len);
                pos = 1 + rnd() % (len - 1);
                switch (trial % 6) {
                case 0: bad[pos] ^= (unsigned char)(1 << (rnd() & 7)); break;
                case 1: bad[pos] = (unsigned char)rnd(); break;
                case 2: bad_len = (bp_uint32)pos; break;
                case 3: bad[pos] = 0x80; bad[pos + 1 < len ? pos + 1 : pos] = 0x80; break;
                case 4: out_len = (bp_uint32)(n - 1 - rnd() % 16 % n); break;
                default: bad[pos] ^= 0x70; out_len = (bp_uint32)(n + 3); break;
                }
                memset(dec, 0xA5, (size_t)out_len);
                rc = boltlz_decompress(bad, bad_len, dec, out_len);
                fprintf(out, "fuzz %lu.%lu rc %d crc %08lx\n", f, trial, rc,
                        crc32(dec, out_len));
                cases++;
            }
        }
    }
    for (level = 1; level <= 2; level++) {
        /* Timing: four 64 KiB blocks of text-like and copy-heavy data. */
        long start, total = 0, dtotal = 0;
        for (kind = 2; kind < 6; kind++) {
            seed = 99 + kind;
            fill(buf, 65536UL, (int)kind);
            len = boltlz_compress_bound(65536UL);
            start = now();
            if (boltlz_compress(buf, 65536UL, enc, &len, work, (int)level) != BP_OK) failures++;
            total += now() - start;
            start = now();
            if (boltlz_decompress(enc, len, dec, 65536UL) != BP_OK ||
                memcmp(dec, buf, 65536UL) != 0) failures++;
            dtotal += now() - start;
        }
        fprintf(out, "time level %u ticks %ld\n", level, total);
        fprintf(out, "dtime level %u ticks %ld\n", level, dtotal);
    }
    fprintf(out, "done cases %lu failures %lu\n", cases, failures);
    fclose(out);
    return 0;
}
