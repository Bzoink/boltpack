/*
 * bp_crypto.c -- hashes, keys, frame sealing for format 8.  Strict C89.
 * Layouts follow work/encryption-format-draft.md (sections 2 to 7).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include <stddef.h>
#include <string.h>
#include "bp_crypto.h"
#include "bp_ascon.h"
#include "bp_entropy.h"
#ifdef BP_CRYPTO_S1
#include "bp_s1.h"
#endif

#define BP_MAX_PARTS 10
#define BP_LABEL_MAX 64

/* the state of bp_hash_ctx must be 8-byte aligned (BLAKE2b has 64-bit members) */
typedef char bp_hash_ctx_state_aligned[(offsetof(bp_hash_ctx, w) % 8 == 0) ? 1 : -1];

/* the S2 hash state must fit the shared bp_hash_ctx storage */
typedef char bp_xof_fits_hash_ctx[(sizeof(bp_xof) <= sizeof(((bp_hash_ctx *)0)->w)) ? 1 : -1];

/* Hash labels, without a terminator (S1 hashes a 0x00 after the label, S2 uses the label as the
 * CXOF customisation string).  L_AAD is different: it is AAD data and keeps its NUL. */
static const bp_uint8 L_MASTER[] = "bp8/master";
static const bp_uint8 L_SEAL[]   = "bp8/seal";
static const bp_uint8 L_NONCE[]  = "bp8/nonce-key";   /* derives k_nonce from the master key */
static const bp_uint8 L_HEDGE[]  = "bp8/hedge";       /* the hedged nonce, keyed with k_nonce */
static const bp_uint8 L_VERIFY[] = "bp8/verify";
static const bp_uint8 L_AAD[]    = "bp8/aad";

/* The frame counter of the hedged nonce: one per process (draft 4.3), so copies of
 * a bp_keys and separate key sets share one sequence.  Not thread-safe. */
static bp_uint32 g_ctr_lo, g_ctr_hi;

#ifdef BP_CRYPTO_TESTING
int (*bp_test_E_hook)(bp_uint8 E[32]) = NULL;
void bp_test_ctr_set(bp_uint32 lo, bp_uint32 hi) { g_ctr_lo = lo; g_ctr_hi = hi; }
void bp_test_ctr_get(bp_uint32 *lo, bp_uint32 *hi) { *lo = g_ctr_lo; *hi = g_ctr_hi; }
#endif

int bp_crypto_has_test_hooks(void)
{
#ifdef BP_CRYPTO_TESTING
    return 1;
#else
    return 0;
#endif
}

/* ---- suites ------------------------------------------------------------- */

int bp_suite_available(int aead)
{
    if (aead == BP_SUITE_S2) return 1;
#ifdef BP_CRYPTO_S1
    if (aead == BP_SUITE_S1) return 1;
#endif
    return 0;
}

bp_uint32 bp_suite_nonce_len(int aead)
{
    return aead == BP_SUITE_S1 ? 24U : aead == BP_SUITE_S2 ? 16U : 0U;
}

bp_uint32 bp_suite_key_len(int aead)
{
    return aead == BP_SUITE_S1 ? 32U : aead == BP_SUITE_S2 ? 16U : 0U;
}

bp_uint32 bp_frame_overhead(int aead)
{
    bp_uint32 nl = bp_suite_nonce_len(aead);
    return nl == 0 ? 0U : BP_ENVELOPE_LEN + nl + BP_TAG_LEN + 4U;
}

/* ---- frame limits (draft 8.1) ---------------------------------------------- */

