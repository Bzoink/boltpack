/*
 * bp_crypto.h -- cryptographic layer for BoltPack format 8 (encrypted archives).
 *
 * Phase 1 of the encryption plan.  Nothing in the archive code calls this yet.
 *
 * Language: this header and bp_crypto.c, bp_cutil.c and bp_entropy.c are
 * strict C89 (no long long; 64-bit counters are bp_uint64 halves; lengths are
 * bp_uint32) because the constrained builds (profile E2, suite S2) must compile
 * them with 16-bit toolchains.  Suite S1 (XChaCha20-Poly1305, BLAKE2b, Argon2id)
 * lives in bp_s1.c, which is C99/C11 and is compiled only when BP_CRYPTO_S1 is
 * defined.  Suite S2 sits on a swappable backend (bp_ascon.h).
 *
 * Threads: none of this is thread-safe.  Seal frames on one thread (the writer
 * thread), or add locking around bp_frame_seal and the entropy pool.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BP_CRYPTO_H
#define BP_CRYPTO_H

#include "../bp_types.h"

/* Status codes of this module.  The archive layer maps them to BP_ERR_* in
 * phase 3 (BP_ERR_PASSWORD and BP_ERR_UNSUPPORTED_CIPHER are new there). */
#define BP_CRYPTO_OK            0
#define BP_CRYPTO_E_ARG        -1   /* bad argument, bad field value */
#define BP_CRYPTO_E_UNSUPPORTED -2  /* suite or (kdf, aead) pair not defined or not built */
#define BP_CRYPTO_E_AUTH       -3   /* tag mismatch */
#define BP_CRYPTO_E_ENTROPY    -5   /* entropy pool not seeded, or the source failed */
#define BP_CRYPTO_E_NOMEM      -6
#define BP_CRYPTO_E_LIMIT      -7   /* KDF cost above the reader's limits */

/* aead ids (key header `aead`) */
#define BP_SUITE_S1 1   /* XChaCha20-Poly1305, BLAKE2b; 32-byte key, 24-byte nonce */
#define BP_SUITE_S2 2   /* Ascon-AEAD128, Ascon-XOF128; 16-byte key, 16-byte nonce */

/* kdf ids (key header `kdf`) */
#define BP_KDF_NONE     0
#define BP_KDF_ARGON2ID 1

#define BP_TAG_LEN      16
#define BP_ENVELOPE_LEN 8
#define BP_AAD_EXTRA_MAX 64

/* Which suites this build contains (S2 is always present). */
int bp_suite_available(int aead);
bp_uint32 bp_suite_nonce_len(int aead);   /* 24 (S1), 16 (S2), 0 if unknown */
bp_uint32 bp_suite_key_len(int aead);     /* 32 (S1), 16 (S2), 0 if unknown */
bp_uint32 bp_frame_overhead(int aead);    /* envelope + nonce + tag + fcrc: 52 or 44, 0 if unknown */

/* ---- helpers (bp_cutil.c) ---------------------------------------------- */

/* Overwrite memory in a way the compiler may not drop. */
void bp_wipe(void *p, bp_uint32 n);

/* 1 if the n bytes are equal, 0 otherwise; time independent of the contents. */
int bp_ct_equal(const bp_uint8 *a, const bp_uint8 *b, bp_uint32 n);

/* ---- hashes ------------------------------------------------------------- */

/* The data of a hash is the concatenation of parts. */
typedef struct {
    const bp_uint8 *p;
    bp_uint32 n;
} bp_part;

/* Hu: unkeyed.  S1: BLAKE2b with digest size outlen (1..64).  S2: Ascon-XOF128, outlen bytes. */
int bp_hu(int aead, const bp_part *parts, int nparts, bp_uint8 *out, bp_uint32 outlen);

/* Hk: keyed with a 32-byte key.  S1: BLAKE2b keyed, digest size outlen.
 * S2: Ascon-XOF128 over key || data, outlen bytes. */
int bp_hk(int aead, const bp_uint8 key[32], const bp_part *parts, int nparts,
          bp_uint8 *out, bp_uint32 outlen);

/* ---- keys --------------------------------------------------------------- */

/* The fields of the key header that determine the keys (draft section 3). */
typedef struct {
    bp_uint8  kdf;            /* BP_KDF_* */
    bp_uint8  aead;           /* BP_SUITE_* */
    bp_uint8  norm_version;   /* 1 with Argon2id, 0 with BP_KDF_NONE */
    bp_uint8  lanes;          /* Argon2id; 0 with BP_KDF_NONE */
    bp_uint32 m_cost_kib;     /* Argon2id memory in KiB; 0 with BP_KDF_NONE */
    bp_uint16 t_cost;         /* Argon2id passes; 0 with BP_KDF_NONE */
    bp_uint8  salt[16];
} bp_keyparams;

/* KDF limits of a reader.  NULL means the defaults. */
typedef struct {
    bp_uint32 max_m_cost_kib;   /* default 262144 (256 MiB) */
    bp_uint32 max_t_cost;       /* default 8 */
    bp_uint32 max_lanes;        /* default 4 */
} bp_kdf_limits;

#define BP_KDF_DEFAULT_MAX_M_KIB 262144UL
#define BP_KDF_DEFAULT_MAX_T     8UL
#define BP_KDF_DEFAULT_MAX_LANES 4UL

/* Keys of one set.  Copying the struct is safe: the frame counter of the hedged
 * nonce is process-wide (bp_crypto.c), not part of the keys. */
