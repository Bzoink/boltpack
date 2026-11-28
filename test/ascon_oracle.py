#!/usr/bin/env python3
"""Independent Ascon (NIST SP 800-232) written from the specification.

No code is shared with ascon-c or with BoltPack's C.  Ascon-AEAD128,
Ascon-Hash256 and Ascon-XOF128.  Words are little-endian (SP 800-232).
Used as an oracle: phase 0 checks it against the KAT files that ship with
ascon-c; later phases check the C89 port against it.
"""
M = (1 << 64) - 1

def ror(x, n):
    return ((x >> n) | (x << (64 - n))) & M

def perm(S, rounds):
    x0, x1, x2, x3, x4 = S
    for i in range(12 - rounds, 12):
        x2 ^= ((0xF - i) << 4) | i
        x0 ^= x4; x4 ^= x3; x2 ^= x1
        t0 = (~x0 & M) & x1; t1 = (~x1 & M) & x2; t2 = (~x2 & M) & x3
        t3 = (~x3 & M) & x4; t4 = (~x4 & M) & x0
        x0 ^= t1; x1 ^= t2; x2 ^= t3; x3 ^= t4; x4 ^= t0
        x1 ^= x0; x0 ^= x4; x3 ^= x2; x2 = ~x2 & M
        x0 ^= ror(x0, 19) ^ ror(x0, 28)
        x1 ^= ror(x1, 61) ^ ror(x1, 39)
        x2 ^= ror(x2, 1) ^ ror(x2, 6)
        x3 ^= ror(x3, 10) ^ ror(x3, 17)
        x4 ^= ror(x4, 7) ^ ror(x4, 41)
    return [x0, x1, x2, x3, x4]

def le(b):
    return int.from_bytes(b, "little")

def tob(x, n):
    return x.to_bytes(n, "little")

def pad(block, rate):
    # partial block of len < rate: append 0x01 then zeros (little-endian word form)
    return block + b"\x01" + b"\x00" * (rate - len(block) - 1)

AEAD_IV = 0x00001000808C0001

def aead_encrypt(key, nonce, ad, pt):
    assert len(key) == 16 and len(nonce) == 16
    K0, K1 = le(key[:8]), le(key[8:])
    S = perm([AEAD_IV, K0, K1, le(nonce[:8]), le(nonce[8:])], 12)
    S[3] ^= K0; S[4] ^= K1
    if ad:
        i = 0
        while len(ad) - i >= 16:
            S[0] ^= le(ad[i:i+8]); S[1] ^= le(ad[i+8:i+16]); S = perm(S, 8); i += 16
        last = pad(ad[i:], 16)
        S[0] ^= le(last[:8]); S[1] ^= le(last[8:])
        S = perm(S, 8)
    S[4] ^= 1 << 63
    ct = b""
    i = 0
    while len(pt) - i >= 16:
        S[0] ^= le(pt[i:i+8]); S[1] ^= le(pt[i+8:i+16])
        ct += tob(S[0], 8) + tob(S[1], 8)
        S = perm(S, 8); i += 16
    last = pt[i:]
    p = pad(last, 16)
    S[0] ^= le(p[:8]); S[1] ^= le(p[8:])
    ct += (tob(S[0], 8) + tob(S[1], 8))[:len(last)]
    S[2] ^= K0; S[3] ^= K1
    S = perm(S, 12)
    S[3] ^= K0; S[4] ^= K1
    return ct + tob(S[3], 8) + tob(S[4], 8)

def aead_decrypt(key, nonce, ad, ct_tag):
    if len(ct_tag) < 16:
        return None
    ct, tag = ct_tag[:-16], ct_tag[-16:]
    K0, K1 = le(key[:8]), le(key[8:])
    S = perm([AEAD_IV, K0, K1, le(nonce[:8]), le(nonce[8:])], 12)
    S[3] ^= K0; S[4] ^= K1
    if ad:
        i = 0
        while len(ad) - i >= 16:
            S[0] ^= le(ad[i:i+8]); S[1] ^= le(ad[i+8:i+16]); S = perm(S, 8); i += 16
        last = pad(ad[i:], 16)
        S[0] ^= le(last[:8]); S[1] ^= le(last[8:])
        S = perm(S, 8)
    S[4] ^= 1 << 63
    pt = b""
    i = 0
    while len(ct) - i >= 16:
        c0, c1 = le(ct[i:i+8]), le(ct[i+8:i+16])
        pt += tob(S[0] ^ c0, 8) + tob(S[1] ^ c1, 8)
        S[0], S[1] = c0, c1
        S = perm(S, 8); i += 16
    last = ct[i:]
    ks = tob(S[0], 8) + tob(S[1], 8)
    plast = bytes(a ^ b for a, b in zip(last, ks))
    pt += plast
    p = pad(plast, 16)
    S[0] ^= le(p[:8]); S[1] ^= le(p[8:])
    S[2] ^= K0; S[3] ^= K1
    S = perm(S, 12)
    S[3] ^= K0; S[4] ^= K1
    t = tob(S[3], 8) + tob(S[4], 8)
    diff = 0
    for a, b in zip(t, tag):
        diff |= a ^ b
    return pt if diff == 0 else None

HASH_IV = 0x0000080100CC0002
XOF_IV = 0x0000080000CC0003

def _sponge(iv, msg, outlen):
    S = perm([iv, 0, 0, 0, 0], 12)
    i = 0
    while len(msg) - i >= 8:
        S[0] ^= le(msg[i:i+8]); S = perm(S, 12); i += 8
    S[0] ^= le(pad(msg[i:], 8))
    out = b""
    while True:
        S = perm(S, 12)
        out += tob(S[0], 8)
        if len(out) >= outlen:
            return out[:outlen]

def hash256(msg):
    return _sponge(HASH_IV, msg, 32)

def xof128(msg, outlen):
    return _sponge(XOF_IV, msg, outlen)

CXOF_IV = 0x0000080000CC0004

def cxof128(msg, outlen, z=b""):
    """Ascon-CXOF128(msg, outlen, customization string z), SP 800-232."""
    assert len(z) <= 256
    S = perm([CXOF_IV, 0, 0, 0, 0], 12)
    S[0] ^= len(z) * 8                      # the length of z in bits, as a 64-bit word
    S = perm(S, 12)
    i = 0
    while len(z) - i >= 8:
        S[0] ^= le(z[i:i+8]); S = perm(S, 12); i += 8
    S[0] ^= le(pad(z[i:], 8)); S = perm(S, 12)
    i = 0
    while len(msg) - i >= 8:
        S[0] ^= le(msg[i:i+8]); S = perm(S, 12); i += 8
    S[0] ^= le(pad(msg[i:], 8))
    out = b""
    while True:
        S = perm(S, 12)
        out += tob(S[0], 8)
        if len(out) >= outlen:
            return out[:outlen]