int bp_frame_body_limits(int aead, int kind, bp_uint32 *min_len, bp_uint32 *max_len)
{
    bp_uint32 lo = 0, hi;
    if (aead != BP_SUITE_S1 && aead != BP_SUITE_S2) return BP_CRYPTO_E_UNSUPPORTED;
    switch (kind) {
    case BP_KIND_ENTRY_HEADER:  hi = 27UL + (aead == BP_SUITE_S1 ? 65501UL : 1024UL); break;
    case BP_KIND_BLOCK:         hi = 8UL + (aead == BP_SUITE_S1 ? 16777216UL : 32768UL); break;
    case BP_KIND_ENTRY_TRAILER: lo = 16; hi = 16; break;
    case BP_KIND_TABLE_CHUNK:   hi = aead == BP_SUITE_S1 ? 1048576UL : 16384UL; break;
    case BP_KIND_END:           hi = 0; break;
    default: return BP_CRYPTO_E_ARG;
    }
    if (min_len != NULL) *min_len = lo;
    if (max_len != NULL) *max_len = hi;
    return BP_CRYPTO_OK;
}

/* ---- hashes ------------------------------------------------------------- */

/* Keyed hashes (draft 2).  S1: BLAKE2b keyed with key32 over label || 0x00 || data, digest size
 * outlen.  S2: Ascon-CXOF128 customised by the label (no terminator), over key32 || data ||
 * outlen as a 32-bit little-endian number, outlen bytes (the output length is bound into the
 * message, as KMAC does, so outputs of different lengths are unrelated). */
static int hash_parts(int aead, const bp_uint8 *key32, const bp_uint8 *label, bp_uint32 label_len,
                      const bp_part *parts, int nparts, bp_uint8 *out, bp_uint32 outlen)
{
    int i;
    if (parts == NULL || nparts < 0 || nparts > BP_MAX_PARTS - 2 || out == NULL || outlen == 0)
        return BP_CRYPTO_E_ARG;
    for (i = 0; i < nparts; i++)
        if (parts[i].n != 0 && parts[i].p == NULL) return BP_CRYPTO_E_ARG;
    if (key32 == NULL ? (label != NULL || label_len != 0) : (label == NULL || label_len == 0 || label_len > BP_LABEL_MAX))
        return BP_CRYPTO_E_ARG;
    if (!bp_suite_available(aead)) return BP_CRYPTO_E_UNSUPPORTED;
    if (aead == BP_SUITE_S2) {
        bp_xof x;
        bp_uint8 ol[4];
        if (key32 != NULL) {
            if (bp_cxof_init(&x, label, label_len) != 0) return BP_CRYPTO_E_ARG;
            bp_xof_absorb(&x, key32, 32);
        } else {
            bp_xof_init(&x);
        }
        for (i = 0; i < nparts; i++) bp_xof_absorb(&x, parts[i].p, parts[i].n);
        if (key32 != NULL) { bp_write_le32(ol, outlen); bp_xof_absorb(&x, ol, 4); }
        bp_xof_squeeze(&x, out, outlen);
        bp_wipe(&x, (bp_uint32)sizeof x);
        return BP_CRYPTO_OK;
    }
#ifdef BP_CRYPTO_S1
    {
        bp_s1_part sp[BP_MAX_PARTS];
        static const bp_uint8 nul = 0;
        int n = 0;
        if (outlen > 64) return BP_CRYPTO_E_ARG;
        if (key32 != NULL) {
            sp[n].p = label; sp[n].n = label_len; n++;
            sp[n].p = &nul; sp[n].n = 1; n++;
        }
        for (i = 0; i < nparts; i++) { sp[n].p = parts[i].p; sp[n].n = parts[i].n; n++; }
        return bp_s1_blake2b(key32, sp, n, out, outlen) == 0 ? BP_CRYPTO_OK : BP_CRYPTO_E_ARG;
    }
#else
    return BP_CRYPTO_E_UNSUPPORTED;
#endif
}

int bp_hu(int aead, const bp_part *parts, int nparts, bp_uint8 *out, bp_uint32 outlen)
{
    return hash_parts(aead, NULL, NULL, 0, parts, nparts, out, outlen);
}

int bp_hk(int aead, const bp_uint8 key[32], const bp_uint8 *label, bp_uint32 label_len,
          const bp_part *parts, int nparts, bp_uint8 *out, bp_uint32 outlen)
{
    if (key == NULL) return BP_CRYPTO_E_ARG;
    return hash_parts(aead, key, label, label_len, parts, nparts, out, outlen);
}

