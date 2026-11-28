/*
 * bp_entropy.h -- the entropy pool (plan section 7).  Strict C89.
 *
 * Sources add bytes with a declared credit in bits (a declaration, not a
 * measurement).  The pool counts as seeded at 128 bits of credit; a secret key
 * (keygen) needs 256.  Output is produced with Ascon-XOF128: the state is
 * replaced after every output, and on a platform with an OS random source 32
 * fresh OS bytes are mixed into every output call (so a forked process or a
 * restored VM does not repeat earlier output).
 *
 * Credits, per source and in total at most 256:
 *   OS source 256, hardware instruction 128, imported entropy 8 per byte,
 *   gathered timing at most 64, weak data (clock, pid, addresses) 0.
 *
 * Not thread-safe.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BP_ENTROPY_H
#define BP_ENTROPY_H

#include "../bp_types.h"

#define BP_ENT_OS      1
#define BP_ENT_HW      2
#define BP_ENT_IMPORT  3
#define BP_ENT_GATHER  4
#define BP_ENT_WEAK    5

#define BP_ENT_SEEDED_BITS 128UL   /* enough for salts, nonces, set IDs */
#define BP_ENT_SECRET_BITS 256UL   /* enough for a secret key (keygen) */

/* Mix data into the pool and credit `bits` for it, within the source's cap.
 * Returns the pool's total credit. */
bp_uint32 bp_entropy_add(int source, const bp_uint8 *data, bp_uint32 len, bp_uint32 bits);

/* Imported entropy: len bytes of uniformly random data, credited 8 bits each. */
bp_uint32 bp_entropy_import(const bp_uint8 *data, bp_uint32 len);

/* Gathered timing jitter (at most 64 bits of credit in total). */
bp_uint32 bp_entropy_gather(void);

/* Mix the weak data (time, clock, addresses, counter); credit 0. */
void bp_entropy_add_weak(void);

/* Ask the OS source for 32 bytes and credit 256 bits.  Returns 0, or -1 if the
 * platform has none or it failed. */
int bp_entropy_seed_os(void);

bp_uint32 bp_entropy_credit(void);

/* Fill buf with n bytes.  On first use this seeds from the OS source if there is
 * one.  BP_CRYPTO_E_ENTROPY (and no output) if the credit is below
 * BP_ENT_SEEDED_BITS. */
int bp_random_bytes(bp_uint8 *buf, bp_uint32 n);

/* Same, but needs BP_ENT_SECRET_BITS of credit (a key file). */
int bp_random_secret(bp_uint8 *buf, bp_uint32 n);

/* Platform file (bp_entropy_os_*.c): n bytes from the OS source, 0 or -1. */
int bp_entropy_os_read(bp_uint8 *buf, bp_uint32 n);

#ifdef BP_CRYPTO_TESTING
/* Forget everything (state, credits, the OS-seeded flag). */
void bp_entropy_test_reset(void);
/* Replace the OS source: NULL restores the real one. */
void bp_entropy_test_set_os(int (*fn)(bp_uint8 *buf, bp_uint32 n));
/* Total bytes the pool has handed out since the last reset. */
bp_uint32 bp_entropy_test_output_count(void);
#endif

#endif /* BP_ENTROPY_H */
