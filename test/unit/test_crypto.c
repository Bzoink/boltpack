/*
 * test_crypto.c -- unit tests for src/core/crypto that need C-level access:
 * the entropy pool, the writer's counters, fail-closed sealing, nonce
 * uniqueness with good, constant and replayed sources, wipe and compare.
 * Built with BP_CRYPTO_TESTING.  Prints "N checks, F failed".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bp_crypto.h"
#include "bp_entropy.h"

static int total, failed;
#define CHECK(c) do { total++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- scripted OS sources ------------------------------------------------ */
static int os_fail(bp_uint8 *b, bp_uint32 n) { (void)b; (void)n; return -1; }
static int os_zero(bp_uint8 *b, bp_uint32 n) { memset(b, 0, n); return 0; }
static bp_uint32 replay_pos;
static int os_replay(bp_uint8 *b, bp_uint32 n)   /* the same byte sequence after every reset */
{
    bp_uint32 i;
    for (i = 0; i < n; i++) b[i] = (bp_uint8)((replay_pos++ * 2654435761U) >> 24);
    return 0;
}
static int os_ok_then_fail_count;
static int os_two_good_then_fail(bp_uint8 *b, bp_uint32 n)   /* the seed read, one draw's fresh read, then failure */
{
    if (os_ok_then_fail_count++ < 2) { memset(b, 0x5A, n); return 0; }
    return -1;
}
static bp_uint8 g_E[32];
static int E_fixed(bp_uint8 E[32]) { memcpy(E, g_E, 32); return 0; }

static void fresh(int (*os)(bp_uint8 *, bp_uint32))
{
    bp_entropy_test_reset();
    bp_entropy_test_set_os(os);
    replay_pos = 0;
    os_ok_then_fail_count = 0;
}

static void test_helpers(void)
{
    bp_uint8 a[16], b[16];
    memset(a, 0xA5, sizeof a);
    bp_wipe(a, sizeof a);
    { int i, z = 1; for (i = 0; i < 16; i++) if (a[i]) z = 0; CHECK(z); }
    memset(a, 1, 16); memset(b, 1, 16);
    CHECK(bp_ct_equal(a, b, 16) == 1);
    CHECK(bp_ct_equal(a, b, 0) == 1);
    b[15] ^= 0x80; CHECK(bp_ct_equal(a, b, 16) == 0);
    b[15] ^= 0x80; b[0] ^= 1; CHECK(bp_ct_equal(a, b, 16) == 0);
    CHECK(bp_suite_nonce_len(1) == 24 && bp_suite_nonce_len(2) == 16 && bp_suite_nonce_len(0) == 0);
    CHECK(bp_suite_key_len(1) == 32 && bp_suite_key_len(2) == 16);
    CHECK(bp_frame_overhead(1) == 52 && bp_frame_overhead(2) == 44 && bp_frame_overhead(9) == 0);
    CHECK(bp_crypto_has_test_hooks() == 1);
    CHECK(bp_suite_available(2) == 1);
}

