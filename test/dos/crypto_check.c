/*
 * crypto_check.c -- the S2 side of the crypto layer on 16-bit DOS: key derivation (no KDF),
 * sealed frames of every kind, commitments, the entropy pool against its Python model, the
 * frame limits and the budget, then timing of bp_frame_seal.  Built with Open Watcom (no suite
 * S1) and run in DOSBox-X by test/check_dos_crypto.py, which generates crypto_vectors.h.
 *
 *   CRYPTO seconds framesize       writes RESULT.TXT (seconds 0: checks only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bp_crypto.h"
#include "bp_ascon.h"
#include "bp_entropy.h"
#include "crypto_vectors.h"

static FILE *res;
static unsigned long failures;
#define DOK(c, what) do { if (!(c)) { failures++; fprintf(res, "FAIL %s\n", what); } } while (0)

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static unsigned hex2bin(const char *s, bp_uint8 *o)
{
    unsigned n = 0;
    while (hexv(s[0]) >= 0 && hexv(s[1]) >= 0) { o[n++] = (bp_uint8)(hexv(s[0]) * 16 + hexv(s[1])); s += 2; }
    return n;
}

static bp_uint8 E_fixed[32];
static int E_hook(bp_uint8 E[32]) { memcpy(E, E_fixed, 32); return 0; }

static bp_uint8 pt[600], o[700], m[700], want[700], extra[80], env[8], sec[64];

static void derive_and_frames(void)
{
    unsigned i;
    unsigned long f0 = failures;
    unsigned nd = 0, nf = 0, nc = 0;
    bp_test_E_hook = E_hook;
    for (i = 0; i < N_DERIVE; i++) {
        const derive_case *c = &DERIVE[i];
        bp_keyparams kp; bp_keys ks; bp_uint8 ver[16], salt[16], set8[8];
        unsigned sl;
        if (c->aead != 2) continue;
        nd++;
        sl = hex2bin(c->secret, sec);
        memset(&kp, 0, sizeof kp);
        kp.kdf = (bp_uint8)c->kdf; kp.aead = (bp_uint8)c->aead; kp.norm_version = (bp_uint8)c->norm;
        hex2bin(c->salt, salt); memcpy(kp.salt, salt, 16); hex2bin(c->set8, set8);
        DOK(bp_keys_derive(&ks, ver, &kp, set8, sec, sl, NULL) == 0, "derive");
        hex2bin(c->k_seal, want); DOK(memcmp(ks.k_seal, want, 32) == 0, "derive k_seal");
        hex2bin(c->k_nonce, want); DOK(memcmp(ks.k_nonce, want, 32) == 0, "derive k_nonce");
        hex2bin(c->verifier, want); DOK(memcmp(ver, want, 16) == 0, "derive verifier");
    }
    for (i = 0; i < N_FRAME; i++) {
        const frame_case *c = &FRAMES[i];
        bp_keys ks; bp_uint8 nonce[24], tag[16];
        unsigned el, pl;
        if (c->aead != 2) continue;
        nf++;
        memset(&ks, 0, sizeof ks);
        ks.aead = c->aead; hex2bin(c->set8, ks.set8); hex2bin(c->k_seal, ks.k_seal); hex2bin(c->k_nonce, ks.k_nonce);
        hex2bin(c->E, E_fixed); hex2bin(c->envelope, env); el = hex2bin(c->extra, extra); pl = hex2bin(c->pt, pt);
        bp_test_ctr_set((bp_uint32)c->ctr, 0);
        DOK(bp_frame_seal(&ks, env, extra, el, pt, pl, nonce, o, tag) == 0, "frame seal");
        hex2bin(c->nonce, want); DOK(memcmp(nonce, want, bp_suite_nonce_len(c->aead)) == 0, "frame nonce");
        hex2bin(c->ct, want); DOK(memcmp(o, want, pl) == 0, "frame ciphertext");
        hex2bin(c->tag, want); DOK(memcmp(tag, want, 16) == 0, "frame tag");
        DOK(bp_frame_open(&ks, env, extra, el, nonce, o, pl, tag, m) == 0 && memcmp(m, pt, pl) == 0, "frame open");
        o[0] ^= 1;
        DOK(pl == 0 || bp_frame_open(&ks, env, extra, el, nonce, o, pl, tag, m) == BP_CRYPTO_E_AUTH, "frame tamper refused");
    }
    for (i = 0; i < N_COMMIT; i++) {
        const commit_case *c = &COMMITS[i];
        bp_hash_ctx hc; bp_uint8 t[16], d[16]; unsigned j;
        if (c->aead != 2) continue;
        nc++;
        DOK(bp_hash_init(&hc, c->aead, NULL, 16) == 0, "commit init");
        for (j = 0; j < 5; j++) { hex2bin(c->tags[j], t); bp_hash_update(&hc, t, 16); }
        bp_hash_final(&hc, d); hex2bin(c->commit, want); DOK(memcmp(d, want, 16) == 0, "commit");
        DOK(bp_hash_init(&hc, c->aead, NULL, 16) == 0, "table_hash init");
        for (j = 0; j < 3; j++) { hex2bin(c->tags[j], t); bp_hash_update(&hc, t, 16); }
        bp_hash_final(&hc, d); hex2bin(c->table_hash, want); DOK(memcmp(d, want, 16) == 0, "table_hash");
    }
    bp_test_E_hook = NULL;
    fprintf(res, "wrapper S2: %u derivations, %u frames, %u commitments: %lu failures\n", nd, nf, nc, failures - f0);
}

static void pool_and_limits(void)
{
    unsigned i, j;
    unsigned long f0 = failures;
    bp_uint32 lo, hi;
    bp_budget_use u;
    for (i = 0; i < N_POOL; i++) {
        const pool_case *c = &POOLS[i];
        unsigned il = hex2bin(c->imp, sec), tot = 0;
        bp_entropy_test_reset();
        bp_entropy_import(sec, il);
        for (j = 0; j < (unsigned)c->ndraw; j++) { DOK(bp_random_bytes(o + tot, c->draws[j]) == 0, "pool draw"); tot += c->draws[j]; }
        hex2bin(c->out, want);
        DOK(memcmp(o, want, tot) == 0, "pool output against the Python model");
    }
    bp_entropy_test_reset();
    DOK(bp_random_bytes(o, 16) == BP_CRYPTO_E_ENTROPY, "unseeded pool refuses");
    DOK(bp_frame_body_limits(2, 1, &lo, &hi) == 0 && hi == 1051UL, "limit kind 1");
    DOK(bp_frame_body_limits(2, 2, &lo, &hi) == 0 && hi == 32776UL, "limit kind 2");
    DOK(bp_frame_body_limits(2, 4, &lo, &hi) == 0 && hi == 16384UL, "limit kind 4");
    DOK(bp_frame_body_limits(2, 3, &lo, &hi) == 0 && lo == 16 && hi == 16, "limit kind 3");
    memset(&u, 0, sizeof u);
    u.frames = 16777216UL;
    DOK(bp_budget_ok(2, &u, 0, 0) == 1 && bp_budget_ok(2, &u, 1, 0) == 0, "budget frames");
    memset(&u, 0, sizeof u);
    u.bytes.hi = 256; u.bytes.lo = 0;
    DOK(bp_budget_ok(2, &u, 0, 0) == 1 && bp_budget_ok(2, &u, 0, 1) == 0, "budget bytes (2^40)");
    {   /* a 16-bit-wrapping length: 65,536 + 20 would be 20 in 16 bits */
        bp_keys ks; bp_uint8 e2[8], nonce[24], tag[16];
        memset(&ks, 0, sizeof ks); ks.aead = 2;
        e2[0] = 2; e2[1] = 0; e2[2] = e2[3] = 0; bp_write_le32(e2 + 4, 16UL + 65536UL + 16UL);
        DOK(bp_frame_seal(&ks, e2, NULL, 0, pt, 65536UL, nonce, o, tag) == BP_CRYPTO_E_ARG, "oversize body refused");
    }
    fprintf(res, "pool, limits, budget: %lu failures\n", failures - f0);
}