int bp_hash_init(bp_hash_ctx *c, int aead, const bp_uint8 *key32, const bp_uint8 *label,
                 bp_uint32 label_len, bp_uint32 outlen)
{
    if (c == NULL || outlen == 0) return BP_CRYPTO_E_ARG;
    if (key32 == NULL ? (label != NULL || label_len != 0) : (label == NULL || label_len == 0 || label_len > BP_LABEL_MAX))
        return BP_CRYPTO_E_ARG;
    if (!bp_suite_available(aead)) return BP_CRYPTO_E_UNSUPPORTED;
    memset(c, 0, sizeof *c);
    c->aead = aead;
    c->outlen = outlen;
    c->keyed = key32 != NULL;
    if (aead == BP_SUITE_S2) {
        if (key32 != NULL) {
            if (bp_cxof_init((bp_xof *)c->w, label, label_len) != 0) return BP_CRYPTO_E_ARG;
            bp_xof_absorb((bp_xof *)c->w, key32, 32);
        } else {
            bp_xof_init((bp_xof *)c->w);
        }
        return BP_CRYPTO_OK;
    }
#ifdef BP_CRYPTO_S1
    if (bp_s1_hash_init(c->w, key32, outlen) != 0) return BP_CRYPTO_E_ARG;
    if (key32 != NULL) {
        static const bp_uint8 nul = 0;
        bp_s1_hash_update(c->w, label, label_len);
        bp_s1_hash_update(c->w, &nul, 1);
    }
    return BP_CRYPTO_OK;
#else
    return BP_CRYPTO_E_UNSUPPORTED;
#endif
}

void bp_hash_update(bp_hash_ctx *c, const bp_uint8 *data, bp_uint32 len)
{
    if (len == 0) return;
    if (c->aead == BP_SUITE_S2) bp_xof_absorb((bp_xof *)c->w, data, len);
#ifdef BP_CRYPTO_S1
    else if (c->aead == BP_SUITE_S1) bp_s1_hash_update(c->w, data, len);
#endif
}

void bp_hash_final(bp_hash_ctx *c, bp_uint8 *out)
{
    if (c->aead == BP_SUITE_S2) {
        if (c->keyed) {
            bp_uint8 ol[4];
            bp_write_le32(ol, c->outlen);
            bp_xof_absorb((bp_xof *)c->w, ol, 4);
        }
        bp_xof_squeeze((bp_xof *)c->w, out, c->outlen);
    }
#ifdef BP_CRYPTO_S1
    else if (c->aead == BP_SUITE_S1) bp_s1_hash_final(c->w, out);
#endif
    bp_wipe(c, (bp_uint32)sizeof *c);
}

/* ---- key-use budget ----------------------------------------------------- */

int bp_budget_ok(int aead, const bp_budget_use *present, bp_uint32 add_frames, bp_uint32 add_bytes)
{
    bp_uint64 frames, bytes, limit;
    bp_uint32 i;
    if (present == NULL) return BP_CRYPTO_E_ARG;
    if (aead != BP_SUITE_S1 && aead != BP_SUITE_S2) return BP_CRYPTO_E_UNSUPPORTED;
    frames = bp_u64_from_u32(present->frames);
    /* the table chunk frames of earlier revisions that appends overwrote */
    for (i = 0; i < present->revision && i < BP_MAX_REVISION; i++)
        frames = bp_u64_add(frames, bp_u64_from_u32(present->table_chunks));
    frames = bp_u64_add(frames, bp_u64_from_u32(add_frames));
    limit.hi = 0; limit.lo = 0;
    if (aead == BP_SUITE_S2) limit.lo = 16777216UL;      /* 2^24 */
    else                     limit.hi = 1;               /* 2^32 */
    if (bp_u64_cmp(frames, limit) > 0) return 0;
    if (aead == BP_SUITE_S2) {                           /* 2^40 bytes; S1 sets no byte limit */
        bytes = bp_u64_add(present->bytes, bp_u64_from_u32(add_bytes));
        limit.hi = 256; limit.lo = 0;
        if (bp_u64_cmp(bytes, limit) > 0) return 0;
    }
    return 1;
}

