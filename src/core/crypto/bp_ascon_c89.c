/*
 * bp_ascon_c89.c -- Ascon-AEAD128 and Ascon-XOF128 (NIST SP 800-232) in strict C89
 * behind bp_ascon.h.  No 64-bit type, no multiplication, no table lookup.
 *
 * A 64-bit Ascon word is held as NL limbs of AW_BITS bits, least significant limb
 * first: two 32-bit limbs by default, four 16-bit limbs with BP_ASCON_WORD16 (for
 * 8086-class compilers, where a 32-bit limb is two registers).  Every rotation of
 * Ascon (19, 28, 61, 39, 1, 6, 10, 17, 7, 41) is neither a multiple of 16 nor of 32,
 * so the bit shift of a limb rotation is never 0 (a shift by the full width is
 * undefined in C) in either variant; a check below asserts it at compile time.
 *
 * Limbs are `unsigned int` or `bp_uint32` and every value is masked to AW_BITS after
 * a complement or a left shift, so the code does not depend on the host's integer
 * width.  BP_ASCON_LIMB_SHORT stores 16-bit limbs in `unsigned short` (which
 * promotes to signed int on a 32-bit host): the tests build that variant to prove
 * the promotion rules are respected.
 *
 * Bytes are loaded and stored with explicit loops: SP 800-232 is little-endian and
 * the 68000 is big-endian.
 *
 * Structure follows the logic of ascon-c ref/aead.c and crypto_hash/asconxof128/ref
 * (vendored under vendor/ascon-c); the tests compare it with them.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include <string.h>
#include "bp_ascon.h"
#include "bp_crypto.h"

#ifdef BP_ASCON_WORD16
#define AW_BITS 16
#define NL 4
#define AW_MASK 0xFFFFU
#ifdef BP_ASCON_LIMB_SHORT
typedef unsigned short aw;
#else
typedef unsigned int aw;
#endif
typedef unsigned int awc;           /* the type calculations are done in (16 bits or more) */
#else
#define AW_BITS 32
#define NL 2
#define AW_MASK 0xFFFFFFFFUL
typedef bp_uint32 aw;
typedef bp_uint32 awc;
#endif

typedef aw word[NL];                /* one 64-bit Ascon word */
typedef struct { word x[5]; } astate;

