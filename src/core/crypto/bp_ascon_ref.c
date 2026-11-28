/*
 * bp_ascon_ref.c -- the Ascon backend (bp_ascon.h) on the vendored ascon-c
 * reference permutation.  C99 (the reference uses stdint.h and inline).
 *
 * This file is the phase 1 stand-in.  Phase 1b replaces it with a strict C89
 * implementation behind the same interface; vendor/ascon-c/ref/aead.c stays in
 * the tree as the reference the replacement is tested against.
 *
 * AEAD logic follows ascon-c ref/aead.c (Ascon-AEAD128, SP 800-232);
 * XOF logic follows ascon-c crypto_hash/asconxof128/ref/hash.c.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include <string.h>
#include "bp_ascon.h"
#include "bp_crypto.h"
#include "ascon.h"
#include "constants.h"
#include "permutations.h"
#include "word.h"

void bp_ascon_aead_seal(const bp_uint8 key[16], const bp_uint8 nonce[16],
                        const bp_uint8 *ad, bp_uint32 adlen,
                        const bp_uint8 *m, bp_uint32 mlen,
                        bp_uint8 *c, bp_uint8 tag[16])
{
    ascon_state_t s;
    const uint64_t K0 = LOADBYTES(key, 8), K1 = LOADBYTES(key + 8, 8);
    const uint64_t N0 = LOADBYTES(nonce, 8), N1 = LOADBYTES(nonce + 8, 8);

    s.x[0] = ASCON_128A_IV; s.x[1] = K0; s.x[2] = K1; s.x[3] = N0; s.x[4] = N1;
    P12(&s);
    s.x[3] ^= K0; s.x[4] ^= K1;
    if (adlen) {
        while (adlen >= ASCON_128A_RATE) {
            s.x[0] ^= LOADBYTES(ad, 8); s.x[1] ^= LOADBYTES(ad + 8, 8);
            P8(&s);
            ad += ASCON_128A_RATE; adlen -= ASCON_128A_RATE;
        }
        if (adlen >= 8) {
            s.x[0] ^= LOADBYTES(ad, 8);
            s.x[1] ^= LOADBYTES(ad + 8, (int)(adlen - 8));
            s.x[1] ^= PAD(adlen - 8);
        } else {
            s.x[0] ^= LOADBYTES(ad, (int)adlen);
            s.x[0] ^= PAD(adlen);
        }
        P8(&s);
    }
    s.x[4] ^= DSEP();
    while (mlen >= ASCON_128A_RATE) {
        s.x[0] ^= LOADBYTES(m, 8); s.x[1] ^= LOADBYTES(m + 8, 8);
        STOREBYTES(c, s.x[0], 8); STOREBYTES(c + 8, s.x[1], 8);
        P8(&s);
        m += ASCON_128A_RATE; c += ASCON_128A_RATE; mlen -= ASCON_128A_RATE;
    }
    if (mlen >= 8) {
        s.x[0] ^= LOADBYTES(m, 8);
        s.x[1] ^= LOADBYTES(m + 8, (int)(mlen - 8));
        STOREBYTES(c, s.x[0], 8);
        STOREBYTES(c + 8, s.x[1], (int)(mlen - 8));
        s.x[1] ^= PAD(mlen - 8);
    } else {
        s.x[0] ^= LOADBYTES(m, (int)mlen);
        STOREBYTES(c, s.x[0], (int)mlen);
        s.x[0] ^= PAD(mlen);
    }
    s.x[2] ^= K0; s.x[3] ^= K1;
    P12(&s);
    s.x[3] ^= K0; s.x[4] ^= K1;
    STOREBYTES(tag, s.x[3], 8);
    STOREBYTES(tag + 8, s.x[4], 8);
    bp_wipe(&s, (bp_uint32)sizeof s);
}

int bp_ascon_aead_open(const bp_uint8 key[16], const bp_uint8 nonce[16],
                       const bp_uint8 *ad, bp_uint32 adlen,
                       const bp_uint8 *c, bp_uint32 clen, const bp_uint8 tag[16],
                       bp_uint8 *m)
{
    ascon_state_t s;
    bp_uint8 t[16];
    bp_uint8 *m0 = m;
    bp_uint32 mlen0 = clen;
    const uint64_t K0 = LOADBYTES(key, 8), K1 = LOADBYTES(key + 8, 8);
    const uint64_t N0 = LOADBYTES(nonce, 8), N1 = LOADBYTES(nonce + 8, 8);
    int ok;

    s.x[0] = ASCON_128A_IV; s.x[1] = K0; s.x[2] = K1; s.x[3] = N0; s.x[4] = N1;
    P12(&s);
    s.x[3] ^= K0; s.x[4] ^= K1;
    if (adlen) {
        while (adlen >= ASCON_128A_RATE) {
            s.x[0] ^= LOADBYTES(ad, 8); s.x[1] ^= LOADBYTES(ad + 8, 8);
            P8(&s);
            ad += ASCON_128A_RATE; adlen -= ASCON_128A_RATE;
        }
        if (adlen >= 8) {
            s.x[0] ^= LOADBYTES(ad, 8);
            s.x[1] ^= LOADBYTES(ad + 8, (int)(adlen - 8));
            s.x[1] ^= PAD(adlen - 8);
        } else {
            s.x[0] ^= LOADBYTES(ad, (int)adlen);
            s.x[0] ^= PAD(adlen);
        }
        P8(&s);
    }
    s.x[4] ^= DSEP();
    while (clen >= ASCON_128A_RATE) {
        uint64_t c0 = LOADBYTES(c, 8), c1 = LOADBYTES(c + 8, 8);
        STOREBYTES(m, s.x[0] ^ c0, 8); STOREBYTES(m + 8, s.x[1] ^ c1, 8);
        s.x[0] = c0; s.x[1] = c1;
        P8(&s);
        m += ASCON_128A_RATE; c += ASCON_128A_RATE; clen -= ASCON_128A_RATE;
    }
    if (clen >= 8) {
        uint64_t c0 = LOADBYTES(c, 8), c1 = LOADBYTES(c + 8, (int)(clen - 8));
        STOREBYTES(m, s.x[0] ^ c0, 8);
        STOREBYTES(m + 8, s.x[1] ^ c1, (int)(clen - 8));
        s.x[0] = c0;
        s.x[1] = CLEARBYTES(s.x[1], (int)(clen - 8));
        s.x[1] |= c1;
        s.x[1] ^= PAD(clen - 8);
    } else {
        uint64_t c0 = LOADBYTES(c, (int)clen);
        STOREBYTES(m, s.x[0] ^ c0, (int)clen);
        s.x[0] = CLEARBYTES(s.x[0], (int)clen);
        s.x[0] |= c0;
        s.x[0] ^= PAD(clen);
    }
    s.x[2] ^= K0; s.x[3] ^= K1;
    P12(&s);
    s.x[3] ^= K0; s.x[4] ^= K1;
    STOREBYTES(t, s.x[3], 8);
    STOREBYTES(t + 8, s.x[4], 8);
    ok = bp_ct_equal(t, tag, 16);
    bp_wipe(&s, (bp_uint32)sizeof s);
    bp_wipe(t, sizeof t);
    if (!ok) { bp_wipe(m0, mlen0); return -1; }
    return 0;
}

/* ---- XOF128 ---------------------------------------------------------------- */

