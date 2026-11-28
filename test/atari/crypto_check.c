/*
 * crypto_check.c -- Atari TOS program: checks the crypto layer (src/core/crypto) against
 * known answers and golden frames on a big-endian 68k, then times it.  Built and run by
 * test/check_hatari_crypto.py, which generates crypto_vectors.h from the repository's
 * reference files.  Output goes to C:\CRYPTO.TXT.  Timings are 200 Hz ticks of emulated time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <osbind.h>
#include "bp_crypto.h"
#include "bp_ascon.h"
#include "bp_entropy.h"
#include "crypto_vectors.h"

#ifndef ARGON_KIB
#define ARGON_KIB 8192UL
#endif
#ifndef MIN_TICKS
#define MIN_TICKS 1000L          /* each timed loop runs at least 5 seconds of emulated time */
#endif

static long ticks;
static long read_hz200(void) { ticks = *(volatile long *)0x4baL; return 0; }
static long now(void) { Supexec(read_hz200); return ticks; }

static FILE *out;
static unsigned long failures;

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static unsigned hex2bin(const char *s, bp_uint8 *o)
{
    unsigned n = 0;
    while (hexv(s[0]) >= 0 && hexv(s[1]) >= 0) { o[n++] = (bp_uint8)(hexv(s[0]) * 16 + hexv(s[1])); s += 2; }
    return n;
}

static bp_uint8 E_fixed[32];
static int E_hook(bp_uint8 E[32]) { memcpy(E, E_fixed, 32); return 0; }

static bp_uint8 k[64], n[64], ad[512], pt[512], ct[600], o[600], tg[16], m[600];

#define DOK(c, what) do { if (!(c)) { failures++; fprintf(out, "FAIL %s\n", what); } } while (0)

static void kats(void)
{
    unsigned i;
    unsigned long f0;
    for (f0 = failures, i = 0; i < N_AEAD; i++) {
        unsigned al = hex2bin(AEAD_KATS[i].ad, ad), pl = hex2bin(AEAD_KATS[i].pt, pt), cl = hex2bin(AEAD_KATS[i].ct, ct);
        hex2bin(AEAD_KATS[i].key, k); hex2bin(AEAD_KATS[i].nonce, n);
        bp_ascon_aead_seal(k, n, ad, al, pt, pl, o, tg);
        DOK(cl == pl + 16 && memcmp(o, ct, pl) == 0 && memcmp(tg, ct + pl, 16) == 0, "ascon aead seal");
        DOK(bp_ascon_aead_open(k, n, ad, al, ct, pl, ct + pl, m) == 0 && memcmp(m, pt, pl) == 0, "ascon aead open");
    }
    fprintf(out, "kat ascon-aead128 %u records, %lu failures\n", (unsigned)N_AEAD, failures - f0);
    for (f0 = failures, i = 0; i < N_XOF; i++) {
        unsigned ml = hex2bin(XOF_KATS[i].msg, m), dl = hex2bin(XOF_KATS[i].md, ct);
        bp_xof x; unsigned g;
        bp_xof_init(&x);
        for (g = 0; g < ml; g += 5) bp_xof_absorb(&x, m + g, ml - g < 5 ? ml - g : 5);
        for (g = 0; g < dl; g += 13) bp_xof_squeeze(&x, o + g, dl - g < 13 ? dl - g : 13);
        DOK(dl == 64 && memcmp(o, ct, dl) == 0, "ascon xof");
    }
    fprintf(out, "kat ascon-xof128 %u records, %lu failures\n", (unsigned)N_XOF, failures - f0);
    f0 = failures;
    {
        unsigned al = hex2bin(XCHACHA[2], ad), pl = hex2bin(XCHACHA[3], pt), cl = hex2bin(XCHACHA[4], ct);
        hex2bin(XCHACHA[0], k); hex2bin(XCHACHA[1], n);
        DOK(bp_test_aead_seal(1, k, n, ad, al, pt, pl, o, tg) == 0 && memcmp(o, ct, pl) == 0 && memcmp(tg, ct + pl, 16) == 0 && cl == pl + 16, "xchacha20-poly1305 draft A.1");
        DOK(bp_test_aead_open(1, k, n, ad, al, ct, pl, ct + pl, m) == 0 && memcmp(m, pt, pl) == 0, "xchacha open");
    }
    {   /* BLAKE2b-512("abc") */
        bp_part p; bp_uint8 d[64];
        p.p = (const bp_uint8 *)"abc"; p.n = 3;
        DOK(bp_hu(1, &p, 1, d, 64) == 0 && hex2bin(BLAKE2B_ABC, ct) == 64 && memcmp(d, ct, 64) == 0, "blake2b abc");
    }
    fprintf(out, "kat xchacha20-poly1305 and blake2b %lu failures\n", failures - f0);
}

