/*
 * bp_entropy.c -- the entropy pool (plan section 7).  Strict C89.
 *
 * State: 32 bytes.  Adding input folds it in with a labelled Ascon-XOF128 call
 * tagged with the source and the length.  Output squeezes n + 32 bytes from the
 * state, a counter and (when the OS has a source) 32 fresh OS bytes; the first
 * n bytes are returned and the last 32 replace the state.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include <string.h>
#include <time.h>
#include "bp_entropy.h"
#include "bp_ascon.h"
#include "bp_crypto.h"

#define NSRC 6   /* source ids 1..5 */

/* customisation strings of Ascon-CXOF128, no terminator; the state is the key-like input */
static const bp_uint8 L_ADD[] = "bp8/pool/add";
static const bp_uint8 L_OUT[] = "bp8/pool/out";

static bp_uint8  g_state[32];
static bp_uint32 g_ctr_lo, g_ctr_hi;
static bp_uint32 g_credit[NSRC];
static int       g_os_ok;

#ifdef BP_CRYPTO_TESTING
static int (*g_os_fn)(bp_uint8 *buf, bp_uint32 n) = NULL;
static bp_uint32 g_out_count;
#endif

static bp_uint32 source_cap(int source)
{
    switch (source) {
    case BP_ENT_OS:     return 256;
    case BP_ENT_HW:     return 128;
    case BP_ENT_IMPORT: return 256;
    case BP_ENT_GATHER: return 64;
    default:            return 0;   /* weak data is never credited */
    }
}

static bp_uint32 g_import_fp[8][4];   /* fingerprints of imported buffers, so the same bytes earn credit once */
static int       g_import_n;

static bp_uint32 total_credit(void)
{
    bp_uint32 t = 0;
    int i;
    for (i = 1; i < NSRC; i++) t += g_credit[i];
    return t > 256 ? 256 : t;
}

/* Credit that can back a secret key: OS, hardware and imported bytes, never the
 * gathered timing (its estimate is unvalidated) and never weak data. */
static bp_uint32 strong_credit(void)
{
    bp_uint32 t = g_credit[BP_ENT_OS] + g_credit[BP_ENT_HW] + g_credit[BP_ENT_IMPORT];
    return t > 256 ? 256 : t;
}

bp_uint32 bp_entropy_credit(void)
{
    return total_credit();
}

#ifdef BP_CRYPTO_TESTING
static int g_os_present_override = -1;     /* -1: follow the platform file; 0 or 1: pretend */
#endif

/* Does this build have an OS random source at all?  Decided by the platform file, not by a read. */
static int os_present(void)
{
#ifdef BP_CRYPTO_TESTING
    if (g_os_present_override >= 0) return g_os_present_override;
    if (g_os_fn != NULL) return 1;
#endif
    return bp_entropy_os_present();
}

static int os_read(bp_uint8 *buf, bp_uint32 n)
{
#ifdef BP_CRYPTO_TESTING
    if (g_os_fn != NULL) return g_os_fn(buf, n);
#endif
    return bp_entropy_os_read(buf, n);
}

bp_uint32 bp_entropy_add(int source, const bp_uint8 *data, bp_uint32 len, bp_uint32 bits)
{
    bp_xof x;
    bp_uint8 meta[5], ns[32];
    bp_uint32 cap;

    if (source < 1 || source >= NSRC || (data == NULL && len != 0)) return total_credit();
    meta[0] = (bp_uint8)source;
    bp_write_le32(meta + 1, len);
    {   /* CXOF128(Z = "bp8/pool/add", M = state || source || len || data || 32 as a u32, 32 bytes) */
        bp_uint8 ol[4];
        bp_write_le32(ol, 32);
        bp_cxof_init(&x, L_ADD, (bp_uint32)sizeof L_ADD - 1);
        bp_xof_absorb(&x, g_state, 32);
        bp_xof_absorb(&x, meta, 5);
        bp_xof_absorb(&x, data, len);
        bp_xof_absorb(&x, ol, 4);
    }
    bp_xof_squeeze(&x, ns, 32);
    memcpy(g_state, ns, 32);
    bp_wipe(ns, sizeof ns);
    bp_wipe(&x, (bp_uint32)sizeof x);

    cap = source_cap(source);
    if (cap != 0 && bits != 0) {
        bp_uint32 c = g_credit[source];
        c = (bits > cap - c) ? cap : c + bits;
        g_credit[source] = c;
    }
    return total_credit();
}