/* every rotation amount must leave a non-zero bit shift in a limb */
typedef char rot_ok_19[(19 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_28[(28 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_61[(61 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_39[(39 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_1[(1 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_6[(6 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_10[(10 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_17[(17 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_7[(7 % AW_BITS) != 0 ? 1 : -1];
typedef char rot_ok_41[(41 % AW_BITS) != 0 ? 1 : -1];

#define AWC(v) ((awc)(v))
#define LOW(v) ((aw)(AWC(v) & AW_MASK))

/* limb l of (w rotated right by n bits); n is a constant, so q and r fold */
#define ROTR_LIMB(w, l, n) \
    LOW(((AWC((w)[((l) + (n) / AW_BITS) % NL]) >> ((n) % AW_BITS)) | \
         (AWC((w)[((l) + (n) / AW_BITS + 1) % NL]) << (AW_BITS - (n) % AW_BITS))))

/* ---- 64-bit word helpers on limbs ---------------------------------------- */

static void w_load(word w, const bp_uint8 *p, unsigned n)   /* n bytes, 0..8, little-endian */
{
    unsigned i;
    for (i = 0; i < NL; i++) w[i] = 0;
    for (i = 0; i < n; i++)
        w[i / (AW_BITS / 8)] = LOW(AWC(w[i / (AW_BITS / 8)]) | (AWC(p[i]) << (8 * (i % (AW_BITS / 8)))));
}

static void w_store(bp_uint8 *p, const word w, unsigned n)  /* the low n bytes, 0..8 */
{
    unsigned i;
    for (i = 0; i < n; i++)
        p[i] = (bp_uint8)((AWC(w[i / (AW_BITS / 8)]) >> (8 * (i % (AW_BITS / 8)))) & 0xFFU);
}

static void w_xor(word a, const word b)
{
    unsigned i;
    for (i = 0; i < NL; i++) a[i] = LOW(AWC(a[i]) ^ AWC(b[i]));
}

static void w_copy(word a, const word b)
{
    unsigned i;
    for (i = 0; i < NL; i++) a[i] = b[i];
}

/* a ^= 0x01 << (8 * n): the padding byte after n message bytes, n in 0..7 */
static void w_pad(word a, unsigned n)
{
    a[n / (AW_BITS / 8)] = LOW(AWC(a[n / (AW_BITS / 8)]) ^ (AWC(1) << (8 * (n % (AW_BITS / 8)))));
}

/* clear the low n bytes (n in 0..8) */
static void w_clear_low(word a, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        unsigned l = i / (AW_BITS / 8), sh = 8 * (i % (AW_BITS / 8));
        a[l] = LOW(AWC(a[l]) & ~(AWC(0xFF) << sh));
    }
}

/* the domain separation bit: 0x80 in byte 7 */
static void w_dsep(word a)
{
    a[NL - 1] = LOW(AWC(a[NL - 1]) ^ (AWC(0x80) << (AW_BITS - 8)));
}

/* ---- the permutation ------------------------------------------------------- */

#define LIN(xw, A, B) do { \
        word t_; unsigned l_; \
        for (l_ = 0; l_ < NL; l_++) t_[l_] = LOW(AWC(xw[l_]) ^ AWC(ROTR_LIMB(xw, l_, A)) ^ AWC(ROTR_LIMB(xw, l_, B))); \
        for (l_ = 0; l_ < NL; l_++) xw[l_] = t_[l_]; \
    } while (0)

static void round_(astate *s, unsigned c)
{
    unsigned l;
    aw t0, t1, t2, t3, t4;
    s->x[2][0] = LOW(AWC(s->x[2][0]) ^ AWC(c));
    for (l = 0; l < NL; l++) {      /* the substitution layer acts on each bit position alone */
        aw x0 = s->x[0][l], x1 = s->x[1][l], x2 = s->x[2][l], x3 = s->x[3][l], x4 = s->x[4][l];
        x0 = LOW(AWC(x0) ^ AWC(x4)); x4 = LOW(AWC(x4) ^ AWC(x3)); x2 = LOW(AWC(x2) ^ AWC(x1));
        t0 = LOW(~AWC(x0) & AWC(x1)); t1 = LOW(~AWC(x1) & AWC(x2)); t2 = LOW(~AWC(x2) & AWC(x3));
        t3 = LOW(~AWC(x3) & AWC(x4)); t4 = LOW(~AWC(x4) & AWC(x0));
        x0 = LOW(AWC(x0) ^ AWC(t1)); x1 = LOW(AWC(x1) ^ AWC(t2)); x2 = LOW(AWC(x2) ^ AWC(t3));
        x3 = LOW(AWC(x3) ^ AWC(t4)); x4 = LOW(AWC(x4) ^ AWC(t0));
        x1 = LOW(AWC(x1) ^ AWC(x0)); x0 = LOW(AWC(x0) ^ AWC(x4)); x3 = LOW(AWC(x3) ^ AWC(x2));
        x2 = LOW(~AWC(x2));
        s->x[0][l] = x0; s->x[1][l] = x1; s->x[2][l] = x2; s->x[3][l] = x3; s->x[4][l] = x4;
    }
    LIN(s->x[0], 19, 28);
    LIN(s->x[1], 61, 39);
    LIN(s->x[2], 1, 6);
    LIN(s->x[3], 10, 17);
    LIN(s->x[4], 7, 41);
}

static void p12(astate *s)
{
    unsigned i;
    for (i = 0; i < 12; i++) round_(s, ((0xFU - i) << 4) | i);
}

static void p8(astate *s)
{
    unsigned i;
    for (i = 4; i < 12; i++) round_(s, ((0xFU - i) << 4) | i);
}

/* ---- AEAD ---------------------------------------------------------------- */

/* the 64-bit constants as limbs, lowest first */
static void w_const(word w, bp_uint32 lo, bp_uint32 hi)
{
    unsigned i;
    for (i = 0; i < NL; i++) {
        bp_uint32 src = (i * AW_BITS < 32) ? lo : hi;
        unsigned sh = (i * AW_BITS) % 32;
        w[i] = LOW((src >> sh) & AW_MASK);
    }
}

/* Ascon-AEAD128 IV 0x00001000808c0001 */
static void aead_init(astate *s, word K0, word K1, const bp_uint8 key[16], const bp_uint8 nonce[16],
                      const bp_uint8 *ad, bp_uint32 adlen)
{
    word iv;
    w_const(iv, 0x808C0001UL, 0x00001000UL);
    w_copy(s->x[0], iv);
    w_load(K0, key, 8); w_load(K1, key + 8, 8);
    w_copy(s->x[1], K0); w_copy(s->x[2], K1);
    w_load(s->x[3], nonce, 8); w_load(s->x[4], nonce + 8, 8);
    p12(s);
    w_xor(s->x[3], K0); w_xor(s->x[4], K1);
    if (adlen) {
        word a;
        while (adlen >= 16) {
            w_load(a, ad, 8); w_xor(s->x[0], a);
            w_load(a, ad + 8, 8); w_xor(s->x[1], a);
            p8(s);
            ad += 16; adlen -= 16;
        }
        if (adlen >= 8) {
            w_load(a, ad, 8); w_xor(s->x[0], a);
            w_load(a, ad + 8, adlen - 8); w_xor(s->x[1], a);
            w_pad(s->x[1], adlen - 8);
        } else {
            w_load(a, ad, adlen); w_xor(s->x[0], a);
            w_pad(s->x[0], adlen);
        }
        p8(s);
    }
    w_dsep(s->x[4]);
}

static void aead_finish(astate *s, const word K0, const word K1, bp_uint8 tag[16])
{
    w_xor(s->x[2], K0); w_xor(s->x[3], K1);
    p12(s);
    w_xor(s->x[3], K0); w_xor(s->x[4], K1);
    w_store(tag, s->x[3], 8);
    w_store(tag + 8, s->x[4], 8);
}

void bp_ascon_aead_seal(const bp_uint8 key[16], const bp_uint8 nonce[16],
                        const bp_uint8 *ad, bp_uint32 adlen,
                        const bp_uint8 *m, bp_uint32 mlen,
                        bp_uint8 *c, bp_uint8 tag[16])
{
    astate s;
    word K0, K1, a;
    aead_init(&s, K0, K1, key, nonce, ad, adlen);
    while (mlen >= 16) {
        w_load(a, m, 8); w_xor(s.x[0], a);
        w_load(a, m + 8, 8); w_xor(s.x[1], a);
        w_store(c, s.x[0], 8); w_store(c + 8, s.x[1], 8);
        p8(&s);
        m += 16; c += 16; mlen -= 16;
    }
    if (mlen >= 8) {
        w_load(a, m, 8); w_xor(s.x[0], a);
        w_load(a, m + 8, mlen - 8); w_xor(s.x[1], a);
        w_store(c, s.x[0], 8); w_store(c + 8, s.x[1], mlen - 8);
        w_pad(s.x[1], mlen - 8);
    } else {
        w_load(a, m, mlen); w_xor(s.x[0], a);
        w_store(c, s.x[0], mlen);
        w_pad(s.x[0], mlen);
    }
    aead_finish(&s, K0, K1, tag);
    bp_wipe(&s, (bp_uint32)sizeof s);
    bp_wipe(K0, (bp_uint32)sizeof K0); bp_wipe(K1, (bp_uint32)sizeof K1);
}

int bp_ascon_aead_open(const bp_uint8 key[16], const bp_uint8 nonce[16],
                       const bp_uint8 *ad, bp_uint32 adlen,
                       const bp_uint8 *c, bp_uint32 clen, const bp_uint8 tag[16],
                       bp_uint8 *m)
{
    astate s;
    word K0, K1, c0, c1, ks;
    bp_uint8 t[16];
    bp_uint8 *m0 = m;
    bp_uint32 mlen0 = clen;
    int ok;

    aead_init(&s, K0, K1, key, nonce, ad, adlen);
    while (clen >= 16) {
        w_load(c0, c, 8); w_load(c1, c + 8, 8);
        w_copy(ks, s.x[0]); w_xor(ks, c0); w_store(m, ks, 8);
        w_copy(ks, s.x[1]); w_xor(ks, c1); w_store(m + 8, ks, 8);
        w_copy(s.x[0], c0); w_copy(s.x[1], c1);
        p8(&s);
        m += 16; c += 16; clen -= 16;
    }
    if (clen >= 8) {
        w_load(c0, c, 8); w_load(c1, c + 8, clen - 8);
        w_copy(ks, s.x[0]); w_xor(ks, c0); w_store(m, ks, 8);
        w_copy(ks, s.x[1]); w_xor(ks, c1); w_store(m + 8, ks, clen - 8);
        w_copy(s.x[0], c0);
        w_clear_low(s.x[1], clen - 8);
        w_xor(s.x[1], c1);
        w_pad(s.x[1], clen - 8);
    } else {
        w_load(c0, c, clen);
        w_copy(ks, s.x[0]); w_xor(ks, c0); w_store(m, ks, clen);
        w_clear_low(s.x[0], clen);
        w_xor(s.x[0], c0);
        w_pad(s.x[0], clen);
    }
    aead_finish(&s, K0, K1, t);
    ok = bp_ct_equal(t, tag, 16);
    bp_wipe(&s, (bp_uint32)sizeof s);
    bp_wipe(K0, (bp_uint32)sizeof K0); bp_wipe(K1, (bp_uint32)sizeof K1);
    bp_wipe(t, sizeof t);
    if (!ok) { bp_wipe(m0, mlen0); return -1; }
    return 0;
}

/* ---- XOF128 ---------------------------------------------------------------- */

/* bp_xof keeps the state as ten bp_uint32 (lo, hi) pairs whatever the limb width */
static void xof_load(const bp_xof *x, astate *s)
{
    unsigned i;
    for (i = 0; i < 5; i++) {
#ifdef BP_ASCON_WORD16
        s->x[i][0] = LOW(x->s[2 * i] & 0xFFFFUL);
        s->x[i][1] = LOW((x->s[2 * i] >> 16) & 0xFFFFUL);
        s->x[i][2] = LOW(x->s[2 * i + 1] & 0xFFFFUL);
        s->x[i][3] = LOW((x->s[2 * i + 1] >> 16) & 0xFFFFUL);
#else
        s->x[i][0] = x->s[2 * i];
        s->x[i][1] = x->s[2 * i + 1];
#endif
    }
}

static void xof_store(bp_xof *x, const astate *s)
{
    unsigned i;
    for (i = 0; i < 5; i++) {
#ifdef BP_ASCON_WORD16
        x->s[2 * i] = (bp_uint32)s->x[i][0] | ((bp_uint32)s->x[i][1] << 16);
        x->s[2 * i + 1] = (bp_uint32)s->x[i][2] | ((bp_uint32)s->x[i][3] << 16);
#else
        x->s[2 * i] = s->x[i][0];
        x->s[2 * i + 1] = s->x[i][1];
#endif
    }
}

void bp_xof_init(bp_xof *x)
{
    astate s;
    unsigned i;
    /* Ascon-XOF128 IV 0x0000080000cc0003 */
    w_const(s.x[0], 0x00CC0003UL, 0x00000800UL);
    for (i = 1; i < 5; i++) { unsigned l; for (l = 0; l < NL; l++) s.x[i][l] = 0; }
    p12(&s);
    xof_store(x, &s);
    memset(x->buf, 0, sizeof x->buf);
    x->pos = 0;
    x->squeezing = 0;
    bp_wipe(&s, (bp_uint32)sizeof s);
}

void bp_xof_absorb(bp_xof *x, const bp_uint8 *data, bp_uint32 len)
{
    astate s;
    word a;
    if (x->squeezing || len == 0) return;
    xof_load(x, &s);
    while (len > 0) {
        if (x->pos == 0 && len >= 8) {
            w_load(a, data, 8); w_xor(s.x[0], a);
            p12(&s);
            data += 8; len -= 8;
        } else {
            bp_uint32 take = 8 - x->pos;
            if (take > len) take = len;
            memcpy(x->buf + x->pos, data, take);
            x->pos += take; data += take; len -= take;
            if (x->pos == 8) {
                w_load(a, x->buf, 8); w_xor(s.x[0], a);
                p12(&s);
                x->pos = 0;
            }
        }
    }
    xof_store(x, &s);
    bp_wipe(&s, (bp_uint32)sizeof s);
    bp_wipe(a, (bp_uint32)sizeof a);
}

void bp_xof_squeeze(bp_xof *x, bp_uint8 *out, bp_uint32 len)
{
    astate s;
    word a;
    xof_load(x, &s);
    if (!x->squeezing) {
        w_load(a, x->buf, x->pos); w_xor(s.x[0], a);
        w_pad(s.x[0], x->pos);
        p12(&s);
        x->squeezing = 1;
        x->pos = 0;
    }
    while (len > 0) {
        bp_uint32 take;
        bp_uint8 blk[8];
        if (x->pos == 8) { p12(&s); x->pos = 0; }
        take = 8 - x->pos;
        if (take > len) take = len;
        w_store(blk, s.x[0], 8);
        memcpy(out, blk + x->pos, take);
        x->pos += take; out += take; len -= take;
    }
    xof_store(x, &s);
    bp_wipe(&s, (bp_uint32)sizeof s);
    bp_wipe(a, (bp_uint32)sizeof a);
}
