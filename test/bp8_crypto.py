#!/usr/bin/env python3
"""Independent Python model of the cryptographic layer of BoltPack format 8.

Written from work/encryption-format-draft.md (sections 2 to 7), not from the C.
It is the first piece of the phase 2 oracle (test/bp8_bytes.py will import it).

Needs the `cryptography` package (ChaCha20-Poly1305) and the `openssl` command
(Argon2id, OpenSSL 3.2 or later); argon2-cffi and PyCryptodome are used as extra
cross-checks when installed.
"""
import hashlib
import os
import struct
import subprocess

from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

import ascon_oracle as ascon

S1, S2 = 1, 2
NONCE_LEN = {S1: 24, S2: 16}
KEY_LEN = {S1: 32, S2: 16}

# --- XChaCha20-Poly1305 from HChaCha20 + the 12-byte-nonce AEAD of `cryptography`

def _rotl(x, n):
    return ((x << n) & 0xFFFFFFFF) | (x >> (32 - n))

def hchacha20(key, nonce16):
    x = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    x += list(struct.unpack("<8I", key)) + list(struct.unpack("<4I", nonce16))
    def qr(a, b, c, d):
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rotl(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rotl(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = _rotl(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = _rotl(x[b] ^ x[c], 7)
    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack("<8I", *(x[0:4] + x[12:16]))

def xchacha_seal(key, nonce24, ad, pt):
    sub = hchacha20(key, nonce24[:16])
    return ChaCha20Poly1305(sub).encrypt(b"\0\0\0\0" + nonce24[16:], pt, ad or None)  # ct || tag

def xchacha_open(key, nonce24, ad, ct_tag):
    sub = hchacha20(key, nonce24[:16])
    try:
        return ChaCha20Poly1305(sub).decrypt(b"\0\0\0\0" + nonce24[16:], ct_tag, ad or None)
    except Exception:
        return None

# --- hashes (draft section 2)

def hu(aead, data, n):
    if aead == S1:
        return hashlib.blake2b(data, digest_size=n).digest()
    return ascon.xof128(data, n)

def hk(aead, key32, data, n):
    assert len(key32) == 32
    if aead == S1:
        return hashlib.blake2b(data, digest_size=n, key=key32).digest()
    return ascon.xof128(key32 + data, n)

def raw_seal(aead, key, nonce, ad, pt):
    if aead == S1:
        return xchacha_seal(key, nonce, ad, pt)
    return ascon.aead_encrypt(key[:16], nonce, ad, pt)

def raw_open(aead, key, nonce, ad, ct_tag):
    if aead == S1:
        return xchacha_open(key, nonce, ad, ct_tag)
    return ascon.aead_decrypt(key[:16], nonce, ad, ct_tag)

# --- Argon2id through the openssl command (independent of Monocypher)

def argon2id(pw, salt, m_kib, t, lanes, outlen, secret=b"", ad=b""):
    opts = ["-kdfopt", "hexpass:" + pw.hex(), "-kdfopt", "hexsalt:" + salt.hex(),
            "-kdfopt", "iter:%d" % t, "-kdfopt", "memcost:%d" % m_kib,
            "-kdfopt", "lanes:%d" % lanes, "-kdfopt", "threads:1"]
    if secret: opts += ["-kdfopt", "hexsecret:" + secret.hex()]
    if ad: opts += ["-kdfopt", "hexad:" + ad.hex()]
    return subprocess.run(["openssl", "kdf", "-keylen", str(outlen)] + opts + ["-binary", "ARGON2ID"],
                          capture_output=True, check=True, timeout=120).stdout

def argon2id_cffi(pw, salt, m_kib, t, lanes, outlen):
    try:
        from argon2.low_level import hash_secret_raw, Type
    except ImportError:
        return None
    return hash_secret_raw(pw, salt, time_cost=t, memory_cost=m_kib, parallelism=lanes, hash_len=outlen, type=Type.ID)

# --- keys (draft sections 3 and 5)

def pack32(kdf, aead, norm, m, t, lanes, salt):
    return b"BKEY" + bytes([1, kdf, aead, 1]) + struct.pack("<IHBB", m, t, lanes, norm) + salt

def derive(kdf, aead, norm, m, t, lanes, salt, set8, secret):
    if kdf == 0:
        master = hu(aead, b"bp8/master\0" + salt + secret, 32)
    else:
        master = argon2id(secret, salt, m, t, lanes, 32)
    k_seal = hk(aead, master, b"bp8/seal\0" + set8, 32)
    k_nonce = hk(aead, master, b"bp8/nonce\0" + set8, 32)
    verifier = hk(aead, master, b"bp8/verify\0" + set8 + pack32(kdf, aead, norm, m, t, lanes, salt), 16)
    return master, k_seal, k_nonce, verifier

# --- frames (draft section 4)

def envelope(kind, fflags, aead, pt_len):
    return bytes([kind, fflags, 0, 0]) + struct.pack("<I", NONCE_LEN[aead] + pt_len + 16)

def aad(aead, set8, env, extra):
    return b"bp8/aad\0" + bytes([aead]) + set8 + env + extra

def hedged_nonce(aead, k_nonce, E, ctr, aad_bytes, pt):
    data = b"bp8/nonce\0" + E + struct.pack("<Q", ctr) + struct.pack("<I", len(aad_bytes)) + aad_bytes + pt
    return hk(aead, k_nonce, data, NONCE_LEN[aead])

def seal_frame(aead, set8, k_seal, k_nonce, ctr, E, env, extra, pt):
    a = aad(aead, set8, env, extra)
    nonce = hedged_nonce(aead, k_nonce, E, ctr, a, pt)
    ct_tag = raw_seal(aead, k_seal, nonce, a, pt)
    return nonce, ct_tag[:-16], ct_tag[-16:]

def open_frame(aead, set8, k_seal, env, extra, nonce, ct, tag):
    return raw_open(aead, k_seal, nonce, aad(aead, set8, env, extra), ct + tag)


# --- the entropy pool (plan 7.1) with no OS source: imported bytes, then draws

def pool_model(imported, draws):
    """State: 32 bytes.  Add: XOF("bp8/pool/add\0" | state | source | len u32 | data, 32).
    Draw n: XOF("bp8/pool/out\0" | state | ctr u64, n + 32): first n returned, last 32 the new state."""
    IMPORT = 3
    state = ascon.xof128(b"bp8/pool/add\0" + bytes(32) + bytes([IMPORT]) + struct.pack("<I", len(imported)) + imported, 32)
    ctr, out = 0, b""
    for n in draws:
        block = ascon.xof128(b"bp8/pool/out\0" + state + struct.pack("<Q", ctr), n + 32)
        out += block[:n]
        state = block[n:]
        ctr += 1
    return out