int main(int argc, char **argv)
{
    unsigned long seconds = argc > 1 ? strtoul(argv[1], NULL, 10) : 0UL;
    unsigned fsize = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 4096U;
    res = fopen("RESULT.TXT", "w");
    if (res == NULL) return 2;
    if (seconds == 0) { derive_and_frames(); fflush(res); pool_and_limits(); }
    if (seconds > 0) {
        static bp_uint8 buf[32768U], obuf[32768U];
        bp_keys ks; bp_keyparams kp; bp_uint8 ver[16], set8[8], sk[32], imp[16], nonce[24], tag[16];
        clock_t t0, t;
        unsigned long iters = 0;
        unsigned i;
        memset(&kp, 0, sizeof kp); kp.kdf = 0; kp.aead = 2; memset(sk, 7, 32); memset(set8, 3, 8); memset(kp.salt, 5, 16);
        if (bp_keys_derive(&ks, ver, &kp, set8, sk, 32, NULL) != 0) { fprintf(res, "derive failed\n"); fclose(res); return 2; }
        for (i = 0; i < 16; i++) imp[i] = (bp_uint8)(0x40 + i);
        bp_entropy_import(imp, 16);
        memset(buf, 0x5A, sizeof buf);
        env[0] = 2; env[1] = 0; env[2] = env[3] = 0; bp_write_le32(env + 4, 16UL + fsize + 16UL);
        t0 = clock();
        do {
            if (bp_frame_seal(&ks, env, NULL, 0, buf, fsize, nonce, obuf, tag) != 0) { failures++; break; }
            iters++;
            t = clock();
        } while ((unsigned long)(t - t0) < seconds * (unsigned long)CLOCKS_PER_SEC);
        fprintf(res, "bench bp_frame_seal S2 (kind 2, hedged nonce), %u-byte frames: %lu frames in %lu ticks (%lu per second): %lu bytes/s\n",
                fsize, iters, (unsigned long)(t - t0), (unsigned long)CLOCKS_PER_SEC,
                (iters * (unsigned long)fsize * (unsigned long)CLOCKS_PER_SEC) / (unsigned long)(t - t0));
    }
    fprintf(res, "done failures %lu\n", failures);
    fclose(res);
    return failures ? 1 : 0;
}