bp_uint32 bp_entropy_import(const bp_uint8 *data, bp_uint32 len)
{
    /* Imported bytes are trusted seed material: the caller says they are random.
     * The same buffer is credited once, so importing a file twice (or in a loop)
     * cannot raise the credit; it is still mixed in. */
    bp_xof x;
    bp_uint32 fp[4];
    bp_uint8 fpb[16];
    int i, j, seen = 0, retained = 0;

    if (data == NULL && len != 0) return total_credit();
    bp_xof_init(&x);
    bp_xof_absorb(&x, data, len);
    bp_xof_squeeze(&x, fpb, 16);
    bp_wipe(&x, (bp_uint32)sizeof x);
    for (i = 0; i < 4; i++) fp[i] = bp_read_le32(fpb + 4 * i);
    for (i = 0; i < g_import_n; i++) {
        int same = 1;
        for (j = 0; j < 4; j++) if (g_import_fp[i][j] != fp[j]) same = 0;
        if (same) seen = 1;
    }
    /* A buffer earns credit only if its fingerprint can be kept, so that it cannot earn credit twice:
     * at most eight imports are credited; further ones are mixed in and earn nothing. */
    if (!seen && g_import_n < 8) { memcpy(g_import_fp[g_import_n], fp, sizeof fp); g_import_n++; retained = 1; }
    return bp_entropy_add(BP_ENT_IMPORT, data, len, retained ? (len > 32 ? 256U : len * 8U) : 0U);
}

void bp_entropy_add_weak(void)
{
    bp_uint8 buf[32];
    unsigned long v;
    int local = 0;
    static bp_uint32 counter;

    memset(buf, 0, sizeof buf);
    v = (unsigned long)time(NULL);
    bp_write_le32(buf, (bp_uint32)(v & 0xFFFFFFFFUL));
    v = (unsigned long)clock();
    bp_write_le32(buf + 4, (bp_uint32)(v & 0xFFFFFFFFUL));
    v = (unsigned long)(size_t)&local;
    bp_write_le32(buf + 8, (bp_uint32)(v & 0xFFFFFFFFUL));
    v = (unsigned long)(size_t)&g_state;
    bp_write_le32(buf + 12, (bp_uint32)(v & 0xFFFFFFFFUL));
    counter++;
    bp_write_le32(buf + 16, counter);
    bp_write_le32(buf + 20, g_ctr_lo);
    bp_entropy_add(BP_ENT_WEAK, buf, 32, 0);
}

bp_uint32 bp_entropy_gather(void)
{
    /* Timing jitter: the clock difference across a short busy loop whose length
     * varies.  One bit is declared for each sample whose delta differs from the
     * one before it; the source is capped at 64 bits.  A coarse clock earns
     * little credit, as it should. */
    bp_uint8 buf[130];
    volatile unsigned long sink = 0;
    clock_t prev, now;
    bp_uint32 d, last = 0, changed = 0;
    unsigned i;
    unsigned long j;

    prev = clock();
    for (i = 0; i < 128; i++) {
        for (j = 0; j < 40UL + (unsigned long)(i % 7) * 31UL; j++) sink += j ^ i;
        now = clock();
        d = (bp_uint32)(now - prev);
        prev = now;
        if (i != 0 && d != last) changed++;
        last = d;
        buf[i] = (bp_uint8)(d & 0xFF);
    }
    buf[128] = (bp_uint8)(sink & 0xFF);
    buf[129] = (bp_uint8)(time(NULL) & 0xFF);
    return bp_entropy_add(BP_ENT_GATHER, buf, 130, changed);
}

