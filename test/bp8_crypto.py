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

def hk(aead, key32, label, data, n):
    """Keyed, labelled hash (draft 2).  S1: BLAKE2b keyed with key32 over label || 0x00 || data.
    S2: Ascon-CXOF128 with the label as customisation string over key32 || data || n (u32 LE),
    n bytes: the idiom of KMAC, with the output length bound into the message."""
    assert len(key32) == 32 and 1 <= len(label) <= 64
    if aead == S1:
        return hashlib.blake2b(label + b"\0" + data, digest_size=n, key=key32).digest()
    return ascon.cxof128(key32 + data + struct.pack("<I", n), n, label)

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
        master = hk(aead, secret, b"bp8/master", salt, 32)      # the 32-byte key is the key of the hash
    else:
        master = argon2id(secret, salt, m, t, lanes, 32)
    k_seal = hk(aead, master, b"bp8/seal", set8, 32)
    k_nonce = hk(aead, master, b"bp8/nonce-key", set8, 32)
    verifier = hk(aead, master, b"bp8/verify", set8 + pack32(kdf, aead, norm, m, t, lanes, salt), 16)
    return master, k_seal, k_nonce, verifier

# --- frames (draft section 4)

def envelope(kind, fflags, aead, pt_len):
    return bytes([kind, fflags, 0, 0]) + struct.pack("<I", NONCE_LEN[aead] + pt_len + 16)

def aad(aead, set8, env, extra):
    return b"bp8/aad\0" + bytes([aead]) + set8 + env + extra

def hedged_nonce(aead, k_nonce, E, ctr, aad_bytes, pt):
    data = E + struct.pack("<Q", ctr) + struct.pack("<I", len(aad_bytes)) + aad_bytes + pt
    return hk(aead, k_nonce, b"bp8/hedge", data, NONCE_LEN[aead])

def seal_frame(aead, set8, k_seal, k_nonce, ctr, E, env, extra, pt):
    a = aad(aead, set8, env, extra)
    nonce = hedged_nonce(aead, k_nonce, E, ctr, a, pt)
    ct_tag = raw_seal(aead, k_seal, nonce, a, pt)
    return nonce, ct_tag[:-16], ct_tag[-16:]

def open_frame(aead, set8, k_seal, env, extra, nonce, ct, tag):
    return raw_open(aead, k_seal, nonce, aad(aead, set8, env, extra), ct + tag)


# --- the entropy pool (plan 7.1) with no OS source: imported bytes, then draws

def pool_model(imported, draws):
    """State: 32 bytes.  Add: CXOF128(Z = "bp8/pool/add", M = state | source | len u32 | data | 32 as u32, 32).
    Draw n: CXOF128(Z = "bp8/pool/out", M = state | ctr u64 | (n + 32) as u32, n + 32): first n returned, last 32 the new state."""
    IMPORT = 3
    msg = bytes(32) + bytes([IMPORT]) + struct.pack("<I", len(imported)) + imported + struct.pack("<I", 32)
    state = ascon.cxof128(msg, 32, b"bp8/pool/add")
    ctr, out = 0, b""
    for n in draws:
        block = ascon.cxof128(state + struct.pack("<Q", ctr) + struct.pack("<I", n + 32), n + 32, b"bp8/pool/out")
        out += block[:n]
        state = block[n:]
        ctr += 1
    return out


# --- limits, budget, commitments (draft 8.1, 9.1, 6, 7)

def limits(aead, kind):
    """(min, max) plaintext length of a frame, or None for an invalid kind."""
    if kind == 1: return (0, 27 + (65501 if aead == S1 else 1024))
    if kind == 2: return (0, 8 + (16777216 if aead == S1 else 32768))
    if kind == 3: return (16, 16)
    if kind == 4: return (0, 1048576 if aead == S1 else 16384)
    if kind == 5: return (0, 0)
    return None

MAX_REVISION = 4096

def budget_ok(aead, frames, nbytes, revision, table_chunks, add_frames, add_bytes):
    est = frames + min(revision, MAX_REVISION) * table_chunks + add_frames
    if est > (2**24 if aead == S2 else 2**32): return 0
    if aead == S2 and nbytes + add_bytes > 2**40: return 0
    return 1

def commit(aead, tags):
    """table record `commit`: Hu(tag of the header frame || tags of the blocks || tag of the trailer, 16)"""
    return hu(aead, b"".join(tags), 16)

def table_hash(aead, chunk_tags):
    """Hu(tag of each table chunk frame in order, 16)"""
    return hu(aead, b"".join(chunk_tags), 16)