static void goldens(void)
{
    unsigned i;
    unsigned long f0 = failures;
    bp_test_E_hook = E_hook;
    for (i = 0; i < N_DERIVE; i++) {
        const derive_case *c = &DERIVE[i];
        bp_keyparams kp; bp_keys ks; bp_uint8 ver[16], salt[16], set8[8], sec[64], want[32];
        unsigned sl = hex2bin(c->secret, sec);
        memset(&kp, 0, sizeof kp);
        kp.kdf = (bp_uint8)c->kdf; kp.aead = (bp_uint8)c->aead; kp.norm_version = (bp_uint8)c->norm;
        kp.m_cost_kib = c->m; kp.t_cost = (bp_uint16)c->t; kp.lanes = (bp_uint8)c->lanes;
        hex2bin(c->salt, salt); memcpy(kp.salt, salt, 16); hex2bin(c->set8, set8);
        DOK(bp_keys_derive(&ks, ver, &kp, set8, sec, sl, NULL) == 0, "derive");
        hex2bin(c->k_seal, want); DOK(memcmp(ks.k_seal, want, 32) == 0, "derive k_seal");
        hex2bin(c->k_nonce, want); DOK(memcmp(ks.k_nonce, want, 32) == 0, "derive k_nonce");
        hex2bin(c->verifier, want); DOK(memcmp(ver, want, 16) == 0, "derive verifier");
    }
    for (i = 0; i < N_FRAME; i++) {
        const frame_case *c = &FRAMES[i];
        bp_keys ks; bp_uint8 env[8], extra[80], nonce[24], want[600], tag[16];
        unsigned el, pl;
        memset(&ks, 0, sizeof ks);
        ks.aead = c->aead; hex2bin(c->set8, ks.set8); hex2bin(c->k_seal, ks.k_seal); hex2bin(c->k_nonce, ks.k_nonce);
        hex2bin(c->E, E_fixed); hex2bin(c->envelope, env); el = hex2bin(c->extra, extra); pl = hex2bin(c->pt, pt);
        bp_test_ctr_set((bp_uint32)c->ctr, 0);
        DOK(bp_frame_seal(&ks, env, extra, el, pt, pl, nonce, o, tag) == 0, "frame seal");
        hex2bin(c->nonce, want); DOK(memcmp(nonce, want, bp_suite_nonce_len(c->aead)) == 0, "frame nonce");
        hex2bin(c->ct, want); DOK(memcmp(o, want, pl) == 0, "frame ciphertext");
        hex2bin(c->tag, want); DOK(memcmp(tag, want, 16) == 0, "frame tag");
        DOK(bp_frame_open(&ks, env, extra, el, nonce, o, pl, tag, m) == 0 && memcmp(m, pt, pl) == 0, "frame open");
    }
    for (i = 0; i < N_COMMIT; i++) {
        const commit_case *c = &COMMITS[i];
        bp_hash_ctx hc; bp_uint8 t[16], d[16], want[16]; unsigned j;
        DOK(bp_hash_init(&hc, c->aead, NULL, 16) == 0, "commit init");
        for (j = 0; j < 5; j++) { hex2bin(c->tags[j], t); bp_hash_update(&hc, t, 16); }
        bp_hash_final(&hc, d); hex2bin(c->commit, want); DOK(memcmp(d, want, 16) == 0, "commit");
        DOK(bp_hash_init(&hc, c->aead, NULL, 16) == 0, "table_hash init");
        for (j = 0; j < 3; j++) { hex2bin(c->tags[j], t); bp_hash_update(&hc, t, 16); }
        bp_hash_final(&hc, d); hex2bin(c->table_hash, want); DOK(memcmp(d, want, 16) == 0, "table_hash");
    }
    bp_test_E_hook = NULL;
    fprintf(out, "golden %u derivations, %u frames, %u commitments: %lu failures\n", (unsigned)N_DERIVE, (unsigned)N_FRAME, (unsigned)N_COMMIT, failures - f0);
}