static void test_pool(void)
{
    bp_uint8 buf[64], prev[32], cur[32], data[64];
    int i, same;
    bp_uint32 c;

    memset(data, 0x77, sizeof data);

    /* no OS source, nothing added: refuses, and outputs nothing */
    fresh(os_fail);
    memset(buf, 0xAA, sizeof buf);
    CHECK(bp_random_bytes(buf, 32) == BP_CRYPTO_E_ENTROPY);
    CHECK(buf[0] == 0xAA && buf[31] == 0xAA);
    CHECK(bp_entropy_credit() == 0 && bp_entropy_test_output_count() == 0);

    /* imported entropy: 15 bytes is 120 bits (below the line), 16 bytes is 128 (on it) */
    CHECK(bp_entropy_import(data, 15) == 120);
    CHECK(bp_random_bytes(buf, 32) == BP_CRYPTO_E_ENTROPY);
    CHECK(bp_entropy_import(data, 1) == 128);
    CHECK(bp_random_bytes(buf, 32) == BP_CRYPTO_OK);
    /* 128 bits is enough for salts and nonces but not for a secret key */
    CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_E_ENTROPY);
    CHECK(bp_entropy_import(data, 16) == 256);     /* capped at 256 in total */
    CHECK(bp_entropy_import(data, 64) == 256);
    CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_OK);

    /* a secret needs 256: 31 imported bytes is 248, 32 is 256 */
    fresh(os_fail);
    CHECK(bp_entropy_import(data, 31) == 248);
    CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_E_ENTROPY);
    CHECK(bp_entropy_import(data, 1) == 256);
    CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_OK);

    /* weak data is never credited; gathered timing is capped at 64 bits */
    fresh(os_fail);
    for (i = 0; i < 50; i++) bp_entropy_add_weak();
    CHECK(bp_entropy_credit() == 0);
    for (i = 0; i < 20; i++) c = bp_entropy_gather();
    CHECK(c <= 64);
    CHECK(bp_random_bytes(buf, 16) == BP_CRYPTO_E_ENTROPY);   /* 64 bits is not enough alone */
    /* the source argument and the credit are bounded: unknown sources earn nothing */
    CHECK(bp_entropy_add(0, data, 16, 999) == c && bp_entropy_add(9, data, 16, 999) == c);
    CHECK(bp_entropy_add(BP_ENT_WEAK, data, 16, 999) == c);
    CHECK(bp_entropy_add(BP_ENT_GATHER, data, 16, 999) == c);   /* capped at 64 */

    /* OS source seeds the pool on first use (256 bits) */
    fresh(os_zero);
    CHECK(bp_random_bytes(buf, 32) == BP_CRYPTO_OK);
    CHECK(bp_entropy_credit() == 256);
    CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_OK);

    /* where the OS source seeded the pool, a later failing fresh read fails the call closed */
    fresh(os_two_good_then_fail);
    CHECK(bp_random_bytes(buf, 32) == BP_CRYPTO_OK);
    memset(buf, 0xAA, sizeof buf);
    CHECK(bp_random_bytes(buf, 32) == BP_CRYPTO_E_ENTROPY);
    CHECK(buf[0] == 0xAA && buf[31] == 0xAA);
    CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_E_ENTROPY);

    /* the same imported buffer earns credit once; different buffers each earn theirs */
    fresh(os_fail);
    CHECK(bp_entropy_import(data, 16) == 128);
    CHECK(bp_entropy_import(data, 16) == 128);       /* the same bytes again: no new credit */
    { int k; for (k = 0; k < 10; k++) bp_entropy_import(data, 16); }
    CHECK(bp_entropy_credit() == 128);
    memset(buf, 0x11, 16);
    CHECK(bp_entropy_import(buf, 16) == 256);        /* a different buffer counts */

    /* a secret needs 256 bits of OS, hardware or imported credit; gathered timing never counts */
    fresh(os_fail);
    { bp_uint8 d2[24]; int k; memset(d2, 0x21, sizeof d2);
      CHECK(bp_entropy_import(d2, 24) == 192);
      for (k = 0; k < 20; k++) bp_entropy_gather();
      CHECK(bp_random_bytes(buf, 16) == BP_CRYPTO_OK);                  /* enough for salts and nonces */
      CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_E_ENTROPY);          /* 192 strong bits: not enough, even with timing */
      memset(d2, 0x22, 8);
      CHECK(bp_entropy_import(d2, 8) == 256);
      CHECK(bp_random_secret(buf, 32) == BP_CRYPTO_OK); }

    /* outputs never repeat, even with a constant OS source (zeros): state is replaced and a counter runs */
    fresh(os_zero);
    memset(prev, 0, 32);
    same = 0;
    for (i = 0; i < 2000; i++) {
        CHECK(bp_random_bytes(cur, 32) == BP_CRYPTO_OK);
        if (memcmp(cur, prev, 32) == 0) same++;
        memcpy(prev, cur, 32);
    }
    CHECK(same == 0);
    CHECK(bp_entropy_test_output_count() == 2000U * 32U);

    /* a replayed source gives the same sequence after a reset (that is what "replayed" means) ... */
    fresh(os_replay);
    CHECK(bp_random_bytes(buf, 32) == 0); memcpy(prev, buf, 32);
    fresh(os_replay);
    CHECK(bp_random_bytes(buf, 32) == 0);
    CHECK(memcmp(prev, buf, 32) == 0);
    /* ... and imported entropy on top changes it */
    fresh(os_replay);
    bp_entropy_import(data, 16);
    CHECK(bp_random_bytes(buf, 32) == 0);
    CHECK(memcmp(prev, buf, 32) != 0);

    /* lengths: zero length, a long draw, and the bytes differ from a second draw */
    fresh(os_zero);
    CHECK(bp_random_bytes(buf, 0) == BP_CRYPTO_OK);
    CHECK(bp_random_bytes(NULL, 5) == BP_CRYPTO_E_ARG);
    { bp_uint8 *big = (bp_uint8 *)malloc(100000); bp_uint8 *big2 = (bp_uint8 *)malloc(100000);
      CHECK(bp_random_bytes(big, 100000) == 0 && bp_random_bytes(big2, 100000) == 0);
      CHECK(memcmp(big, big2, 100000) != 0);
      free(big); free(big2); }
}

