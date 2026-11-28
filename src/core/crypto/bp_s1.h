/*
 * bp_s1.h -- suite S1 backend: XChaCha20-Poly1305, BLAKE2b and Argon2id from the
 * vendored Monocypher 4.0.3.  C89 header; the implementation (bp_s1.c) is C99 and
 * is compiled only when BP_CRYPTO_S1 is defined.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BP_S1_H
#define BP_S1_H

#include "../bp_types.h"

/* AEAD: 32-byte key, 24-byte nonce, 16-byte tag.  ct must not overlap pt. */
void bp_s1_aead_seal(const bp_uint8 key[32], const bp_uint8 nonce[24],
                     const bp_uint8 *ad, bp_uint32 ad_len,
                     const bp_uint8 *pt, bp_uint32 pt_len,
                     bp_uint8 *ct, bp_uint8 tag[16]);
int bp_s1_aead_open(const bp_uint8 key[32], const bp_uint8 nonce[24],
                    const bp_uint8 *ad, bp_uint32 ad_len,
                    const bp_uint8 *ct, bp_uint32 ct_len, const bp_uint8 tag[16],
                    bp_uint8 *pt);

/* BLAKE2b over parts; key may be NULL (unkeyed) or 32 bytes; outlen 1..64. */
typedef struct { const bp_uint8 *p; bp_uint32 n; } bp_s1_part;
int bp_s1_blake2b(const bp_uint8 *key32, const bp_s1_part *parts, int nparts,
                  bp_uint8 *out, bp_uint32 outlen);

/* Incremental BLAKE2b in caller storage of BP_S1_HASH_STATE_BYTES bytes (8-byte aligned).
 * key32 NULL for unkeyed; outlen 1..64.  Returns 0, or -1 for a bad outlen. */
#define BP_S1_HASH_STATE_BYTES 288
int  bp_s1_hash_init(void *state, const bp_uint8 *key32, bp_uint32 outlen);
void bp_s1_hash_update(void *state, const bp_uint8 *data, bp_uint32 len);
void bp_s1_hash_final(void *state, bp_uint8 *out);

/* Argon2id (version 0x13).  The caller has checked the parameters:
 * lanes >= 1, passes >= 1, m_kib >= 8 * lanes.  Returns 0, or -1 if the
 * work area cannot be allocated.  secret/ad may be NULL with length 0. */
int bp_s1_argon2id(bp_uint8 *out, bp_uint32 outlen,
                   const bp_uint8 *pw, bp_uint32 pwlen,
                   const bp_uint8 *salt, bp_uint32 saltlen,
                   const bp_uint8 *secret, bp_uint32 secretlen,
                   const bp_uint8 *ad, bp_uint32 adlen,
                   bp_uint32 m_kib, bp_uint32 passes, bp_uint32 lanes);

#endif /* BP_S1_H */