/* ---- keys --------------------------------------------------------------- */

int bp_keyparams_pack32(const bp_keyparams *kp, bp_uint8 out[32])
{
    if (kp == NULL || out == NULL) return BP_CRYPTO_E_ARG;
    out[0] = 0x42; out[1] = 0x4B; out[2] = 0x45; out[3] = 0x59;   /* "BKEY" */
    out[4] = 1;                                                    /* key_version */
    out[5] = kp->kdf;
    out[6] = kp->aead;
    out[7] = 1;                                                    /* slot_count */
    bp_write_le32(out + 8, kp->m_cost_kib);
    bp_write_le16(out + 12, kp->t_cost);
    out[14] = kp->lanes;
    out[15] = kp->norm_version;
    memcpy(out + 16, kp->salt, 16);
    return BP_CRYPTO_OK;
}

int bp_keyparams_check(const bp_keyparams *kp, const bp_kdf_limits *lim)
{
    bp_uint32 max_m = BP_KDF_DEFAULT_MAX_M_KIB, max_t = BP_KDF_DEFAULT_MAX_T,
              max_l = BP_KDF_DEFAULT_MAX_LANES;
    if (kp == NULL) return BP_CRYPTO_E_ARG;
    if (lim != NULL) { max_m = lim->max_m_cost_kib; max_t = lim->max_t_cost; max_l = lim->max_lanes; }
    /* the defined pairs, and no others (draft section 2) */
    if (!((kp->kdf == BP_KDF_ARGON2ID && kp->aead == BP_SUITE_S1) ||
          (kp->kdf == BP_KDF_NONE && (kp->aead == BP_SUITE_S1 || kp->aead == BP_SUITE_S2))))
        return BP_CRYPTO_E_UNSUPPORTED;
    if (!bp_suite_available(kp->aead)) return BP_CRYPTO_E_UNSUPPORTED;
    if (kp->kdf == BP_KDF_NONE) {
        if (kp->m_cost_kib != 0 || kp->t_cost != 0 || kp->lanes != 0 || kp->norm_version != 0)
            return BP_CRYPTO_E_ARG;
        return BP_CRYPTO_OK;
    }
    if (kp->norm_version != 1) return BP_CRYPTO_E_ARG;
    /* contract of Argon2: lanes >= 1, passes >= 1, memory >= 8 * lanes */
    if (kp->lanes < 1 || kp->t_cost < 1 || kp->m_cost_kib < 8UL * kp->lanes) return BP_CRYPTO_E_ARG;
    if (kp->m_cost_kib > max_m || (bp_uint32)kp->t_cost > max_t || (bp_uint32)kp->lanes > max_l)
        return BP_CRYPTO_E_LIMIT;
    return BP_CRYPTO_OK;
}