int bp_entropy_seed_os(void)
{
    bp_uint8 b[32];
    if (os_read(b, 32) != 0) { bp_wipe(b, sizeof b); return -1; }
    bp_entropy_add(BP_ENT_OS, b, 32, 256);
    bp_wipe(b, sizeof b);
    g_os_ok = 1;
    return 0;
}

static int draw(bp_uint8 *buf, bp_uint32 n, bp_uint32 need, int secret)
{
    bp_xof x;
    bp_uint8 fresh[32], ctr8[8], ns[32];
    int have_fresh = 0;

    if (buf == NULL && n != 0) return BP_CRYPTO_E_ARG;
    /* Where the build has an OS source it is required: seed from it (retrying on every call until it
     * works) and fail closed if it does not.  Only a build with no OS source runs on imported and
     * gathered credit. */
    if (os_present() && !g_os_ok && bp_entropy_seed_os() != 0) return BP_CRYPTO_E_ENTROPY;
    if ((secret ? strong_credit() : total_credit()) < need) return BP_CRYPTO_E_ENTROPY;

    /* Where the OS source seeded the pool, every output mixes 32 fresh OS bytes (a
     * forked process or a restored snapshot must not repeat earlier output).  If that
     * read fails the call fails closed and outputs nothing.  Where the OS never
     * worked (tiers 2 and 3) the pool runs on its imported and gathered credit. */
    if (g_os_ok) {
        if (os_read(fresh, 32) != 0) { bp_wipe(fresh, sizeof fresh); return BP_CRYPTO_E_ENTROPY; }
        have_fresh = 1;
    }
    bp_write_le32(ctr8, g_ctr_lo);
    bp_write_le32(ctr8 + 4, g_ctr_hi);
    {   /* CXOF128(Z = "bp8/pool/out", M = state || ctr || [fresh OS bytes] || (n + 32) as a u32, n + 32 bytes) */
        bp_uint8 ol[4];
        bp_write_le32(ol, n + 32);
        bp_cxof_init(&x, L_OUT, (bp_uint32)sizeof L_OUT - 1);
        bp_xof_absorb(&x, g_state, 32);
        bp_xof_absorb(&x, ctr8, 8);
        if (have_fresh) bp_xof_absorb(&x, fresh, 32);
        bp_xof_absorb(&x, ol, 4);
    }
    bp_xof_squeeze(&x, buf, n);
    bp_xof_squeeze(&x, ns, 32);
    memcpy(g_state, ns, 32);          /* replace the state */
    g_ctr_lo++;
    if (g_ctr_lo == 0) g_ctr_hi++;
#ifdef BP_CRYPTO_TESTING
    g_out_count += n;
#endif
    bp_wipe(ns, sizeof ns);
    bp_wipe(fresh, sizeof fresh);
    bp_wipe(&x, (bp_uint32)sizeof x);
    return BP_CRYPTO_OK;
}

int bp_random_bytes(bp_uint8 *buf, bp_uint32 n)
{
    return draw(buf, n, BP_ENT_SEEDED_BITS, 0);
}

int bp_random_secret(bp_uint8 *buf, bp_uint32 n)
{
    return draw(buf, n, BP_ENT_SECRET_BITS, 1);
}

#ifdef BP_CRYPTO_TESTING
void bp_entropy_test_reset(void)
{
    memset(g_state, 0, sizeof g_state);
    g_ctr_lo = g_ctr_hi = 0;
    memset(g_credit, 0, sizeof g_credit);
    memset(g_import_fp, 0, sizeof g_import_fp);
    g_import_n = 0;
    g_os_ok = 0;
    g_os_present_override = -1;
    g_out_count = 0;
}

void bp_entropy_test_set_os(int (*fn)(bp_uint8 *buf, bp_uint32 n))
{
    g_os_fn = fn;
}

void bp_entropy_test_set_os_present(int present)
{
    g_os_present_override = present;
}

bp_uint32 bp_entropy_test_output_count(void)
{
    return g_out_count;
}
#endif