#define FRAME 32768UL
static bp_uint8 big[FRAME], bigct[FRAME];

typedef void (*job_fn)(void);
static bp_keys bk;
static bp_uint8 benv[8];

static void job_raw2(void) { bp_uint8 t[16]; bp_ascon_aead_seal(k, n, NULL, 0, big, FRAME, bigct, t); n[0]++; }
static void job_raw1(void) { bp_uint8 t[16]; bp_test_aead_seal(1, k, n, NULL, 0, big, FRAME, bigct, t); n[0]++; }
static void job_frame(void) { bp_uint8 nonce[24], t[16]; if (bp_frame_seal(&bk, benv, NULL, 0, big, FRAME, nonce, bigct, t) != 0) failures++; }

static void bench(const char *name, job_fn fn)
{
    long t0 = now(), t1;
    unsigned long it = 0;
    do { fn(); it++; t1 = now(); } while (t1 - t0 < MIN_TICKS);
    fprintf(out, "bench %s: %lu frames of %lu bytes in %ld ticks (200 Hz): %lu bytes per second\n", name, it, FRAME, t1 - t0,
            (unsigned long)((double)it * FRAME * 200.0 / (double)(t1 - t0)));
    fflush(out);
}

static void make_keys(bp_keys *ks, int aead)
{
    bp_keyparams kp; bp_uint8 ver[16], set8[8], sec[32];
    memset(&kp, 0, sizeof kp); kp.kdf = 0; kp.aead = (bp_uint8)aead;
    memset(sec, 7, 32); memset(set8, 3, 8); memset(kp.salt, 5, 16);
    if (bp_keys_derive(ks, ver, &kp, set8, sec, 32, NULL) != 0) failures++;
}

int main(void)
{
    unsigned i;
    out = fopen("C:\\CRYPTO.TXT", "w");
    if (out == NULL) return 2;
    fprintf(out, "start\n"); fflush(out);
    for (i = 0; i < FRAME; i++) big[i] = (bp_uint8)i;
    memset(k, 1, 32); memset(n, 2, 24);
    kats(); fflush(out);
    goldens(); fflush(out);
    if (failures == 0) {
        static bp_uint8 imp[16];
        for (i = 0; i < 16; i++) imp[i] = (bp_uint8)(0x40 + i);
        bp_entropy_import(imp, 16);            /* no OS source here: a fixed import seeds the pool for timing only */
        bench("raw ascon-aead128 seal", job_raw2);
        bench("raw xchacha20-poly1305 seal", job_raw1);
        benv[0] = 2; benv[1] = 0; benv[2] = benv[3] = 0;
        make_keys(&bk, 2); bp_write_le32(benv + 4, 16 + FRAME + 16); bench("bp_frame_seal S2 (kind 2, hedged nonce)", job_frame);
        make_keys(&bk, 1); bp_write_le32(benv + 4, 24 + FRAME + 16); bench("bp_frame_seal S1 (kind 2, hedged nonce)", job_frame);
        {
            bp_keyparams kp; bp_keys ks; bp_uint8 ver[16], set8[8], sec[16]; long t0, t1;
            memset(&kp, 0, sizeof kp); kp.kdf = 1; kp.aead = 1; kp.norm_version = 1; kp.m_cost_kib = ARGON_KIB; kp.t_cost = 1; kp.lanes = 1;
            memset(kp.salt, 9, 16); memset(set8, 3, 8); memset(sec, 4, 16);
            t0 = now();
            i = (unsigned)bp_keys_derive(&ks, ver, &kp, set8, sec, 16, NULL);
            t1 = now();
            fprintf(out, "bench argon2id %lu KiB, 1 pass, 1 lane: result %d, %ld ticks (200 Hz)\n", ARGON_KIB, (int)i, t1 - t0);
        }
    }
    fprintf(out, "done failures %lu\n", failures);
    fclose(out);
    return failures ? 1 : 0;
}