int bp_keys_derive(bp_keys *out, bp_uint8 verifier_out[16], const bp_keyparams *kp,
                   const bp_uint8 set8[8], const bp_uint8 *secret, bp_uint32 secret_len,
                   const bp_kdf_limits *lim)
{
    bp_uint8 master[32], packed[32];
    bp_part p[3];
    int rc;

    if (out == NULL || verifier_out == NULL) return BP_CRYPTO_E_ARG;
    /* every failure below leaves both outputs empty */
    memset(out, 0, sizeof *out);
    memset(verifier_out, 0, 16);
    if (kp == NULL || set8 == NULL || (secret == NULL && secret_len != 0)) return BP_CRYPTO_E_ARG;
    rc = bp_keyparams_check(kp, lim);
    if (rc != BP_CRYPTO_OK) return rc;

    if (kp->kdf == BP_KDF_NONE) {
        if (secret_len != 32) return BP_CRYPTO_E_ARG;
        /* master = Hk(secret32, "bp8/master", salt, 32): the 32-byte key is the key of the hash */
        p[0].p = kp->salt; p[0].n = 16;
        rc = bp_hk(kp->aead, secret, L_MASTER, (bp_uint32)sizeof L_MASTER - 1, p, 1, master, 32);
        if (rc != BP_CRYPTO_OK) return rc;
    } else {
#ifdef BP_CRYPTO_S1
        if (bp_s1_argon2id(master, 32, secret, secret_len, kp->salt, 16, NULL, 0, NULL, 0,
                           kp->m_cost_kib, kp->t_cost, kp->lanes) != 0)
            return BP_CRYPTO_E_NOMEM;
#else
        return BP_CRYPTO_E_UNSUPPORTED;
#endif
    }

    out->aead = kp->aead;
    memcpy(out->set8, set8, 8);

    p[0].p = set8; p[0].n = 8;
    rc = bp_hk(kp->aead, master, L_SEAL, (bp_uint32)sizeof L_SEAL - 1, p, 1, out->k_seal, 32);
    if (rc == BP_CRYPTO_OK)
        rc = bp_hk(kp->aead, master, L_NONCE, (bp_uint32)sizeof L_NONCE - 1, p, 1, out->k_nonce, 32);
    if (rc == BP_CRYPTO_OK) {
        bp_keyparams_pack32(kp, packed);
        p[1].p = packed; p[1].n = 32;
        rc = bp_hk(kp->aead, master, L_VERIFY, (bp_uint32)sizeof L_VERIFY - 1, p, 2, verifier_out, 16);
    }
    bp_wipe(master, sizeof master);
    if (rc != BP_CRYPTO_OK) {
        bp_wipe(out, (bp_uint32)sizeof *out);
        bp_wipe(verifier_out, 16);
    }
    return rc;
}

/* ---- frames ------------------------------------------------------------- */

/* AAD = "bp8/aad\0" || aead || set_id || envelope || extra */
static bp_uint32 build_aad(bp_uint8 *aad, int aead, const bp_uint8 set8[8],
                           const bp_uint8 envelope[8], const bp_uint8 *extra, bp_uint32 extra_len)
{
    bp_uint32 n = 0;
    memcpy(aad + n, L_AAD, sizeof L_AAD); n += (bp_uint32)sizeof L_AAD;
    aad[n++] = (bp_uint8)aead;
    memcpy(aad + n, set8, 8); n += 8;
    memcpy(aad + n, envelope, 8); n += 8;
    if (extra_len != 0) { memcpy(aad + n, extra, extra_len); n += extra_len; }
    return n;
}

static int aead_seal(int aead, const bp_uint8 *key, const bp_uint8 *nonce,
                     const bp_uint8 *ad, bp_uint32 ad_len, const bp_uint8 *pt, bp_uint32 pt_len,
                     bp_uint8 *ct, bp_uint8 tag[16])
{
    if (aead == BP_SUITE_S2) {
        bp_ascon_aead_seal(key, nonce, ad, ad_len, pt, pt_len, ct, tag);
        return BP_CRYPTO_OK;
    }
#ifdef BP_CRYPTO_S1
    if (aead == BP_SUITE_S1) {
        bp_s1_aead_seal(key, nonce, ad, ad_len, pt, pt_len, ct, tag);
        return BP_CRYPTO_OK;
    }
#endif
    return BP_CRYPTO_E_UNSUPPORTED;
}

