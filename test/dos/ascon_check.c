/*
 * ascon_check.c -- runs the strict C89 Ascon (bp_ascon_c89.c) against the NIST SP 800-232
 * known-answer files, on any host; built for 16-bit DOS with Open Watcom it is run in
 * DOSBox-X by test/check_dos_crypto.py.
 *
 *   ASCHK KAT.TXT XOF.TXT [seconds]      writes RESULT.TXT
 *   ASCHK - - seconds                    timing only (no known-answer files)
 *
 * KAT.TXT is an LWC AEAD KAT file (Count/Key/Nonce/PT/AD/CT), XOF.TXT an LWC XOF KAT
 * file (Count/Msg/MD).  After the checks it times Ascon-AEAD128 sealing of a 4 KiB buffer
 * for the given number of seconds of the C library clock (default 0 = no timing).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bp_ascon.h"
#include "bp_crypto.h"

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* parse the hex after "= " into buf; returns the byte count */
static unsigned parse_hex(const char *line, bp_uint8 *buf, unsigned max)
{
    unsigned n = 0;
    const char *p = strchr(line, '=');
    if (p == NULL) return 0;
    p++;
    while (*p == ' ') p++;
    while (hexval(p[0]) >= 0 && hexval(p[1]) >= 0 && n < max) {
        buf[n++] = (bp_uint8)(hexval(p[0]) * 16 + hexval(p[1]));
        p += 2;
    }
    return n;
}

static bp_uint8 key[16], nonce[16], pt[600], ad[600], ct[700], out[700], tag[16], md[300], msg[600];

int main(int argc, char **argv)
{
    FILE *f, *res;
    char line[1400];
    unsigned long aead_n = 0, aead_f = 0, xof_n = 0, xof_f = 0;
    unsigned ptl = 0, adl = 0, ctl = 0, have = 0;
    unsigned long seconds = argc > 3 ? strtoul(argv[3], NULL, 10) : 0UL;

    res = fopen("RESULT.TXT", "w");
    if (res == NULL) return 2;

    f = (argc > 1 && argv[1][0] == '-') ? NULL : fopen(argc > 1 ? argv[1] : "KAT.TXT", "r");
    if (f == NULL && !(argc > 1 && argv[1][0] == '-')) { fprintf(res, "cannot open the AEAD KAT file\n"); fclose(res); return 2; }
    while (f != NULL && fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "Key", 3) == 0) { parse_hex(line, key, 16); have |= 1; }
        else if (strncmp(line, "Nonce", 5) == 0) { parse_hex(line, nonce, 16); have |= 2; }
        else if (strncmp(line, "PT", 2) == 0) { ptl = parse_hex(line, pt, sizeof pt); have |= 4; }
        else if (strncmp(line, "AD", 2) == 0) { adl = parse_hex(line, ad, sizeof ad); have |= 8; }
        else if (strncmp(line, "CT", 2) == 0) { ctl = parse_hex(line, ct, sizeof ct); have |= 16; }
        if (have == 31) {
            have = 0;
            aead_n++;
            memset(out, 0, sizeof out);
            bp_ascon_aead_seal(key, nonce, ad, adl, pt, ptl, out, tag);
            if (ctl != ptl + 16 || memcmp(out, ct, ptl) != 0 || memcmp(tag, ct + ptl, 16) != 0) aead_f++;
            else {
                memset(out, 0xEE, sizeof out);
                if (bp_ascon_aead_open(key, nonce, ad, adl, ct, ptl, ct + ptl, out) != 0 || memcmp(out, pt, ptl) != 0) aead_f++;
                else {
                    tag[0] = (bp_uint8)(ct[ptl] ^ 1);        /* a flipped tag bit must be refused */
                    if (bp_ascon_aead_open(key, nonce, ad, adl, ct, ptl, tag, out) == 0) aead_f++;
                }
            }
        }
    }
    if (f != NULL) fclose(f);
    fprintf(res, "aead %lu records, %lu failures\n", aead_n, aead_f);

    f = (argc > 2 && argv[2][0] == '-') ? NULL : fopen(argc > 2 ? argv[2] : "XOF.TXT", "r");
    if (f == NULL && !(argc > 2 && argv[2][0] == '-')) { fprintf(res, "cannot open the XOF KAT file\n"); fclose(res); return 2; }
    {
        unsigned ml = 0, dl = 0, have2 = 0;
        while (f != NULL && fgets(line, sizeof line, f) != NULL) {
            if (strncmp(line, "Msg", 3) == 0) { ml = parse_hex(line, msg, sizeof msg); have2 |= 1; }
            else if (strncmp(line, "MD", 2) == 0) { dl = parse_hex(line, md, sizeof md); have2 |= 2; }
            if (have2 == 3) {
                bp_xof x;
                unsigned i, got;
                have2 = 0;
                xof_n++;
                memset(out, 0, 100);
                bp_xof_init(&x);
                for (i = 0; i < ml; i += 5) bp_xof_absorb(&x, msg + i, ml - i < 5 ? ml - i : 5);   /* chunked absorb */
                for (got = 0; got < dl; got += 13) bp_xof_squeeze(&x, out + got, dl - got < 13 ? dl - got : 13);   /* split squeeze */
                if (dl != 64 || memcmp(out, md, dl) != 0) xof_f++;
            }
        }
    }
    if (f != NULL) fclose(f);
    fprintf(res, "xof %lu records, %lu failures\n", xof_n, xof_f);

    if (seconds > 0) {
        static bp_uint8 buf[4096], obuf[4096];
        clock_t t0 = clock(), t;
        unsigned long iters = 0;
        memset(buf, 0x5A, sizeof buf);
        do {
            bp_ascon_aead_seal(key, nonce, NULL, 0, buf, sizeof buf, obuf, tag);
            nonce[0]++;
            iters++;
            t = clock();
        } while ((unsigned long)(t - t0) < seconds * (unsigned long)CLOCKS_PER_SEC);
        fprintf(res, "bench ascon-aead128 seal 4096 bytes: %lu iterations in %lu clock ticks (%lu per second): %lu bytes/s\n",
                iters, (unsigned long)(t - t0), (unsigned long)CLOCKS_PER_SEC,
                (iters * 4096UL * (unsigned long)CLOCKS_PER_SEC) / (unsigned long)(t - t0));
    }
    fprintf(res, "done failures %lu\n", aead_f + xof_f);
    fclose(res);
    return (aead_f + xof_f) ? 1 : 0;
}
