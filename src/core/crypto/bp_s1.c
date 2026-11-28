/*
 * bp_s1.c -- suite S1 backend on Monocypher 4.0.3.  C99; compiled only with
 * BP_CRYPTO_S1.  See bp_s1.h.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include <stdlib.h>
#include <string.h>
#include "bp_s1.h"
#include "bp_crypto.h"
#include "vendor/monocypher/monocypher.h"

void bp_s1_aead_seal(const bp_uint8 key[32], const bp_uint8 nonce[24],
                     const bp_uint8 *ad, bp_uint32 ad_len,
                     const bp_uint8 *pt, bp_uint32 pt_len,
                     bp_uint8 *ct, bp_uint8 tag[16])
{
    crypto_aead_lock(ct, tag, key, nonce, ad, ad_len, pt, pt_len);
}

int bp_s1_aead_open(const bp_uint8 key[32], const bp_uint8 nonce[24],
                    const bp_uint8 *ad, bp_uint32 ad_len,
                    const bp_uint8 *ct, bp_uint32 ct_len, const bp_uint8 tag[16],
                    bp_uint8 *pt)
{
    /* Monocypher checks the tag before it writes any plaintext; bp_crypto.c wipes pt on failure */
    return crypto_aead_unlock(pt, tag, key, nonce, ad, ad_len, ct, ct_len) == 0 ? 0 : -1;
}

int bp_s1_blake2b(const bp_uint8 *key32, const bp_s1_part *parts, int nparts,
                  bp_uint8 *out, bp_uint32 outlen)
{
    crypto_blake2b_ctx ctx;
    int i;
    if (outlen < 1 || outlen > 64) return -1;
    if (key32 != NULL) crypto_blake2b_keyed_init(&ctx, outlen, key32, 32);
    else               crypto_blake2b_init(&ctx, outlen);
    for (i = 0; i < nparts; i++)
        if (parts[i].n != 0) crypto_blake2b_update(&ctx, parts[i].p, parts[i].n);
    crypto_blake2b_final(&ctx, out);
    crypto_wipe(&ctx, sizeof ctx);
    return 0;
}

/* the caller's state must hold a Monocypher BLAKE2b context */
typedef char bp_s1_state_fits[(sizeof(crypto_blake2b_ctx) <= BP_S1_HASH_STATE_BYTES) ? 1 : -1];

int bp_s1_hash_init(void *state, const bp_uint8 *key32, bp_uint32 outlen)
{
    crypto_blake2b_ctx *ctx = (crypto_blake2b_ctx *)state;
    if (outlen < 1 || outlen > 64) return -1;
    if (key32 != NULL) crypto_blake2b_keyed_init(ctx, outlen, key32, 32);
    else               crypto_blake2b_init(ctx, outlen);
    return 0;
}

void bp_s1_hash_update(void *state, const bp_uint8 *data, bp_uint32 len)
{
    if (len != 0) crypto_blake2b_update((crypto_blake2b_ctx *)state, data, len);
}

void bp_s1_hash_final(void *state, bp_uint8 *out)
{
    crypto_blake2b_final((crypto_blake2b_ctx *)state, out);
    crypto_wipe(state, sizeof(crypto_blake2b_ctx));
}

int bp_s1_argon2id(bp_uint8 *out, bp_uint32 outlen,
                   const bp_uint8 *pw, bp_uint32 pwlen,
                   const bp_uint8 *salt, bp_uint32 saltlen,
                   const bp_uint8 *secret, bp_uint32 secretlen,
                   const bp_uint8 *ad, bp_uint32 adlen,
                   bp_uint32 m_kib, bp_uint32 passes, bp_uint32 lanes)
{
    crypto_argon2_config cfg;
    crypto_argon2_inputs in;
    crypto_argon2_extras ex;
    size_t bytes;
    void *work;

    if (lanes < 1 || passes < 1 || m_kib < 8 * lanes) return -1;
    if (sizeof(size_t) < 8 && m_kib > 4194303U) return -1;   /* m_kib * 1024 must fit */
    bytes = (size_t)m_kib * 1024;
    work = malloc(bytes);
    if (work == NULL) return -1;
    cfg.algorithm = CRYPTO_ARGON2_ID;
    cfg.nb_blocks = m_kib;
    cfg.nb_passes = passes;
    cfg.nb_lanes = lanes;
    in.pass = pw; in.pass_size = pwlen; in.salt = salt; in.salt_size = saltlen;
    ex.key = secret; ex.key_size = secretlen; ex.ad = ad; ex.ad_size = adlen;
    crypto_argon2(out, outlen, work, cfg, in, ex);
    crypto_wipe(work, bytes);   /* the work area held password-derived blocks */
    free(work);
    return 0;
}