static int aead_open(int aead, const bp_uint8 *key, const bp_uint8 *nonce,
                     const bp_uint8 *ad, bp_uint32 ad_len, const bp_uint8 *ct, bp_uint32 ct_len,
                     const bp_uint8 tag[16], bp_uint8 *pt)
{
    int rc = -2;
    if (aead == BP_SUITE_S2) rc = bp_ascon_aead_open(key, nonce, ad, ad_len, ct, ct_len, tag, pt);
#ifdef BP_CRYPTO_S1
    else if (aead == BP_SUITE_S1) rc = bp_s1_aead_open(key, nonce, ad, ad_len, ct, ct_len, tag, pt);
#endif
    if (rc == -2) return BP_CRYPTO_E_UNSUPPORTED;
    if (rc != 0) {                      /* a forged frame leaves no plaintext behind */
        if (ct_len != 0) bp_wipe(pt, ct_len);
        return BP_CRYPTO_E_AUTH;
    }
    return BP_CRYPTO_OK;
}

static int check_envelope(int aead, const bp_uint8 envelope[8], bp_uint32 body_len)
{
    bp_uint32 nl = bp_suite_nonce_len(aead), lo, hi;
    if (nl == 0 || !bp_suite_available(aead)) return BP_CRYPTO_E_UNSUPPORTED;
    if (envelope[2] != 0 || envelope[3] != 0) return BP_CRYPTO_E_ARG;
    /* the kind must be valid and the body inside its limits for this suite (draft 8.1) */
    if (bp_frame_body_limits(aead, envelope[0], &lo, &hi) != BP_CRYPTO_OK) return BP_CRYPTO_E_ARG;
    if (body_len < lo || body_len > hi) return BP_CRYPTO_E_ARG;
    /* len = nonce + plaintext + tag; refuse a body whose frame length cannot be
     * represented before adding the overhead (it would wrap) */
    if (body_len > 0xFFFFFFFFUL - nl - BP_TAG_LEN) return BP_CRYPTO_E_ARG;
    if (bp_read_le32(envelope + 4) != nl + body_len + BP_TAG_LEN) return BP_CRYPTO_E_ARG;
    return BP_CRYPTO_OK;
}

int bp_frame_seal(bp_keys *k, const bp_uint8 envelope[8],
                  const bp_uint8 *extra, bp_uint32 extra_len,
                  const bp_uint8 *pt, bp_uint32 pt_len,
                  bp_uint8 *nonce_out, bp_uint8 *ct_out, bp_uint8 tag_out[16])
{
    bp_uint8 E[32], aad[25 + BP_AAD_EXTRA_MAX], ctr8[8], aadlen4[4], nonce[24];
    bp_part p[6];
    bp_uint32 aad_len, nl;
    int rc;

    if (k == NULL || envelope == NULL || nonce_out == NULL || tag_out == NULL ||
        (pt_len != 0 && (pt == NULL || ct_out == NULL)) ||
        extra_len > BP_AAD_EXTRA_MAX || (extra_len != 0 && extra == NULL))
        return BP_CRYPTO_E_ARG;
    rc = check_envelope(k->aead, envelope, pt_len);
    if (rc != BP_CRYPTO_OK) return rc;
    nl = bp_suite_nonce_len(k->aead);

    /* rule 2 of plan 5.4: E from the pool (fails closed), ctr, AAD, plaintext */
#ifdef BP_CRYPTO_TESTING
    if (bp_test_E_hook != NULL) rc = bp_test_E_hook(E) == 0 ? 0 : -1;
    else
#endif
    rc = bp_random_bytes(E, 32);
    if (rc != 0) { bp_wipe(E, sizeof E); return BP_CRYPTO_E_ENTROPY; }

    aad_len = build_aad(aad, k->aead, k->set8, envelope, extra, extra_len);
    bp_write_le32(ctr8, g_ctr_lo);
    bp_write_le32(ctr8 + 4, g_ctr_hi);
    bp_write_le32(aadlen4, aad_len);
    p[0].p = E;       p[0].n = 32;
    p[1].p = ctr8;    p[1].n = 8;
    p[2].p = aadlen4; p[2].n = 4;
    p[3].p = aad;     p[3].n = aad_len;
    p[4].p = pt;      p[4].n = pt_len;
    rc = bp_hk(k->aead, k->k_nonce, L_HEDGE, (bp_uint32)sizeof L_HEDGE - 1, p, 5, nonce, nl);
    bp_wipe(E, sizeof E);
    if (rc != BP_CRYPTO_OK) return rc;

    rc = aead_seal(k->aead, k->k_seal, nonce, aad, aad_len, pt, pt_len, ct_out, tag_out);
    if (rc != BP_CRYPTO_OK) return rc;
    memcpy(nonce_out, nonce, nl);
    g_ctr_lo++;
    if (g_ctr_lo == 0) g_ctr_hi++;
    k->seals++;
    return BP_CRYPTO_OK;
}