static void xof_load(const bp_xof *x, ascon_state_t *s)
{
    int i;
    for (i = 0; i < 5; i++)
        s->x[i] = (uint64_t)x->s[2 * i] | ((uint64_t)x->s[2 * i + 1] << 32);
}

static void xof_store(bp_xof *x, const ascon_state_t *s)
{
    int i;
    for (i = 0; i < 5; i++) {
        x->s[2 * i] = (bp_uint32)(s->x[i] & 0xFFFFFFFFu);
        x->s[2 * i + 1] = (bp_uint32)(s->x[i] >> 32);
    }
}

void bp_xof_init(bp_xof *x)
{
    ascon_state_t s;
    s.x[0] = ASCON_XOF_IV; s.x[1] = 0; s.x[2] = 0; s.x[3] = 0; s.x[4] = 0;
    P12(&s);
    xof_store(x, &s);
    memset(x->buf, 0, sizeof x->buf);
    x->pos = 0;
    x->squeezing = 0;
    bp_wipe(&s, (bp_uint32)sizeof s);
}

void bp_xof_absorb(bp_xof *x, const bp_uint8 *data, bp_uint32 len)
{
    ascon_state_t s;
    if (x->squeezing || len == 0) return;
    xof_load(x, &s);
    while (len > 0) {
        if (x->pos == 0 && len >= 8) {
            s.x[0] ^= LOADBYTES(data, 8);
            P12(&s);
            data += 8; len -= 8;
        } else {
            bp_uint32 take = 8 - x->pos;
            if (take > len) take = len;
            memcpy(x->buf + x->pos, data, take);
            x->pos += take; data += take; len -= take;
            if (x->pos == 8) {
                s.x[0] ^= LOADBYTES(x->buf, 8);
                P12(&s);
                x->pos = 0;
            }
        }
    }
    xof_store(x, &s);
    bp_wipe(&s, (bp_uint32)sizeof s);
}

void bp_xof_squeeze(bp_xof *x, bp_uint8 *out, bp_uint32 len)
{
    ascon_state_t s;
    xof_load(x, &s);
    if (!x->squeezing) {
        s.x[0] ^= LOADBYTES(x->buf, (int)x->pos);
        s.x[0] ^= PAD(x->pos);
        P12(&s);
        x->squeezing = 1;
        x->pos = 0;
    }
    while (len > 0) {
        bp_uint32 take;
        if (x->pos == 8) { P12(&s); x->pos = 0; }
        take = 8 - x->pos;
        if (take > len) take = len;
        {
            bp_uint32 i;
            for (i = 0; i < take; i++) out[i] = GETBYTE(s.x[0], x->pos + i);
        }
        x->pos += take; out += take; len -= take;
    }
    xof_store(x, &s);
    bp_wipe(&s, (bp_uint32)sizeof s);
}
