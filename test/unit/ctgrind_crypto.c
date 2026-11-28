/*
 * ctgrind_crypto.c -- secret-dependent branch and index check for the crypto layer,
 * in the style of Monocypher's tests/ctgrind.c: secrets are marked "undefined" for
 * valgrind's memcheck, so any branch or table index that depends on them is reported.
 *
 * Covers: bp_ct_equal, bp_keys_derive with kdf 0 (both suites), bp_frame_seal
 * (both suites, secret key and plaintext).  Not covered, by design: Argon2id
 * (data-dependent by definition in its second half) and the accept/reject branch
 * of bp_frame_open (that outcome is public).
 *
 * Run: valgrind --error-exitcode=9 ./ctgrind_crypto   (exit 0 and no report = pass)
 */
#include <stdio.h>
#include <string.h>
#include <valgrind/memcheck.h>
#include "bp_crypto.h"

static bp_uint8 g_E[32];
static int E_fixed(bp_uint8 E[32]) { memcpy(E, g_E, 32); return 0; }

int main(void)
{
    int aead, i;
    bp_uint8 a[16], b[16], secret[32], ver[16], expect[16], env[8], pt[200], ct[200], tag[16], nonce[24];
    bp_keyparams kp;
    bp_keys k;
    bp_uint8 set8[8];
    int r;

    /* compare */
    for (i = 0; i < 16; i++) { a[i] = (bp_uint8)i; b[i] = (bp_uint8)i; }
    VALGRIND_MAKE_MEM_UNDEFINED(a, 16);
    r = bp_ct_equal(a, b, 16);
    VALGRIND_MAKE_MEM_DEFINED(&r, sizeof r);
    printf("compare: %d\n", r);

    bp_test_E_hook = E_fixed;
    for (aead = 1; aead <= 2; aead++) {
        memset(&kp, 0, sizeof kp);
        kp.kdf = BP_KDF_NONE; kp.aead = (bp_uint8)aead;
        memset(kp.salt, 5, 16); memset(set8, 6, 8);
        memset(secret, 7, 32); memset(expect, 0, 16);
        VALGRIND_MAKE_MEM_UNDEFINED(secret, 32);
        if (bp_keys_derive(&k, ver, &kp, set8, secret, 32, NULL) != 0) { printf("derive failed\n"); return 1; }
        /* the verifier compare is on secret-derived data */
        r = bp_ct_equal(ver, expect, 16);
        VALGRIND_MAKE_MEM_DEFINED(&r, sizeof r);
        printf("S%d derive + verifier compare: %d\n", aead, r);

        /* seal with secret keys (derived above, still undefined), secret plaintext and secret E */
        memset(pt, 1, sizeof pt); memset(g_E, 2, 32);
        VALGRIND_MAKE_MEM_UNDEFINED(pt, sizeof pt);
        VALGRIND_MAKE_MEM_UNDEFINED(g_E, 32);
        env[0] = 2; env[1] = 0; env[2] = env[3] = 0;
        bp_write_le32(env + 4, bp_suite_nonce_len(aead) + 100 + 16);
        if (bp_frame_seal(&k, env, NULL, 0, pt, 100, nonce, ct, tag) != 0) { printf("seal failed\n"); return 1; }
        /* a tag compared with another tag: public outcome, marked defined before use */
        r = bp_ct_equal(tag, ver, 16);
        VALGRIND_MAKE_MEM_DEFINED(&r, sizeof r);
        printf("S%d seal: done (%d)\n", aead, r);
    }
    return 0;
}
