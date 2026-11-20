/*
 * boltlz_check.c -- BoltLZ encoder and decoder check for 68k and RISC OS
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 *
 * Reports compressed lengths and CRC-32s for both levels, decoded CRC-32s
 * and statuses for corrupted streams, round-trip failures, and timings.
 * Timings are 200 Hz ticks on TOS, centiseconds on RISC OS, and zero on
 * the QL and X68000.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boltlz.h"

#define MAXB 262144UL

#if defined(QDOS)
#define CHECK_OUTPUT "win1_boltlz_txt"
static long now(void) { return 0; }
#elif defined(__human68k__)
#define CHECK_OUTPUT "BOLTLZ.TXT"
static long now(void) { return 0; }
#elif defined(__riscos__)
#include <kernel.h>
#include <swis.h>
#define CHECK_OUTPUT "BOLTLZ"
#define TIMING_REPEATS 32
static long now(void)
{
    _kernel_swi_regs regs = {{0}};
    _kernel_swi(OS_ReadMonotonicTime, &regs, &regs);
    return regs.r[0];
}
#else
#include <osbind.h>
#define CHECK_OUTPUT "C:\\BOLTLZ.TXT"
static long ticks;
static long read_hz200(void) { ticks = *(volatile long *)0x4baL; return 0; }
static long now(void) { Supexec(read_hz200); return ticks; }
#endif

#ifndef TIMING_REPEATS
#define TIMING_REPEATS 1
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
#if defined(__riscos__)
    _kernel_swi_regs memory = {{0}};
    unsigned char *buf, *enc, *dec, *work;
    bp_uint32 work_size = boltlz_work_size(BOLTLZ_BALANCED);
    bp_uint32 enc_size = boltlz_compress_bound(MAXB);
#else
    unsigned char *buf = malloc(MAXB + 2), *enc = malloc(boltlz_compress_bound(MAXB)),
                  *dec = malloc(MAXB), *work = malloc(boltlz_work_size(BOLTLZ_BALANCED));
#endif
    unsigned int si, kind, level, odd;
    unsigned long n, failures = 0, cases = 0;
    bp_uint32 len;
    if (!out) return 1;
#if defined(__riscos__)
    /* Claim the fixture buffers in the RMA so it can run from a small
     * application slot, including when launched by RPCEmu HostCmd. */
    memory.r[0] = 6; /* OS_Module: claim */
    memory.r[3] = work_size + enc_size + MAXB * 2 + 2;
    if (_kernel_swi(OS_Module, &memory, &memory) != NULL) {
        fputs("alloc failed\n", out);
        fclose(out);
        return 1;
    }
    work = (unsigned char *)memory.r[2];
    buf = work + work_size;
    dec = buf + MAXB + 2;
    enc = dec + MAXB;
#else
    if (!buf || !enc || !dec || !work) { fputs("alloc failed\n", out); fclose(out); return 1; }
#endif
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
        unsigned int repeat;
        for (kind = 2; kind < 6; kind++) {
            seed = 99 + kind;
            fill(buf, 65536UL, (int)kind);
            start = now();
            for (repeat = 0; repeat < TIMING_REPEATS; repeat++) {
                len = boltlz_compress_bound(65536UL);
                if (boltlz_compress(buf, 65536UL, enc, &len, work, (int)level) != BP_OK)
                    failures++;
            }
            total += now() - start;
            start = now();
            for (repeat = 0; repeat < TIMING_REPEATS; repeat++)
                if (boltlz_decompress(enc, len, dec, 65536UL) != BP_OK) failures++;
#if !defined(__riscos__)
            if (memcmp(dec, buf, 65536UL) != 0) failures++;
#endif
            dtotal += now() - start;
#if defined(__riscos__)
            if (memcmp(dec, buf, 65536UL) != 0) failures++;
#endif
        }
        fprintf(out, "time level %u ticks %ld\n", level, total);
        fprintf(out, "dtime level %u ticks %ld\n", level, dtotal);
    }
    fprintf(out, "done cases %lu failures %lu\n", cases, failures);
    fclose(out);
#if defined(__riscos__)
    memory.r[0] = 7; /* OS_Module: free */
    memory.r[2] = (int)work;
    _kernel_swi(OS_Module, &memory, &memory);
#endif
    return 0;
}