int bp_frame_open(const bp_keys *k, const bp_uint8 envelope[8],
                  const bp_uint8 *extra, bp_uint32 extra_len,
                  const bp_uint8 *nonce, const bp_uint8 *ct, bp_uint32 ct_len,
                  const bp_uint8 tag[16], bp_uint8 *pt_out)
{
    bp_uint8 aad[25 + BP_AAD_EXTRA_MAX];
    bp_uint32 aad_len;
    int rc;

    if (k == NULL || envelope == NULL || nonce == NULL || tag == NULL ||
        (ct_len != 0 && (ct == NULL || pt_out == NULL)) ||
        extra_len > BP_AAD_EXTRA_MAX || (extra_len != 0 && extra == NULL))
        return BP_CRYPTO_E_ARG;
    rc = check_envelope(k->aead, envelope, ct_len);
    if (rc != BP_CRYPTO_OK) return rc;
    aad_len = build_aad(aad, k->aead, k->set8, envelope, extra, extra_len);
    return aead_open(k->aead, k->k_seal, nonce, aad, aad_len, ct, ct_len, tag, pt_out);
}

/* ---- test support -------------------------------------------------------- */

#ifdef BP_CRYPTO_TESTING
bp_uint32 bp_test_build_aad(int aead, const bp_uint8 set8[8], const bp_uint8 envelope[8],
                            const bp_uint8 *extra, bp_uint32 extra_len, bp_uint8 *out)
{
    return build_aad(out, aead, set8, envelope, extra, extra_len);
}

int bp_test_aead_seal(int aead, const bp_uint8 *key, const bp_uint8 *nonce,
                      const bp_uint8 *ad, bp_uint32 ad_len, const bp_uint8 *pt,
                      bp_uint32 pt_len, bp_uint8 *ct, bp_uint8 tag[16])
{
    return aead_seal(aead, key, nonce, ad, ad_len, pt, pt_len, ct, tag);
}

int bp_test_aead_open(int aead, const bp_uint8 *key, const bp_uint8 *nonce,
                      const bp_uint8 *ad, bp_uint32 ad_len, const bp_uint8 *ct,
                      bp_uint32 ct_len, const bp_uint8 tag[16], bp_uint8 *pt)
{
    return aead_open(aead, key, nonce, ad, ad_len, ct, ct_len, tag, pt);
}

int bp_test_argon2id_raw(bp_uint8 *out, bp_uint32 outlen, const bp_uint8 *pw, bp_uint32 pwlen,
                         const bp_uint8 *salt, bp_uint32 saltlen, const bp_uint8 *secret,
                         bp_uint32 secretlen, const bp_uint8 *ad, bp_uint32 adlen,
                         bp_uint32 m_kib, bp_uint32 t, bp_uint32 lanes)
{
#ifdef BP_CRYPTO_S1
    return bp_s1_argon2id(out, outlen, pw, pwlen, salt, saltlen, secret, secretlen, ad, adlen,
                          m_kib, t, lanes) == 0 ? BP_CRYPTO_OK : BP_CRYPTO_E_ARG;
#else
    (void)out; (void)outlen; (void)pw; (void)pwlen; (void)salt; (void)saltlen;
    (void)secret; (void)secretlen; (void)ad; (void)adlen; (void)m_kib; (void)t; (void)lanes;
    return BP_CRYPTO_E_UNSUPPORTED;
#endif
}
#endif