typedef struct {
    int       aead;
    bp_uint8  set8[8];        /* set ID, little-endian */
    bp_uint8  k_seal[32];     /* S2 uses the first 16 bytes */
    bp_uint8  k_nonce[32];
    bp_uint32 seals;          /* frames sealed under this copy of the keys (a counting aid) */
} bp_keys;

/* The first 32 bytes of the key header: magic "BKEY", key_version 1, kdf, aead,
 * slot_count 1, m_cost_kib, t_cost, lanes, norm_version, salt.  (The verifier
 * and the CRC follow in the archive layer.)  Returns BP_CRYPTO_OK or E_ARG. */
int bp_keyparams_pack32(const bp_keyparams *kp, bp_uint8 out[32]);

/* Validate the fields before any work or allocation: the defined (kdf, aead)
 * pairs only, norm_version and cost fields consistent with kdf, Argon2id costs
 * within lim (and m >= 8 * lanes, t >= 1, lanes >= 1).  E_UNSUPPORTED for an
 * undefined or unbuilt pair, E_LIMIT for a cost above lim, E_ARG otherwise. */
int bp_keyparams_check(const bp_keyparams *kp, const bp_kdf_limits *lim);

/* Derive the keys and the verifier.  secret is the password bytes (already
 * normalised, kdf 1) or the 32-byte key (kdf 0).  The caller compares
 * verifier_out with the archive's verifier using bp_ct_equal (a mismatch is a
 * wrong secret, section 10.1 of the plan).  On error nothing is left in *out. */
int bp_keys_derive(bp_keys *out, bp_uint8 verifier_out[16], const bp_keyparams *kp,
                   const bp_uint8 set8[8], const bp_uint8 *secret, bp_uint32 secret_len,
                   const bp_kdf_limits *lim);

/* ---- frames (draft sections 4, 7) ---------------------------------------- */

/* The writer seals with this function and no other: it draws E from the entropy
 * pool, uses and bumps the process-wide counter ctr (starts at 0, +1 per frame,
 * never reset), computes the hedged nonce, seals, and returns the nonce
 * (rules 1 and 2 of plan section 5.4).  envelope is the 8 bytes kind, fflags,
 * 0, 0, len(u32 LE) with len = nonce length + pt_len + 16.  extra is the AAD
 * tail (at most BP_AAD_EXTRA_MAX bytes).  nonce_out has bp_suite_nonce_len()
 * bytes, ct_out pt_len bytes (must not overlap pt), tag_out 16 bytes.
 * Fails with BP_CRYPTO_E_ENTROPY if the pool is not seeded; nothing is output. */
int bp_frame_seal(bp_keys *k, const bp_uint8 envelope[8],
                  const bp_uint8 *extra, bp_uint32 extra_len,
                  const bp_uint8 *pt, bp_uint32 pt_len,
                  bp_uint8 *nonce_out, bp_uint8 *ct_out, bp_uint8 tag_out[16]);

/* Open a frame.  BP_CRYPTO_E_AUTH if the tag does not verify (pt_out is wiped). */
int bp_frame_open(const bp_keys *k, const bp_uint8 envelope[8],
                  const bp_uint8 *extra, bp_uint32 extra_len,
                  const bp_uint8 *nonce, const bp_uint8 *ct, bp_uint32 ct_len,
                  const bp_uint8 tag[16], bp_uint8 *pt_out);

/* ---- test support (never in a release build) ------------------------------ */

/* Returns 1 if this object was built with BP_CRYPTO_TESTING (hooks present). */
int bp_crypto_has_test_hooks(void);

#ifdef BP_CRYPTO_TESTING
/* Raw primitives with caller-chosen nonces, for known-answer tests only. */
int bp_test_aead_seal(int aead, const bp_uint8 *key, const bp_uint8 *nonce,
                      const bp_uint8 *ad, bp_uint32 ad_len, const bp_uint8 *pt,
                      bp_uint32 pt_len, bp_uint8 *ct, bp_uint8 tag[16]);
int bp_test_aead_open(int aead, const bp_uint8 *key, const bp_uint8 *nonce,
                      const bp_uint8 *ad, bp_uint32 ad_len, const bp_uint8 *ct,
                      bp_uint32 ct_len, const bp_uint8 tag[16], bp_uint8 *pt);
/* The process-wide frame counter of the hedged nonce. */
void bp_test_ctr_set(bp_uint32 lo, bp_uint32 hi);
void bp_test_ctr_get(bp_uint32 *lo, bp_uint32 *hi);
/* The AAD bytes of a frame (draft 4.2 / 7); out has room for 25 + extra_len bytes. */
bp_uint32 bp_test_build_aad(int aead, const bp_uint8 set8[8], const bp_uint8 envelope[8],
                            const bp_uint8 *extra, bp_uint32 extra_len, bp_uint8 *out);
/* If set, bp_frame_seal takes E (32 bytes) from this function instead of the pool. */
extern int (*bp_test_E_hook)(bp_uint8 E[32]);
/* Argon2id with a secret and associated data (RFC 9106 vector), S1 builds only. */
int bp_test_argon2id_raw(bp_uint8 *out, bp_uint32 outlen, const bp_uint8 *pw, bp_uint32 pwlen,
                         const bp_uint8 *salt, bp_uint32 saltlen, const bp_uint8 *secret,
                         bp_uint32 secretlen, const bp_uint8 *ad, bp_uint32 adlen,
                         bp_uint32 m_kib, bp_uint32 t, bp_uint32 lanes);
#endif

#endif /* BP_CRYPTO_H */