static void make_keys(bp_keys *k, int aead)
{
    bp_keyparams kp;
    bp_uint8 set8[8], secret[32], ver[16];
    int i;
    memset(&kp, 0, sizeof kp);
    kp.kdf = BP_KDF_NONE; kp.aead = (bp_uint8)aead;
    for (i = 0; i < 16; i++) kp.salt[i] = (bp_uint8)(i + 1);
    for (i = 0; i < 8; i++) set8[i] = (bp_uint8)(0x10 + i);
    for (i = 0; i < 32; i++) secret[i] = (bp_uint8)(0x40 + i);
    CHECK(bp_keys_derive(k, ver, &kp, set8, secret, 32, NULL) == BP_CRYPTO_OK);
}

static void put_env(bp_uint8 env[8], int kind, int fflags, int aead, bp_uint32 ptlen)
{
    env[0] = (bp_uint8)kind; env[1] = (bp_uint8)fflags; env[2] = env[3] = 0;
    bp_write_le32(env + 4, bp_suite_nonce_len(aead) + ptlen + 16);
}

static void test_frames(void)
{
    int aead;
    for (aead = 1; aead <= 2; aead++) {
        bp_keys k;
        bp_keyparams kp;
        bp_uint8 env[8], nonce[24], ct[512], tag[16], pt[512], out[512], extra[20], sentinel[512];
        bp_uint8 set8[8], secret[32], v1[16], v2[16];
        static bp_uint8 nonces[3000][24];
        bp_uint32 n, i, j;
        int dup;

        if (!bp_suite_available(aead)) continue;     /* S1 is not in an S2-only build */
        make_keys(&k, aead);
        for (i = 0; i < 512; i++) pt[i] = (bp_uint8)i;
        memset(extra, 7, sizeof extra);

        /* fails closed when the pool is not seeded: no output, no counter, no seal count */
        fresh(os_fail);
        bp_test_ctr_set(0, 0);
        memset(ct, 0xAA, sizeof ct); memset(nonce, 0xAA, sizeof nonce); memset(tag, 0xAA, sizeof tag);
        put_env(env, 2, 0, aead, 40);
        CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonce, ct, tag) == BP_CRYPTO_E_ENTROPY);
        CHECK(ct[0] == 0xAA && nonce[0] == 0xAA && tag[0] == 0xAA);
        { bp_uint32 lo, hi; bp_test_ctr_get(&lo, &hi); CHECK(k.seals == 0 && lo == 0 && hi == 0); }

        /* round trip for sizes 0..300 with a good source; counters advance once per frame */
        fresh(os_zero);
        bp_test_ctr_set(0, 0);
        for (n = 0; n <= 300; n++) {
            put_env(env, 2, (int)(n & 1), aead, n);
            CHECK(bp_frame_seal(&k, env, extra, 20, pt, n, nonce, ct, tag) == BP_CRYPTO_OK);
            CHECK(bp_frame_open(&k, env, extra, 20, nonce, ct, n, tag, out) == BP_CRYPTO_OK);
            CHECK(n == 0 || memcmp(out, pt, n) == 0);
        }
        { bp_uint32 lo, hi; bp_test_ctr_get(&lo, &hi); CHECK(k.seals == 301 && lo == 301 && hi == 0); }

        /* the counter carries into the high half */
        bp_test_ctr_set(0xFFFFFFFFU, 0);
        put_env(env, 1, 0, aead, 10);
        CHECK(bp_frame_seal(&k, env, NULL, 0, pt, 10, nonce, ct, tag) == BP_CRYPTO_OK);
        { bp_uint32 lo, hi; bp_test_ctr_get(&lo, &hi); CHECK(lo == 0 && hi == 1); }

        /* a wrong key, a changed AAD part, a wrong tag: E_AUTH, and the output is wiped */
        put_env(env, 2, 0, aead, 40);
        CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonce, ct, tag) == BP_CRYPTO_OK);
        memset(sentinel, 0xCC, 40);
        memset(out, 0xCC, 40);
        extra[3] ^= 1;
        CHECK(bp_frame_open(&k, env, extra, 20, nonce, ct, 40, tag, out) == BP_CRYPTO_E_AUTH);
        { int z = 1; for (i = 0; i < 40; i++) if (out[i]) z = 0; CHECK(z); }
        extra[3] ^= 1;
        CHECK(bp_frame_open(&k, env, extra, 20, nonce, ct, 40, tag, out) == BP_CRYPTO_OK);
        k.k_seal[0] ^= 1;
        CHECK(bp_frame_open(&k, env, extra, 20, nonce, ct, 40, tag, out) == BP_CRYPTO_E_AUTH);
        k.k_seal[0] ^= 1;

        /* the counter is process-wide: a copy of the keys, and a second set, continue one sequence */
        { bp_keys k2; bp_uint32 lo1, hi1, lo2, hi2;
          fresh(os_zero); bp_test_ctr_set(5, 0); k2 = k;
          put_env(env, 1, 0, aead, 10);
          CHECK(bp_frame_seal(&k, env, NULL, 0, pt, 10, nonce, ct, tag) == BP_CRYPTO_OK);
          bp_test_ctr_get(&lo1, &hi1);
          CHECK(bp_frame_seal(&k2, env, NULL, 0, pt, 10, nonce, ct, tag) == BP_CRYPTO_OK);
          bp_test_ctr_get(&lo2, &hi2);
          CHECK(lo1 == 6 && lo2 == 7 && hi1 == 0 && hi2 == 0 && k.seals != 0 && k2.seals == k.seals); }

        /* a body length whose frame length would wrap is refused before the addition: the
         * envelope below carries exactly the wrapped sum, which an unguarded check would accept */
        { bp_uint8 e2[8]; bp_uint32 body = 0xFFFFFFFAU;
          e2[0] = 2; e2[1] = 0; e2[2] = e2[3] = 0;
          bp_write_le32(e2 + 4, (bp_uint32)(bp_suite_nonce_len(aead) + body + 16U));
          CHECK(bp_frame_open(&k, e2, NULL, 0, nonce, ct, body, tag, out) == BP_CRYPTO_E_ARG);
          CHECK(bp_frame_seal(&k, e2, NULL, 0, pt, body, nonce, ct, tag) == BP_CRYPTO_E_ARG); }

        /* argument checks: envelope length, zero bytes, oversize extra, unknown suite */
        put_env(env, 2, 0, aead, 41);
        CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonce, ct, tag) == BP_CRYPTO_E_ARG);
        put_env(env, 2, 0, aead, 40); env[2] = 1;
        CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonce, ct, tag) == BP_CRYPTO_E_ARG);
        env[2] = 0;
        CHECK(bp_frame_seal(&k, env, extra, BP_AAD_EXTRA_MAX + 1, pt, 40, nonce, ct, tag) == BP_CRYPTO_E_ARG);
        k.aead = 9;
        CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonce, ct, tag) == BP_CRYPTO_E_UNSUPPORTED);
        k.aead = aead;

        /* nonce uniqueness, constant OS source: 3000 frames, same plaintext, counter running */
        fresh(os_zero);
        bp_test_ctr_set(0, 0);
        put_env(env, 2, 0, aead, 40);
        for (i = 0; i < 3000; i++) {
            CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonces[i], ct, tag) == BP_CRYPTO_OK);
        }
        dup = 0;
        for (i = 0; i < 3000; i++) for (j = i + 1; j < 3000; j++)
            if (memcmp(nonces[i], nonces[j], bp_suite_nonce_len(aead)) == 0) dup++;
        CHECK(dup == 0);

        /* the hedge, with E and ctr both replayed (a deterministic machine restarting):
         * the same plaintext and AAD give the same nonce (and so the same frame) ... */
        bp_test_E_hook = E_fixed;
        memset(g_E, 0x33, 32);
        bp_test_ctr_set(0, 0);
        put_env(env, 2, 0, aead, 40);
        CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonces[0], ct, tag) == BP_CRYPTO_OK);
        bp_test_ctr_set(0, 0);
        CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonces[1], out, tag) == BP_CRYPTO_OK);
        CHECK(memcmp(nonces[0], nonces[1], bp_suite_nonce_len(aead)) == 0 && memcmp(ct, out, 40) == 0);
        /* ... but different plaintext, or different AAD, never gives the same nonce */
        { bp_uint8 pt2[40], x2[20]; bp_uint32 pos;
          memcpy(pt2, pt, 40);
          for (pos = 0; pos < 40; pos++) {
              pt2[pos] ^= 1; bp_test_ctr_set(0, 0);
              CHECK(bp_frame_seal(&k, env, extra, 20, pt2, 40, nonces[2], out, tag) == BP_CRYPTO_OK);
              CHECK(memcmp(nonces[0], nonces[2], bp_suite_nonce_len(aead)) != 0);
              pt2[pos] ^= 1;
          }
          memcpy(x2, extra, 20);
          for (pos = 0; pos < 20; pos++) {
              x2[pos] ^= 1; bp_test_ctr_set(0, 0);
              CHECK(bp_frame_seal(&k, env, x2, 20, pt, 40, nonces[2], out, tag) == BP_CRYPTO_OK);
              CHECK(memcmp(nonces[0], nonces[2], bp_suite_nonce_len(aead)) != 0);
              x2[pos] ^= 1;
          }
          bp_test_ctr_set(0, 0); env[0] = 1;   /* another valid kind: the envelope is in the AAD */
          CHECK(bp_frame_seal(&k, env, extra, 20, pt, 40, nonces[2], out, tag) == BP_CRYPTO_OK);
          CHECK(memcmp(nonces[0], nonces[2], bp_suite_nonce_len(aead)) != 0);
          env[0] = 2; }
        bp_test_E_hook = NULL;

        /* key derivation: same inputs, same keys; a different secret, set ID or salt, different verifier */
        memset(&kp, 0, sizeof kp); kp.kdf = BP_KDF_NONE; kp.aead = (bp_uint8)aead;
        memset(kp.salt, 9, 16); memset(set8, 3, 8); memset(secret, 4, 32);
        { bp_keys a, b;
          CHECK(bp_keys_derive(&a, v1, &kp, set8, secret, 32, NULL) == 0);
          CHECK(bp_keys_derive(&b, v2, &kp, set8, secret, 32, NULL) == 0);
          CHECK(memcmp(&a, &b, sizeof a) == 0 && bp_ct_equal(v1, v2, 16));
          secret[31] ^= 1;
          CHECK(bp_keys_derive(&b, v2, &kp, set8, secret, 32, NULL) == 0);
          CHECK(!bp_ct_equal(v1, v2, 16) && memcmp(a.k_seal, b.k_seal, 32) != 0);
          secret[31] ^= 1; set8[0] ^= 1;
          CHECK(bp_keys_derive(&b, v2, &kp, set8, secret, 32, NULL) == 0);
          CHECK(!bp_ct_equal(v1, v2, 16) && memcmp(a.k_seal, b.k_seal, 32) != 0);
          set8[0] ^= 1; kp.salt[0] ^= 1;
          CHECK(bp_keys_derive(&b, v2, &kp, set8, secret, 32, NULL) == 0);
          CHECK(!bp_ct_equal(v1, v2, 16)); }
        /* every error leaves both outputs zeroed, whichever check fails */
        { bp_keys z; bp_uint8 vz[16]; bp_uint32 zi; int zero;
          bp_keyparams bad = kp;
          bp_uint8 *zp = (bp_uint8 *)&z;
          bad.m_cost_kib = 5;   /* kdf none with a cost field: refused before any work */
          memset(&z, 0x55, sizeof z); memset(vz, 0x55, 16);
          CHECK(bp_keys_derive(&z, vz, &bad, set8, secret, 32, NULL) == BP_CRYPTO_E_ARG);
          for (zero = 1, zi = 0; zi < sizeof z; zi++) if (zp[zi]) zero = 0;
          for (zi = 0; zi < 16; zi++) if (vz[zi]) zero = 0;
          CHECK(zero);
          memset(&z, 0x55, sizeof z); memset(vz, 0x55, 16);
          CHECK(bp_keys_derive(&z, vz, &kp, set8, secret, 31, NULL) == BP_CRYPTO_E_ARG);   /* wrong secret length */
          for (zero = 1, zi = 0; zi < sizeof z; zi++) if (zp[zi]) zero = 0;
          for (zi = 0; zi < 16; zi++) if (vz[zi]) zero = 0;
          CHECK(zero);
          memset(&z, 0x55, sizeof z); memset(vz, 0x55, 16);
          bad = kp; bad.aead = 3;
          CHECK(bp_keys_derive(&z, vz, &bad, set8, secret, 32, NULL) == BP_CRYPTO_E_UNSUPPORTED);
          for (zero = 1, zi = 0; zi < sizeof z; zi++) if (zp[zi]) zero = 0;
          for (zi = 0; zi < 16; zi++) if (vz[zi]) zero = 0;
          CHECK(zero); }
    }
}

