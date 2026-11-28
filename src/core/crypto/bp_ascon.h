/*
 * bp_ascon.h -- the Ascon (NIST SP 800-232) backend interface used by suite S2
 * and by the entropy pool.  C89 header.
 *
 * Phase 1 implements it with the vendored ascon-c reference permutation
 * (bp_ascon_ref.c, C99).  Phase 1b replaces that file with a strict C89
 * implementation (16- and 32-bit words) behind this same interface, and tests it
 * against the reference and the NIST known-answer files.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BP_ASCON_H
#define BP_ASCON_H

#include "../bp_types.h"

/* Ascon-AEAD128: key, nonce and tag are 16 bytes.  ct has pt_len bytes and must
 * not overlap pt. */
void bp_ascon_aead_seal(const bp_uint8 key[16], const bp_uint8 nonce[16],
                        const bp_uint8 *ad, bp_uint32 ad_len,
                        const bp_uint8 *pt, bp_uint32 pt_len,
                        bp_uint8 *ct, bp_uint8 tag[16]);

/* Returns 0 if the tag verifies (pt then holds the plaintext), -1 otherwise
 * (pt is wiped).  pt must not overlap ct. */
int bp_ascon_aead_open(const bp_uint8 key[16], const bp_uint8 nonce[16],
                       const bp_uint8 *ad, bp_uint32 ad_len,
                       const bp_uint8 *ct, bp_uint32 ct_len, const bp_uint8 tag[16],
                       bp_uint8 *pt);

/* Ascon-XOF128, incremental: init, absorb any number of times, then squeeze any
 * number of times.  Absorbing after the first squeeze is an error (undefined). */
typedef struct {
    bp_uint32 s[10];     /* the five 64-bit state words as (lo, hi) pairs */
    bp_uint8  buf[8];    /* absorb buffer, or the current squeeze block */
    bp_uint32 pos;       /* bytes used in buf */
    bp_uint32 squeezing; /* 0 while absorbing, 1 after the first squeeze */
} bp_xof;

void bp_xof_init(bp_xof *x);
void bp_xof_absorb(bp_xof *x, const bp_uint8 *data, bp_uint32 len);
void bp_xof_squeeze(bp_xof *x, bp_uint8 *out, bp_uint32 len);

#endif /* BP_ASCON_H */