static void test_limits_budget_hash(void)
{
    int aead;
    bp_uint32 lo, hi;
    bp_keys k;
    bp_uint8 env[8], nonce[24], tag[16], big[40000], ct[40000];
    bp_budget_use u;

    CHECK(bp_frame_body_limits(1, 3, &lo, &hi) == 0 && lo == 16 && hi == 16);
    CHECK(bp_frame_body_limits(2, 1, &lo, &hi) == 0 && hi == 1051);
    CHECK(bp_frame_body_limits(2, 2, &lo, &hi) == 0 && hi == 32776);
    CHECK(bp_frame_body_limits(2, 4, &lo, &hi) == 0 && hi == 16384);
    CHECK(bp_frame_body_limits(1, 1, &lo, &hi) == 0 && hi == 65528);
    CHECK(bp_frame_body_limits(1, 4, &lo, &hi) == 0 && hi == 1048576UL);
    CHECK(bp_frame_body_limits(2, 5, &lo, &hi) == 0 && lo == 0 && hi == 0);
    CHECK(bp_frame_body_limits(2, 0, &lo, &hi) == BP_CRYPTO_E_ARG && bp_frame_body_limits(2, 6, &lo, &hi) == BP_CRYPTO_E_ARG);
    CHECK(bp_frame_body_limits(3, 2, &lo, &hi) == BP_CRYPTO_E_UNSUPPORTED);

    /* the limits are enforced by seal and open at the byte */
    memset(big, 0, sizeof big);
    for (aead = 1; aead <= 2; aead++) {
        int kind;
        if (!bp_suite_available(aead)) continue;
        make_keys(&k, aead);
        fresh(os_zero);
        for (kind = 1; kind <= 5; kind++) {
            bp_uint32 mn, mx;
            bp_frame_body_limits(aead, kind, &mn, &mx);
            if (mx < sizeof big) {
                put_env(env, kind, 0, aead, mx);
                CHECK(bp_frame_seal(&k, env, NULL, 0, big, mx, nonce, ct, tag) == BP_CRYPTO_OK);
                put_env(env, kind, 0, aead, mx + 1);
                CHECK(bp_frame_seal(&k, env, NULL, 0, big, mx + 1, nonce, ct, tag) == BP_CRYPTO_E_ARG);
                CHECK(bp_frame_open(&k, env, NULL, 0, nonce, big, mx + 1, tag, ct) == BP_CRYPTO_E_ARG);
            }
            if (mn > 0) {
                put_env(env, kind, 0, aead, mn - 1);
                CHECK(bp_frame_seal(&k, env, NULL, 0, big, mn - 1, nonce, ct, tag) == BP_CRYPTO_E_ARG);
            }
        }
        put_env(env, 0, 0, aead, 16);
        CHECK(bp_frame_seal(&k, env, NULL, 0, big, 16, nonce, ct, tag) == BP_CRYPTO_E_ARG);
    }

    /* budget */
    memset(&u, 0, sizeof u);
    CHECK(bp_budget_ok(2, &u, 0, 0) == 1 && bp_budget_ok(1, &u, 0, 0) == 1);
    u.frames = 16777216UL;
    CHECK(bp_budget_ok(2, &u, 0, 0) == 1 && bp_budget_ok(2, &u, 1, 0) == 0);
    CHECK(bp_budget_ok(1, &u, 1000000UL, 0) == 1);               /* S1 allows 2^32 frames */
    u.frames = 0xFFFFFFFFUL;
    CHECK(bp_budget_ok(1, &u, 1, 0) == 1 && bp_budget_ok(1, &u, 2, 0) == 0);
    u.frames = 16777216UL - 100; u.revision = 3; u.table_chunks = 30;   /* 3 * 30 dead table frames */
    CHECK(bp_budget_ok(2, &u, 10, 0) == 1 && bp_budget_ok(2, &u, 11, 0) == 0);
    u.frames = 0; u.revision = 4096; u.table_chunks = 5000;             /* 4096 * 5000 > 2^24 */
    CHECK(bp_budget_ok(2, &u, 0, 0) == 0);
    memset(&u, 0, sizeof u);
    u.bytes.hi = 256; u.bytes.lo = 0;                                    /* exactly 2^40 bytes */
    CHECK(bp_budget_ok(2, &u, 0, 0) == 1 && bp_budget_ok(2, &u, 0, 1) == 0);
    CHECK(bp_budget_ok(1, &u, 0, 1000) == 1);                            /* S1 sets no byte limit */
    CHECK(bp_budget_ok(3, &u, 0, 0) == BP_CRYPTO_E_UNSUPPORTED && bp_budget_ok(2, NULL, 0, 0) == BP_CRYPTO_E_ARG);

    /* incremental hash equals the one-shot hash and wipes its state */
    for (aead = 1; aead <= 2; aead++) {
        bp_hash_ctx c; bp_uint8 d[100], o1[16], o2[16], key[32]; bp_part pp; bp_uint32 i, nz = 0; const bp_uint8 *cp;
        if (!bp_suite_available(aead)) continue;
        for (i = 0; i < 100; i++) d[i] = (bp_uint8)i;
        for (i = 0; i < 32; i++) key[i] = (bp_uint8)(0xA0 + i);
        pp.p = d; pp.n = 100;
        CHECK(bp_hk(aead, key, (const bp_uint8 *)"bp8/test", 8, &pp, 1, o1, 16) == 0);
        CHECK(bp_hash_init(&c, aead, key, (const bp_uint8 *)"bp8/test", 8, 16) == 0);
        bp_hash_update(&c, d, 33); bp_hash_update(&c, d + 33, 0); bp_hash_update(&c, d + 33, 67);
        bp_hash_final(&c, o2);
        CHECK(memcmp(o1, o2, 16) == 0);
        cp = (const bp_uint8 *)&c;
        for (i = 0; i < sizeof c; i++) if (cp[i]) nz++;
        CHECK(nz == 0);                       /* final wipes the context */
        CHECK(bp_hash_init(&c, aead, NULL, NULL, 0, 0) == BP_CRYPTO_E_ARG);
        CHECK(bp_hash_init(&c, aead, key, NULL, 0, 16) == BP_CRYPTO_E_ARG);                 /* a keyed hash needs a label */
        CHECK(bp_hash_init(&c, aead, NULL, (const bp_uint8 *)"x", 1, 16) == BP_CRYPTO_E_ARG); /* and an unkeyed one has none */
    }
}

int main(void)
{
    test_helpers();
    test_pool();
    test_frames();
    test_limits_budget_hash();
    printf("%d checks, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
